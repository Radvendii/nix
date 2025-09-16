#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/util/util.hh"
#include "nix/expr/print.hh"

#include <cstdlib>
#include <sstream>

#include "nix/util/strings-inline.hh"

namespace nix {

unsigned long Expr::nrExprs = 0;

ExprBlackHole eBlackHole;

void ExprRef::eval(EvalState & state, EnvRef env, ValueRef v)
{
DYNAMIC_DISPATCH(eval(state, env, v))
}

void ExprRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
DYNAMIC_DISPATCH(bindVars(es, env))
}

void ExprRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
DYNAMIC_DISPATCH(show(exprs, values, symbols, str))
}

void ExprInheritFromRef::eval(EvalState & state, EnvRef env, ValueRef v)
{
    ExprVarRef(*this).eval(state, env, v);
}

void ExprInheritFromRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    ExprVarRef(*this).show(exprs, values, symbols, str);
}

PosIdx ExprRef::getPos(Exprs & exprs) const
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (type()) {
    case teInheritFrom:
    case teVar:
        return exprs.ERtoEP(ExprVarRef(*this))->pos;
    case teSelect:
        return exprs.ERtoEP(ExprSelectRef(*this))->pos;
    case teAttrs:
        return exprs.ERtoEP(ExprAttrsRef(*this))->pos;
    case teLambda:
        return exprs.ERtoEP(ExprLambdaRef(*this))->pos;
    case teCall:
        return exprs.ERtoEP(ExprCallRef(*this))->pos;
    case teWith:
        return exprs.ERtoEP(ExprWithRef(*this))->pos;
    case teIf:
        return exprs.ERtoEP(ExprIfRef(*this))->pos;
    case teAssert:
        return exprs.ERtoEP(ExprAssertRef(*this))->pos;
    case teOpAnd:
        return exprs.ERtoEP(ExprOpAndRef(*this))->pos;
    case teOpOr:
        return exprs.ERtoEP(ExprOpOrRef(*this))->pos;
    case teOpEq:
        return exprs.ERtoEP(ExprOpEqRef(*this))->pos;
    case teOpNEq:
        return exprs.ERtoEP(ExprOpNEqRef(*this))->pos;
    case teOpImpl:
        return exprs.ERtoEP(ExprOpImplRef(*this))->pos;
    case teConcatStrings:
        return exprs.ERtoEP(ExprConcatStringsRef(*this))->pos;
    case tePos:
        return exprs.ERtoEP(ExprPosRef(*this))->pos;
    case teOpHasAttr:
        return exprs.ERtoEP(ExprOpHasAttrRef(*this))->e.getPos(exprs);
    case teOpNot:
        return exprs.ERtoEP(ExprOpNotRef(*this))->e.getPos(exprs);
    case teList:
        return exprs.ERtoEP(ExprListRef(*this))->elems.empty() ? noPos : exprs.ERtoEP(ExprListRef(*this))->elems.front().getPos(exprs);
    default:
        return noPos;
    }
#pragma GCC diagnostic pop
};

void ExprBlackHoleRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    unreachable();
}


void ExprBlackHoleRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    unreachable();
}

Exprs::Exprs() {
#define NIX_EXPR_RESERVE(TYPE, DISCRIMINANT, VECTOR) \
VECTOR.reserve(1000000);
    NIX_FOR_EACH_EXPR(NIX_EXPR_RESERVE)
NIX_EXPR_RESERVE(ExprVar, teVar, vars)
#undef NIX_EXPR_RESERVE
}
ExprCallRef Exprs::addExprCall(const PosIdx & pos, ExprRef fun, std::vector<ExprRef> && args)
{
    calls.emplace_back(pos, fun, std::move(args));
    if(calls.size() > 999000)
        std::cout << "we're in trouble ExprCall\n";
    return ExprCallRef(calls.size() - 1);
}
ExprCallRef Exprs::addExprCall(const PosIdx & pos, ExprRef fun, std::vector<ExprRef> && args, PosIdx && cursedOrEndPos)
{
    calls.emplace_back(pos, fun, std::move(args), std::move(cursedOrEndPos));
    if(calls.size() > 999000)
        std::cout << "we're in trouble ExprCall\n";
    return ExprCallRef(calls.size() - 1);
}

#define NIX_DEFINE_GET(TYPE, DISCRIMINANT, VECTOR)  \
TYPE * Exprs::ERtoEP(TYPE##Ref ref) {               \
    assert(ExprRef(ref).type() == DISCRIMINANT);    \
    return &VECTOR[ref.ref & 0x00FFFFFF];           \
}
    NIX_FOR_EACH_EXPR(NIX_DEFINE_GET)
#undef NIX_DEFINE_GET

ExprVar * Exprs::ERtoEP(ExprVarRef ref) {
    auto type = ExprRef(ref).type();
    if (type == teVar)
        return &vars[ref.ref & 0x00FFFFFF];
    if (type == teInheritFrom)
        return &inheritFroms[ref.ref & 0x00FFFFFF];
    unreachable();
}

ExprBlackHole * Exprs::ERtoEP(ExprBlackHoleRef ref) {
    assert(ExprRef(ref).type() == teBlackHole);
    return &eBlackHole;
}

ExprRef Exprs::EPtoER(Expr * p) {
    if (!p)
        return ExprRef::null;
    if (p == &eBlackHole) {
        return ExprBlackHoleRef(0);
    }
#define NIX_EXPR_LOOK_FOR_POINTER(TYPE, DISCRIMINANT, VECTOR)         \
if (!VECTOR.empty() && p >= &VECTOR.front() && p <= &VECTOR.back()) { \
    return TYPE##Ref((TYPE *)p - &VECTOR.front());                    \
}
NIX_FOR_EACH_EXPR(NIX_EXPR_LOOK_FOR_POINTER)
NIX_EXPR_LOOK_FOR_POINTER(ExprVar, teVar, vars)
#undef NIX_EXPR_LOOK_FOR_POINTER
// this would mean this is pointing to an Expr outside this struct
unreachable();
}

#define NIX_EXPR_EPTOER(TYPE, DISCRIMINANT, VECTOR) \
TYPE##Ref Exprs::EPtoER(TYPE * p) { \
    if (!p) \
        return TYPE##Ref::null; \
    return TYPE##Ref(p - &VECTOR.front()); \
}
NIX_FOR_EACH_EXPR(NIX_EXPR_EPTOER)
#undef NIX_EXPR_EPTOER

ExprBlackHoleRef Exprs::EPtoER(ExprBlackHole * p) {
    assert(p == &eBlackHole);
    return ExprBlackHoleRef(0);
}

// ExprVar must be different because ExprInheritFrom is a subtype
ExprVarRef Exprs::EPtoER(ExprVar * p) {
    if (!p)
        return ExprVarRef::null;
    if (!vars.empty() && p >= &vars.front() && p <= &vars.back())
        return ExprVarRef(p - &vars.front());
    if (!inheritFroms.empty() && p >= &inheritFroms.front() && p <= &inheritFroms.back())
        return ExprInheritFromRef((ExprInheritFrom *)p - &inheritFroms.front());

    unreachable();
}

Expr * Exprs::ERtoEP(ExprRef ref) {
    if (!ref)
        return nullptr;
    switch (ref.type()) {
#define NIX_EXPR_SWITCH_GET_REF(TYPE, DISCRIMINANT, VECTOR) \
    case DISCRIMINANT:                                      \
        return &VECTOR[ref.ref & 0x00FFFFFF];
    NIX_FOR_EACH_EXPR(NIX_EXPR_SWITCH_GET_REF)
    NIX_EXPR_SWITCH_GET_REF(ExprVar, teVar, vars)
#undef NIX_EXPR_SWITCH_GET_REF
    case teBlackHole:
        return &eBlackHole;
    }
    unreachable();
}

// FIXME: remove, because *symbols* are abstract and do not have a single
//        textual representation; see printIdentifier()
std::ostream & operator<<(std::ostream & str, const Symbol & symbol)
{
    std::string_view s = symbol;
    return printIdentifier(str, s);
}

#define NIX_BINOP_SHOW(TYPE, STRING)                                                                        \
void TYPE##Ref::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const \
{                                                                                                           \
    str << "(";                                                                                             \
    exprs.ERtoEP(*this)->e1.show(exprs, values, symbols, str);                                              \
    str << " " STRING " ";                                                                                  \
    exprs.ERtoEP(*this)->e2.show(exprs, values, symbols, str);                                              \
    str << ")";                                                                                             \
}

NIX_FOR_EACH_BINOP(NIX_BINOP_SHOW)
#undef NIX_BINOP_SHOW

void ExprIntRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << exprs.ERtoEP(*this)->v.integer(values);
}

void ExprFloatRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << exprs.ERtoEP(*this)->v.fpoint(values);
}

void ExprStringRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    printLiteralString(str, exprs.ERtoEP(*this)->s);
}

void ExprPathRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << exprs.ERtoEP(*this)->s;
}

void ExprVarRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << symbols[exprs.ERtoEP(*this)->name];
}

void ExprSelectRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(";
    exprs.ERtoEP(*this)->e.show(exprs, values, symbols, str);
    str << ")." << showAttrPath(exprs, values, symbols, exprs.ERtoEP(*this)->attrPath);
    if (exprs.ERtoEP(*this)->def) {
        str << " or (";
        exprs.ERtoEP(*this)->def.show(exprs, values, symbols, str);
        str << ")";
    }
}

void ExprOpHasAttrRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "((";
    exprs.ERtoEP(*this)->e.show(exprs, values, symbols, str);
    str << ") ? " << showAttrPath(exprs, values, symbols, exprs.ERtoEP(*this)->attrPath) << ")";
}

void ExprAttrsRef::showBindings(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    typedef const decltype(exprs.ERtoEP(*this)->attrs)::value_type * Attr;
    std::vector<Attr> sorted;
    for (auto & i : exprs.ERtoEP(*this)->attrs)
        sorted.push_back(&i);
    std::sort(sorted.begin(), sorted.end(), [&](Attr a, Attr b) {
        std::string_view sa = symbols[a->first], sb = symbols[b->first];
        return sa < sb;
    });
    std::vector<SymbolRef> inherits;
    // We can use the displacement as a proxy for the order in which the symbols were parsed.
    // The assignment of displacements should be deterministic, so that showBindings is deterministic.
    std::map<Displacement, std::vector<SymbolRef>> inheritsFrom;
    for (auto & i : sorted) {
        switch (i->second.kind) {
        case ExprAttrs::AttrDef::Kind::Plain:
            break;
        case ExprAttrs::AttrDef::Kind::Inherited:
            inherits.push_back(i->first);
            break;
        case ExprAttrs::AttrDef::Kind::InheritedFrom: {
            auto select = i->second.e.dyn_cast<ExprSelectRef>();
            auto from = exprs.ERtoEP(select)->e.dyn_cast<ExprInheritFromRef>();
            inheritsFrom[exprs.ERtoEP(from)->displ].push_back(i->first);
            break;
        }
        }
    }
    if (!inherits.empty()) {
        str << "inherit";
        for (auto sym : inherits)
            str << " " << symbols[sym];
        str << "; ";
    }
    for (const auto & [from, syms] : inheritsFrom) {
        str << "inherit (";
        (*exprs.ERtoEP(*this)->inheritFromExprs)[from].show(exprs, values, symbols, str);
        str << ")";
        for (auto sym : syms)
            str << " " << symbols[sym];
        str << "; ";
    }
    for (auto & i : sorted) {
        if (i->second.kind == ExprAttrs::AttrDef::Kind::Plain) {
            str << symbols[i->first] << " = ";
            i->second.e.show(exprs, values, symbols, str);
            str << "; ";
        }
    }
    for (auto & i : exprs.ERtoEP(*this)->dynamicAttrs) {
        str << "\"${";
        i.nameExpr.show(exprs, values, symbols, str);
        str << "}\" = ";
        i.valueExpr.show(exprs, values, symbols, str);
        str << "; ";
    }
}

void ExprAttrsRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    if (exprs.ERtoEP(*this)->recursive)
        str << "rec ";
    str << "{ ";
    showBindings(exprs, values, symbols, str);
    str << "}";
}

void ExprListRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "[ ";
    for (auto & i : exprs.ERtoEP(*this)->elems) {
        str << "(";
        i.show(exprs, values, symbols, str);
        str << ") ";
    }
    str << "]";
}

void ExprLambdaRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(";
    if (hasFormals(exprs)) {
        str << "{ ";
        bool first = true;
        // the natural Symbol ordering is by creation time, which can lead to the
        // same expression being printed in two different ways depending on its
        // context. always use lexicographic ordering to avoid this.
        for (auto & i : exprs.ERtoEP(*this)->formals->lexicographicOrder(symbols)) {
            if (first)
                first = false;
            else
                str << ", ";
            str << symbols[i.name];
            if (i.def) {
                str << " ? ";
                i.def.show(exprs, values, symbols, str);
            }
        }
        if (exprs.ERtoEP(*this)->formals->ellipsis) {
            if (!first)
                str << ", ";
            str << "...";
        }
        str << " }";
        if (exprs.ERtoEP(*this)->arg)
            str << " @ ";
    }
    if (exprs.ERtoEP(*this)->arg)
        str << symbols[exprs.ERtoEP(*this)->arg];
    str << ": ";
    exprs.ERtoEP(*this)->body.show(exprs, values, symbols, str);
    str << ")";
}

void ExprCallRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << '(';
    exprs.ERtoEP(*this)->fun.show(exprs, values, symbols, str);
    for (auto e : exprs.ERtoEP(*this)->args) {
        str << ' ';
        e.show(exprs, values, symbols, str);
    }
    str << ')';
}

void ExprLetRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(let ";
    exprs.ERtoEP(*this)->attrs.showBindings(exprs, values, symbols, str);
    str << "in ";
    exprs.ERtoEP(*this)->body.show(exprs, values, symbols, str);
    str << ")";
}

void ExprWithRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(with ";
    exprs.ERtoEP(*this)->attrs.show(exprs, values, symbols, str);
    str << "; ";
    exprs.ERtoEP(*this)->body.show(exprs, values, symbols, str);
    str << ")";
}

void ExprIfRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(if ";
    exprs.ERtoEP(*this)->cond.show(exprs, values, symbols, str);
    str << " then ";
    exprs.ERtoEP(*this)->then.show(exprs, values, symbols, str);
    str << " else ";
    exprs.ERtoEP(*this)->else_.show(exprs, values, symbols, str);
    str << ")";
}

void ExprAssertRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "assert ";
    exprs.ERtoEP(*this)->cond.show(exprs, values, symbols, str);
    str << "; ";
    exprs.ERtoEP(*this)->body.show(exprs, values, symbols, str);
}

void ExprOpNotRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(! ";
    exprs.ERtoEP(*this)->e.show(exprs, values, symbols, str);
    str << ")";
}

void ExprConcatStringsRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    bool first = true;
    str << "(";
    for (auto & i : *exprs.ERtoEP(*this)->es) {
        if (first)
            first = false;
        else
            str << " + ";
        i.second.show(exprs, values, symbols, str);
    }
    str << ")";
}

void ExprPosRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "__curPos";
}

std::string showAttrPath(Exprs & exprs, Values & values, const SymbolTable & symbols, const AttrPath & attrPath)
{
    std::ostringstream out;
    bool first = true;
    for (auto & i : attrPath) {
        if (!first)
            out << '.';
        else
            first = false;
        if (i.symbol)
            out << symbols[i.symbol];
        else {
            out << "\"${";
            i.expr.show(exprs, values, symbols, out);
            out << "}\"";
        }
    }
    return out.str();
}

/* Computing levels/displacements for variables. */

void ExprIntRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

void ExprFloatRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

void ExprStringRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

void ExprPathRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

void ExprVarRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->fromWith = ExprWithRef::null;

    /* Check whether the variable appears in the environment.  If so,
       set its level and displacement. */
    const StaticEnv * curEnv;
    Level level;
    int withLevel = -1;
    for (curEnv = env.get(), level = 0; curEnv; curEnv = curEnv->up.get(), level++) {
        if (curEnv->isWith) {
            if (withLevel == -1)
                withLevel = level;
        } else {
            auto i = curEnv->find(es.exprs.ERtoEP(*this)->name);
            if (i != curEnv->vars.end()) {
                // XXX [speed]: why do we store a level, that we have to trace back up to later, and not store the EnvRef directly? Because the Env hasn't been populated yet, perhaps? We're working at the AST level right now, not the Value level.
                es.exprs.ERtoEP(*this)->level = level;
                es.exprs.ERtoEP(*this)->displ = i->second;
                return;
            }
        }
    }

    /* Otherwise, the variable must be obtained from the nearest
       enclosing `with'.  If there is no `with', then we can issue an
       "undefined variable" error now. */
    if (withLevel == -1)
        es.error<UndefinedVarError>("undefined variable '%1%'", es.symbols[es.exprs.ERtoEP(*this)->name]).atPos(es.exprs.ERtoEP(*this)->pos).debugThrow();
    for (auto * e = env.get(); e && !es.exprs.ERtoEP(*this)->fromWith; e = e->up.get())
        es.exprs.ERtoEP(*this)->fromWith = e->isWith;
    es.exprs.ERtoEP(*this)->level = withLevel;
}

void ExprInheritFromRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

void ExprSelectRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->e.bindVars(es, env);
    if (es.exprs.ERtoEP(*this)->def)
        es.exprs.ERtoEP(*this)->def.bindVars(es, env);
    for (auto & i : es.exprs.ERtoEP(*this)->attrPath)
        if (!i.symbol)
            i.expr.bindVars(es, env);
}

void ExprOpHasAttrRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->e.bindVars(es, env);
    for (auto & i : es.exprs.ERtoEP(*this)->attrPath)
        if (!i.symbol)
            i.expr.bindVars(es, env);
}

std::shared_ptr<const StaticEnv>
ExprAttrsRef::bindInheritSources(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (!es.exprs.ERtoEP(*this)->inheritFromExprs)
        return nullptr;

    // the inherit (from) source values are inserted into an env of its own, which
    // does not introduce any variable names.
    // analysis must see an empty env, or an env that contains only entries with
    // otherwise unused names to not interfere with regular names. the parser
    // has already filled all exprs that access this env with appropriate level
    // and displacement, and nothing else is allowed to access it. ideally we'd
    // not even *have* an expr that grabs anything from this env since it's fully
    // invisible, but the evaluator does not allow for this yet.
    auto inner = std::make_shared<StaticEnv>(ExprWithRef::null, env, 0);
    for (auto from : *es.exprs.ERtoEP(*this)->inheritFromExprs)
        from.bindVars(es, env);

    return inner;
}

void ExprAttrsRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    if (es.exprs.ERtoEP(*this)->recursive) {
        auto newEnv = [&]() -> std::shared_ptr<const StaticEnv> {
            auto newEnv = std::make_shared<StaticEnv>(ExprWithRef::null, env, es.exprs.ERtoEP(*this)->attrs.size());

            Displacement displ = 0;
            for (auto & i : es.exprs.ERtoEP(*this)->attrs)
                newEnv->vars.emplace_back(i.first, i.second.displ = displ++);
            return newEnv;
        }();

        // No need to sort newEnv since attrs is in sorted order.

        auto inheritFromEnv = bindInheritSources(es, newEnv);
        for (auto & i : es.exprs.ERtoEP(*this)->attrs)
            i.second.e.bindVars(es, i.second.chooseByKind(newEnv, env, inheritFromEnv));

        for (auto & i : es.exprs.ERtoEP(*this)->dynamicAttrs) {
            i.nameExpr.bindVars(es, newEnv);
            i.valueExpr.bindVars(es, newEnv);
        }
    } else {
        auto inheritFromEnv = bindInheritSources(es, env);

        for (auto & i : es.exprs.ERtoEP(*this)->attrs)
            i.second.e.bindVars(es, i.second.chooseByKind(env, env, inheritFromEnv));

        for (auto & i : es.exprs.ERtoEP(*this)->dynamicAttrs) {
            i.nameExpr.bindVars(es, env);
            i.valueExpr.bindVars(es, env);
        }
    }
}

void ExprListRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    for (auto & i : es.exprs.ERtoEP(*this)->elems)
        i.bindVars(es, env);
}

void ExprLambdaRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    auto newEnv =
        std::make_shared<StaticEnv>(ExprWithRef::null, env, (hasFormals(es.exprs) ? es.exprs.ERtoEP(*this)->formals->formals.size() : 0) + (!es.exprs.ERtoEP(*this)->arg ? 0 : 1));

    Displacement displ = 0;

    if (es.exprs.ERtoEP(*this)->arg)
        newEnv->vars.emplace_back(es.exprs.ERtoEP(*this)->arg, displ++);

    if (hasFormals(es.exprs)) {
        for (auto & i : es.exprs.ERtoEP(*this)->formals->formals)
            newEnv->vars.emplace_back(i.name, displ++);

        newEnv->sort();

        for (auto & i : es.exprs.ERtoEP(*this)->formals->formals)
            if (i.def)
                i.def.bindVars(es, newEnv);
    }

    es.exprs.ERtoEP(*this)->body.bindVars(es, newEnv);
}

void ExprCallRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->fun.bindVars(es, env);
    for (auto e : es.exprs.ERtoEP(*this)->args)
        e.bindVars(es, env);
}

void ExprLetRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    auto newEnv = [&]() -> std::shared_ptr<const StaticEnv> {
        auto newEnv = std::make_shared<StaticEnv>(ExprWithRef::null, env, es.exprs.ERtoEP(es.exprs.ERtoEP(*this)->attrs)->attrs.size());

        Displacement displ = 0;
        for (auto & i : es.exprs.ERtoEP(es.exprs.ERtoEP(*this)->attrs)->attrs)
            newEnv->vars.emplace_back(i.first, i.second.displ = displ++);
        return newEnv;
    }();

    // No need to sort newEnv since attrs->attrs is in sorted order.

    auto inheritFromEnv = es.exprs.ERtoEP(*this)->attrs.bindInheritSources(es, newEnv);
    for (auto & i : es.exprs.ERtoEP(es.exprs.ERtoEP(*this)->attrs)->attrs)
        i.second.e.bindVars(es, i.second.chooseByKind(newEnv, env, inheritFromEnv));

    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, newEnv));

    es.exprs.ERtoEP(*this)->body.bindVars(es, newEnv);
}

void ExprWithRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->parentWith = ExprWithRef::null;
    for (auto * e = env.get(); e && !es.exprs.ERtoEP(*this)->parentWith; e = e->up.get())
        es.exprs.ERtoEP(*this)->parentWith = e->isWith;

    /* Does this `with' have an enclosing `with'?  If so, record its
       level so that `lookupVar' can look up variables in the previous
       `with' if this one doesn't contain the desired attribute. */
    const StaticEnv * curEnv;
    Level level;
    es.exprs.ERtoEP(*this)->prevWith = 0;
    for (curEnv = env.get(), level = 1; curEnv; curEnv = curEnv->up.get(), level++)
        if (curEnv->isWith) {
            es.exprs.ERtoEP(*this)->prevWith = level;
            break;
        }

    es.exprs.ERtoEP(*this)->attrs.bindVars(es, env);
    auto newEnv = std::make_shared<StaticEnv>(*this, env);
    es.exprs.ERtoEP(*this)->body.bindVars(es, newEnv);
}

void ExprIfRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->cond.bindVars(es, env);
    es.exprs.ERtoEP(*this)->then.bindVars(es, env);
    es.exprs.ERtoEP(*this)->else_.bindVars(es, env);
}

void ExprAssertRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->cond.bindVars(es, env);
    es.exprs.ERtoEP(*this)->body.bindVars(es, env);
}

void ExprOpNotRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    es.exprs.ERtoEP(*this)->e.bindVars(es, env);
}

void ExprConcatStringsRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    for (auto & i : *es.exprs.ERtoEP(*this)->es)
        i.second.bindVars(es, env);
}

void ExprPosRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

/* Storing function names. */

void ExprRef::setName(Exprs & exprs, SymbolRef name) {
    if (type() == teLambda) {
        ExprLambdaRef(*this).setName(exprs, name);
    }
}
void ExprRef::setDocComment(Exprs & exprs, DocComment docComment) {
    if (type() == teLambda) {
        ExprLambdaRef(*this).setDocComment(exprs, docComment);
    }
}

void ExprLambdaRef::setName(Exprs & exprs, SymbolRef name)
{
    exprs.ERtoEP(*this)->name = name;
    exprs.ERtoEP(*this)->body.setName(exprs, name);
}

std::string ExprLambdaRef::showNamePos(EvalState & state)
{
    std::string id(state.exprs.ERtoEP(*this)->name ? concatStrings("'", state.symbols[state.exprs.ERtoEP(*this)->name], "'") : "anonymous function");
    return fmt("%1% at %2%", id, state.positions[state.exprs.ERtoEP(*this)->pos]);
}

void ExprLambdaRef::setDocComment(Exprs & exprs, DocComment docComment)
{
    // RFC 145 specifies that the innermost doc comment wins.
    // See https://github.com/NixOS/rfcs/blob/master/rfcs/0145-doc-strings.md#ambiguous-placement
    if (!exprs.ERtoEP(*this)->docComment) {
        exprs.ERtoEP(*this)->docComment = docComment;

        // Curried functions are defined by putting a function directly
        // in the body of another function. To render docs for those, we
        // need to propagate the doc comment to the innermost function.
        //
        // If we have our own comment, we've already propagated it, so this
        // belongs in the same conditional.
        exprs.ERtoEP(*this)->body.setDocComment(exprs, docComment);
    }
};

/* Symbol table. */

size_t SymbolTable::totalSize() const
{
    size_t n = 0;
    dump([&](Symbol s) { n += s.size(); });
    return n;
}

std::string DocComment::getInnerText(const PosTable & positions) const
{
    auto beginPos = positions[begin];
    auto endPos = positions[end];
    auto docCommentStr = beginPos.getSnippetUpTo(endPos).value_or("");

    // Strip "/**" and "*/"
    constexpr size_t prefixLen = 3;
    constexpr size_t suffixLen = 2;
    std::string docStr = docCommentStr.substr(prefixLen, docCommentStr.size() - prefixLen - suffixLen);
    if (docStr.empty())
        return {};
    // Turn the now missing "/**" into indentation
    docStr = "   " + docStr;
    // Strip indentation (for the whole, potentially multi-line string)
    docStr = stripIndentation(docStr);
    return docStr;
}

/* ‘Cursed or’ handling.
 *
 * In parser.y, every use of expr_select in a production must call one of the
 * two below functions.
 *
 * To be removed by https://github.com/NixOS/nix/pull/11121
 */

 void ExprRef::resetCursedOr(Exprs & exprs)
 {
     if (type() == teCall)
         ExprCallRef(*this).resetCursedOr(exprs);
 }
void ExprRef::warnIfCursedOr(Exprs & exprs, const SymbolTable & symbols, const PosTable & positions)
 {
     if (type() == teCall)
         ExprCallRef(*this).warnIfCursedOr(exprs, symbols, positions);
 }

void ExprCallRef::resetCursedOr(Exprs & exprs)
{
    exprs.ERtoEP(*this)->cursedOrEndPos.reset();
}

void ExprCallRef::warnIfCursedOr(Exprs & exprs, const SymbolTable & symbols, const PosTable & positions)
{
    if (exprs.ERtoEP(*this)->cursedOrEndPos.has_value()) {
        std::ostringstream out;
        out << "at " << positions[exprs.ERtoEP(*this)->pos]
            << ": "
               "This expression uses `or` as an identifier in a way that will change in a future Nix release.\n"
               "Wrap this entire expression in parentheses to preserve its current meaning:\n"
               "    ("
            << positions[exprs.ERtoEP(*this)->pos].getSnippetUpTo(positions[*exprs.ERtoEP(*this)->cursedOrEndPos]).value_or("could not read expression")
            << ")\n"
               "Give feedback at https://github.com/NixOS/nix/pull/11121";
        warn(out.str());
    }
}

} // namespace nix
