#pragma once
///@file

#include <map>
#include <vector>

#include "nix/expr/value.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/eval-error.hh"
#include "nix/util/pos-idx.hh"

namespace nix {

class EvalState;
class PosTable;
struct Env;
struct ExprWith;
struct StaticEnv;
struct Value;

/**
 * A documentation comment, in the sense of [RFC
 * 145](https://github.com/NixOS/rfcs/blob/master/rfcs/0145-doc-strings.md)
 *
 * Note that this does not implement the following:
 *  - argument attribute names ("formals"): TBD
 *  - argument names: these are internal to the function and their names may not be optimal for documentation
 *  - function arity (degree of currying or number of ':'s):
 *      - Functions returning partially applied functions have a higher arity
 *        than can be determined locally and without evaluation.
 *        We do not want to present false data.
 *      - Some functions should be thought of as transformations of other
 *        functions. For instance `overlay -> overlay -> overlay` is the simplest
 *        way to understand `composeExtensions`, but its implementation looks like
 *        `f: g: final: prev: <...>`. The parameters `final` and `prev` are part
 *        of the overlay concept, while distracting from the function's purpose.
 */
struct DocComment
{

    /**
     * Start of the comment, including the opening, ie `/` and `**`.
     */
    PosIdx begin;

    /**
     * Position right after the final asterisk and `/` that terminate the comment.
     */
    PosIdx end;

    /**
     * Whether the comment is set.
     *
     * A `DocComment` is small enough that it makes sense to pass by value, and
     * therefore baking optionality into it is also useful, to avoiding the memory
     * overhead of `std::optional`.
     */
    operator bool() const
    {
        return static_cast<bool>(begin);
    }

    std::string getInnerText(const PosTable & positions) const;
};

/**
 * An attribute path is a sequence of attribute names.
 */
struct AttrName
{
    SymbolRef symbol;
    ExprRef expr = ExprRef::null;
    AttrName(SymbolRef s)
        : symbol(s) {};
    AttrName(ExprRef e)
        : expr(e) {};
};

typedef std::vector<AttrName> AttrPath;

std::string showAttrPath(Exprs & exprs, Values & values, const SymbolTable & symbols, const AttrPath & attrPath);

/* Abstract syntax of Nix expressions. */

struct Expr
{
    struct AstSymbols
    {
        SymbolRef sub, lessThan, mul, div, or_, findFile, nixPath, body;
    };

    static unsigned long nrExprs;
};

struct ExprInt
{
    ValueRef v;

    ExprInt(Values & values, NixInt n);

    ExprInt(Values & values, NixInt::Inner n);
};

struct ExprFloat
{
    ValueRef v;

    ExprFloat(Values & values, NixFloat nf);
};

struct ExprString
{
    std::string s;
    ValueRef v;

    ExprString(Values & values, std::string && s);
};

struct ExprPath
{
    ref<SourceAccessor> accessor;
    std::string s;
    ValueRef v;

    ExprPath(Values & values, ref<SourceAccessor> accessor, std::string && s);
};

typedef uint32_t Level;
typedef uint32_t Displacement;

struct ExprVar
{
    PosIdx pos;
    SymbolRef name;

    /* Whether the variable comes from an environment (e.g. a rec, let
       or function argument) or from a "with".

       `nullptr`: Not from a `with`.
       Valid pointer: the nearest, innermost `with` expression to query first. */
    ExprRefOf<teWith> fromWith = ExprRefOf<teWith>::null;

    /* In the former case, the value is obtained by going `level`
       levels up from the current environment and getting the
       `displ`th value in that environment.  In the latter case, the
       value is obtained by getting the attribute named `name` from
       the set stored in the environment that is `level` levels up
       from the current one.*/
    Level level = 0;
    Displacement displ = 0;

    ExprVar(SymbolRef name)
        : name(name) {};
    ExprVar(const PosIdx & pos, SymbolRef name)
        : pos(pos)
        , name(name) {};
};

struct ExprSelect
{
    PosIdx pos;
    ExprRef e, def;
    AttrPath attrPath;
    ExprSelect(const PosIdx & pos, ExprRef e, AttrPath attrPath, ExprRef def)
        : pos(pos)
        , e(e)
        , def(def)
        , attrPath(std::move(attrPath)) {};

    ExprSelect(const PosIdx & pos, ExprRef e, SymbolRef name)
        : pos(pos)
        , e(e)
        , def(0)
    {
        attrPath.push_back(AttrName(name));
    };
};

struct ExprOpHasAttr
{
    ExprRef e;
    AttrPath attrPath;
    ExprOpHasAttr(ExprRef e, AttrPath attrPath)
        : e(e)
        , attrPath(std::move(attrPath)) {};
};

struct AttrDef
{
    enum class Kind {
        /** `attr = expr;` */
        Plain,
        /** `inherit attr1 attrn;` */
        Inherited,
        /** `inherit (expr) attr1 attrn;` */
        InheritedFrom,
    };

    Kind kind;
    ExprRef e;
    PosIdx pos;
    Displacement displ = 0; // displacement
    AttrDef(ExprRef e, const PosIdx & pos, Kind kind = Kind::Plain)
        : kind(kind)
        , e(e)
        , pos(pos) {};
    AttrDef() {};

    template<typename T>
    const T & chooseByKind(const T & plain, const T & inherited, const T & inheritedFrom) const
    {
        switch (kind) {
        case Kind::Plain:
            return plain;
        case Kind::Inherited:
            return inherited;
        default:
        case Kind::InheritedFrom:
            return inheritedFrom;
        }
    }
};

struct DynamicAttrDef
{
    ExprRef nameExpr, valueExpr;
    PosIdx pos;
    DynamicAttrDef(ExprRef nameExpr, ExprRef valueExpr, const PosIdx & pos)
        : nameExpr(nameExpr)
        , valueExpr(valueExpr)
        , pos(pos) {};
};

typedef std::map<SymbolRef, AttrDef> AttrDefs;
typedef std::vector<DynamicAttrDef> DynamicAttrDefs;

struct ExprAttrs
{
    bool recursive;
    PosIdx pos;

    AttrDefs attrs;
    std::unique_ptr<std::vector<ExprRef>> inheritFromExprs;

    DynamicAttrDefs dynamicAttrs;
    ExprAttrs(const PosIdx & pos)
        : recursive(false)
        , pos(pos) {};
    ExprAttrs()
        : recursive(false) {};
};

struct ExprList
{
    std::vector<ExprRef> elems;
    ExprList() {};
};

struct Formal
{
    PosIdx pos;
    SymbolRef name;
    ExprRef def;
};

struct Formals
{
    typedef std::vector<Formal> Formals_;
    /**
     * @pre Sorted according to predicate (std::tie(a.name, a.pos) < std::tie(b.name, b.pos)).
     */
    Formals_ formals;
    bool ellipsis;

    bool has(SymbolRef arg) const
    {
        auto it = std::lower_bound(
            formals.begin(), formals.end(), arg, [](const Formal & f, const SymbolRef & sym) { return f.name < sym; });
        return it != formals.end() && it->name == arg;
    }

    std::vector<Formal> lexicographicOrder(const SymbolTable & symbols) const
    {
        std::vector<Formal> result(formals.begin(), formals.end());
        std::sort(result.begin(), result.end(), [&](const Formal & a, const Formal & b) {
            std::string_view sa = symbols[a.name], sb = symbols[b.name];
            return sa < sb;
        });
        return result;
    }
};

struct ExprLambda
{
    PosIdx pos;
    SymbolRef name;
    SymbolRef arg;
    Formals * formals;
    ExprRef body;
    DocComment docComment;

    ExprLambda(PosIdx pos, SymbolRef arg, Formals * formals, ExprRef body)
        : pos(pos)
        , arg(arg)
        , formals(formals)
        , body(body) {};

    ExprLambda(PosIdx pos, Formals * formals, ExprRef body)
        : pos(pos)
        , formals(formals)
        , body(body)
    {
    }
};

struct ExprCall
{
    ExprRef fun;
    std::vector<ExprRef> args;
    PosIdx pos;
    std::optional<PosIdx> cursedOrEndPos; // used during parsing to warn about https://github.com/NixOS/nix/issues/11118

    ExprCall(const PosIdx & pos, ExprRef fun, std::vector<ExprRef> && args)
        : fun(fun)
        , args(args)
        , pos(pos)
        , cursedOrEndPos({})
    {
    }

    ExprCall(const PosIdx & pos, ExprRef fun, std::vector<ExprRef> && args, PosIdx && cursedOrEndPos)
        : fun(fun)
        , args(args)
        , pos(pos)
        , cursedOrEndPos(cursedOrEndPos)
    {
    }
};

struct ExprLet
{
    ExprRefOf<teAttrs> attrs;
    ExprRef body;
    ExprLet(ExprRefOf<teAttrs> attrs, ExprRef body)
        : attrs(attrs)
        , body(body) {};
};

struct ExprWith
{
    PosIdx pos;
    ExprRef attrs, body;
    size_t prevWith;
    ExprRefOf<teWith> parentWith;
    ExprWith(const PosIdx & pos, ExprRef attrs, ExprRef body)
        : pos(pos)
        , attrs(attrs)
        , body(body) {};
};

struct ExprIf
{
    PosIdx pos;
    ExprRef cond, then, else_;
    ExprIf(const PosIdx & pos, ExprRef cond, ExprRef then, ExprRef else_)
        : pos(pos)
        , cond(cond)
        , then(then)
        , else_(else_) {};
};

struct ExprAssert
{
    PosIdx pos;
    ExprRef cond, body;
    ExprAssert(const PosIdx & pos, ExprRef cond, ExprRef body)
        : pos(pos)
        , cond(cond)
        , body(body) {};
};

struct ExprOpNot
{
    ExprRef e;
    ExprOpNot(ExprRef e)
        : e(e) {};
};

#define NIX_FOR_EACH_BINOP(MACRO)               \
MACRO(ExprOpEq, teOpEq, "==")                   \
MACRO(ExprOpNEq, teOpNEq, "!=")                 \
MACRO(ExprOpAnd, teOpAnd, "&&")                 \
MACRO(ExprOpOr, teOpOr, "||")                   \
MACRO(ExprOpImpl, teOpImpl, "->")               \
MACRO(ExprOpUpdate, teOpUpdate, "//")           \
MACRO(ExprOpConcatLists, teOpConcatLists, "++")

#define MakeBinOp(name, discr, s)                                  \
struct name                                                        \
{                                                                  \
    PosIdx pos;                                                    \
    ExprRef e1, e2;                                                \
    name(ExprRef e1, ExprRef e2)                                   \
        : e1(e1)                                                   \
        , e2(e2) {};                                               \
    name(const PosIdx & pos, ExprRef e1, ExprRef e2)               \
        : pos(pos)                                                 \
        , e1(e1)                                                   \
        , e2(e2) {};                                               \
};

NIX_FOR_EACH_BINOP(MakeBinOp)

struct ExprConcatStrings
{
    PosIdx pos;
    bool forceString;
    std::vector<std::pair<PosIdx, ExprRef>> * es;
    ExprConcatStrings(const PosIdx & pos, bool forceString, std::vector<std::pair<PosIdx, ExprRef>> * es)
        : pos(pos)
        , forceString(forceString)
        , es(es) {};
};

struct ExprPos
{
    PosIdx pos;
    ExprPos(const PosIdx & pos)
        : pos(pos) {};
};

/* Static environments are used to map variable names onto (level,
   displacement) pairs used to obtain the value of the variable at
   runtime. */
struct StaticEnv
{
    ExprRefOf<teWith> isWith;
    std::shared_ptr<const StaticEnv> up;

    // Note: these must be in sorted order.
    typedef std::vector<std::pair<SymbolRef, Displacement>> Vars;
    Vars vars;

    StaticEnv(ExprRefOf<teWith> isWith, std::shared_ptr<const StaticEnv> up, size_t expectedSize = 0)
        : isWith(isWith)
        , up(std::move(up))
    {
        vars.reserve(expectedSize);
    };

    void sort()
    {
        std::stable_sort(vars.begin(), vars.end(), [](const Vars::value_type & a, const Vars::value_type & b) {
            return a.first < b.first;
        });
    }

    void deduplicate()
    {
        auto it = vars.begin(), jt = it, end = vars.end();
        while (jt != end) {
            *it = *jt++;
            while (jt != end && it->first == jt->first)
                *it = *jt++;
            it++;
        }
        vars.erase(it, end);
    }

    Vars::const_iterator find(SymbolRef name) const
    {
        Vars::value_type key(name, 0);
        auto i = std::lower_bound(vars.begin(), vars.end(), key);
        if (i != vars.end() && i->first == name)
            return i;
        return vars.end();
    }
};

template<Type ty>
ExprRefOf<ty> Exprs::add(auto && ...args) {
    payloads<ty>().emplace_back(std::forward<decltype(args)>(args)...);
    if (payloads<ty>().size() > 999000)
        std::cout << "we're in trouble " << ty << "\n";
    Expr::nrExprs++;
    return ExprRefOf<ty>(payloads<ty>().size() - 1);
}

template<Type ty>
inline PayloadOf<ty> & ExprRefOf<ty>::payload(Exprs & exprs) const noexcept {
    assert(ExprRef(*this).type() == ty);
    return exprs.payloads<ty>()[ref & 0x00FFFFFF];
}
template<>
inline PayloadOf<teVar> & ExprRefOf<teVar>::payload(Exprs & exprs) const noexcept {
    auto type = ExprRef(*this).type();
    if (type == teVar)
        return exprs.payloads<teVar>()[ref & 0x00FFFFFF];
    if (type == teInheritFrom)
        return exprs.payloads<teInheritFrom>()[ref & 0x00FFFFFF];
    unreachable();
}

template<>
inline bool ExprRefOf<teLambda>::hasFormals(Exprs & exprs) const
{
    return payload(exprs).formals != nullptr;
}
} // namespace nix
