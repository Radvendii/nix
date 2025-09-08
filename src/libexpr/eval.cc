#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/print-options.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/exit.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"
#include "nix/store/downstream-placeholder.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/filetransfer.hh"
#include "nix/expr/function-trace.hh"
#include "nix/store/profiles.hh"
#include "nix/expr/print.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/expr/gc-small-vector.hh"
#include "nix/util/url.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/tarball.hh"
#include "nix/fetchers/input-cache.hh"

#include "parser-tab.hh"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstring>
#include <optional>
#include <unistd.h>
#include <sys/time.h>
#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>
#include <boost/container/small_vector.hpp>

#ifndef _WIN32 // TODO use portable implementation
#  include <sys/resource.h>
#endif

#include "nix/util/strings-inline.hh"

using json = nlohmann::json;

namespace nix {

unsigned long nrUninitialized = 0;
unsigned long nrInt = 0;
unsigned long nrBool = 0;
unsigned long nrNull = 0;
unsigned long nrFloat = 0;
unsigned long nrExternal = 0;
unsigned long nrPrimOp = 0;
unsigned long nrAttrs = 0;
unsigned long nrListSmall = 0;
unsigned long nrPrimOpApp = 0;
unsigned long nrApp = 0;
unsigned long nrThunk = 0;
unsigned long nrLambda = 0;
unsigned long nrListN = 0;
unsigned long nrString = 0;
unsigned long nrPath = 0;

static char * allocString(size_t size)
{
    char * t;
    t = (char *) GC_MALLOC_ATOMIC(size);
    if (!t)
        throw std::bad_alloc();
    return t;
}

// When there's no need to write to the string, we can optimize away empty
// string allocations.
// This function handles makeImmutableString(std::string_view()) by returning
// the empty string.
static const char * makeImmutableString(std::string_view s)
{
    const size_t size = s.size();
    if (size == 0)
        return "";
    auto t = allocString(size + 1);
    memcpy(t, s.data(), size);
    t[size] = '\0';
    return t;
}

RootValue allocRootValue(ValueRef v)
{
    return std::allocate_shared<ValueRef>(traceable_allocator<ValueRef>(), v);
}

// Pretty print types for assertion errors
std::ostream & operator<<(std::ostream & os, const ValueType t)
{
    os << showType(t);
    return os;
}

std::string printValue(EvalState & state, ValueRef v)
{
    std::ostringstream out;
    v.print(state, out);
    return out.str();
}

Symbol::Symbol(const Key & key)
{
    auto size = key.str.size();
    // XXX [speed]: check if this is still a limitation
    if (size >= std::numeric_limits<uint32_t>::max()) {
        throw Error("Size of symbol exceeds 4GiB and cannot be stored");
    }
    // XXX [speed]: check if this comment still makes sense
    // for multi-threaded implementations: lock store and allocator here
    auto v = key.es.allocValue();

    // allocate enough bytes at the end of the SymbolData for our string
    auto data = (SymbolData *)key.alloc.allocate(sizeof(SymbolData) + size + 1);

    data->ref = SymbolRef{v};
    data->size = size;
    // XXX [speed]: had to remove a special-case for empty string that didn't require any allocation. I'm not sure if that impacts e.g. string comparison times, but the c_str pointer must point back to the SymbolData.
    memcpy(data->c_str, key.str.data(), size);
    data->c_str[size] = '\0';
    // XXX [speed]: there should either be a tSymbol Value type that fits in 8 bytes, or at least a contextless string type that does
    v.mkString(key.es.values, data->c_str, nullptr);
    this->data = data;
}

Symbol SymbolTable::operator[](SymbolRef ref) const
{
    // to get from our Value to the SymbolData we look behind the start of the
    // string by the length of one SymbolData
    return Symbol((SymbolData *) ref.c_str(es.values) - 1);
}

void Value::print(EvalState & state, std::ostream & str, PrintOptions options)
{
    printValue(state, str, this->ref(state.values), options);
}
void ValueRef::print(EvalState & state, std::ostream & str, PrintOptions options)
{
    printValue(state, str, *this, options);
}

std::string_view showType(ValueType type, bool withArticle)
{
#define WA(a, w) withArticle ? a " " w : w
    switch (type) {
    case nInt:
        return WA("an", "integer");
    case nBool:
        return WA("a", "Boolean");
    case nString:
        return WA("a", "string");
    case nPath:
        return WA("a", "path");
    case nNull:
        return "null";
    case nAttrs:
        return WA("a", "set");
    case nList:
        return WA("a", "list");
    case nFunction:
        return WA("a", "function");
    case nExternal:
        return WA("an", "external value");
    case nFloat:
        return WA("a", "float");
    case nThunk:
        return WA("a", "thunk");
    }
    unreachable();
}

std::string showType(EvalState & state, const ValueRef v)
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (v.getInternalType(state.values)) {
    case tString:
        return v.context(state.values) ? "a string with context" : "a string";
    case tPrimOp:
        return fmt("the built-in function '%s'", std::string(v.primOp(state.values)->name));
    case tPrimOpApp:
        return fmt("the partially applied built-in function '%s'", v.primOpAppPrimOp(state.values)->name);
    case tExternal:
        return v.external(state.values)->showType();
    case tThunk:
        return v.isBlackhole(state.values) ? "a black hole" : "a thunk";
    case tApp:
        return "a function application";
    default:
        return std::string(showType(v.type(state.values)));
    }
#pragma GCC diagnostic pop
}

PosIdx Value::determinePos(Values & values, const PosIdx pos) const
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (getInternalType()) {
    case tAttrs:
        return attrs()->pos;
    case tLambda:
        return lambda().fun->pos;
    case tApp:
        return app().left.determinePos(values, pos);
    default:
        return pos;
    }
#pragma GCC diagnostic pop
}
PosIdx ValueRef::determinePos(Values & values, const PosIdx pos) const
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (getInternalType(values)) {
    case tAttrs:
        return attrs(values)->pos;
    case tLambda:
        return lambda(values).fun->pos;
    case tApp:
        return app(values).left.determinePos(values, pos);
    default:
        return pos;
    }
#pragma GCC diagnostic pop
}

bool Value::isTrivial() const
{
    return !isa<tApp, tPrimOpApp>()
           && (!isa<tThunk>()
               || (dynamic_cast<ExprAttrs *>(thunk().expr) && ((ExprAttrs *) thunk().expr)->dynamicAttrs.empty())
               || dynamic_cast<ExprLambda *>(thunk().expr) || dynamic_cast<ExprList *>(thunk().expr));
}
bool ValueRef::isTrivial(Values & values) const
{
    return !isa<tApp, tPrimOpApp>(values)
           && (!isa<tThunk>(values)
               || (dynamic_cast<ExprAttrs *>(thunk(values).expr) && ((ExprAttrs *) thunk(values).expr)->dynamicAttrs.empty())
               || dynamic_cast<ExprLambda *>(thunk(values).expr) || dynamic_cast<ExprList *>(thunk(values).expr));
}

static SymbolRef getName(const AttrName & name, EvalState & state, Env & env)
{
    if (name.symbol) {
        return name.symbol;
    } else {
        Value nameValue;
        name.expr->eval(state, env, nameValue.ref(state.values));
        state.forceStringNoCtx(nameValue.ref(state.values), name.expr->getPos(), "while evaluating an attribute name");
        return state.symbols.create(nameValue.string_view());
    }
}

static constexpr size_t BASE_ENV_SIZE = 128;

EvalState::EvalState(
    const LookupPath & lookupPathFromArguments,
    ref<Store> store,
    const fetchers::Settings & fetchSettings,
    const EvalSettings & settings,
    std::shared_ptr<Store> buildStore)
    : fetchSettings{fetchSettings}
    , settings{settings}
    , values()
    , symbols(*this)
    , sWith(symbols.create("<with>"))
    , sOutPath(symbols.create("outPath"))
    , sDrvPath(symbols.create("drvPath"))
    , sType(symbols.create("type"))
    , sMeta(symbols.create("meta"))
    , sName(symbols.create("name"))
    , sValue(symbols.create("value"))
    , sSystem(symbols.create("system"))
    , sOverrides(symbols.create("__overrides"))
    , sOutputs(symbols.create("outputs"))
    , sOutputName(symbols.create("outputName"))
    , sIgnoreNulls(symbols.create("__ignoreNulls"))
    , sFile(symbols.create("file"))
    , sLine(symbols.create("line"))
    , sColumn(symbols.create("column"))
    , sFunctor(symbols.create("__functor"))
    , sToString(symbols.create("__toString"))
    , sRight(symbols.create("right"))
    , sWrong(symbols.create("wrong"))
    , sStructuredAttrs(symbols.create("__structuredAttrs"))
    , sJson(symbols.create("__json"))
    , sAllowedReferences(symbols.create("allowedReferences"))
    , sAllowedRequisites(symbols.create("allowedRequisites"))
    , sDisallowedReferences(symbols.create("disallowedReferences"))
    , sDisallowedRequisites(symbols.create("disallowedRequisites"))
    , sMaxSize(symbols.create("maxSize"))
    , sMaxClosureSize(symbols.create("maxClosureSize"))
    , sBuilder(symbols.create("builder"))
    , sArgs(symbols.create("args"))
    , sContentAddressed(symbols.create("__contentAddressed"))
    , sImpure(symbols.create("__impure"))
    , sOutputHash(symbols.create("outputHash"))
    , sOutputHashAlgo(symbols.create("outputHashAlgo"))
    , sOutputHashMode(symbols.create("outputHashMode"))
    , sRecurseForDerivations(symbols.create("recurseForDerivations"))
    , sDescription(symbols.create("description"))
    , sSelf(symbols.create("self"))
    , sEpsilon(symbols.create(""))
    , sStartSet(symbols.create("startSet"))
    , sOperator(symbols.create("operator"))
    , sKey(symbols.create("key"))
    , sPath(symbols.create("path"))
    , sPrefix(symbols.create("prefix"))
    , sOutputSpecified(symbols.create("outputSpecified"))
    , exprSymbols{
        .sub = symbols.create("__sub"),
        .lessThan = symbols.create("__lessThan"),
        .mul = symbols.create("__mul"),
        .div = symbols.create("__div"),
        .or_ = symbols.create("or"),
        .findFile = symbols.create("__findFile"),
        .nixPath = symbols.create("__nixPath"),
        .body = symbols.create("body"),
    }
    , repair(NoRepair)
    , emptyBindings(0)
    , storeFS(
        makeMountedSourceAccessor(
            {
                {CanonPath::root, makeEmptySourceAccessor()},
                /* In the pure eval case, we can simply require
                   valid paths. However, in the *impure* eval
                   case this gets in the way of the union
                   mechanism, because an invalid access in the
                   upper layer will *not* be caught by the union
                   source accessor, but instead abort the entire
                   lookup.

                   This happens when the store dir in the
                   ambient file system has a path (e.g. because
                   another Nix store there), but the relocated
                   store does not.

                   TODO make the various source accessors doing
                   access control all throw the same type of
                   exception, and make union source accessor
                   catch it, so we don't need to do this hack.
                 */
                {CanonPath(store->storeDir), store->getFSAccessor(settings.pureEval)},
            }))
    , rootFS(
        ({
            /* In pure eval mode, we provide a filesystem that only
               contains the Nix store.

               If we have a chroot store and pure eval is not enabled,
               use a union accessor to make the chroot store available
               at its logical location while still having the
               underlying directory available. This is necessary for
               instance if we're evaluating a file from the physical
               /nix/store while using a chroot store. */
            auto accessor = getFSSourceAccessor();

            auto realStoreDir = dirOf(store->toRealPath(StorePath::dummy));
            if (settings.pureEval || store->storeDir != realStoreDir) {
                accessor = settings.pureEval
                    ? storeFS
                    : makeUnionSourceAccessor({accessor, storeFS});
            }

            /* Apply access control if needed. */
            if (settings.restrictEval || settings.pureEval)
                accessor = AllowListSourceAccessor::create(accessor, {}, {},
                    [&settings](const CanonPath & path) -> RestrictedPathError {
                        auto modeInformation = settings.pureEval
                            ? "in pure evaluation mode (use '--impure' to override)"
                            : "in restricted mode";
                        throw RestrictedPathError("access to absolute path '%1%' is forbidden %2%", path, modeInformation);
                    });

            accessor;
        }))
    , corepkgsFS(make_ref<MemorySourceAccessor>())
    , internalFS(make_ref<MemorySourceAccessor>())
    , derivationInternal{corepkgsFS->addFile(
        CanonPath("derivation-internal.nix"),
#include "primops/derivation.nix.gen.hh"
    )}
    , store(store)
    , buildStore(buildStore ? buildStore : store)
    , inputCache(fetchers::InputCache::create())
    , debugRepl(nullptr)
    , debugStop(false)
    , trylevel(0)
    , regexCache(makeRegexCache())
#if NIX_USE_BOEHMGC
    // , values(100000)
    // , valueAllocCache(std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr))
    , env1AllocCache(std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr))
    , baseEnvP(std::allocate_shared<Env *>(traceable_allocator<Env *>(), &allocEnv(BASE_ENV_SIZE)))
    , baseEnv(**baseEnvP)
#else
    , baseEnv(allocEnv(BASE_ENV_SIZE))
#endif
    , staticBaseEnv{std::make_shared<StaticEnv>(nullptr, nullptr)}
{
    corepkgsFS->setPathDisplay("<nix", ">");
    internalFS->setPathDisplay("«nix-internal»", "");

    countCalls = getEnv("NIX_COUNT_CALLS").value_or("0") != "0";

    assertGCInitialized();

    static_assert(sizeof(Env) <= 16, "environment must be <= 16 bytes");

    (vEmptyList = allocValue()).mkList(values, buildList(0));
    (vNull = allocValue()).mkNull(values);
    (vTrue = allocValue()).mkBool(values, true);
    (vFalse = allocValue()).mkBool(values, false);
    (vStringRegular = allocValue()).mkString(values, "regular");
    (vStringDirectory = allocValue()).mkString(values, "directory");
    (vStringSymlink = allocValue()).mkString(values, "symlink");
    (vStringUnknown = allocValue()).mkString(values, "unknown");
    (vLineOfPosPrimOp = allocValue()).mkPrimOp(values, &primop_lineOfPos);
    (vColumnOfPosPrimOp = allocValue()).mkPrimOp(values, &primop_columnOfPos);

    /* Construct the Nix expression search path. */
    assert(lookupPath.elements.empty());
    if (!settings.pureEval) {
        for (auto & i : lookupPathFromArguments.elements) {
            lookupPath.elements.emplace_back(LookupPath::Elem{i});
        }
        /* $NIX_PATH overriding regular settings is implemented as a hack in `initGC()` */
        for (auto & i : settings.nixPath.get()) {
            lookupPath.elements.emplace_back(LookupPath::Elem::parse(i));
        }
        if (!settings.restrictEval) {
            for (auto & i : EvalSettings::getDefaultNixPath()) {
                lookupPath.elements.emplace_back(LookupPath::Elem::parse(i));
            }
        }
    }

    /* Allow access to all paths in the search path. */
    if (rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        for (auto & i : lookupPath.elements)
            resolveLookupPathPath(i.path, true);

    corepkgsFS->addFile(
        CanonPath("fetchurl.nix"),
#include "fetchurl.nix.gen.hh"
    );

    createBaseEnv(settings);

    /* Register function call tracer. */
    if (settings.traceFunctionCalls)
        profiler.addProfiler(make_ref<FunctionCallTrace>());

    switch (settings.evalProfilerMode) {
    case EvalProfilerMode::flamegraph:
        profiler.addProfiler(
            makeSampleStackProfiler(*this, settings.evalProfileFile.get(), settings.evalProfilerFrequency));
        break;
    case EvalProfilerMode::disabled:
        break;
    }
}

EvalState::~EvalState() {}

void EvalState::allowPath(const Path & path)
{
    if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        rootFS2->allowPrefix(CanonPath(path));
}

void EvalState::allowPath(const StorePath & storePath)
{
    if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        rootFS2->allowPrefix(CanonPath(store->printStorePath(storePath)));
}

void EvalState::allowClosure(const StorePath & storePath)
{
    if (!rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        return;

    StorePathSet closure;
    store->computeFSClosure(storePath, closure);
    for (auto & p : closure)
        allowPath(p);
}

void EvalState::allowAndSetStorePathString(const StorePath & storePath, ValueRef v)
{
    allowPath(storePath);

    mkStorePathString(storePath, v);
}

inline static bool isJustSchemePrefix(std::string_view prefix)
{
    return !prefix.empty() && prefix[prefix.size() - 1] == ':'
           && isValidSchemeName(prefix.substr(0, prefix.size() - 1));
}

bool isAllowedURI(std::string_view uri, const Strings & allowedUris)
{
    /* 'uri' should be equal to a prefix, or in a subdirectory of a
       prefix. Thus, the prefix https://github.co does not permit
       access to https://github.com. */
    for (auto & prefix : allowedUris) {
        if (uri == prefix
            // Allow access to subdirectories of the prefix.
            || (uri.size() > prefix.size() && prefix.size() > 0 && hasPrefix(uri, prefix)
                && (
                    // Allow access to subdirectories of the prefix.
                    prefix[prefix.size() - 1] == '/'
                    || uri[prefix.size()] == '/'

                    // Allow access to whole schemes
                    || isJustSchemePrefix(prefix))))
            return true;
    }

    return false;
}

void EvalState::checkURI(const std::string & uri)
{
    if (!settings.restrictEval)
        return;

    if (isAllowedURI(uri, settings.allowedUris.get()))
        return;

    /* If the URI is a path, then check it against allowedPaths as
       well. */
    if (isAbsolute(uri)) {
        if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
            rootFS2->checkAccess(CanonPath(uri));
        return;
    }

    if (hasPrefix(uri, "file://")) {
        if (auto rootFS2 = rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
            rootFS2->checkAccess(CanonPath(uri.substr(7)));
        return;
    }

    throw RestrictedPathError("access to URI '%s' is forbidden in restricted mode", uri);
}

void EvalState::addConstant(const std::string & name, Value v, Constant info)
{
    ValueRef v2 = allocValue();
    v2.setFromStack(values, v);
    addConstant(name, v2, info);
}

void EvalState::addConstant(const std::string & name, ValueRef v, Constant info)
{
    // Can't pass in a reference to value-on-the-stack. Pass in the stack value directly!
    // XXX [speed]: factor this out into a onStack() function
    if (v.ref & 0x1) [[unlikely]]
        unreachable();

    auto name2 = name.substr(0, 2) == "__" ? name.substr(2) : name;

    constantInfos.push_back({name2, info});

    if (!(settings.pureEval && info.impureOnly)) {
        /* Check the type, if possible.

           We might know the type of a thunk in advance, so be allowed
           to just write it down in that case. */
        if (auto gotType = v.type(values, true); gotType != nThunk)
            assert(info.type == gotType);

        /* Install value the base environment. */
        staticBaseEnv->vars.emplace_back(symbols.create(name), baseEnvDispl);
        baseEnv.values[baseEnvDispl++] = v;
        const_cast<Bindings *>(getBuiltins().attrs(values))->push_back(Attr(symbols.create(name2), v));
    }
}

void PrimOp::check()
{
    if (arity > maxPrimOpArity) {
        throw Error("primop arity must not exceed %1%", maxPrimOpArity);
    }
}

std::ostream & operator<<(std::ostream & output, const PrimOp & primOp)
{
    output << "primop " << primOp.name;
    return output;
}

const PrimOp * Value::primOpAppPrimOp(Values & values) const
{
    ValueRef left = primOpApp().left;
    while (left && !left.isPrimOp(values)) {
        left = left.primOpApp(values).left;
    }

    if (!left)
        return nullptr;

    assert(left.isPrimOp(values));
    return left.primOp(values);
}
const PrimOp * ValueRef::primOpAppPrimOp(Values & values) const
{
    ValueRef left = primOpApp(values).left;
    while (left && !left.isPrimOp(values)) {
        left = left.primOpApp(values).left;
    }

    if (!left)
        return nullptr;

    assert(left.isPrimOp(values));
    return left.primOp(values);
}

void Value::mkPrimOp(PrimOp * p)
{
    p->check();
    setStorage(p);
    nrPrimOp++;
}
void ValueRef::mkPrimOp(Values & values, PrimOp * p)
{
    p->check();
    setStorage(values, p);
    nrPrimOp++;
}

void EvalState::addPrimOp(PrimOp && primOp)
{
    /* Hack to make constants lazy: turn them into a application of
       the primop to a dummy value. */
    if (primOp.arity == 0) {
        primOp.arity = 1;
        auto vPrimOp = allocValue();
        vPrimOp.mkPrimOp(values, new PrimOp(primOp));
        Value v;
        v.mkApp(vPrimOp, vPrimOp);
        addConstant(
            primOp.name,
            v,
            {
                .type = nThunk, // FIXME
                .doc = primOp.doc,
            });
    }

    auto envName = symbols.create(primOp.name);
    if (hasPrefix(primOp.name, "__"))
        primOp.name = primOp.name.substr(2);

    ValueRef v = allocValue();
    v.mkPrimOp(values, new PrimOp(primOp));

    if (primOp.internal)
        internalPrimOps.emplace(primOp.name, v);
    else {
        staticBaseEnv->vars.emplace_back(envName, baseEnvDispl);
        baseEnv.values[baseEnvDispl++] = v;
        const_cast<Bindings *>(getBuiltins().attrs(values))->push_back(Attr(symbols.create(primOp.name), v));
    }
}

ValueRef EvalState::getBuiltins()
{
    return baseEnv.values[0];
}

ValueRef EvalState::getBuiltin(const std::string & name)
{
    auto it = getBuiltins().attrs(values)->get(symbols.create(name));
    if (it)
        return it->value;
    else
        error<EvalError>("builtin '%1%' not found", name).debugThrow();
}

std::optional<EvalState::Doc> EvalState::getDoc(ValueRef v)
{
    if (v.isPrimOp(values)) {
        if (auto * doc = v.primOp(values)->doc)
            return Doc{
                .pos = {},
                .name = v.primOp(values)->name,
                .arity = v.primOp(values)->arity,
                .args = v.primOp(values)->args,
                .doc = doc,
            };
    }
    if (v.isLambda(values)) {
        auto exprLambda = v.lambda(values).fun;

        std::ostringstream s;
        std::string name;
        auto pos = positions[exprLambda->getPos()];
        std::string docStr;

        if (exprLambda->name) {
            name = symbols[exprLambda->name];
        }

        if (exprLambda->docComment) {
            docStr = exprLambda->docComment.getInnerText(positions);
        }

        if (name.empty()) {
            s << "Function ";
        } else {
            s << "Function `" << name << "`";
            if (pos)
                s << "\\\n  … ";
            else
                s << "\\\n";
        }
        if (pos) {
            s << "defined at " << pos;
        }
        if (!docStr.empty()) {
            s << "\n\n";
        }

        s << docStr;

        return Doc{
            .pos = pos,
            .name = name,
            .arity = 0, // FIXME: figure out how deep by syntax only? It's not semantically useful though...
            .args = {},
            .doc = makeImmutableString(toView(s)), // NOTE: memory leak when compiled without GC
        };
    }
    if (isFunctor(v)) {
        try {
            ValueRef functor = v.attrs(values)->find(sFunctor)->value;
            ValueRef vp[] = {v};
            Value partiallyApplied;
            // The first parameter is not user-provided, and may be
            // handled by code that is opaque to the user, like lib.const = x: y: y;
            // So preferably we show docs that are relevant to the
            // "partially applied" function returned by e.g. `const`.
            // We apply the first argument:
            callFunction(functor, vp, partiallyApplied.ref(values), noPos);
            auto _level = addCallDepth(noPos);
            return getDoc(partiallyApplied.ref(values));
        } catch (Error & e) {
            e.addTrace(nullptr, "while partially calling '%1%' to retrieve documentation", "__functor");
            throw;
        }
    }
    return {};
}

// just for the current level of StaticEnv, not the whole chain.
void printStaticEnvBindings(const SymbolTable & st, const StaticEnv & se)
{
    std::cout << ANSI_MAGENTA;
    for (auto & i : se.vars)
        std::cout << st[i.first] << " ";
    std::cout << ANSI_NORMAL;
    std::cout << std::endl;
}

// just for the current level of Env, not the whole chain.
void printWithBindings(EvalState & state, const SymbolTable & st, const Env & env)
{
    if (!env.values[0].isThunk(state.values)) {
        std::cout << "with: ";
        std::cout << ANSI_MAGENTA;
        auto j = env.values[0].attrs(state.values)->begin();
        while (j != env.values[0].attrs(state.values)->end()) {
            std::cout << st[j->name] << " ";
            ++j;
        }
        std::cout << ANSI_NORMAL;
        std::cout << std::endl;
    }
}

void printEnvBindings(EvalState & es, const SymbolTable & st, const StaticEnv & se, const Env & env, int lvl)
{
    std::cout << "Env level " << lvl << std::endl;

    if (se.up && env.up) {
        std::cout << "static: ";
        printStaticEnvBindings(st, se);
        if (se.isWith)
            printWithBindings(es, st, env);
        std::cout << std::endl;
        printEnvBindings(es, st, *se.up, *env.up, ++lvl);
    } else {
        std::cout << ANSI_MAGENTA;
        // for the top level, don't print the double underscore ones;
        // they are in builtins.
        for (auto & i : se.vars)
            if (!hasPrefix(st[i.first], "__"))
                std::cout << st[i.first] << " ";
        std::cout << ANSI_NORMAL;
        std::cout << std::endl;
        if (se.isWith)
            printWithBindings(es, st, env); // probably nothing there for the top level.
        std::cout << std::endl;
    }
}

void printEnvBindings(EvalState & es, const Expr & expr, const Env & env)
{
    // just print the names for now
    auto se = es.getStaticEnv(expr);
    if (se)
        printEnvBindings(es, es.symbols, *se, env, 0);
}

void mapStaticEnvBindings(EvalState & es, const SymbolTable & st, const StaticEnv & se, const Env & env, ValMap & vm)
{
    // add bindings for the next level up first, so that the bindings for this level
    // override the higher levels.
    // The top level bindings (builtins) are skipped since they are added for us by initEnv()
    if (env.up && se.up) {
        mapStaticEnvBindings(es, st, *se.up, *env.up, vm);

        if (se.isWith && !env.values[0].isThunk(es.values)) {
            // add 'with' bindings.
            for (auto & j : *env.values[0].attrs(es.values))
                vm.insert_or_assign(std::string(st[j.name]), j.value);
        } else {
            // iterate through staticenv bindings and add them.
            for (auto & i : se.vars)
                vm.insert_or_assign(std::string(st[i.first]), env.values[i.second]);
        }
    }
}

std::unique_ptr<ValMap> mapStaticEnvBindings(EvalState & state, const SymbolTable & st, const StaticEnv & se, const Env & env)
{
    auto vm = std::make_unique<ValMap>();
    mapStaticEnvBindings(state, st, se, env, *vm);
    return vm;
}

/**
 * Sets `inDebugger` to true on construction and false on destruction.
 */
class DebuggerGuard
{
    bool & inDebugger;
public:
    DebuggerGuard(bool & inDebugger)
        : inDebugger(inDebugger)
    {
        inDebugger = true;
    }

    ~DebuggerGuard()
    {
        inDebugger = false;
    }
};

bool EvalState::canDebug()
{
    return debugRepl && !debugTraces.empty();
}

void EvalState::runDebugRepl(const Error * error)
{
    if (!canDebug())
        return;

    assert(!debugTraces.empty());
    const DebugTrace & last = debugTraces.front();
    const Env & env = last.env;
    const Expr & expr = last.expr;

    runDebugRepl(error, env, expr);
}

void EvalState::runDebugRepl(const Error * error, const Env & env, const Expr & expr)
{
    // Make sure we have a debugger to run and we're not already in a debugger.
    if (!debugRepl || inDebugger)
        return;

    auto dts = [&]() -> std::unique_ptr<DebugTraceStacker> {
        if (error && expr.getPos()) {
            auto trace = DebugTrace{
                .pos = [&]() -> std::variant<Pos, PosIdx> {
                    if (error->info().pos) {
                        if (auto * pos = error->info().pos.get())
                            return *pos;
                        return noPos;
                    }
                    return expr.getPos();
                }(),
                .expr = expr,
                .env = env,
                .hint = error->info().msg,
                .isError = true};

            return std::make_unique<DebugTraceStacker>(*this, std::move(trace));
        }
        return nullptr;
    }();

    if (error) {
        printError("%s\n", error->what());

        if (trylevel > 0 && error->info().level != lvlInfo)
            printError(
                "This exception occurred in a 'tryEval' call. Use " ANSI_GREEN "--ignore-try" ANSI_NORMAL
                " to skip these.\n");
    }

    auto se = getStaticEnv(expr);
    if (se) {
        auto vm = mapStaticEnvBindings(*this, symbols, *se.get(), env);
        DebuggerGuard _guard(inDebugger);
        auto exitStatus = (debugRepl) (ref<EvalState>(shared_from_this()), *vm);
        switch (exitStatus) {
        case ReplExitStatus::QuitAll:
            if (error)
                throw *error;
            throw Exit(0);
        case ReplExitStatus::Continue:
            break;
        default:
            unreachable();
        }
    }
}

template<typename... Args>
void EvalState::addErrorTrace(Error & e, const Args &... formatArgs) const
{
    e.addTrace(nullptr, HintFmt(formatArgs...));
}

template<typename... Args>
void EvalState::addErrorTrace(Error & e, const PosIdx pos, const Args &... formatArgs) const
{
    e.addTrace(positions[pos], HintFmt(formatArgs...));
}

template<typename... Args>
static std::unique_ptr<DebugTraceStacker> makeDebugTraceStacker(
    EvalState & state, Expr & expr, Env & env, std::variant<Pos, PosIdx> pos, const Args &... formatArgs)
{
    return std::make_unique<DebugTraceStacker>(
        state,
        DebugTrace{.pos = std::move(pos), .expr = expr, .env = env, .hint = HintFmt(formatArgs...), .isError = false});
}

DebugTraceStacker::DebugTraceStacker(EvalState & evalState, DebugTrace t)
    : evalState(evalState)
    , trace(std::move(t))
{
    evalState.debugTraces.push_front(trace);
    if (evalState.debugStop && evalState.debugRepl)
        evalState.runDebugRepl(nullptr, trace.env, trace.expr);
}

void Value::mkString(std::string_view s)
{
    mkString(makeImmutableString(s));
}
void ValueRef::mkString(Values & values, std::string_view s)
{
    mkString(values, makeImmutableString(s));
}

static const char ** encodeContext(const NixStringContext & context)
{
    if (!context.empty()) {
        size_t n = 0;
        auto ctx = (const char **) allocBytes((context.size() + 1) * sizeof(char *));
        for (auto & i : context) {
            ctx[n++] = makeImmutableString({i.to_string()});
        }
        ctx[n] = nullptr;
        return ctx;
    } else
        return nullptr;
}

void Value::mkString(std::string_view s, const NixStringContext & context)
{
    mkString(makeImmutableString(s), encodeContext(context));
}
void ValueRef::mkString(Values & values, std::string_view s, const NixStringContext & context)
{
    mkString(values, makeImmutableString(s), encodeContext(context));
}

void Value::mkStringMove(const char * s, const NixStringContext & context)
{
    mkString(s, encodeContext(context));
}
void ValueRef::mkStringMove(Values & values, const char * s, const NixStringContext & context)
{
    mkString(values, s, encodeContext(context));
}

void Value::mkPath(const SourcePath & path)
{
    mkPath(&*path.accessor, makeImmutableString(path.path.abs()));
}
void ValueRef::mkPath(Values & values, const SourcePath & path)
{
    mkPath(values, &*path.accessor, makeImmutableString(path.path.abs()));
}

// XXX [speed]: return these to their homes
ExprInt::ExprInt(EvalState & state, NixInt n)
{
    v = state.allocValue();
    v.mkInt(state.values, n);
};

ExprInt::ExprInt(EvalState & state, NixInt::Inner n)
{
    v = state.allocValue();
    v.mkInt(state.values, n);
};
ExprFloat::ExprFloat(EvalState & state, NixFloat nf)
{
    v = state.allocValue();
    v.mkFloat(state.values, nf);
};
ExprString::ExprString(EvalState & state, std::string && s)
    : s(std::move(s))
{
    v = state.allocValue();
    v.mkString(state.values, this->s.data());
};
ExprPath::ExprPath(EvalState & state, ref<SourceAccessor> accessor, std::string s)
    : accessor(accessor)
    , s(std::move(s))
{
    v = state.allocValue();
    v.mkPath(state.values, &*accessor, this->s.c_str());
}
ValueRef ValueRef::null{0};
SymbolRef SymbolRef::null{ValueRef::null};

void ValueRef::mkList(Values & values, const ListBuilder & builder) noexcept
{
    if (builder.size == 1) {
        setStorage(values, std::array<ValueRef, 2>{builder.inlineElems[0], ValueRef::null});
        nrListSmall++;
    }
    else if (builder.size == 2) {
        setStorage(values, std::array<ValueRef, 2>{builder.inlineElems[0], builder.inlineElems[1]});
        nrListSmall++;
    }
    else {
        setStorage(values, detail::List{.size = builder.size, .elems = builder.elems});
        nrListN++;
    }
}

bool ValueRef::isList(Values & values) const noexcept
{
    return isa<tListSmall, tListN>(values);
}

ListView ValueRef::listView(Values & values) const noexcept
{
    return isa<tListSmall>(values) ? ListView(getStorage<detail::SmallList>(values)) : ListView(getStorage<detail::List>(values));
}

size_t ValueRef::listSize(Values & values) const noexcept
{
    return isa<tListSmall>(values) ? (getStorage<detail::SmallList>(values)[1] == ValueRef::null ? 1 : 2) : getStorage<detail::List>(values).size;
}

SourcePath ValueRef::path(Values & values) const
{
    return SourcePath(nix::ref(pathAccessor(values)->shared_from_this()), CanonPath(CanonPath::unchecked_t(), pathStr(values)));
}

std::string_view ValueRef::string_view(Values & values) const noexcept
{
    return std::string_view(getStorage<detail::StringWithContext>(values).c_str);
}

const char * ValueRef::c_str(Values & values) const noexcept
{
    return getStorage<detail::StringWithContext>(values).c_str;
}

const char ** ValueRef::context(Values & values) const noexcept
{
    return getStorage<detail::StringWithContext>(values).context;
}

ExternalValueBase * ValueRef::external(Values & values) const noexcept
{
    return getStorage<ExternalValueBase *>(values);
}

const Bindings * ValueRef::attrs(Values & values) const noexcept
{
    return getStorage<Bindings *>(values);
}

const PrimOp * ValueRef::primOp(Values & values) const noexcept
{
    return getStorage<PrimOp *>(values);
}

bool ValueRef::boolean(Values & values) const noexcept
{
    return getStorage<bool>(values);
}

NixInt ValueRef::integer(Values & values) const noexcept
{
    return getStorage<NixInt>(values);
}

NixFloat ValueRef::fpoint(Values & values) const noexcept
{
    return getStorage<NixFloat>(values);
}

detail::Lambda ValueRef::lambda(Values & values) const noexcept
{
    return getStorage<detail::Lambda>(values);
}

detail::ClosureThunk ValueRef::thunk(Values & values) const noexcept
{
    return getStorage<detail::ClosureThunk>(values);
}

detail::PrimOpApplicationThunk ValueRef::primOpApp(Values & values) const noexcept
{
    return getStorage<detail::PrimOpApplicationThunk>(values);
}

detail::FunctionApplicationThunk ValueRef::app(Values & values) const noexcept
{
    return getStorage<detail::FunctionApplicationThunk>(values);
}

const char * ValueRef::pathStr(Values & values) const noexcept
{
    return getStorage<detail::Path>(values).path;
}

SourceAccessor * ValueRef::pathAccessor(Values & values) const noexcept
{
    return getStorage<detail::Path>(values).accessor;
}
InternalType ValueRef::getInternalType(Values & values) const noexcept
{
    return values.typeOf(*this);
}
void ValueRef::set(Values & values, ValueRef other) noexcept
{
    values.typeOf(*this) = values.typeOf(other);
    values.payloadOf(*this) = values.payloadOf(other);
}
void ValueRef::setFromStack(Values & values, Value const & v) noexcept
{
    values.typeOf(*this) = v.internalType;
    values.payloadOf(*this) = v.payload;
}
Value ValueRef::toStack(Values & values) const
{
    return {values.typeOf(*this), values.payloadOf(*this)};
}

template<typename T>
T ValueRef::getStorage(Values & values) const noexcept
{
    if (getInternalType(values) != detail::payloadTypeToInternalType<T>) [[unlikely]]
        unreachable();
    T out;
    values.getStorage(*this, out);
    return out;
}

#define NIX_VALUE_REF_SET_IMPL(K, FIELD_NAME, DISCRIMINATOR) \
void ValueRef::setStorage(Values & values, K val) noexcept   \
{                                                            \
    values.setStorage(*this, val);                           \
}

NIX_VALUE_FOR_EACH_FIELD(NIX_VALUE_REF_SET_IMPL)
#undef NIX_VALUE_REF_SET_IMPL


// XXX [speed]


inline ValueRef EvalState::lookupVar(Env * env, const ExprVar & var, bool noEval)
{
    for (auto l = var.level; l; --l, env = env->up)
        ;

    if (!var.fromWith)
        return env->values[var.displ];

    // This early exit defeats the `maybeThunk` optimization for variables from `with`,
    // The added complexity of handling this appears to be similarly in cost, or
    // the cases where applicable were insignificant in the first place.
    if (noEval)
        return ValueRef::null;

    auto * fromWith = var.fromWith;
    while (1) {
        forceAttrs(env->values[0], fromWith->pos, "while evaluating the first subexpression of a with expression");
        if (auto j = env->values[0].attrs(values)->get(var.name)) {
            if (countCalls)
                attrSelects[j->pos]++;
            return j->value;
        }
        if (!fromWith->parentWith)
            error<UndefinedVarError>("undefined variable '%1%'", symbols[var.name])
                .atPos(var.pos)
                .withFrame(*env, var)
                .debugThrow();
        for (size_t l = fromWith->prevWith; l; --l, env = env->up)
            ;
        fromWith = fromWith->parentWith;
    }
}

ListBuilder::ListBuilder(EvalState & state, size_t size)
    : size(size)
    , elems(size <= 2 ? inlineElems : (ValueRef *) allocBytes(size * sizeof(ValueRef)))
{
    state.nrListElems += size;
}

ValueRef EvalState::getBool(bool b)
{
    return b ? vTrue : vFalse;
}

unsigned long nrThunks = 0;

static inline void mkThunk(EvalState & state, ValueRef v, Env & env, Expr * expr)
{
    v.mkThunk(state.values, &env, expr);
    nrThunks++;
}

void EvalState::mkThunk_(ValueRef v, Expr * expr)
{
    mkThunk(*this, v, baseEnv, expr);
}

void EvalState::mkPos(ValueRef v, PosIdx p)
{
    auto origin = positions.originOf(p);
    if (auto path = std::get_if<SourcePath>(&origin)) {
        auto attrs = buildBindings(3);
        attrs.alloc(sFile).mkString(values, path->path.abs());
        makePositionThunks(*this, p, attrs.alloc(sLine), attrs.alloc(sColumn));
        v.mkAttrs(values, attrs);
    } else
        v.mkNull(values);
}

void EvalState::mkStorePathString(const StorePath & p, ValueRef v)
{
    v.mkString(
        values,
        store->printStorePath(p),
        NixStringContext{
            NixStringContextElem::Opaque{.path = p},
        });
}

std::string EvalState::mkOutputStringRaw(
    const SingleDerivedPath::Built & b,
    std::optional<StorePath> optStaticOutputPath,
    const ExperimentalFeatureSettings & xpSettings)
{
    /* In practice, this is testing for the case of CA derivations, or
       dynamic derivations. */
    return optStaticOutputPath ? store->printStorePath(std::move(*optStaticOutputPath))
                               /* Downstream we would substitute this for an actual path once
                                  we build the floating CA derivation */
                               : DownstreamPlaceholder::fromSingleDerivedPathBuilt(b, xpSettings).render();
}

void EvalState::mkOutputString(
    ValueRef value,
    const SingleDerivedPath::Built & b,
    std::optional<StorePath> optStaticOutputPath,
    const ExperimentalFeatureSettings & xpSettings)
{
    value.mkString(values, mkOutputStringRaw(b, optStaticOutputPath, xpSettings), NixStringContext{b});
}

std::string EvalState::mkSingleDerivedPathStringRaw(const SingleDerivedPath & p)
{
    return std::visit(
        overloaded{
            [&](const SingleDerivedPath::Opaque & o) { return store->printStorePath(o.path); },
            [&](const SingleDerivedPath::Built & b) {
                auto optStaticOutputPath = std::visit(
                    overloaded{
                        [&](const SingleDerivedPath::Opaque & o) {
                            auto drv = store->readDerivation(o.path);
                            auto i = drv.outputs.find(b.output);
                            if (i == drv.outputs.end())
                                throw Error(
                                    "derivation '%s' does not have output '%s'",
                                    b.drvPath->to_string(*store),
                                    b.output);
                            return i->second.path(*store, drv.name, b.output);
                        },
                        [&](const SingleDerivedPath::Built & o) -> std::optional<StorePath> { return std::nullopt; },
                    },
                    b.drvPath->raw());
                return mkOutputStringRaw(b, optStaticOutputPath);
            }},
        p.raw());
}

void EvalState::mkSingleDerivedPathString(const SingleDerivedPath & p, ValueRef v)
{
    v.mkString(
        values,
        mkSingleDerivedPathStringRaw(p),
        NixStringContext{
            std::visit([](auto && v) -> NixStringContextElem { return v; }, p),
        });
}

/* Create a thunk for the delayed computation of the given expression
   in the given environment.  But if the expression is a variable,
   then look it up right away.  This significantly reduces the number
   of thunks allocated. */
ValueRef Expr::maybeThunk(EvalState & state, Env & env)
{
    ValueRef v = state.allocValue();
    mkThunk(state, v, env, this);
    return v;
}

ValueRef ExprVar::maybeThunk(EvalState & state, Env & env)
{
    ValueRef v = state.lookupVar(&env, *this, true);
    /* The value might not be initialised in the environment yet.
       In that case, ignore it. */
    if (v) {
        state.nrAvoided++;
        return v;
    }
    return Expr::maybeThunk(state, env);
}

ValueRef ExprString::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return v;
}

ValueRef ExprInt::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return v;
}

ValueRef ExprFloat::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return v;
}

ValueRef ExprPath::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return v;
}

void EvalState::evalFile(const SourcePath & path, ValueRef v, bool mustBeTrivial)
{
    FileEvalCache::iterator i;
    if ((i = fileEvalCache.find(path)) != fileEvalCache.end()) {
        v.setFromStack(values, i->second);
        return;
    }

    auto resolvedPath = resolveExprPath(path);
    if ((i = fileEvalCache.find(resolvedPath)) != fileEvalCache.end()) {
        v.setFromStack(values, i->second);
        return;
    }

    printTalkative("evaluating file '%1%'", resolvedPath);
    Expr * e = nullptr;

    auto j = fileParseCache.find(resolvedPath);
    if (j != fileParseCache.end())
        e = j->second;

    if (!e)
        e = parseExprFromFile(resolvedPath);

    fileParseCache.emplace(resolvedPath, e);

    try {
        auto dts = debugRepl ? makeDebugTraceStacker(
                                   *this,
                                   *e,
                                   this->baseEnv,
                                   e->getPos(),
                                   "while evaluating the file '%1%':",
                                   resolvedPath.to_string())
                             : nullptr;

        // Enforce that 'flake.nix' is a direct attrset, not a
        // computation.
        if (mustBeTrivial && !(dynamic_cast<ExprAttrs *>(e)))
            error<EvalError>("file '%s' must be an attribute set", path).debugThrow();
        eval(e, v);
    } catch (Error & e) {
        addErrorTrace(e, "while evaluating the file '%1%':", resolvedPath.to_string());
        throw;
    }

    fileEvalCache.emplace(resolvedPath, v.toStack(values));
    if (path != resolvedPath)
        fileEvalCache.emplace(path, v.toStack(values));
}

void EvalState::resetFileCache()
{
    fileEvalCache.clear();
    fileParseCache.clear();
    inputCache->clear();
}

void EvalState::eval(Expr * e, ValueRef v)
{
    e->eval(*this, baseEnv, v);
}

inline bool EvalState::evalBool(Env & env, Expr * e, const PosIdx pos, std::string_view errorCtx)
{
    try {
        Value v;
        e->eval(*this, env, v.ref(values));
        if (v.type() != nBool)
            error<TypeError>(
                "expected a Boolean but found %1%: %2%", showType(*this, v.ref(values)), ValuePrinter(*this, v.ref(values), errorPrintOptions))
                .atPos(pos)
                .withFrame(env, *e)
                .debugThrow();
        return v.boolean();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

inline void EvalState::evalAttrs(Env & env, Expr * e, ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        e->eval(*this, env, v);
        if (v.type(values) != nAttrs)
            error<TypeError>(
                "expected a set but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
                .withFrame(env, *e)
                .debugThrow();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

void Expr::eval(EvalState & state, Env & env, ValueRef v)
{
    unreachable();
}

void ExprInt::eval(EvalState & state, Env & env, ValueRef v)
{
    v.set(state.values, this->v);
}

void ExprFloat::eval(EvalState & state, Env & env, ValueRef v)
{
    v.set(state.values, this->v);
}

void ExprString::eval(EvalState & state, Env & env, ValueRef v)
{
    v.set(state.values, this->v);
}

void ExprPath::eval(EvalState & state, Env & env, ValueRef v)
{
    v.set(state.values, this->v);
}

Env * ExprAttrs::buildInheritFromEnv(EvalState & state, Env & up)
{
    Env & inheritEnv = state.allocEnv(inheritFromExprs->size());
    inheritEnv.up = &up;

    Displacement displ = 0;
    for (auto from : *inheritFromExprs)
        inheritEnv.values[displ++] = from->maybeThunk(state, up);

    return &inheritEnv;
}

void ExprAttrs::eval(EvalState & state, Env & env, ValueRef v)
{
    auto bindings = state.buildBindings(attrs.size() + dynamicAttrs.size());
    auto dynamicEnv = &env;
    bool sort = false;

    if (recursive) {
        /* Create a new environment that contains the attributes in
           this `rec'. */
        Env & env2(state.allocEnv(attrs.size()));
        env2.up = &env;
        dynamicEnv = &env2;
        Env * inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env2) : nullptr;

        AttrDefs::iterator overrides = attrs.find(state.sOverrides);
        bool hasOverrides = overrides != attrs.end();

        /* The recursive attributes are evaluated in the new
           environment, while the inherited attributes are evaluated
           in the original environment. */
        Displacement displ = 0;
        for (auto & i : attrs) {
            ValueRef vAttr;
            if (hasOverrides && i.second.kind != AttrDef::Kind::Inherited) {
                vAttr = state.allocValue();
                mkThunk(state, vAttr, *i.second.chooseByKind(&env2, &env, inheritEnv), i.second.e);
            } else
                vAttr = i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
            env2.values[displ++] = vAttr;
            bindings.insert(i.first, vAttr, i.second.pos);
        }

        /* If the rec contains an attribute called `__overrides', then
           evaluate it, and add the attributes in that set to the rec.
           This allows overriding of recursive attributes, which is
           otherwise not possible.  (You can use the // operator to
           replace an attribute, but other attributes in the rec will
           still reference the original value, because that value has
           been substituted into the bodies of the other attributes.
           Hence we need __overrides.) */
        if (hasOverrides) {
            ValueRef vOverrides = (*bindings.bindings)[overrides->second.displ].value;
            state.forceAttrs(
                vOverrides,
                [&]() { return vOverrides.determinePos(state.values, noPos); },
                "while evaluating the `__overrides` attribute");
            bindings.grow(state.allocBindings(bindings.capacity() + vOverrides.attrs(state.values)->size()));
            for (auto & i : *vOverrides.attrs(state.values)) {
                AttrDefs::iterator j = attrs.find(i.name);
                if (j != attrs.end()) {
                    (*bindings.bindings)[j->second.displ] = i;
                    env2.values[j->second.displ] = i.value;
                } else
                    bindings.push_back(i);
            }
            sort = true;
        }
    }

    else {
        Env * inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env) : nullptr;
        for (auto & i : attrs)
            bindings.insert(
                i.first, i.second.e->maybeThunk(state, *i.second.chooseByKind(&env, &env, inheritEnv)), i.second.pos);
    }

    /* Dynamic attrs apply *after* rec and __overrides. */
    for (auto & i : dynamicAttrs) {
        Value nameVal;
        i.nameExpr->eval(state, *dynamicEnv, nameVal.ref(state.values));
        state.forceValue(nameVal.ref(state.values), i.pos);
        if (nameVal.type() == nNull)
            continue;
        state.forceStringNoCtx(nameVal.ref(state.values), i.pos, "while evaluating the name of a dynamic attribute");
        auto nameSym = state.symbols.create(nameVal.string_view());
        if (sort)
            // FIXME: inefficient
            bindings.bindings->sort();
        if (auto j = bindings.bindings->get(nameSym))
            state
                .error<EvalError>(
                    "dynamic attribute '%1%' already defined at %2%", state.symbols[nameSym], state.positions[j->pos])
                .atPos(i.pos)
                .withFrame(env, *this)
                .debugThrow();

        i.valueExpr->setName(nameSym);
        /* Keep sorted order so find can catch duplicates */
        bindings.insert(nameSym, i.valueExpr->maybeThunk(state, *dynamicEnv), i.pos);
        sort = true;
    }

    bindings.bindings->pos = pos;

    v.mkAttrs(state.values, sort ? bindings.finish() : bindings.alreadySorted());
}

void ExprLet::eval(EvalState & state, Env & env, ValueRef v)
{
    /* Create a new environment that contains the attributes in this
       `let'. */
    Env & env2(state.allocEnv(attrs->attrs.size()));
    env2.up = &env;

    Env * inheritEnv = attrs->inheritFromExprs ? attrs->buildInheritFromEnv(state, env2) : nullptr;

    /* The recursive attributes are evaluated in the new environment,
       while the inherited attributes are evaluated in the original
       environment. */
    Displacement displ = 0;
    for (auto & i : attrs->attrs) {
        env2.values[displ++] = i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
    }

    auto dts = state.debugRepl
                   ? makeDebugTraceStacker(state, *this, env2, getPos(), "while evaluating a '%1%' expression", "let")
                   : nullptr;

    body->eval(state, env2, v);
}

void ExprList::eval(EvalState & state, Env & env, ValueRef v)
{
    auto list = state.buildList(elems.size());
    for (const auto & [n, v2] : enumerate(list))
        v2 = elems[n]->maybeThunk(state, env);
    v.mkList(state.values, list);
}

ValueRef ExprList::maybeThunk(EvalState & state, Env & env)
{
    if (elems.empty()) {
        return state.vEmptyList;
    }
    return Expr::maybeThunk(state, env);
}

void ExprVar::eval(EvalState & state, Env & env, ValueRef v)
{
    ValueRef v2 = state.lookupVar(&env, *this, false);
    state.forceValue(v2, pos);
    v.set(state.values, v2);
}

static std::string showAttrPath(EvalState & state, Env & env, const AttrPath & attrPath)
{
    std::ostringstream out;
    bool first = true;
    for (auto & i : attrPath) {
        if (!first)
            out << '.';
        else
            first = false;
        try {
            out << state.symbols[getName(i, state, env)];
        } catch (Error & e) {
            assert(!i.symbol);
            out << "\"${";
            i.expr->show(state, state.symbols, out);
            out << "}\"";
        }
    }
    return out.str();
}

void ExprSelect::eval(EvalState & state, Env & env, ValueRef v)
{
    Value vTmp;
    PosIdx pos2;
    ValueRef vAttrs = vTmp.ref(state.values);

    e->eval(state, env, vTmp.ref(state.values));

    try {
        auto dts = state.debugRepl ? makeDebugTraceStacker(
                                         state,
                                         *this,
                                         env,
                                         getPos(),
                                         "while evaluating the attribute '%1%'",
                                         showAttrPath(state, env, attrPath))
                                   : nullptr;

        for (auto & i : attrPath) {
            state.nrLookups++;
            const Attr * j;
            auto name = getName(i, state, env);
            if (def) {
                state.forceValue(vAttrs, pos);
                if (vAttrs.type(state.values) != nAttrs || !(j = vAttrs.attrs(state.values)->get(name))) {
                    def->eval(state, env, v);
                    return;
                }
            } else {
                state.forceAttrs(vAttrs, pos, "while selecting an attribute");
                if (!(j = vAttrs.attrs(state.values)->get(name))) {
                    StringSet allAttrNames;
                    for (auto & attr : *vAttrs.attrs(state.values))
                        allAttrNames.insert(std::string(state.symbols[attr.name]));
                    auto suggestions = Suggestions::bestMatches(allAttrNames, state.symbols[name]);
                    state.error<EvalError>("attribute '%1%' missing", state.symbols[name])
                        .atPos(pos)
                        .withSuggestions(suggestions)
                        .withFrame(env, *this)
                        .debugThrow();
                }
            }
            vAttrs = j->value;
            pos2 = j->pos;
            if (state.countCalls)
                state.attrSelects[pos2]++;
        }

        state.forceValue(vAttrs, (pos2 ? pos2 : this->pos));

    } catch (Error & e) {
        if (pos2) {
            auto pos2r = state.positions[pos2];
            auto origin = std::get_if<SourcePath>(&pos2r.origin);
            if (!(origin && *origin == state.derivationInternal))
                state.addErrorTrace(
                    e, pos2, "while evaluating the attribute '%1%'", showAttrPath(state, env, attrPath));
        }
        throw;
    }

    v.set(state.values, vAttrs);
}

SymbolRef ExprSelect::evalExceptFinalSelect(EvalState & state, Env & env, ValueRef attrs)
{
    Value vTmp;
    SymbolRef name = getName(attrPath[attrPath.size() - 1], state, env);

    if (attrPath.size() == 1) {
        e->eval(state, env, vTmp.ref(state.values));
    } else {
        ExprSelect init(*this);
        init.attrPath.pop_back();
        init.eval(state, env, vTmp.ref(state.values));
    }
    attrs.setFromStack(state.values, vTmp);
    return name;
}

void ExprOpHasAttr::eval(EvalState & state, Env & env, ValueRef v)
{
    Value vTmp;
    ValueRef vAttrs = vTmp.ref(state.values);

    e->eval(state, env, vTmp.ref(state.values));

    for (auto & i : attrPath) {
        state.forceValue(vAttrs, getPos());
        const Attr * j;
        auto name = getName(i, state, env);
        if (vAttrs.type(state.values) == nAttrs && (j = vAttrs.attrs(state.values)->get(name))) {
            vAttrs = j->value;
        } else {
            v.mkBool(state.values, false);
            return;
        }
    }

    v.mkBool(state.values, true);
}

void ExprLambda::eval(EvalState & state, Env & env, ValueRef v)
{
    v.mkLambda(state.values, &env, this);
}

void EvalState::callFunction(ValueRef fun, std::span<ValueRef> args, ValueRef vRes, const PosIdx pos)
{
    auto _level = addCallDepth(pos);

    auto neededHooks = profiler.getNeededHooks();
    if (neededHooks.test(EvalProfiler::preFunctionCall)) [[unlikely]]
        profiler.preFunctionCallHook(*this, fun, args, pos);

    Finally traceExit_{[&]() {
        if (profiler.getNeededHooks().test(EvalProfiler::postFunctionCall)) [[unlikely]]
            profiler.postFunctionCallHook(*this, fun, args, pos);
    }};

    forceValue(fun, pos);

    Value vCur(fun.toStack(values));

    auto makeAppChain = [&]() {
        vRes.setFromStack(values, vCur);
        for (auto arg : args) {
            auto fun2 = allocValue();
            fun2.set(values, vRes);
            vRes.mkPrimOpApp(values, fun2, arg);
        }
    };

    const Attr * functor;

    while (args.size() > 0) {

        if (vCur.isLambda()) {

            ExprLambda & lambda(*vCur.lambda().fun);

            auto size = (!lambda.arg ? 0 : 1) + (lambda.hasFormals() ? lambda.formals->formals.size() : 0);
            Env & env2(allocEnv(size));
            env2.up = vCur.lambda().env;

            Displacement displ = 0;

            if (!lambda.hasFormals())
                env2.values[displ++] = args[0];
            else {
                try {
                    forceAttrs(args[0], lambda.pos, "while evaluating the value passed for the lambda argument");
                } catch (Error & e) {
                    if (pos)
                        e.addTrace(positions[pos], "from call site");
                    throw;
                }

                if (lambda.arg)
                    env2.values[displ++] = args[0];

                /* For each formal argument, get the actual argument.  If
                   there is no matching actual argument but the formal
                   argument has a default, use the default. */
                size_t attrsUsed = 0;
                for (auto & i : lambda.formals->formals) {
                    auto j = args[0].attrs(values)->get(i.name);
                    if (!j) {
                        if (!i.def) {
                            error<TypeError>(
                                "function '%1%' called without required argument '%2%'",
                                (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                                symbols[i.name])
                                .atPos(lambda.pos)
                                .withTrace(pos, "from call site")
                                .withFrame(*vCur.lambda().env, lambda)
                                .debugThrow();
                        }
                        env2.values[displ++] = i.def->maybeThunk(*this, env2);
                    } else {
                        attrsUsed++;
                        env2.values[displ++] = j->value;
                    }
                }

                /* Check that each actual argument is listed as a formal
                   argument (unless the attribute match specifies a `...'). */
                if (!lambda.formals->ellipsis && attrsUsed != args[0].attrs(values)->size()) {
                    /* Nope, so show the first unexpected argument to the
                       user. */
                    for (auto & i : *args[0].attrs(values))
                        if (!lambda.formals->has(i.name)) {
                            StringSet formalNames;
                            for (auto & formal : lambda.formals->formals)
                                formalNames.insert(std::string(symbols[formal.name]));
                            auto suggestions = Suggestions::bestMatches(formalNames, symbols[i.name]);
                            error<TypeError>(
                                "function '%1%' called with unexpected argument '%2%'",
                                (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                                symbols[i.name])
                                .atPos(lambda.pos)
                                .withTrace(pos, "from call site")
                                .withSuggestions(suggestions)
                                .withFrame(*vCur.lambda().env, lambda)
                                .debugThrow();
                        }
                    unreachable();
                }
            }

            nrFunctionCalls++;
            if (countCalls)
                incrFunctionCall(&lambda);

            /* Evaluate the body. */
            try {
                auto dts = debugRepl
                               ? makeDebugTraceStacker(
                                     *this,
                                     *lambda.body,
                                     env2,
                                     lambda.pos,
                                     "while calling %s",
                                     lambda.name ? concatStrings("'", symbols[lambda.name], "'") : "anonymous lambda")
                               : nullptr;

                lambda.body->eval(*this, env2, vCur.ref(values));
            } catch (Error & e) {
                if (loggerSettings.showTrace.get()) {
                    addErrorTrace(
                        e,
                        lambda.pos,
                        "while calling %s",
                        lambda.name ? concatStrings("'", symbols[lambda.name], "'") : "anonymous lambda");
                    if (pos)
                        addErrorTrace(e, pos, "from call site");
                }
                throw;
            }

            args = args.subspan(1);
        }

        else if (vCur.isPrimOp()) {

            size_t argsLeft = vCur.primOp()->arity;

            if (args.size() < argsLeft) {
                /* We don't have enough arguments, so create a tPrimOpApp chain. */
                makeAppChain();
                return;
            } else {
                /* We have all the arguments, so call the primop. */
                auto * fn = vCur.primOp();

                nrPrimOpCalls++;
                if (countCalls)
                    primOpCalls[fn->name]++;

                try {
                    fn->fun(*this, vCur.determinePos(values, noPos), args.data(), vCur.ref(values));
                } catch (Error & e) {
                    if (fn->addTrace)
                        addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
                    throw;
                }

                args = args.subspan(argsLeft);
            }
        }

        else if (vCur.isPrimOpApp()) {
            /* Figure out the number of arguments still needed. */
            size_t argsDone = 0;
            ValueRef primOp = vCur.ref(values);
            while (primOp.isPrimOpApp(values)) {
                argsDone++;
                primOp = primOp.primOpApp(values).left;
            }
            assert(primOp.isPrimOp(values));
            auto arity = primOp.primOp(values)->arity;
            auto argsLeft = arity - argsDone;

            if (args.size() < argsLeft) {
                /* We still don't have enough arguments, so extend the tPrimOpApp chain. */
                makeAppChain();
                return;
            } else {
                /* We have all the arguments, so call the primop with
                   the previous and new arguments. */

                ValueRef vArgs[maxPrimOpArity];
                auto n = argsDone;
                for (ValueRef arg = vCur.ref(values); arg.isPrimOpApp(values); arg = arg.primOpApp(values).left)
                    vArgs[--n] = arg.primOpApp(values).right;

                for (size_t i = 0; i < argsLeft; ++i)
                    vArgs[argsDone + i] = args[i];

                auto fn = primOp.primOp(values);
                nrPrimOpCalls++;
                if (countCalls)
                    primOpCalls[fn->name]++;

                try {
                    // TODO:
                    // 1. Unify this and above code. Heavily redundant.
                    // 2. Create a fake env (arg1, arg2, etc.) and a fake expr (arg1: arg2: etc: builtins.name arg1 arg2
                    // etc)
                    //    so the debugger allows to inspect the wrong parameters passed to the builtin.
                    fn->fun(*this, vCur.determinePos(values, noPos), vArgs, vCur.ref(values));
                } catch (Error & e) {
                    if (fn->addTrace)
                        addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
                    throw;
                }

                args = args.subspan(argsLeft);
            }
        }

        else if (vCur.type() == nAttrs && (functor = vCur.attrs()->get(sFunctor))) {
            /* 'vCur' may be allocated on the stack of the calling
               function, but for functors we may keep a reference, so
               heap-allocate a copy and use that instead. */
            ValueRef args2[] = {allocValue(), args[0]};
            args2[0].setFromStack(values, vCur);
            try {
                callFunction(functor->value, args2, vCur.ref(values), functor->pos);
            } catch (Error & e) {
                e.addTrace(positions[pos], "while calling a functor (an attribute set with a '__functor' attribute)");
                throw;
            }
            args = args.subspan(1);
        }

        else
            error<TypeError>(
                "attempt to call something which is not a function but %1%: %2%",
                showType(*this, vCur.ref(values)),
                ValuePrinter(*this, vCur.ref(values), errorPrintOptions))
                .atPos(pos)
                .debugThrow();
    }

    vRes.setFromStack(values, vCur);
}

void ExprCall::eval(EvalState & state, Env & env, ValueRef v)
{
    auto dts =
        state.debugRepl ? makeDebugTraceStacker(state, *this, env, getPos(), "while calling a function") : nullptr;

    Value vFun;
    fun->eval(state, env, vFun.ref(state.values));

    // Empirical arity of Nixpkgs lambdas by regex e.g. ([a-zA-Z]+:(\s|(/\*.*\/)|(#.*\n))*){5}
    // 2: over 4000
    // 3: about 300
    // 4: about 60
    // 5: under 10
    // This excluded attrset lambdas (`{...}:`). Contributions of mixed lambdas appears insignificant at ~150 total.
    SmallValueVector<4> vArgs(args.size());
    for (size_t i = 0; i < args.size(); ++i)
        vArgs[i] = args[i]->maybeThunk(state, env);

    state.callFunction(vFun.ref(state.values), vArgs, v, pos);
}

// Lifted out of callFunction() because it creates a temporary that
// prevents tail-call optimisation.
void EvalState::incrFunctionCall(ExprLambda * fun)
{
    functionCalls[fun]++;
}

void EvalState::autoCallFunction(const Bindings & args, ValueRef fun, ValueRef res)
{
    auto pos = fun.determinePos(values, noPos);

    forceValue(fun, pos);

    if (fun.type(values) == nAttrs) {
        auto found = fun.attrs(values)->find(sFunctor);
        if (found != fun.attrs(values)->end()) {
            ValueRef v = allocValue();
            callFunction(found->value, fun, v, pos);
            forceValue(v, pos);
            return autoCallFunction(args, v, res);
        }
    }

    if (!fun.isLambda(values) || !fun.lambda(values).fun->hasFormals()) {
        res.set(values, fun);
        return;
    }

    auto attrs = buildBindings(std::max(static_cast<uint32_t>(fun.lambda(values).fun->formals->formals.size()), args.size()));

    if (fun.lambda(values).fun->formals->ellipsis) {
        // If the formals have an ellipsis (eg the function accepts extra args) pass
        // all available automatic arguments (which includes arguments specified on
        // the command line via --arg/--argstr)
        for (auto & v : args)
            attrs.insert(v);
    } else {
        // Otherwise, only pass the arguments that the function accepts
        for (auto & i : fun.lambda(values).fun->formals->formals) {
            auto j = args.get(i.name);
            if (j) {
                attrs.insert(*j);
            } else if (!i.def) {
                error<MissingArgumentError>(
                    R"(cannot evaluate a function that has an argument without a value ('%1%')
Nix attempted to evaluate a function as a top level expression; in
this case it must have its arguments supplied either by default
values, or passed explicitly with '--arg' or '--argstr'. See
https://nix.dev/manual/nix/stable/language/syntax.html#functions.)",
                    symbols[i.name])
                    .atPos(i.pos)
                    .withFrame(*fun.lambda(values).env, *fun.lambda(values).fun)
                    .debugThrow();
            }
        }
    }

    auto vAttrs = allocValue();
    vAttrs.mkAttrs(values, attrs);
    callFunction(fun, vAttrs, res, pos);
}

void ExprWith::eval(EvalState & state, Env & env, ValueRef v)
{
    Env & env2(state.allocEnv(1));
    env2.up = &env;
    env2.values[0] = attrs->maybeThunk(state, env);

    body->eval(state, env2, v);
}

void ExprIf::eval(EvalState & state, Env & env, ValueRef v)
{
    // We cheat in the parser, and pass the position of the condition as the position of the if itself.
    (state.evalBool(env, cond, pos, "while evaluating a branch condition") ? then : else_)->eval(state, env, v);
}

void ExprAssert::eval(EvalState & state, Env & env, ValueRef v)
{
    if (!state.evalBool(env, cond, pos, "in the condition of the assert statement")) {
        std::ostringstream out;
        cond->show(state, state.symbols, out);
        auto exprStr = toView(out);

        if (auto eq = dynamic_cast<ExprOpEq *>(cond)) {
            try {
                Value v1;
                eq->e1->eval(state, env, v1.ref(state.values));
                Value v2;
                eq->e2->eval(state, env, v2.ref(state.values));
                state.assertEqValues(v1.ref(state.values), v2.ref(state.values), eq->pos, "in an equality assertion");
            } catch (AssertionError & e) {
                e.addTrace(state.positions[pos], "while evaluating the condition of the assertion '%s'", exprStr);
                throw;
            }
        }

        state.error<AssertionError>("assertion '%1%' failed", exprStr).atPos(pos).withFrame(env, *this).debugThrow();
    }
    body->eval(state, env, v);
}

void ExprOpNot::eval(EvalState & state, Env & env, ValueRef v)
{
    v.mkBool(state.values, !state.evalBool(env, e, getPos(), "in the argument of the not operator")); // XXX: FIXME: !
}

void ExprOpEq::eval(EvalState & state, Env & env, ValueRef v)
{
    Value v1;
    e1->eval(state, env, v1.ref(state.values));
    Value v2;
    e2->eval(state, env, v2.ref(state.values));
    v.mkBool(state.values, state.eqValues(v1.ref(state.values), v2.ref(state.values), pos, "while testing two values for equality"));
}

void ExprOpNEq::eval(EvalState & state, Env & env, ValueRef v)
{
    Value v1;
    e1->eval(state, env, v1.ref(state.values));
    Value v2;
    e2->eval(state, env, v2.ref(state.values));
    v.mkBool(state.values, !state.eqValues(v1.ref(state.values), v2.ref(state.values), pos, "while testing two values for inequality"));
}

void ExprOpAnd::eval(EvalState & state, Env & env, ValueRef v)
{
    auto b = state.evalBool(env, e1, pos, "in the left operand of the AND (&&) operator")
             && state.evalBool(env, e2, pos, "in the right operand of the AND (&&) operator");
    v.mkBool(state.values, b);
}

void ExprOpOr::eval(EvalState & state, Env & env, ValueRef v)
{
    // XXX [speed]: cursed. we can't put this expression inside mkBool() or it might allocValue() thus invalidating the return from VRtoV(v). This should get cleaned up when we make mkBool() act directly on ValueRefs.
    auto b = state.evalBool(env, e1, pos, "in the left operand of the OR (||) operator")
             || state.evalBool(env, e2, pos, "in the right operand of the OR (||) operator");
    v.mkBool(state.values, b);
}

void ExprOpImpl::eval(EvalState & state, Env & env, ValueRef v)
{
    auto b = !state.evalBool(env, e1, pos, "in the left operand of the IMPL (->) operator")
             || state.evalBool(env, e2, pos, "in the right operand of the IMPL (->) operator");
    v.mkBool(state.values, b);
}

void ExprOpUpdate::eval(EvalState & state, Env & env, ValueRef v)
{
    Value v1, v2;
    state.evalAttrs(env, e1, v1.ref(state.values), pos, "in the left operand of the update (//) operator");
    state.evalAttrs(env, e2, v2.ref(state.values), pos, "in the right operand of the update (//) operator");

    state.nrOpUpdates++;

    if (v1.attrs()->size() == 0) {
        v.setFromStack(state.values, v2);
        return;
    }
    if (v2.attrs()->size() == 0) {
        v.setFromStack(state.values, v1);
        return;
    }

    auto attrs = state.buildBindings(v1.attrs()->size() + v2.attrs()->size());

    /* Merge the sets, preferring values from the second set.  Make
       sure to keep the resulting vector in sorted order. */
    auto i = v1.attrs()->begin();
    auto j = v2.attrs()->begin();

    while (i != v1.attrs()->end() && j != v2.attrs()->end()) {
        if (i->name == j->name) {
            attrs.insert(*j);
            ++i;
            ++j;
        } else if (i->name < j->name)
            attrs.insert(*i++);
        else
            attrs.insert(*j++);
    }

    while (i != v1.attrs()->end())
        attrs.insert(*i++);
    while (j != v2.attrs()->end())
        attrs.insert(*j++);

    v.mkAttrs(state.values, attrs.alreadySorted());

    state.nrOpUpdateValuesCopied += v.attrs(state.values)->size();
}

void ExprOpConcatLists::eval(EvalState & state, Env & env, ValueRef v)
{
    Value v1;
    e1->eval(state, env, v1.ref(state.values));
    Value v2;
    e2->eval(state, env, v2.ref(state.values));
    ValueRef lists[2] = {v1.ref(state.values), v2.ref(state.values)};
    state.concatLists(v, 2, lists, pos, "while evaluating one of the elements to concatenate");
}

void EvalState::concatLists(
    ValueRef v, size_t nrLists, ValueRef const * lists, const PosIdx pos, std::string_view errorCtx)
{
    nrListConcats++;

    ValueRef nonEmpty = ValueRef::null;
    size_t len = 0;
    for (size_t n = 0; n < nrLists; ++n) {
        forceList(lists[n], pos, errorCtx);
        auto l = lists[n].listSize(values);
        len += l;
        if (l)
            nonEmpty = lists[n];
    }

    if (nonEmpty && len == nonEmpty.listSize(values)) {
        v.set(values, nonEmpty);
        return;
    }

    auto list = buildList(len);
    auto out = list.elems;
    for (size_t n = 0, pos = 0; n < nrLists; ++n) {
        auto listView = lists[n].listView(values);
        auto l = listView.size();
        if (l)
            memcpy(out + pos, listView.data(), l * sizeof(ValueRef));
        pos += l;
    }
    v.mkList(values, list);
}

void ExprConcatStrings::eval(EvalState & state, Env & env, ValueRef v)
{
    NixStringContext context;
    std::vector<BackedStringView> s;
    size_t sSize = 0;
    NixInt n{0};
    NixFloat nf = 0;

    bool first = !forceString;
    ValueType firstType = nString;

    const auto str = [&] {
        std::string result;
        result.reserve(sSize);
        for (const auto & part : s)
            result += *part;
        return result;
    };
    /* c_str() is not str().c_str() because we want to create a string
       Value. allocating a GC'd string directly and moving it into a
       Value lets us avoid an allocation and copy. */
    const auto c_str = [&] {
        char * result = allocString(sSize + 1);
        char * tmp = result;
        for (const auto & part : s) {
            memcpy(tmp, part->data(), part->size());
            tmp += part->size();
        }
        *tmp = 0;
        return result;
    };

    for (auto & [i_pos, i] : *es) {
        Value vTmp;

        i->eval(state, env, vTmp.ref(state.values));

        /* If the first element is a path, then the result will also
           be a path, we don't copy anything (yet - that's done later,
           since paths are copied when they are used in a derivation),
           and none of the strings are allowed to have contexts. */
        if (first) {
            firstType = vTmp.type();
        }

        if (firstType == nInt) {
            if (vTmp.type() == nInt) {
                auto newN = n + vTmp.integer();
                if (auto checked = newN.valueChecked(); checked.has_value()) {
                    n = NixInt(*checked);
                } else {
                    state.error<EvalError>("integer overflow in adding %1% + %2%", n, vTmp.integer())
                        .atPos(i_pos)
                        .debugThrow();
                }
            } else if (vTmp.type() == nFloat) {
                // Upgrade the type from int to float;
                firstType = nFloat;
                nf = n.value;
                nf += vTmp.fpoint();
            } else
                state.error<EvalError>("cannot add %1% to an integer", showType(state, vTmp.ref(state.values)))
                    .atPos(i_pos)
                    .withFrame(env, *this)
                    .debugThrow();
        } else if (firstType == nFloat) {
            if (vTmp.type() == nInt) {
                nf += vTmp.integer().value;
            } else if (vTmp.type() == nFloat) {
                nf += vTmp.fpoint();
            } else
                state.error<EvalError>("cannot add %1% to a float", showType(state, vTmp.ref(state.values)))
                    .atPos(i_pos)
                    .withFrame(env, *this)
                    .debugThrow();
        } else {
            if (s.empty())
                s.reserve(es->size());
            /* skip canonization of first path, which would only be not
            canonized in the first place if it's coming from a ./${foo} type
            path */
            auto part = state.coerceToString(
                i_pos, vTmp.ref(state.values), context, "while evaluating a path segment", false, firstType == nString, !first);
            sSize += part->size();
            s.emplace_back(std::move(part));
        }

        first = false;
    }

    if (firstType == nInt)
        v.mkInt(state.values, n);
    else if (firstType == nFloat)
        v.mkFloat(state.values, nf);
    else if (firstType == nPath) {
        if (!context.empty())
            state.error<EvalError>("a string that refers to a store path cannot be appended to a path")
                .atPos(pos)
                .withFrame(env, *this)
                .debugThrow();
        v.mkPath(state.values, state.rootPath(CanonPath(str())));
    } else
        v.mkStringMove(state.values, c_str(), context);
}

void ExprPos::eval(EvalState & state, Env & env, ValueRef v)
{
    state.mkPos(v, pos);
}

void ExprBlackHole::eval(EvalState & state, [[maybe_unused]] Env & env, ValueRef v)
{
    throwInfiniteRecursionError(state, v);
}

[[gnu::noinline]] [[noreturn]] void ExprBlackHole::throwInfiniteRecursionError(EvalState & state, ValueRef v)
{
    state.error<InfiniteRecursionError>("infinite recursion encountered").atPos(v.determinePos(state.values, noPos)).debugThrow();
}

// always force this to be separate, otherwise forceValue may inline it and take
// a massive perf hit
[[gnu::noinline]]
void EvalState::tryFixupBlackHolePos(ValueRef v, PosIdx pos)
{
    if (!v.isBlackhole(values))
        return;
    auto e = std::current_exception();
    try {
        std::rethrow_exception(e);
    } catch (InfiniteRecursionError & e) {
        e.atPos(positions[pos]);
    } catch (...) {
    }
}

void EvalState::forceValueDeep(ValueRef v)
{
    std::set<ValueRef> seen;

    std::function<void(ValueRef v)> recurse;

    recurse = [&](ValueRef v) {
        if (!seen.insert(v).second)
            return;

        forceValue(v, v.determinePos(values, noPos));

        if (v.type(values) == nAttrs) {
            for (auto & i : *v.attrs(values))
                try {
                    // If the value is a thunk, we're evaling. Otherwise no trace necessary.
                    auto dts = debugRepl && i.value.isThunk(values) ? makeDebugTraceStacker(
                                                                     *this,
                                                                     *i.value.thunk(values).expr,
                                                                     *i.value.thunk(values).env,
                                                                     i.pos,
                                                                     "while evaluating the attribute '%1%'",
                                                                     symbols[i.name])
                                                               : nullptr;

                    recurse(i.value);
                } catch (Error & e) {
                    addErrorTrace(e, i.pos, "while evaluating the attribute '%1%'", symbols[i.name]);
                    throw;
                }
        }

        else if (v.isList(values)) {
            for (auto v2 : v.listView(values))
                recurse(v2);
        }
    };

    recurse(v);
}

NixInt EvalState::forceInt(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type(values) != nInt)
            error<TypeError>(
                "expected an integer but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.integer(values);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    return v.integer(values);
}

NixFloat EvalState::forceFloat(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type(values) == nInt)
            return v.integer(values).value;
        else if (v.type(values) != nFloat)
            error<TypeError>(
                "expected a float but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.fpoint(values);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

bool EvalState::forceBool(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type(values) != nBool)
            error<TypeError>(
                "expected a Boolean but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.boolean(values);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    return v.boolean(values);
}

Bindings::const_iterator EvalState::getAttr(SymbolRef attrSym, const Bindings * attrSet, std::string_view errorCtx)
{
    auto value = attrSet->find(attrSym);
    if (value == attrSet->end()) {
        error<TypeError>("attribute '%s' missing", symbols[attrSym]).withTrace(noPos, errorCtx).debugThrow();
    }
    return value;
}

bool EvalState::isFunctor(const ValueRef fun) /* XXX [speed] const */
{
    return fun.type(values) == nAttrs && fun.attrs(values)->find(sFunctor) != fun.attrs(values)->end();
}

void EvalState::forceFunction(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type(values) != nFunction && !isFunctor(v))
            error<TypeError>(
                "expected a function but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

std::string_view EvalState::forceString(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type(values) != nString)
            error<TypeError>(
                "expected a string but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.string_view(values);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

void copyContext(EvalState & state, const ValueRef v, NixStringContext & context, const ExperimentalFeatureSettings & xpSettings)
{
    if (v.context(state.values))
        for (const char ** p = v.context(state.values); *p; ++p)
            context.insert(NixStringContextElem::parse(*p, xpSettings));
}

std::string_view EvalState::forceString(
    ValueRef v,
    NixStringContext & context,
    const PosIdx pos,
    std::string_view errorCtx,
    const ExperimentalFeatureSettings & xpSettings)
{
    auto s = forceString(v, pos, errorCtx);
    copyContext(*this, v, context, xpSettings);
    return s;
}

std::string_view EvalState::forceStringNoCtx(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    auto s = forceString(v, pos, errorCtx);
    if (v.context(values)) {
        error<EvalError>(
            "the string '%1%' is not allowed to refer to a store path (such as '%2%')", v.string_view(values), v.context(values)[0])
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
    return s;
}

bool EvalState::isDerivation(ValueRef v)
{
    if (v.type(values) != nAttrs)
        return false;
    auto i = v.attrs(values)->get(sType);
    if (!i)
        return false;
    forceValue(i->value, i->pos);
    if (i->value.type(values) != nString)
        return false;
    return i->value.string_view(values).compare("derivation") == 0;
}

std::optional<std::string>
EvalState::tryAttrsToString(const PosIdx pos, ValueRef v, NixStringContext & context, bool coerceMore, bool copyToStore)
{
    auto i = v.attrs(values)->find(sToString);
    if (i != v.attrs(values)->end()) {
        Value v1;
        callFunction(i->value, v, v1.ref(values), pos);
        return coerceToString(
                   pos,
                   v1.ref(values),
                   context,
                   "while evaluating the result of the `__toString` attribute",
                   coerceMore,
                   copyToStore)
            .toOwned();
    }

    return {};
}

BackedStringView EvalState::coerceToString(
    const PosIdx pos,
    ValueRef v,
    NixStringContext & context,
    std::string_view errorCtx,
    bool coerceMore,
    bool copyToStore,
    bool canonicalizePath)
{
    forceValue(v, pos);

    if (v.type(values) == nString) {
        copyContext(*this, v, context);
        return v.string_view(values);
    }

    if (v.type(values) == nPath) {
        return !canonicalizePath && !copyToStore
                   ? // FIXME: hack to preserve path literals that end in a
                     // slash, as in /foo/${x}.
                   v.pathStr(values)
                   : copyToStore ? store->printStorePath(copyPathToStore(context, v.path(values)))
                                 : std::string(v.path(values).path.abs());
    }

    if (v.type(values) == nAttrs) {
        auto maybeString = tryAttrsToString(pos, v, context, coerceMore, copyToStore);
        if (maybeString)
            return std::move(*maybeString);
        auto i = v.attrs(values)->find(sOutPath);
        if (i == v.attrs(values)->end()) {
            error<TypeError>(
                "cannot coerce %1% to a string: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
                .withTrace(pos, errorCtx)
                .debugThrow();
        }
        return coerceToString(pos, i->value, context, errorCtx, coerceMore, copyToStore, canonicalizePath);
    }

    if (v.type(values) == nExternal) {
        try {
            return v.external(values)->coerceToString(*this, pos, context, coerceMore, copyToStore);
        } catch (Error & e) {
            e.addTrace(nullptr, errorCtx);
            throw;
        }
    }

    if (coerceMore) {
        /* Note that `false' is represented as an empty string for
           shell scripting convenience, just like `null'. */
        if (v.type(values) == nBool && v.boolean(values))
            return "1";
        if (v.type(values) == nBool && !v.boolean(values))
            return "";
        if (v.type(values) == nInt)
            return std::to_string(v.integer(values).value);
        if (v.type(values) == nFloat)
            return std::to_string(v.fpoint(values));
        if (v.type(values) == nNull)
            return "";

        if (v.isList(values)) {
            std::string result;
            auto listView = v.listView(values);
            for (auto [n, v2] : enumerate(listView)) {
                try {
                    result += *coerceToString(
                        pos,
                        v2,
                        context,
                        "while evaluating one element of the list",
                        coerceMore,
                        copyToStore,
                        canonicalizePath);
                } catch (Error & e) {
                    e.addTrace(positions[pos], errorCtx);
                    throw;
                }
                if (n < v.listSize(values) - 1
                    /* !!! not quite correct */
                    && (!v2.isList(values) || v2.listSize(values) != 0))
                    result += " ";
            }
            return result;
        }
    }

    error<TypeError>("cannot coerce %1% to a string: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
        .withTrace(pos, errorCtx)
        .debugThrow();
}

StorePath EvalState::copyPathToStore(NixStringContext & context, const SourcePath & path)
{
    if (nix::isDerivation(path.path.abs()))
        error<EvalError>("file names are not allowed to end in '%1%'", drvExtension).debugThrow();

    auto dstPathCached = get(*srcToStore.lock(), path);

    auto dstPath = dstPathCached ? *dstPathCached : [&]() {
        auto dstPath = fetchToStore(
            fetchSettings,
            *store,
            path.resolveSymlinks(SymlinkResolution::Ancestors),
            settings.readOnlyMode ? FetchMode::DryRun : FetchMode::Copy,
            path.baseName(),
            ContentAddressMethod::Raw::NixArchive,
            nullptr,
            repair);
        allowPath(dstPath);
        srcToStore.lock()->try_emplace(path, dstPath);
        printMsg(lvlChatty, "copied source '%1%' -> '%2%'", path, store->printStorePath(dstPath));
        return dstPath;
    }();

    context.insert(NixStringContextElem::Opaque{.path = dstPath});
    return dstPath;
}

SourcePath EvalState::coerceToPath(const PosIdx pos, ValueRef v, NixStringContext & context, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    /* Handle path values directly, without coercing to a string. */
    if (v.type(values) == nPath)
        return v.path(values);

    /* Similarly, handle __toString where the result may be a path
       value. */
    if (v.type(values) == nAttrs) {
        auto i = v.attrs(values)->find(sToString);
        if (i != v.attrs(values)->end()) {
            Value v1;
            callFunction(i->value, v, v1.ref(values), pos);
            return coerceToPath(pos, v1.ref(values), context, errorCtx);
        }
    }

    /* Any other value should be coercible to a string, interpreted
       relative to the root filesystem. */
    auto path = coerceToString(pos, v, context, errorCtx, false, false, true).toOwned();
    if (path == "" || path[0] != '/')
        error<EvalError>("string '%1%' doesn't represent an absolute path", path).withTrace(pos, errorCtx).debugThrow();
    return rootPath(path);
}

StorePath
EvalState::coerceToStorePath(const PosIdx pos, ValueRef v, NixStringContext & context, std::string_view errorCtx)
{
    auto path = coerceToString(pos, v, context, errorCtx, false, false, true).toOwned();
    if (auto storePath = store->maybeParseStorePath(path))
        return *storePath;
    error<EvalError>("path '%1%' is not in the Nix store", path).withTrace(pos, errorCtx).debugThrow();
}

std::pair<SingleDerivedPath, std::string_view> EvalState::coerceToSingleDerivedPathUnchecked(
    const PosIdx pos, ValueRef v, std::string_view errorCtx, const ExperimentalFeatureSettings & xpSettings)
{
    NixStringContext context;
    auto s = forceString(v, context, pos, errorCtx, xpSettings);
    auto csize = context.size();
    if (csize != 1)
        error<EvalError>("string '%s' has %d entries in its context. It should only have exactly one entry", s, csize)
            .withTrace(pos, errorCtx)
            .debugThrow();
    auto derivedPath = std::visit(
        overloaded{
            [&](NixStringContextElem::Opaque && o) -> SingleDerivedPath { return std::move(o); },
            [&](NixStringContextElem::DrvDeep &&) -> SingleDerivedPath {
                error<EvalError>(
                    "string '%s' has a context which refers to a complete source and binary closure. This is not supported at this time",
                    s)
                    .withTrace(pos, errorCtx)
                    .debugThrow();
            },
            [&](NixStringContextElem::Built && b) -> SingleDerivedPath { return std::move(b); },
        },
        ((NixStringContextElem &&) *context.begin()).raw);
    return {
        std::move(derivedPath),
        std::move(s),
    };
}

SingleDerivedPath EvalState::coerceToSingleDerivedPath(const PosIdx pos, ValueRef v, std::string_view errorCtx)
{
    auto [derivedPath, s_] = coerceToSingleDerivedPathUnchecked(pos, v, errorCtx);
    auto s = s_;
    auto sExpected = mkSingleDerivedPathStringRaw(derivedPath);
    if (s != sExpected) {
        /* `std::visit` is used here just to provide a more precise
           error message. */
        std::visit(
            overloaded{
                [&](const SingleDerivedPath::Opaque & o) {
                    error<EvalError>("path string '%s' has context with the different path '%s'", s, sExpected)
                        .withTrace(pos, errorCtx)
                        .debugThrow();
                },
                [&](const SingleDerivedPath::Built & b) {
                    error<EvalError>(
                        "string '%s' has context with the output '%s' from derivation '%s', but the string is not the right placeholder for this derivation output. It should be '%s'",
                        s,
                        b.output,
                        b.drvPath->to_string(*store),
                        sExpected)
                        .withTrace(pos, errorCtx)
                        .debugThrow();
                }},
            derivedPath.raw());
    }
    return derivedPath;
}

// NOTE: This implementation must match eqValues!
// We accept this burden because informative error messages for
// `assert a == b; x` are critical for our users' testing UX.
void EvalState::assertEqValues(ValueRef v1, ValueRef v2, const PosIdx pos, std::string_view errorCtx)
{
    // This implementation must match eqValues.
    forceValue(v1, pos);
    forceValue(v2, pos);

    if (v1 == v2)
        return;

    // Special case type-compatibility between float and int
    if ((v1.type(values) == nInt || v1.type(values) == nFloat) && (v2.type(values) == nInt || v2.type(values) == nFloat)) {
        if (eqValues(v1, v2, pos, errorCtx)) {
            return;
        } else {
            error<AssertionError>(
                "%s with value '%s' is not equal to %s with value '%s'",
                showType(*this, v1),
                ValuePrinter(*this, v1, errorPrintOptions),
                showType(*this, v2),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
    }

    if (v1.type(values) != v2.type(values)) {
        error<AssertionError>(
            "%s of value '%s' is not equal to %s of value '%s'",
            showType(*this, v1),
            ValuePrinter(*this, v1, errorPrintOptions),
            showType(*this, v2),
            ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
    }

    switch (v1.type(values)) {
    case nInt:
        if (v1.integer(values) != v2.integer(values)) {
            error<AssertionError>("integer '%d' is not equal to integer '%d'", v1.integer(values), v2.integer(values)).debugThrow();
        }
        return;

    case nBool:
        if (v1.boolean(values) != v2.boolean(values)) {
            error<AssertionError>(
                "boolean '%s' is not equal to boolean '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nString:
        if (strcmp(v1.c_str(values), v2.c_str(values)) != 0) {
            error<AssertionError>(
                "string '%s' is not equal to string '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nPath:
        if (v1.pathAccessor(values) != v2.pathAccessor(values)) {
            error<AssertionError>(
                "path '%s' is not equal to path '%s' because their accessors are different",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        if (strcmp(v1.pathStr(values), v2.pathStr(values)) != 0) {
            error<AssertionError>(
                "path '%s' is not equal to path '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nNull:
        return;

    case nList:
        if (v1.listSize(values) != v2.listSize(values)) {
            error<AssertionError>(
                "list of size '%d' is not equal to list of size '%d', left hand side is '%s', right hand side is '%s'",
                v1.listSize(values),
                v2.listSize(values),
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        for (size_t n = 0; n < v1.listSize(values); ++n) {
            try {
                assertEqValues(v1.listView(values)[n], v2.listView(values)[n], pos, errorCtx);
            } catch (Error & e) {
                e.addTrace(positions[pos], "while comparing list element %d", n);
                throw;
            }
        }
        return;

    case nAttrs: {
        if (isDerivation(v1) && isDerivation(v2)) {
            auto i = v1.attrs(values)->get(sOutPath);
            auto j = v2.attrs(values)->get(sOutPath);
            if (i && j) {
                try {
                    assertEqValues(i->value, j->value, pos, errorCtx);
                    return;
                } catch (Error & e) {
                    e.addTrace(positions[pos], "while comparing a derivation by its '%s' attribute", "outPath");
                    throw;
                }
                assert(false);
            }
        }

        if (v1.attrs(values)->size() != v2.attrs(values)->size()) {
            error<AssertionError>(
                "attribute names of attribute set '%s' differs from attribute set '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }

        // Like normal comparison, we compare the attributes in non-deterministic Symbol index order.
        // This function is called when eqValues has found a difference, so to reliably
        // report about its result, we should follow in its literal footsteps and not
        // try anything fancy that could lead to an error.
        Bindings::const_iterator i, j;
        for (i = v1.attrs(values)->begin(), j = v2.attrs(values)->begin(); i != v1.attrs(values)->end(); ++i, ++j) {
            if (i->name != j->name) {
                // A difference in a sorted list means that one attribute is not contained in the other, but we don't
                // know which. Let's find out. Could use <, but this is more clear.
                if (!v2.attrs(values)->get(i->name)) {
                    error<AssertionError>(
                        "attribute name '%s' is contained in '%s', but not in '%s'",
                        symbols[i->name],
                        ValuePrinter(*this, v1, errorPrintOptions),
                        ValuePrinter(*this, v2, errorPrintOptions))
                        .debugThrow();
                }
                if (!v1.attrs(values)->get(j->name)) {
                    error<AssertionError>(
                        "attribute name '%s' is missing in '%s', but is contained in '%s'",
                        symbols[j->name],
                        ValuePrinter(*this, v1, errorPrintOptions),
                        ValuePrinter(*this, v2, errorPrintOptions))
                        .debugThrow();
                }
                assert(false);
            }
            try {
                assertEqValues(i->value, j->value, pos, errorCtx);
            } catch (Error & e) {
                // The order of traces is reversed, so this presents as
                //  where left hand side is
                //    at <pos>
                //  where right hand side is
                //    at <pos>
                //  while comparing attribute '<name>'
                if (j->pos != noPos)
                    e.addTrace(positions[j->pos], "where right hand side is");
                if (i->pos != noPos)
                    e.addTrace(positions[i->pos], "where left hand side is");
                e.addTrace(positions[pos], "while comparing attribute '%s'", symbols[i->name]);
                throw;
            }
        }
        return;
    }

    case nFunction:
        error<AssertionError>("distinct functions and immediate comparisons of identical functions compare as unequal")
            .debugThrow();

    case nExternal:
        if (!(*v1.external(values) == *v2.external(values))) {
            error<AssertionError>(
                "external value '%s' is not equal to external value '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nFloat:
        // !!!
        if (!(v1.fpoint(values) == v2.fpoint(values))) {
            error<AssertionError>("float '%f' is not equal to float '%f'", v1.fpoint(values), v2.fpoint(values)).debugThrow();
        }
        return;

    case nThunk: // Must not be left by forceValue
        assert(false);
    default: // Note that we pass compiler flags that should make `default:` unreachable.
        // Also note that this probably ran after `eqValues`, which implements
        // the same logic more efficiently (without having to unwind stacks),
        // so maybe `assertEqValues` and `eqValues` are out of sync. Check it for solutions.
        error<EvalError>("assertEqValues: cannot compare %1% with %2%", showType(*this, v1), showType(*this, v2))
            .withTrace(pos, errorCtx)
            .panic();
    }
}

// This implementation must match assertEqValues
bool EvalState::eqValues(ValueRef v1, ValueRef v2, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v1, pos);
    forceValue(v2, pos);

    // XXX [speed]: go look into this. maybe this works differently with ValueRef. Neither "builderDefs" nor "uniqList" appear anywhere in the codebase. Is this still relevant at all?
    /* !!! Hack to support some old broken code that relies on pointer
       equality tests between sets.  (Specifically, builderDefs calls
       uniqList on a list of sets.)  Will remove this eventually. */
    if (v1 == v2)
        return true;

    // Special case type-compatibility between float and int
    if (v1.type(values) == nInt && v2.type(values) == nFloat)
        return v1.integer(values).value == v2.fpoint(values);
    if (v1.type(values) == nFloat && v2.type(values) == nInt)
        return v1.fpoint(values) == v2.integer(values).value;

    // All other types are not compatible with each other.
    if (v1.type(values) != v2.type(values))
        return false;

    switch (v1.type(values)) {
    case nInt:
        return v1.integer(values) == v2.integer(values);

    case nBool:
        return v1.boolean(values) == v2.boolean(values);

    case nString:
        return strcmp(v1.c_str(values), v2.c_str(values)) == 0;

    case nPath:
        return
            // FIXME: compare accessors by their fingerprint.
            v1.pathAccessor(values) == v2.pathAccessor(values) && strcmp(v1.pathStr(values), v2.pathStr(values)) == 0;

    case nNull:
        return true;

    case nList:
        if (v1.listSize(values) != v2.listSize(values))
            return false;
        for (size_t n = 0; n < v1.listSize(values); ++n)
            if (!eqValues(v1.listView(values)[n], v2.listView(values)[n], pos, errorCtx))
                return false;
        return true;

    case nAttrs: {
        /* If both sets denote a derivation (type = "derivation"),
           then compare their outPaths. */
        if (isDerivation(v1) && isDerivation(v2)) {
            auto i = v1.attrs(values)->get(sOutPath);
            auto j = v2.attrs(values)->get(sOutPath);
            if (i && j)
                return eqValues(i->value, j->value, pos, errorCtx);
        }

        if (v1.attrs(values)->size() != v2.attrs(values)->size())
            return false;

        /* Otherwise, compare the attributes one by one. */
        Bindings::const_iterator i, j;
        for (i = v1.attrs(values)->begin(), j = v2.attrs(values)->begin(); i != v1.attrs(values)->end(); ++i, ++j)
            if (i->name != j->name || !eqValues(i->value, j->value, pos, errorCtx))
                return false;

        return true;
    }

    /* Functions are incomparable. */
    case nFunction:
        return false;

    case nExternal:
        return *v1.external(values) == *v2.external(values);

    case nFloat:
        // !!!
        return v1.fpoint(values) == v2.fpoint(values);

    case nThunk: // Must not be left by forceValue
        assert(false);
    default: // Note that we pass compiler flags that should make `default:` unreachable.
        error<EvalError>("eqValues: cannot compare %1% with %2%", showType(*this, v1), showType(*this, v2))
            .withTrace(pos, errorCtx)
            .panic();
    }
}

bool EvalState::fullGC()
{
#if NIX_USE_BOEHMGC
    GC_gcollect();
    // Check that it ran. We might replace this with a version that uses more
    // of the boehm API to get this reliably, at a maintenance cost.
    // We use a 1K margin because technically this has a race condition, but we
    // probably won't encounter it in practice, because the CLI isn't concurrent
    // like that.
    return GC_get_bytes_since_gc() < 1024;
#else
    return false;
#endif
}

void EvalState::maybePrintStats()
{
    bool showStats = getEnv("NIX_SHOW_STATS").value_or("0") != "0";

    if (showStats) {
        // Make the final heap size more deterministic.
#if NIX_USE_BOEHMGC
        if (!fullGC()) {
            warn("failed to perform a full GC before reporting stats");
        }
#endif
        printStatistics();
    }
}

void EvalState::printStatistics()
{
#ifndef _WIN32 // TODO use portable implementation
    struct rusage buf;
    getrusage(RUSAGE_SELF, &buf);
    float cpuTime = buf.ru_utime.tv_sec + ((float) buf.ru_utime.tv_usec / 1000000);
#endif

    // XXX [speed]: Come back to these
    uint64_t bEnvs = nrEnvs * sizeof(Env) + nrValuesInEnvs * sizeof(Value *);
    uint64_t bLists = nrListElems * sizeof(Value *);
    uint64_t bValues = nrValues * sizeof(Value);
    uint64_t bAttrsets = nrAttrsets * sizeof(Bindings) + nrAttrsInAttrsets * sizeof(Attr);

#if NIX_USE_BOEHMGC
    GC_word heapSize, totalBytes;
    GC_get_heap_usage_safe(&heapSize, 0, 0, 0, &totalBytes);
    double gcFullOnlyTime = ({
        auto ms = GC_get_full_gc_total_time();
        ms * 0.001;
    });
    auto gcCycles = getGCCycles();
#endif

    auto outPath = getEnv("NIX_SHOW_STATS_PATH").value_or("-");
    std::fstream fs;
    if (outPath != "-")
        fs.open(outPath, std::fstream::out);
    json topObj = json::object();
#ifndef _WIN32 // TODO implement
    topObj["cpuTime"] = cpuTime;
#endif
    topObj["time"] = {
#ifndef _WIN32 // TODO implement
        {"cpu", cpuTime},
#endif
#if NIX_USE_BOEHMGC
        {GC_is_incremental_mode() ? "gcNonIncremental" : "gc", gcFullOnlyTime},
#  ifndef _WIN32 // TODO implement
        {GC_is_incremental_mode() ? "gcNonIncrementalFraction" : "gcFraction", gcFullOnlyTime / cpuTime},
#  endif
#endif
    };
    topObj["envs"] = {
        {"number", nrEnvs},
        {"elements", nrValuesInEnvs},
        {"bytes", bEnvs},
    };
    topObj["nrExprs"] = Expr::nrExprs;
    topObj["list"] = {
        {"elements", nrListElems},
        {"bytes", bLists},
        {"concats", nrListConcats},
    };
    topObj["values"] = {
        {"number", nrValues},
        {"bytes", bValues},

        {"tUninitialized", nrUninitialized},
        {"tInt", nrInt},
        {"tBool", nrBool},
        {"tNull", nrNull},
        {"tFloat", nrFloat},
        {"tExternal", nrExternal},
        {"tPrimOp", nrPrimOp},
        {"tAttrs", nrAttrs},
        {"tListSmall", nrListSmall},
        {"tPrimOpApp", nrPrimOpApp},
        {"tApp", nrApp},
        {"tThunk", nrThunk},
        {"tLambda", nrLambda},
        {"tListN", nrListN},
        {"tString", nrString},
        {"tPath", nrPath},
        {"total", nrInt + nrBool + nrNull + nrFloat + nrExternal + nrPrimOp + nrAttrs + nrListSmall + nrPrimOpApp + nrApp + nrThunk + nrLambda + nrListN + nrString + nrPath},
        {"total_small", nrInt + nrBool + nrNull + nrFloat + nrExternal + nrAttrs + nrListSmall + nrPrimOpApp + nrApp},
        {"total_big", nrPrimOp + nrThunk + nrLambda + nrListN + nrString + nrPath},
    };
    topObj["symbols"] = {
        {"number", symbols.size()},
        {"bytes", symbols.totalSize()},
    };
    topObj["sets"] = {
        {"number", nrAttrsets},
        {"bytes", bAttrsets},
        {"elements", nrAttrsInAttrsets},
    };
    topObj["sizes"] = {
        {"Env", sizeof(Env)},
        {"Value", sizeof(Value)},
        {"Bindings", sizeof(Bindings)},
        {"Attr", sizeof(Attr)},
    };
    topObj["nrOpUpdates"] = nrOpUpdates;
    topObj["nrOpUpdateValuesCopied"] = nrOpUpdateValuesCopied;
    topObj["nrThunks"] = nrThunks;
    topObj["nrAvoided"] = nrAvoided;
    topObj["nrLookups"] = nrLookups;
    topObj["nrPrimOpCalls"] = nrPrimOpCalls;
    topObj["nrFunctionCalls"] = nrFunctionCalls;
#if NIX_USE_BOEHMGC
    topObj["gc"] = {
        {"heapSize", heapSize},
        {"totalBytes", totalBytes},
        {"cycles", gcCycles},
    };
#endif

    if (countCalls) {
        topObj["primops"] = primOpCalls;
        {
            auto & list = topObj["functions"];
            list = json::array();
            for (auto & [fun, count] : functionCalls) {
                json obj = json::object();
                if (fun->name)
                    obj["name"] = (std::string_view) symbols[fun->name];
                else
                    obj["name"] = nullptr;
                if (auto pos = positions[fun->pos]) {
                    if (auto path = std::get_if<SourcePath>(&pos.origin))
                        obj["file"] = path->to_string();
                    obj["line"] = pos.line;
                    obj["column"] = pos.column;
                }
                obj["count"] = count;
                list.push_back(obj);
            }
        }
        {
            auto list = topObj["attributes"];
            list = json::array();
            for (auto & i : attrSelects) {
                json obj = json::object();
                if (auto pos = positions[i.first]) {
                    if (auto path = std::get_if<SourcePath>(&pos.origin))
                        obj["file"] = path->to_string();
                    obj["line"] = pos.line;
                    obj["column"] = pos.column;
                }
                obj["count"] = i.second;
                list.push_back(obj);
            }
        }
    }

    if (getEnv("NIX_SHOW_SYMBOLS").value_or("0") != "0") {
        // XXX: overrides earlier assignment
        topObj["symbols"] = json::array();
        auto & list = topObj["symbols"];
        symbols.dump([&](std::string_view s) { list.emplace_back(s); });
    }
    if (outPath == "-") {
        std::cerr << topObj.dump(2) << std::endl;
    } else {
        fs << topObj.dump(2) << std::endl;
    }
}

SourcePath resolveExprPath(SourcePath path, bool addDefaultNix)
{
    unsigned int followCount = 0, maxFollow = 1024;

    /* If `path' is a symlink, follow it.  This is so that relative
       path references work. */
    while (!path.path.isRoot()) {
        // Basic cycle/depth limit to avoid infinite loops.
        if (++followCount >= maxFollow)
            throw Error("too many symbolic links encountered while traversing the path '%s'", path);
        auto p = path.parent().resolveSymlinks() / path.baseName();
        if (p.lstat().type != SourceAccessor::tSymlink)
            break;
        path = {path.accessor, CanonPath(p.readLink(), path.path.parent().value_or(CanonPath::root))};
    }

    /* If `path' refers to a directory, append `/default.nix'. */
    if (addDefaultNix && path.resolveSymlinks().lstat().type == SourceAccessor::tDirectory)
        return path / "default.nix";

    return path;
}

Expr * EvalState::parseExprFromFile(const SourcePath & path)
{
    return parseExprFromFile(path, staticBaseEnv);
}

Expr * EvalState::parseExprFromFile(const SourcePath & path, std::shared_ptr<StaticEnv> & staticEnv)
{
    auto buffer = path.resolveSymlinks().readFile();
    // readFile hopefully have left some extra space for terminators
    buffer.append("\0\0", 2);
    return parse(buffer.data(), buffer.size(), Pos::Origin(path), path.parent(), staticEnv);
}

Expr *
EvalState::parseExprFromString(std::string s_, const SourcePath & basePath, std::shared_ptr<StaticEnv> & staticEnv)
{
    // NOTE this method (and parseStdin) must take care to *fully copy* their input
    // into their respective Pos::Origin until the parser stops overwriting its input
    // data.
    auto s = make_ref<std::string>(s_);
    s_.append("\0\0", 2);
    return parse(s_.data(), s_.size(), Pos::String{.source = s}, basePath, staticEnv);
}

Expr * EvalState::parseExprFromString(std::string s, const SourcePath & basePath)
{
    return parseExprFromString(std::move(s), basePath, staticBaseEnv);
}

Expr * EvalState::parseStdin()
{
    // NOTE this method (and parseExprFromString) must take care to *fully copy* their
    // input into their respective Pos::Origin until the parser stops overwriting its
    // input data.
    // Activity act(*logger, lvlTalkative, "parsing standard input");
    auto buffer = drainFD(0);
    // drainFD should have left some extra space for terminators
    buffer.append("\0\0", 2);
    auto s = make_ref<std::string>(buffer);
    return parse(buffer.data(), buffer.size(), Pos::Stdin{.source = s}, rootPath("."), staticBaseEnv);
}

SourcePath EvalState::findFile(const std::string_view path)
{
    return findFile(lookupPath, path);
}

SourcePath EvalState::findFile(const LookupPath & lookupPath, const std::string_view path, const PosIdx pos)
{
    for (auto & i : lookupPath.elements) {
        auto suffixOpt = i.prefix.suffixIfPotentialMatch(path);

        if (!suffixOpt)
            continue;
        auto suffix = *suffixOpt;

        auto rOpt = resolveLookupPathPath(i.path);
        if (!rOpt)
            continue;
        auto r = *rOpt;

        auto res = (r / CanonPath(suffix)).resolveSymlinks();
        if (res.pathExists())
            return res;
    }

    if (hasPrefix(path, "nix/"))
        return {corepkgsFS, CanonPath(path.substr(3))};

    error<ThrownError>(
        settings.pureEval ? "cannot look up '<%s>' in pure evaluation mode (use '--impure' to override)"
                          : "file '%s' was not found in the Nix search path (add it using $NIX_PATH or -I)",
        path)
        .atPos(pos)
        .debugThrow();
}

std::optional<SourcePath> EvalState::resolveLookupPathPath(const LookupPath::Path & value0, bool initAccessControl)
{
    auto & value = value0.s;
    auto i = lookupPathResolved.find(value);
    if (i != lookupPathResolved.end())
        return i->second;

    auto finish = [&](std::optional<SourcePath> res) {
        if (res)
            debug("resolved search path element '%s' to '%s'", value, *res);
        else
            debug("failed to resolve search path element '%s'", value);
        lookupPathResolved.emplace(value, res);
        return res;
    };

    if (EvalSettings::isPseudoUrl(value)) {
        try {
            auto accessor = fetchers::downloadTarball(store, fetchSettings, EvalSettings::resolvePseudoUrl(value));
            auto storePath = fetchToStore(fetchSettings, *store, SourcePath(accessor), FetchMode::Copy);
            return finish(this->storePath(storePath));
        } catch (Error & e) {
            logWarning({.msg = HintFmt("Nix search path entry '%1%' cannot be downloaded, ignoring", value)});
        }
    }

    if (auto colPos = value.find(':'); colPos != value.npos) {
        auto scheme = value.substr(0, colPos);
        auto rest = value.substr(colPos + 1);
        if (auto * hook = get(settings.lookupPathHooks, scheme)) {
            auto res = (*hook)(*this, rest);
            if (res)
                return finish(std::move(*res));
        }
    }

    {
        auto path = rootPath(value);

        /* Allow access to paths in the search path. */
        if (initAccessControl) {
            allowPath(path.path.abs());
            if (store->isInStore(path.path.abs())) {
                try {
                    allowClosure(store->toStorePath(path.path.abs()).first);
                } catch (InvalidPath &) {
                }
            }
        }

        if (path.resolveSymlinks().pathExists())
            return finish(std::move(path));
        else {
            logWarning({.msg = HintFmt("Nix search path entry '%1%' does not exist, ignoring", value)});
        }
    }

    return finish(std::nullopt);
}

Expr * EvalState::parse(
    char * text, size_t length, Pos::Origin origin, const SourcePath & basePath, std::shared_ptr<StaticEnv> & staticEnv)
{
    DocCommentMap tmpDocComments; // Only used when not origin is not a SourcePath
    DocCommentMap * docComments = &tmpDocComments;

    if (auto sourcePath = std::get_if<SourcePath>(&origin)) {
        auto [it, _] = positionToDocComment.try_emplace(*sourcePath);
        docComments = &it->second;
    }

    auto result = parseExprFromBuf(
        text, length, origin, basePath, symbols, settings, *this, positions, *docComments, rootFS, exprSymbols);

    result->bindVars(*this, staticEnv);

    return result;
}

DocComment EvalState::getDocCommentForPos(PosIdx pos)
{
    auto pos2 = positions[pos];
    auto path = pos2.getSourcePath();
    if (!path)
        return {};

    auto table = positionToDocComment.find(*path);
    if (table == positionToDocComment.end())
        return {};

    auto it = table->second.find(pos);
    if (it == table->second.end())
        return {};
    return it->second;
}

std::string ExternalValueBase::coerceToString(
    EvalState & state, const PosIdx & pos, NixStringContext & context, bool copyMore, bool copyToStore) const
{
    state.error<TypeError>("cannot coerce %1% to a string: %2%", showType(), *this).atPos(pos).debugThrow();
}

bool ExternalValueBase::operator==(const ExternalValueBase & b) const noexcept
{
    return false;
}

std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v)
{
    return v.print(str);
}

void forceNoNullByte(std::string_view s, std::function<Pos()> pos)
{
    if (s.find('\0') != s.npos) {
        using namespace std::string_view_literals;
        auto str = replaceStrings(std::string(s), "\0"sv, "␀"sv);
        Error error("input string '%s' cannot be represented as Nix string because it contains null bytes", str);
        if (pos) {
            error.atPos(pos());
        }
        throw error;
    }
}

} // namespace nix
