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

void ExprRef::eval(EvalState & state, EnvRef env, ValueRef v)
{
    if (*this == ExprRef::null)
        unreachable();
    if (*this == ExprRef::blackHole)
        state.throwInfiniteRecursionError(v);
    DYNAMIC_DISPATCH(eval(state, env, v))
}

void ExprRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (*this == ExprRef::null)
        unreachable();
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
        return ExprVarRef(*this).pos(exprs);
    case teSelect:
        return ExprSelectRef(*this).pos(exprs);
    case teAttrs:
        return ExprAttrsRef(*this).pos(exprs);
    case teLambda:
        return ExprLambdaRef(*this).pos(exprs);
    case teCall:
        return ExprCallRef(*this).pos(exprs);
    case teWith:
        return ExprWithRef(*this).pos(exprs);
    case teIf:
        return ExprIfRef(*this).pos(exprs);
    case teAssert:
        return ExprAssertRef(*this).pos(exprs);
    case teOpAnd:
        return ExprOpAndRef(*this).pos(exprs);
    case teOpOr:
        return ExprOpOrRef(*this).pos(exprs);
    case teOpEq:
        return ExprOpEqRef(*this).pos(exprs);
    case teOpNEq:
        return ExprOpNEqRef(*this).pos(exprs);
    case teOpImpl:
        return ExprOpImplRef(*this).pos(exprs);
    case teConcatStrings:
        return ExprConcatStringsRef(*this).pos(exprs);
    case tePos:
        return ExprPosRef(*this).pos(exprs);
    case teOpHasAttr:
        return ExprOpHasAttrRef(*this).e(exprs).getPos(exprs);
    case teOpNot:
        return ExprOpNotRef(*this).e(exprs).getPos(exprs);
    case teList:
        return ExprListRef(*this).elems(exprs).empty() ? noPos : ExprListRef(*this).elems(exprs).front().getPos(exprs);
    default:
        return noPos;
    }
#pragma GCC diagnostic pop
};

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
    Expr::nrExprs++;
    return ExprCallRef(calls.size() - 1);
}
ExprCallRef Exprs::addExprCall(const PosIdx & pos, ExprRef fun, std::vector<ExprRef> && args, PosIdx && cursedOrEndPos)
{
    calls.emplace_back(pos, fun, std::move(args), std::move(cursedOrEndPos));
    if(calls.size() > 999000)
        std::cout << "we're in trouble ExprCall\n";
    Expr::nrExprs++;
    return ExprCallRef(calls.size() - 1);
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
    e1(exprs).show(exprs, values, symbols, str);                                                            \
    str << " " STRING " ";                                                                                  \
    e2(exprs).show(exprs, values, symbols, str);                                                            \
    str << ")";                                                                                             \
}

NIX_FOR_EACH_BINOP(NIX_BINOP_SHOW)
#undef NIX_BINOP_SHOW

void ExprIntRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << v(exprs).integer(values);
}

void ExprFloatRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << v(exprs).fpoint(values);
}

void ExprStringRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    printLiteralString(str, s(exprs));
}

void ExprPathRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << s(exprs);
}

void ExprVarRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << symbols[name(exprs)];
}

void ExprSelectRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(";
    e(exprs).show(exprs, values, symbols, str);
    str << ")." << showAttrPath(exprs, values, symbols, attrPath(exprs));
    if (def(exprs)) {
        str << " or (";
        def(exprs).show(exprs, values, symbols, str);
        str << ")";
    }
}

void ExprOpHasAttrRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "((";
    e(exprs).show(exprs, values, symbols, str);
    str << ") ? " << showAttrPath(exprs, values, symbols, attrPath(exprs)) << ")";
}

void ExprAttrsRef::showBindings(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    typedef const AttrDefs::value_type * Attr;
    std::vector<Attr> sorted;
    for (auto & i : attrs(exprs))
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
        case AttrDef::Kind::Plain:
            break;
        case AttrDef::Kind::Inherited:
            inherits.push_back(i->first);
            break;
        case AttrDef::Kind::InheritedFrom: {
            auto select = i->second.e.dyn_cast<ExprSelectRef>();
            auto from = select.e(exprs).dyn_cast<ExprInheritFromRef>();
            inheritsFrom[from.displ(exprs)].push_back(i->first);
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
        (*inheritFromExprs(exprs))[from].show(exprs, values, symbols, str);
        str << ")";
        for (auto sym : syms)
            str << " " << symbols[sym];
        str << "; ";
    }
    for (auto & i : sorted) {
        if (i->second.kind == AttrDef::Kind::Plain) {
            str << symbols[i->first] << " = ";
            i->second.e.show(exprs, values, symbols, str);
            str << "; ";
        }
    }
    for (auto & i : dynamicAttrs(exprs)) {
        str << "\"${";
        i.nameExpr.show(exprs, values, symbols, str);
        str << "}\" = ";
        i.valueExpr.show(exprs, values, symbols, str);
        str << "; ";
    }
}

void ExprAttrsRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    if (recursive(exprs))
        str << "rec ";
    str << "{ ";
    showBindings(exprs, values, symbols, str);
    str << "}";
}

void ExprListRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "[ ";
    for (auto & i : elems(exprs)) {
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
        for (auto & i : formals(exprs)->lexicographicOrder(symbols)) {
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
        if (formals(exprs)->ellipsis) {
            if (!first)
                str << ", ";
            str << "...";
        }
        str << " }";
        if (arg(exprs))
            str << " @ ";
    }
    if (arg(exprs))
        str << symbols[arg(exprs)];
    str << ": ";
    body(exprs).show(exprs, values, symbols, str);
    str << ")";
}

void ExprCallRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << '(';
    fun(exprs).show(exprs, values, symbols, str);
    for (auto e : args(exprs)) {
        str << ' ';
        e.show(exprs, values, symbols, str);
    }
    str << ')';
}

void ExprLetRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(let ";
    attrs(exprs).showBindings(exprs, values, symbols, str);
    str << "in ";
    body(exprs).show(exprs, values, symbols, str);
    str << ")";
}

void ExprWithRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(with ";
    attrs(exprs).show(exprs, values, symbols, str);
    str << "; ";
    body(exprs).show(exprs, values, symbols, str);
    str << ")";
}

void ExprIfRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(if ";
    cond(exprs).show(exprs, values, symbols, str);
    str << " then ";
    then(exprs).show(exprs, values, symbols, str);
    str << " else ";
    else_(exprs).show(exprs, values, symbols, str);
    str << ")";
}

void ExprAssertRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "assert ";
    cond(exprs).show(exprs, values, symbols, str);
    str << "; ";
    body(exprs).show(exprs, values, symbols, str);
}

void ExprOpNotRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(! ";
    e(exprs).show(exprs, values, symbols, str);
    str << ")";
}

void ExprConcatStringsRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    bool first = true;
    str << "(";
    for (auto & i : *es(exprs)) {
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

    fromWith(es.exprs) = ExprWithRef::null;

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
            auto i = curEnv->find(name(es.exprs));
            if (i != curEnv->vars.end()) {
                // XXX [speed]: why do we store a level, that we have to trace back up to later, and not store the EnvRef directly? Because the Env hasn't been populated yet, perhaps? We're working at the AST level right now, not the Value level.
                this->level(es.exprs) = level;
                this->displ(es.exprs) = i->second;
                return;
            }
        }
    }

    /* Otherwise, the variable must be obtained from the nearest
       enclosing `with'.  If there is no `with', then we can issue an
       "undefined variable" error now. */
    if (withLevel == -1)
        es.error<UndefinedVarError>("undefined variable '%1%'", es.symbols[name(es.exprs)]).atPos(pos(es.exprs)).debugThrow();
    for (auto * e = env.get(); e && !fromWith(es.exprs); e = e->up.get())
        fromWith(es.exprs) = e->isWith;
    this->level(es.exprs) = withLevel;
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

    e(es.exprs).bindVars(es, env);
    if (def(es.exprs))
        def(es.exprs).bindVars(es, env);
    for (auto & i : attrPath(es.exprs))
        if (!i.symbol)
            i.expr.bindVars(es, env);
}

void ExprOpHasAttrRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    e(es.exprs).bindVars(es, env);
    for (auto & i : attrPath(es.exprs))
        if (!i.symbol)
            i.expr.bindVars(es, env);
}

std::shared_ptr<const StaticEnv>
ExprAttrsRef::bindInheritSources(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (!inheritFromExprs(es.exprs))
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
    for (auto from : *inheritFromExprs(es.exprs))
        from.bindVars(es, env);

    return inner;
}

void ExprAttrsRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    if (recursive(es.exprs)) {
        auto newEnv = [&]() -> std::shared_ptr<const StaticEnv> {
            auto newEnv = std::make_shared<StaticEnv>(ExprWithRef::null, env, attrs(es.exprs).size());

            Displacement displ = 0;
            for (auto & i : attrs(es.exprs))
                newEnv->vars.emplace_back(i.first, i.second.displ = displ++);
            return newEnv;
        }();

        // No need to sort newEnv since attrs is in sorted order.

        auto inheritFromEnv = bindInheritSources(es, newEnv);
        for (auto & i : attrs(es.exprs))
            i.second.e.bindVars(es, i.second.chooseByKind(newEnv, env, inheritFromEnv));

        for (auto & i : dynamicAttrs(es.exprs)) {
            i.nameExpr.bindVars(es, newEnv);
            i.valueExpr.bindVars(es, newEnv);
        }
    } else {
        auto inheritFromEnv = bindInheritSources(es, env);

        for (auto & i : attrs(es.exprs))
            i.second.e.bindVars(es, i.second.chooseByKind(env, env, inheritFromEnv));

        for (auto & i : dynamicAttrs(es.exprs)) {
            i.nameExpr.bindVars(es, env);
            i.valueExpr.bindVars(es, env);
        }
    }
}

void ExprListRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    for (auto & i : elems(es.exprs))
        i.bindVars(es, env);
}

void ExprLambdaRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    auto newEnv =
        std::make_shared<StaticEnv>(ExprWithRef::null, env, (hasFormals(es.exprs) ? formals(es.exprs)->formals.size() : 0) + (!arg(es.exprs) ? 0 : 1));

    Displacement displ = 0;

    if (arg(es.exprs))
        newEnv->vars.emplace_back(arg(es.exprs), displ++);

    if (hasFormals(es.exprs)) {
        for (auto & i : formals(es.exprs)->formals)
            newEnv->vars.emplace_back(i.name, displ++);

        newEnv->sort();

        for (auto & i : formals(es.exprs)->formals)
            if (i.def)
                i.def.bindVars(es, newEnv);
    }

    body(es.exprs).bindVars(es, newEnv);
}

void ExprCallRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    fun(es.exprs).bindVars(es, env);
    for (auto e : args(es.exprs))
        e.bindVars(es, env);
}

void ExprLetRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    auto newEnv = [&]() -> std::shared_ptr<const StaticEnv> {
        auto newEnv = std::make_shared<StaticEnv>(ExprWithRef::null, env, attrs(es.exprs).attrs(es.exprs).size());

        Displacement displ = 0;
        for (auto & i : attrs(es.exprs).attrs(es.exprs))
            newEnv->vars.emplace_back(i.first, i.second.displ = displ++);
        return newEnv;
    }();

    // No need to sort newEnv since attrs->attrs is in sorted order.

    auto inheritFromEnv = attrs(es.exprs).bindInheritSources(es, newEnv);
    for (auto & i : attrs(es.exprs).attrs(es.exprs))
        i.second.e.bindVars(es, i.second.chooseByKind(newEnv, env, inheritFromEnv));

    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, newEnv));

    body(es.exprs).bindVars(es, newEnv);
}

void ExprWithRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    parentWith(es.exprs) = ExprWithRef::null;
    for (auto * e = env.get(); e && !parentWith(es.exprs); e = e->up.get())
        parentWith(es.exprs) = e->isWith;

    /* Does this `with' have an enclosing `with'?  If so, record its
       level so that `lookupVar' can look up variables in the previous
       `with' if this one doesn't contain the desired attribute. */
    const StaticEnv * curEnv;
    Level level;
    prevWith(es.exprs) = 0;
    for (curEnv = env.get(), level = 1; curEnv; curEnv = curEnv->up.get(), level++)
        if (curEnv->isWith) {
            prevWith(es.exprs) = level;
            break;
        }

    attrs(es.exprs).bindVars(es, env);
    auto newEnv = std::make_shared<StaticEnv>(*this, env);
    body(es.exprs).bindVars(es, newEnv);
}

void ExprIfRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    cond(es.exprs).bindVars(es, env);
    then(es.exprs).bindVars(es, env);
    else_(es.exprs).bindVars(es, env);
}

void ExprAssertRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    cond(es.exprs).bindVars(es, env);
    body(es.exprs).bindVars(es, env);
}

void ExprOpNotRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    e(es.exprs).bindVars(es, env);
}

void ExprConcatStringsRef::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    for (auto & i : *this->es(es.exprs))
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
    this->name(exprs) = name;
    this->body(exprs).setName(exprs, name);
}

std::string ExprLambdaRef::showNamePos(EvalState & state)
{
    std::string id(this->name(state.exprs) ? concatStrings("'", state.symbols[this->name(state.exprs)], "'") : "anonymous function");
    return fmt("%1% at %2%", id, state.positions[this->pos(state.exprs)]);
}

void ExprLambdaRef::setDocComment(Exprs & exprs, DocComment docComment)
{
    // RFC 145 specifies that the innermost doc comment wins.
    // See https://github.com/NixOS/rfcs/blob/master/rfcs/0145-doc-strings.md#ambiguous-placement
    if (!this->docComment(exprs)) {
        this->docComment(exprs) = docComment;

        // Curried functions are defined by putting a function directly
        // in the body of another function. To render docs for those, we
        // need to propagate the doc comment to the innermost function.
        //
        // If we have our own comment, we've already propagated it, so this
        // belongs in the same conditional.
        body(exprs).setDocComment(exprs, docComment);
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
    cursedOrEndPos(exprs).reset();
}

void ExprCallRef::warnIfCursedOr(Exprs & exprs, const SymbolTable & symbols, const PosTable & positions)
{
    if (cursedOrEndPos(exprs).has_value()) {
        std::ostringstream out;
        out << "at " << positions[pos(exprs)]
            << ": "
               "This expression uses `or` as an identifier in a way that will change in a future Nix release.\n"
               "Wrap this entire expression in parentheses to preserve its current meaning:\n"
               "    ("
            << positions[pos(exprs)].getSnippetUpTo(positions[*cursedOrEndPos(exprs)]).value_or("could not read expression")
            << ")\n"
               "Give feedback at https://github.com/NixOS/nix/pull/11121";
        warn(out.str());
    }
}

} // namespace nix
