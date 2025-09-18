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

PosIdx ExprRef::getPos(Exprs & exprs) const
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (type()) {
    case teInheritFrom:
    case teVar:
        return payload<teVar>(exprs).pos;
    case teSelect:
        return payload<teSelect>(exprs).pos;
    case teAttrs:
        return payload<teAttrs>(exprs).pos;
    case teLambda:
        return payload<teLambda>(exprs).pos;
    case teCall:
        return payload<teCall>(exprs).pos;
    case teWith:
        return payload<teWith>(exprs).pos;
    case teIf:
        return payload<teIf>(exprs).pos;
    case teAssert:
        return payload<teAssert>(exprs).pos;
    case teOpAnd:
        return payload<teOpAnd>(exprs).pos;
    case teOpOr:
        return payload<teOpOr>(exprs).pos;
    case teOpEq:
        return payload<teOpEq>(exprs).pos;
    case teOpNEq:
        return payload<teOpNEq>(exprs).pos;
    case teOpImpl:
        return payload<teOpImpl>(exprs).pos;
    case teConcatStrings:
        return payload<teConcatStrings>(exprs).pos;
    case tePos:
        return payload<tePos>(exprs).pos;
    case teOpHasAttr:
        return payload<teOpHasAttr>(exprs).e.getPos(exprs);
    case teOpNot:
        return payload<teOpNot>(exprs).e.getPos(exprs);
    case teList:
        return payload<teList>(exprs).elems.empty() ? noPos : payload<teList>(exprs).elems.front().getPos(exprs);
    default:
        return noPos;
    }
#pragma GCC diagnostic pop
};

Exprs::Exprs() {
#define NIX_EXPR_RESERVE(DISCR) \
payloads<DISCR>().reserve(1000000);
    NIX_FOR_EACH_EXPR(NIX_EXPR_RESERVE)
#undef NIX_EXPR_RESERVE
}
template<>
ExprRefOf<teCall> Exprs::add<teCall>(const PosIdx & pos, ExprRef fun, std::vector<ExprRef> && args)
{
    payloads<teCall>().emplace_back(pos, fun, std::move(args));
    if(payloads<teCall>().size() > 999000)
        std::cout << "we're in trouble ExprCall\n";
    Expr::nrExprs++;
    return ExprRefOf<teCall>(payloads<teCall>().size() - 1);
}
template<>
ExprRefOf<teCall> Exprs::add<teCall>(const PosIdx & pos, ExprRef fun, std::vector<ExprRef> && args, PosIdx && cursedOrEndPos)
{
    payloads<teCall>().emplace_back(pos, fun, std::move(args), std::move(cursedOrEndPos));
    if(payloads<teCall>().size() > 999000)
        std::cout << "we're in trouble ExprCall\n";
    Expr::nrExprs++;
    return ExprRefOf<teCall>(payloads<teCall>().size() - 1);
}

// FIXME: remove, because *symbols* are abstract and do not have a single
//        textual representation; see printIdentifier()
std::ostream & operator<<(std::ostream & str, const Symbol & symbol)
{
    std::string_view s = symbol;
    return printIdentifier(str, s);
}

#define NIX_BINOP_SHOW(TYPE, DISCR, STRING)                \
template<>                                                 \
void ExprRefOf<DISCR>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const \
{                                                          \
    str << "(";                                            \
    payload(exprs).e1.show(exprs, values, symbols, str);   \
    str << " " STRING " ";                                 \
    payload(exprs).e2.show(exprs, values, symbols, str);   \
    str << ")";                                            \
}

NIX_FOR_EACH_BINOP(NIX_BINOP_SHOW)
#undef NIX_BINOP_SHOW

template<>
void ExprRefOf<teInt>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << payload(exprs).v.integer(values);
}

template<>
void ExprRefOf<teFloat>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << payload(exprs).v.fpoint(values);
}

template<>
void ExprRefOf<teString>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    printLiteralString(str, payload(exprs).s);
}

template<>
void ExprRefOf<tePath>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << payload(exprs).s;
}

template<>
void ExprRefOf<teVar>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << symbols[payload(exprs).name];
}

template<>
void ExprRefOf<teSelect>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(";
    payload(exprs).e.show(exprs, values, symbols, str);
    str << ")." << showAttrPath(exprs, values, symbols, payload(exprs).attrPath);
    if (payload(exprs).def) {
        str << " or (";
        payload(exprs).def.show(exprs, values, symbols, str);
        str << ")";
    }
}

template<>
void ExprRefOf<teOpHasAttr>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "((";
    payload(exprs).e.show(exprs, values, symbols, str);
    str << ") ? " << showAttrPath(exprs, values, symbols, payload(exprs).attrPath) << ")";
}

template<>
void ExprRefOf<teAttrs>::showBindings(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    typedef const AttrDefs::value_type * Attr;
    std::vector<Attr> sorted;
    for (auto & i : payload(exprs).attrs)
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
            auto select = i->second.e.dyn_cast<teSelect>();
            auto from = select.payload(exprs).e.dyn_cast<teInheritFrom>();
            inheritsFrom[from.payload(exprs).displ].push_back(i->first);
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
        (*payload(exprs).inheritFromExprs)[from].show(exprs, values, symbols, str);
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
    for (auto & i : payload(exprs).dynamicAttrs) {
        str << "\"${";
        i.nameExpr.show(exprs, values, symbols, str);
        str << "}\" = ";
        i.valueExpr.show(exprs, values, symbols, str);
        str << "; ";
    }
}

template<>
void ExprRefOf<teAttrs>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    if (payload(exprs).recursive)
        str << "rec ";
    str << "{ ";
    showBindings(exprs, values, symbols, str);
    str << "}";
}

template<>
void ExprRefOf<teList>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "[ ";
    for (auto & i : payload(exprs).elems) {
        str << "(";
        i.show(exprs, values, symbols, str);
        str << ") ";
    }
    str << "]";
}

template<>
void ExprRefOf<teLambda>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(";
    if (hasFormals(exprs)) {
        str << "{ ";
        bool first = true;
        // the natural Symbol ordering is by creation time, which can lead to the
        // same expression being printed in two different ways depending on its
        // context. always use lexicographic ordering to avoid this.
        for (auto & i : payload(exprs).formals->lexicographicOrder(symbols)) {
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
        if (payload(exprs).formals->ellipsis) {
            if (!first)
                str << ", ";
            str << "...";
        }
        str << " }";
        if (payload(exprs).arg)
            str << " @ ";
    }
    if (payload(exprs).arg)
        str << symbols[payload(exprs).arg];
    str << ": ";
    payload(exprs).body.show(exprs, values, symbols, str);
    str << ")";
}

template<>
void ExprRefOf<teCall>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << '(';
    payload(exprs).fun.show(exprs, values, symbols, str);
    for (auto e : payload(exprs).args) {
        str << ' ';
        e.show(exprs, values, symbols, str);
    }
    str << ')';
}

template<>
void ExprRefOf<teLet>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(let ";
    payload(exprs).attrs.showBindings(exprs, values, symbols, str);
    str << "in ";
    payload(exprs).body.show(exprs, values, symbols, str);
    str << ")";
}

template<>
void ExprRefOf<teWith>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(with ";
    payload(exprs).attrs.show(exprs, values, symbols, str);
    str << "; ";
    payload(exprs).body.show(exprs, values, symbols, str);
    str << ")";
}

template<>
void ExprRefOf<teIf>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(if ";
    payload(exprs).cond.show(exprs, values, symbols, str);
    str << " then ";
    payload(exprs).then.show(exprs, values, symbols, str);
    str << " else ";
    payload(exprs).else_.show(exprs, values, symbols, str);
    str << ")";
}

template<>
void ExprRefOf<teAssert>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "assert ";
    payload(exprs).cond.show(exprs, values, symbols, str);
    str << "; ";
    payload(exprs).body.show(exprs, values, symbols, str);
}

template<>
void ExprRefOf<teOpNot>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    str << "(! ";
    payload(exprs).e.show(exprs, values, symbols, str);
    str << ")";
}

template<>
void ExprRefOf<teConcatStrings>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    bool first = true;
    str << "(";
    for (auto & i : *payload(exprs).es) {
        if (first)
            first = false;
        else
            str << " + ";
        i.second.show(exprs, values, symbols, str);
    }
    str << ")";
}

template<>
void ExprRefOf<tePos>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
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

template<>
void ExprRefOf<teInt>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

template<>
void ExprRefOf<teFloat>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

template<>
void ExprRefOf<teString>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

template<>
void ExprRefOf<tePath>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

template<>
void ExprRefOf<teVar>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).fromWith = ExprRefOf<teWith>::null;

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
            auto i = curEnv->find(payload(es.exprs).name);
            if (i != curEnv->vars.end()) {
                // XXX [speed]: why do we store a level, that we have to trace back up to later, and not store the EnvRef directly? Because the Env hasn't been populated yet, perhaps? We're working at the AST level right now, not the Value level.
                payload(es.exprs).level = level;
                payload(es.exprs).displ = i->second;
                return;
            }
        }
    }

    /* Otherwise, the variable must be obtained from the nearest
       enclosing `with'.  If there is no `with', then we can issue an
       "undefined variable" error now. */
    if (withLevel == -1)
        es.error<UndefinedVarError>("undefined variable '%1%'", es.symbols[payload(es.exprs).name]).atPos(payload(es.exprs).pos).debugThrow();
    for (auto * e = env.get(); e && !payload(es.exprs).fromWith; e = e->up.get())
        payload(es.exprs).fromWith = e->isWith;
    payload(es.exprs).level = withLevel;
}

template<>
void ExprRefOf<teInheritFrom>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

template<>
void ExprRefOf<teSelect>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).e.bindVars(es, env);
    if (payload(es.exprs).def)
        payload(es.exprs).def.bindVars(es, env);
    for (auto & i : payload(es.exprs).attrPath)
        if (!i.symbol)
            i.expr.bindVars(es, env);
}

template<>
void ExprRefOf<teOpHasAttr>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).e.bindVars(es, env);
    for (auto & i : payload(es.exprs).attrPath)
        if (!i.symbol)
            i.expr.bindVars(es, env);
}

template<>
std::shared_ptr<const StaticEnv>
ExprRefOf<teAttrs>::bindInheritSources(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (!payload(es.exprs).inheritFromExprs)
        return nullptr;

    // the inherit (from) source values are inserted into an env of its own, which
    // does not introduce any variable names.
    // analysis must see an empty env, or an env that contains only entries with
    // otherwise unused names to not interfere with regular names. the parser
    // has already filled all exprs that access this env with appropriate level
    // and displacement, and nothing else is allowed to access it. ideally we'd
    // not even *have* an expr that grabs anything from this env since it's fully
    // invisible, but the evaluator does not allow for this yet.
    auto inner = std::make_shared<StaticEnv>(ExprRefOf<teWith>::null, env, 0);
    for (auto from : *payload(es.exprs).inheritFromExprs)
        from.bindVars(es, env);

    return inner;
}

template<>
void ExprRefOf<teAttrs>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    if (payload(es.exprs).recursive) {
        auto newEnv = [&]() -> std::shared_ptr<const StaticEnv> {
            auto newEnv = std::make_shared<StaticEnv>(ExprRefOf<teWith>::null, env, payload(es.exprs).attrs.size());

            Displacement displ = 0;
            for (auto & i : payload(es.exprs).attrs)
                newEnv->vars.emplace_back(i.first, i.second.displ = displ++);
            return newEnv;
        }();

        // No need to sort newEnv since attrs is in sorted order.

        auto inheritFromEnv = bindInheritSources(es, newEnv);
        for (auto & i : payload(es.exprs).attrs)
            i.second.e.bindVars(es, i.second.chooseByKind(newEnv, env, inheritFromEnv));

        for (auto & i : payload(es.exprs).dynamicAttrs) {
            i.nameExpr.bindVars(es, newEnv);
            i.valueExpr.bindVars(es, newEnv);
        }
    } else {
        auto inheritFromEnv = bindInheritSources(es, env);

        for (auto & i : payload(es.exprs).attrs)
            i.second.e.bindVars(es, i.second.chooseByKind(env, env, inheritFromEnv));

        for (auto & i : payload(es.exprs).dynamicAttrs) {
            i.nameExpr.bindVars(es, env);
            i.valueExpr.bindVars(es, env);
        }
    }
}

template<>
void ExprRefOf<teList>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    for (auto & i : payload(es.exprs).elems)
        i.bindVars(es, env);
}

template<>
void ExprRefOf<teLambda>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    auto newEnv =
        std::make_shared<StaticEnv>(ExprRefOf<teWith>::null, env, (hasFormals(es.exprs) ? payload(es.exprs).formals->formals.size() : 0) + (!payload(es.exprs).arg ? 0 : 1));

    Displacement displ = 0;

    if (payload(es.exprs).arg)
        newEnv->vars.emplace_back(payload(es.exprs).arg, displ++);

    if (hasFormals(es.exprs)) {
        for (auto & i : payload(es.exprs).formals->formals)
            newEnv->vars.emplace_back(i.name, displ++);

        newEnv->sort();

        for (auto & i : payload(es.exprs).formals->formals)
            if (i.def)
                i.def.bindVars(es, newEnv);
    }

    payload(es.exprs).body.bindVars(es, newEnv);
}

template<>
void ExprRefOf<teCall>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).fun.bindVars(es, env);
    for (auto e : payload(es.exprs).args)
        e.bindVars(es, env);
}

template<>
void ExprRefOf<teLet>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    auto newEnv = [&]() -> std::shared_ptr<const StaticEnv> {
        auto newEnv = std::make_shared<StaticEnv>(ExprRefOf<teWith>::null, env, payload(es.exprs).attrs.payload(es.exprs).attrs.size());

        Displacement displ = 0;
        for (auto & i : payload(es.exprs).attrs.payload(es.exprs).attrs)
            newEnv->vars.emplace_back(i.first, i.second.displ = displ++);
        return newEnv;
    }();

    // No need to sort newEnv since attrs->attrs is in sorted order.

    auto inheritFromEnv = payload(es.exprs).attrs.bindInheritSources(es, newEnv);
    for (auto & i : payload(es.exprs).attrs.payload(es.exprs).attrs)
        i.second.e.bindVars(es, i.second.chooseByKind(newEnv, env, inheritFromEnv));

    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, newEnv));

    payload(es.exprs).body.bindVars(es, newEnv);
}

template<>
void ExprRefOf<teWith>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).parentWith = ExprRefOf<teWith>::null;
    for (auto * e = env.get(); e && !payload(es.exprs).parentWith; e = e->up.get())
        payload(es.exprs).parentWith = e->isWith;

    /* Does this `with' have an enclosing `with'?  If so, record its
       level so that `lookupVar' can look up variables in the previous
       `with' if this one doesn't contain the desired attribute. */
    const StaticEnv * curEnv;
    Level level;
    payload(es.exprs).prevWith = 0;
    for (curEnv = env.get(), level = 1; curEnv; curEnv = curEnv->up.get(), level++)
        if (curEnv->isWith) {
            payload(es.exprs).prevWith = level;
            break;
        }

    payload(es.exprs).attrs.bindVars(es, env);
    auto newEnv = std::make_shared<StaticEnv>(*this, env);
    payload(es.exprs).body.bindVars(es, newEnv);
}

template<>
void ExprRefOf<teIf>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).cond.bindVars(es, env);
    payload(es.exprs).then.bindVars(es, env);
    payload(es.exprs).else_.bindVars(es, env);
}

template<>
void ExprRefOf<teAssert>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).cond.bindVars(es, env);
    payload(es.exprs).body.bindVars(es, env);
}

template<>
void ExprRefOf<teOpNot>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    payload(es.exprs).e.bindVars(es, env);
}

template<>
void ExprRefOf<teConcatStrings>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));

    for (auto & i : *payload(es.exprs).es)
        i.second.bindVars(es, env);
}

template<>
void ExprRefOf<tePos>::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    if (es.debugRepl)
        es.exprEnvs.insert(std::make_pair(*this, env));
}

/* Storing function names. */

template<>
void ExprRefOf<teLambda>::setName(Exprs & exprs, SymbolRef name)
{
    payload(exprs).name = name;
    payload(exprs).body.setName(exprs, name);
}

template<>
void ExprRefOf<teLambda>::setDocComment(Exprs & exprs, DocComment docComment)
{
    // RFC 145 specifies that the innermost doc comment wins.
    // See https://github.com/NixOS/rfcs/blob/master/rfcs/0145-doc-strings.md#ambiguous-placement
    if (!payload(exprs).docComment) {
        payload(exprs).docComment = docComment;

        // Curried functions are defined by putting a function directly
        // in the body of another function. To render docs for those, we
        // need to propagate the doc comment to the innermost function.
        //
        // If we have our own comment, we've already propagated it, so this
        // belongs in the same conditional.
        payload(exprs).body.setDocComment(exprs, docComment);
    }
};

void ExprRef::setName(Exprs & exprs, SymbolRef name) {
    if (type() == teLambda) {
        dyn_cast<teLambda>().setName(exprs, name);
    }
}
void ExprRef::setDocComment(Exprs & exprs, DocComment docComment) {
    if (type() == teLambda) {
        dyn_cast<teLambda>().setDocComment(exprs, docComment);
    }
}

template<>
std::string ExprRefOf<teLambda>::showNamePos(EvalState & state)
{
    std::string id(payload(state.exprs).name ? concatStrings("'", state.symbols[payload(state.exprs).name], "'") : "anonymous function");
    return fmt("%1% at %2%", id, state.positions[payload(state.exprs).pos]);
}

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

template<>
void ExprRefOf<teCall>::resetCursedOr(Exprs & exprs)
{
    payload(exprs).cursedOrEndPos.reset();
}

template<>
void ExprRefOf<teCall>::warnIfCursedOr(Exprs & exprs, const SymbolTable & symbols, const PosTable & positions)
{
    if (payload(exprs).cursedOrEndPos.has_value()) {
        std::ostringstream out;
        out << "at " << positions[payload(exprs).pos]
            << ": "
               "This expression uses `or` as an identifier in a way that will change in a future Nix release.\n"
               "Wrap this entire expression in parentheses to preserve its current meaning:\n"
               "    ("
            << positions[payload(exprs).pos].getSnippetUpTo(positions[*payload(exprs).cursedOrEndPos]).value_or("could not read expression")
            << ")\n"
               "Give feedback at https://github.com/NixOS/nix/pull/11121";
        warn(out.str());
    }
}

 void ExprRef::resetCursedOr(Exprs & exprs)
 {
     if (type() == teCall)
         dyn_cast<teCall>().resetCursedOr(exprs);
 }
void ExprRef::warnIfCursedOr(Exprs & exprs, const SymbolTable & symbols, const PosTable & positions)
 {
     if (type() == teCall)
         dyn_cast<teCall>().warnIfCursedOr(exprs, symbols, positions);
 }

template<>
void ExprRefOf<teInheritFrom>::eval(EvalState & state, EnvRef env, ValueRef v)
{
    ExprRefOf<teVar>(*this).eval(state, env, v);
}

template<>
void ExprRefOf<teInheritFrom>::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    ExprRefOf<teVar>(*this).show(exprs, values, symbols, str);
}

#define DYNAMIC_DISPATCH_CASE(DISCR, FUN)     \
case DISCR:                                   \
    return dyn_cast<DISCR>().FUN;
#define DYNAMIC_DISPATCH(FUN)                 \
switch(type()) {                              \
NIX_FOR_EACH_EXPR(DYNAMIC_DISPATCH_CASE, FUN) \
}

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
    if (*this == ExprRef::null || *this == ExprRef::blackHole)
        unreachable();
    DYNAMIC_DISPATCH(bindVars(es, env))
}

void ExprRef::show(Exprs & exprs, Values & values, const SymbolTable & symbols, std::ostream & str) const
{
    if (*this == ExprRef::null || *this == ExprRef::blackHole)
        unreachable();
    DYNAMIC_DISPATCH(show(exprs, values, symbols, str))
}

} // namespace nix
