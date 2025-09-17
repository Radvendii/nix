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
    ExprWithRef fromWith = ExprWithRef::null;

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

/**
 * A pseudo-expression for the purpose of evaluating the `from` expression in `inherit (from)` syntax.
 * Unlike normal variable references, the displacement is set during parsing, and always refers to
 * `ExprAttrs::inheritFromExprs` (by itself or in `ExprLet`), whose values are put into their own `Env`.
 */
struct ExprInheritFrom
{
    PosIdx pos;
    SymbolRef name;
    ExprWithRef fromWith = ExprWithRef::null;
    Level level = 0;
    Displacement displ = 0;

    ExprInheritFrom(PosIdx pos, Displacement displ)
        : pos(pos)
        , name(SymbolRef::null)
        , fromWith(ExprWithRef::null)
        , level(0)
        , displ(displ) {};
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

struct ExprAttrs
{
    bool recursive;
    PosIdx pos;

    typedef std::map<SymbolRef, AttrDef> AttrDefs;
    AttrDefs attrs;
    std::unique_ptr<std::vector<ExprRef>> inheritFromExprs;

    typedef std::vector<DynamicAttrDef> DynamicAttrDefs;
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
    ExprAttrsRef attrs;
    ExprRef body;
    ExprLet(ExprAttrsRef attrs, ExprRef body)
        : attrs(attrs)
        , body(body) {};
};

struct ExprWith
{
    PosIdx pos;
    ExprRef attrs, body;
    size_t prevWith;
    ExprWithRef parentWith;
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

#define NIX_FOR_EACH_BINOP(MACRO) \
MACRO(ExprOpEq, "==")             \
MACRO(ExprOpNEq, "!=")            \
MACRO(ExprOpAnd, "&&")            \
MACRO(ExprOpOr, "||")             \
MACRO(ExprOpImpl, "->")           \
MACRO(ExprOpUpdate, "//")         \
MACRO(ExprOpConcatLists, "++")

#define MakeBinOp(name, s)                                         \
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
    ExprWithRef isWith;
    std::shared_ptr<const StaticEnv> up;

    // Note: these must be in sorted order.
    typedef std::vector<std::pair<SymbolRef, Displacement>> Vars;
    Vars vars;

    StaticEnv(ExprWithRef isWith, std::shared_ptr<const StaticEnv> up, size_t expectedSize = 0)
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
#define NIX_DEFINE_ADD(TYPE, DISCRIMINANT, VECTOR)              \
TYPE##Ref Exprs::add##TYPE(auto && ...args) {                   \
    VECTOR.emplace_back(std::forward<decltype(args)>(args)...); \
    if(VECTOR.size() > 999000)                                  \
        std::cout << "we're in trouble " #TYPE "\n";            \
    Expr::nrExprs++;                                            \
    return TYPE##Ref(VECTOR.size() - 1);                        \
}
NIX_FOR_EACH_EXPR(NIX_DEFINE_ADD)
NIX_DEFINE_ADD(ExprVar, teVar, vars)
#undef NIX_DEFINE_ADD
// No addExprBlackHole!
inline bool ExprLambdaRef::hasFormals(Exprs & exprs) const
{
    return formals(exprs) != nullptr;
}

#define NIX_EXPR_MEMBER_ACCESS(ExprType, DISCRIMINANT, VECTOR, MemberType, name)     \
inline MemberType & ExprType##Ref::name(Exprs & exprs) const {                       \
    assert(ExprRef(*this).type() == DISCRIMINANT);                                   \
    return exprs.VECTOR[ref & 0x00FFFFFF].name;                                      \
}
#define NIX_EXPR_VAR_MEMBER_ACCESS(ExprType, DISCRIMINANT, VECTOR, MemberType, name) \
inline MemberType & ExprType##Ref::name(Exprs & exprs) const {                       \
    auto type = ExprRef(*this).type();                                               \
    if (type == teVar)                                                               \
        return exprs.vars[ref & 0x00FFFFFF].name;                                    \
    if (type == teInheritFrom)                                                       \
        return exprs.inheritFroms[ref & 0x00FFFFFF].name;                            \
    unreachable();                                                                   \
}

NIX_EXPR_MEMBER_ACCESS(ExprWith, teWith, withs, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprWith, teWith, withs, ExprRef, attrs)
NIX_EXPR_MEMBER_ACCESS(ExprWith, teWith, withs, ExprRef, body)
NIX_EXPR_MEMBER_ACCESS(ExprWith, teWith, withs, size_t, prevWith)
NIX_EXPR_MEMBER_ACCESS(ExprWith, teWith, withs, ExprWithRef, parentWith)

NIX_EXPR_MEMBER_ACCESS(ExprLet, teLet, lets, ExprAttrsRef, attrs)
NIX_EXPR_MEMBER_ACCESS(ExprLet, teLet, lets, ExprRef, body)

NIX_EXPR_MEMBER_ACCESS(ExprIf, teIf, ifs, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprIf, teIf, ifs, ExprRef, cond)
NIX_EXPR_MEMBER_ACCESS(ExprIf, teIf, ifs, ExprRef, then)
NIX_EXPR_MEMBER_ACCESS(ExprIf, teIf, ifs, ExprRef, else_)

NIX_EXPR_MEMBER_ACCESS(ExprAttrs, teAttrs, attrss, bool, recursive)
NIX_EXPR_MEMBER_ACCESS(ExprAttrs, teAttrs, attrss, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprAttrs, teAttrs, attrss, AttrDefs, attrs)
// XXX [speed]: i don't really know how to work safely with std::unique_ptr
NIX_EXPR_MEMBER_ACCESS(ExprAttrs, teAttrs, attrss, std::unique_ptr<std::vector<ExprRef>>, inheritFromExprs)
NIX_EXPR_MEMBER_ACCESS(ExprAttrs, teAttrs, attrss, DynamicAttrDefs, dynamicAttrs)

NIX_EXPR_MEMBER_ACCESS(ExprCall, teCall, calls, ExprRef, fun)
NIX_EXPR_MEMBER_ACCESS(ExprCall, teCall, calls, std::vector<ExprRef>, args)
NIX_EXPR_MEMBER_ACCESS(ExprCall, teCall, calls, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprCall, teCall, calls, std::optional<PosIdx>, cursedOrEndPos)


NIX_EXPR_MEMBER_ACCESS(ExprFloat, teFloat, floats, ValueRef, v)

NIX_EXPR_MEMBER_ACCESS(ExprInt, teInt, ints, ValueRef, v)

NIX_EXPR_MEMBER_ACCESS(ExprPath, tePath, paths, ValueRef, v)
NIX_EXPR_MEMBER_ACCESS(ExprPath, tePath, paths, std::string, s)

NIX_EXPR_MEMBER_ACCESS(ExprSelect, teSelect, selects, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprSelect, teSelect, selects, ExprRef, e)
NIX_EXPR_MEMBER_ACCESS(ExprSelect, teSelect, selects, ExprRef, def)
NIX_EXPR_MEMBER_ACCESS(ExprSelect, teSelect, selects, AttrPath, attrPath)

NIX_EXPR_MEMBER_ACCESS(ExprLambda, teLambda, lambdas, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprLambda, teLambda, lambdas, SymbolRef, name)
NIX_EXPR_MEMBER_ACCESS(ExprLambda, teLambda, lambdas, SymbolRef, arg)
NIX_EXPR_MEMBER_ACCESS(ExprLambda, teLambda, lambdas, Formals *, formals)
NIX_EXPR_MEMBER_ACCESS(ExprLambda, teLambda, lambdas, ExprRef, body)
NIX_EXPR_MEMBER_ACCESS(ExprLambda, teLambda, lambdas, DocComment, docComment)

NIX_EXPR_MEMBER_ACCESS(ExprList, teList, lists, std::vector<ExprRef>, elems)

NIX_EXPR_MEMBER_ACCESS(ExprString, teString, strings, ValueRef, v)
NIX_EXPR_MEMBER_ACCESS(ExprString, teString, strings, std::string, s)

NIX_EXPR_MEMBER_ACCESS(ExprAssert, teAssert, asserts, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprAssert, teAssert, asserts, ExprRef, cond)
NIX_EXPR_MEMBER_ACCESS(ExprAssert, teAssert, asserts, ExprRef, body)

NIX_EXPR_MEMBER_ACCESS(ExprPos, tePos, poss, PosIdx, pos)

NIX_EXPR_MEMBER_ACCESS(ExprConcatStrings, teConcatStrings, concatStringss, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprConcatStrings, teConcatStrings, concatStringss, bool, forceString)
#define COMMA ,
NIX_EXPR_MEMBER_ACCESS(ExprConcatStrings, teConcatStrings, concatStringss, std::vector<std::pair<PosIdx COMMA ExprRef>> *, es)

NIX_EXPR_MEMBER_ACCESS(ExprOpHasAttr, teOpHasAttr, opHasAttrs, ExprRef, e)
NIX_EXPR_MEMBER_ACCESS(ExprOpHasAttr, teOpHasAttr, opHasAttrs, AttrPath, attrPath)

NIX_EXPR_MEMBER_ACCESS(ExprOpNot, teOpNot, opNots, ExprRef, e)

NIX_EXPR_MEMBER_ACCESS(ExprOpEq, teOpEq, opEqs, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprOpEq, teOpEq, opEqs, ExprRef, e1)
NIX_EXPR_MEMBER_ACCESS(ExprOpEq, teOpEq, opEqs, ExprRef, e2)
NIX_EXPR_MEMBER_ACCESS(ExprOpNEq, teOpNEq, opNEqs, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprOpNEq, teOpNEq, opNEqs, ExprRef, e1)
NIX_EXPR_MEMBER_ACCESS(ExprOpNEq, teOpNEq, opNEqs, ExprRef, e2)
NIX_EXPR_MEMBER_ACCESS(ExprOpAnd, teOpAnd, opAnds, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprOpAnd, teOpAnd, opAnds, ExprRef, e1)
NIX_EXPR_MEMBER_ACCESS(ExprOpAnd, teOpAnd, opAnds, ExprRef, e2)
NIX_EXPR_MEMBER_ACCESS(ExprOpOr, teOpOr, opOrs, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprOpOr, teOpOr, opOrs, ExprRef, e1)
NIX_EXPR_MEMBER_ACCESS(ExprOpOr, teOpOr, opOrs, ExprRef, e2)
NIX_EXPR_MEMBER_ACCESS(ExprOpImpl, teOpImpl, opImpls, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprOpImpl, teOpImpl, opImpls, ExprRef, e1)
NIX_EXPR_MEMBER_ACCESS(ExprOpImpl, teOpImpl, opImpls, ExprRef, e2)
NIX_EXPR_MEMBER_ACCESS(ExprOpUpdate, teOpUpdate, opUpdates, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprOpUpdate, teOpUpdate, opUpdates, ExprRef, e1)
NIX_EXPR_MEMBER_ACCESS(ExprOpUpdate, teOpUpdate, opUpdates, ExprRef, e2)
NIX_EXPR_MEMBER_ACCESS(ExprOpConcatLists, teOpConcatLists, opConcatListss, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprOpConcatLists, teOpConcatLists, opConcatListss, ExprRef, e1)
NIX_EXPR_MEMBER_ACCESS(ExprOpConcatLists, teOpConcatLists, opConcatListss, ExprRef, e2)

NIX_EXPR_MEMBER_ACCESS(ExprInheritFrom, teInheritFrom, inheritFroms, PosIdx, pos)
NIX_EXPR_MEMBER_ACCESS(ExprInheritFrom, teInheritFrom, inheritFroms, SymbolRef, name)
NIX_EXPR_MEMBER_ACCESS(ExprInheritFrom, teInheritFrom, inheritFroms, ExprWithRef, fromWith)
NIX_EXPR_MEMBER_ACCESS(ExprInheritFrom, teInheritFrom, inheritFroms, Level, level)
NIX_EXPR_MEMBER_ACCESS(ExprInheritFrom, teInheritFrom, inheritFroms, Displacement, displ)

NIX_EXPR_VAR_MEMBER_ACCESS(ExprVar, teVar, vars, PosIdx, pos)
NIX_EXPR_VAR_MEMBER_ACCESS(ExprVar, teVar, vars, SymbolRef, name)
NIX_EXPR_VAR_MEMBER_ACCESS(ExprVar, teVar, vars, ExprWithRef, fromWith)
NIX_EXPR_VAR_MEMBER_ACCESS(ExprVar, teVar, vars, Level, level)
NIX_EXPR_VAR_MEMBER_ACCESS(ExprVar, teVar, vars, Displacement, displ)
} // namespace nix
