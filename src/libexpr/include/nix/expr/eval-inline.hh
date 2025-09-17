#pragma once
///@file

#include "nix/expr/print.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-settings.hh"

namespace nix {

[[gnu::always_inline]]
ValueRef EvalState::allocValue()
{
    nrValues++;
    return values.create();
}

[[gnu::always_inline]]
EnvRef EvalState::allocEnv(size_t size)
{
    nrEnvs++;
    nrValuesInEnvs += size;

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */
    return envs.create(size);
}

[[gnu::always_inline]]
void EvalState::forceValue(ValueRef v, const PosIdx pos)
{
    if (v.isThunk(values)) {
        EnvRef env = v.thunk(values).env;
        assert(env || v.isBlackhole(exprs, values));
        ExprRef expr = v.thunk(values).expr;
        try {
            v.mkBlackhole(exprs, values);
            // checkInterrupt();
            if (env) [[likely]]
                expr.eval(*this, env, v);
            else
                throwInfiniteRecursionError(v);
        } catch (...) {
            v.mkThunk(exprs, values, env, expr);
            tryFixupBlackHolePos(v, pos);
            throw;
        }
    } else if (v.isApp(values))
        callFunction(v.app(values).left, v.app(values).right, v, pos);
}

[[gnu::always_inline]]
inline void EvalState::forceAttrs(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    forceAttrs(v, [&]() { return pos; }, errorCtx);
}

template<typename Callable>
[[gnu::always_inline]]
inline void EvalState::forceAttrs(ValueRef v, Callable getPos, std::string_view errorCtx)
{
    PosIdx pos = getPos();
    forceValue(v, pos);
    if (v.type(values) != nAttrs) {
        error<TypeError>("expected a set but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline void EvalState::forceList(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v, pos);
    if (!v.isList(values)) {
        error<TypeError>("expected a list but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline CallDepth EvalState::addCallDepth(const PosIdx pos)
{
    if (callDepth > settings.maxCallDepth)
        error<EvalBaseError>("stack overflow; max-call-depth exceeded").atPos(pos).debugThrow();

    return CallDepth(callDepth);
};

} // namespace nix
