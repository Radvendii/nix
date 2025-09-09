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
Env & EvalState::allocEnv(size_t size)
{
    nrEnvs++;
    nrValuesInEnvs += size;

    Env * env;

#if NIX_USE_BOEHMGC
    if (size == 1) {
        /* see allocValue for explanations. */
        if (!*env1AllocCache) {
            *env1AllocCache = GC_malloc_many(sizeof(Env) + sizeof(ValueRef));
            if (!*env1AllocCache)
                throw std::bad_alloc();
        }

        void * p = *env1AllocCache;
        *env1AllocCache = GC_NEXT(p);
        GC_NEXT(p) = nullptr;
        env = (Env *) p;
    } else
#endif
        env = (Env *) allocBytes(sizeof(Env) + size * sizeof(ValueRef));

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */

    return *env;
}

[[gnu::always_inline]]
void EvalState::forceValue(ValueRef v, const PosIdx pos)
{
    if (v.isThunk(values)) {
        Env * env = v.thunk(values).env;
        assert(env || v.isBlackhole(values));
        Expr * expr = v.thunk(values).expr;
        try {
            v.mkBlackhole(values);
            // checkInterrupt();
            if (env) [[likely]]
                expr->eval(*this, *env, v);
            else
                ExprBlackHole::throwInfiniteRecursionError(*this, v);
        } catch (...) {
            v.mkThunk(values, env, expr);
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
