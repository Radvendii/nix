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

    Expr()
    {
        nrExprs++;
    }

    virtual ~Expr() {};
    virtual ValueRef maybeThunk(EvalState & state, EnvRef env);
    virtual void setName(Exprs & exprs, SymbolRef name);
    virtual void setDocComment(Exprs & exprs, DocComment docComment) {};

    virtual PosIdx getPos(Exprs & exprs) const
    {
        return noPos;
    }

    // These are temporary methods to be used only in parser.y
    virtual void resetCursedOr() {};
    virtual void warnIfCursedOr(const SymbolTable & symbols, const PosTable & positions) {};
};

#define COMMON_METHODS                                                                                         \

struct ExprInt : Expr
{
    ValueRef v;

    ExprInt(Values & values, NixInt n);

    ExprInt(Values & values, NixInt::Inner n);

    ValueRef maybeThunk(EvalState & state, EnvRef env) override;
    COMMON_METHODS
};

struct ExprFloat : Expr
{
    ValueRef v;

    ExprFloat(Values & values, NixFloat nf);

    ValueRef maybeThunk(EvalState & state, EnvRef env) override;
    COMMON_METHODS
};

struct ExprString : Expr
{
    std::string s;
    ValueRef v;

    ExprString(Values & values, std::string && s);

    ValueRef maybeThunk(EvalState & state, EnvRef env) override;
    COMMON_METHODS
};

struct ExprPath : Expr
{
    ref<SourceAccessor> accessor;
    std::string s;
    ValueRef v;

    ExprPath(Values & values, ref<SourceAccessor> accessor, std::string && s);

    ValueRef maybeThunk(EvalState & state, EnvRef env) override;
    COMMON_METHODS
};

typedef uint32_t Level;
typedef uint32_t Displacement;

struct ExprVar : Expr
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
    ValueRef maybeThunk(EvalState & state, EnvRef env) override;

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    COMMON_METHODS
};

/**
 * A pseudo-expression for the purpose of evaluating the `from` expression in `inherit (from)` syntax.
 * Unlike normal variable references, the displacement is set during parsing, and always refers to
 * `ExprAttrs::inheritFromExprs` (by itself or in `ExprLet`), whose values are put into their own `Env`.
 */
struct ExprInheritFrom : ExprVar
{
    ExprInheritFrom(PosIdx pos, Displacement displ)
        : ExprVar(pos, {})
    {
        this->level = 0;
        this->displ = displ;
        this->fromWith = ExprWithRef::null;
    }
};

struct ExprSelect : Expr
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

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    /**
     * Evaluate the `a.b.c` part of `a.b.c.d`. This exists mostly for the purpose of :doc in the repl.
     *
     * @param[out] attrs The attribute set that should contain the last attribute name (if it exists).
     * @return The last attribute name in `attrPath`
     *
     * @note This does *not* evaluate the final attribute, and does not fail if that's the only attribute that does not
     * exist.
     */
    SymbolRef evalExceptFinalSelect(EvalState & state, EnvRef env, ValueRef attrs);

    COMMON_METHODS
};

struct ExprOpHasAttr : Expr
{
    ExprRef e;
    AttrPath attrPath;
    ExprOpHasAttr(ExprRef e, AttrPath attrPath)
        : e(e)
        , attrPath(std::move(attrPath)) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return exprs.ERtoEP(e)->getPos(exprs);
    }

    COMMON_METHODS
};

struct ExprAttrs : Expr
{
    bool recursive;
    PosIdx pos;

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

    typedef std::map<SymbolRef, AttrDef> AttrDefs;
    AttrDefs attrs;
    std::unique_ptr<std::vector<ExprRef>> inheritFromExprs;

    struct DynamicAttrDef
    {
        ExprRef nameExpr, valueExpr;
        PosIdx pos;
        DynamicAttrDef(ExprRef nameExpr, ExprRef valueExpr, const PosIdx & pos)
            : nameExpr(nameExpr)
            , valueExpr(valueExpr)
            , pos(pos) {};
    };

    typedef std::vector<DynamicAttrDef> DynamicAttrDefs;
    DynamicAttrDefs dynamicAttrs;
    ExprAttrs(const PosIdx & pos)
        : recursive(false)
        , pos(pos) {};
    ExprAttrs()
        : recursive(false) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    COMMON_METHODS

    std::shared_ptr<const StaticEnv> bindInheritSources(EvalState & es, const std::shared_ptr<const StaticEnv> & env);
    EnvRef buildInheritFromEnv(EvalState & state, EnvRef up);
    void showBindings(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const;
};

struct ExprList : Expr
{
    std::vector<ExprRef> elems;
    ExprList() {};
    COMMON_METHODS
    ValueRef maybeThunk(EvalState & state, EnvRef env) override;

    PosIdx getPos(Exprs & exprs) const override
    {
        return elems.empty() ? noPos : exprs.ERtoEP(elems.front())->getPos(exprs);
    }
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

struct ExprLambda : Expr
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

    void setName(Exprs & exprs, SymbolRef name) override;
    std::string showNamePos(const EvalState & state) const;

    inline bool hasFormals() const
    {
        return formals != nullptr;
    }

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    virtual void setDocComment(Exprs & exprs, DocComment docComment) override;
    COMMON_METHODS
};

struct ExprCall : Expr
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

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    virtual void resetCursedOr() override;
    virtual void warnIfCursedOr(const SymbolTable & symbols, const PosTable & positions) override;
    COMMON_METHODS
};

struct ExprLet : Expr
{
    ExprAttrsRef attrs;
    ExprRef body;
    ExprLet(ExprAttrsRef attrs, ExprRef body)
        : attrs(attrs)
        , body(body) {};
    COMMON_METHODS
};

struct ExprWith : Expr
{
    PosIdx pos;
    ExprRef attrs, body;
    size_t prevWith;
    ExprWithRef parentWith;
    ExprWith(const PosIdx & pos, ExprRef attrs, ExprRef body)
        : pos(pos)
        , attrs(attrs)
        , body(body) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    COMMON_METHODS
};

struct ExprIf : Expr
{
    PosIdx pos;
    ExprRef cond, then, else_;
    ExprIf(const PosIdx & pos, ExprRef cond, ExprRef then, ExprRef else_)
        : pos(pos)
        , cond(cond)
        , then(then)
        , else_(else_) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    COMMON_METHODS
};

struct ExprAssert : Expr
{
    PosIdx pos;
    ExprRef cond, body;
    ExprAssert(const PosIdx & pos, ExprRef cond, ExprRef body)
        : pos(pos)
        , cond(cond)
        , body(body) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    COMMON_METHODS
};

struct ExprOpNot : Expr
{
    ExprRef e;
    ExprOpNot(ExprRef e)
        : e(e) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return exprs.ERtoEP(e)->getPos(exprs);
    }

    COMMON_METHODS
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
struct name : Expr                                                 \
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
    PosIdx getPos(Exprs & exprs) const override                    \
    {                                                              \
        return pos;                                                \
    }                                                              \
};

NIX_FOR_EACH_BINOP(MakeBinOp)

struct ExprConcatStrings : Expr
{
    PosIdx pos;
    bool forceString;
    std::vector<std::pair<PosIdx, ExprRef>> * es;
    ExprConcatStrings(const PosIdx & pos, bool forceString, std::vector<std::pair<PosIdx, ExprRef>> * es)
        : pos(pos)
        , forceString(forceString)
        , es(es) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    COMMON_METHODS
};

struct ExprPos : Expr
{
    PosIdx pos;
    ExprPos(const PosIdx & pos)
        : pos(pos) {};

    PosIdx getPos(Exprs & exprs) const override
    {
        return pos;
    }

    COMMON_METHODS
};

/* only used to mark thunks as black holes. */
struct ExprBlackHole : Expr
{
    [[noreturn]] static void throwInfiniteRecursionError(EvalState & state, ValueRef v);
};

extern ExprBlackHole eBlackHole;

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
#define NIX_DEFINE_ADD(TYPE, DISCRIMINANT, VECTOR)          \
TYPE##Ref Exprs::add##TYPE(auto && ...args) {               \
VECTOR.emplace_back(std::forward<decltype(args)>(args)...); \
if(VECTOR.size() > 999000)                                  \
    std::cout << "we're in trouble " #TYPE "\n";            \
return TYPE##Ref(VECTOR.size() - 1);                        \
}
NIX_FOR_EACH_EXPR(NIX_DEFINE_ADD)
NIX_DEFINE_ADD(ExprVar, teVar, vars)
#undef NIX_DEFINE_ADD
// No addExprBlackHole!
} // namespace nix
