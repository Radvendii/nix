#pragma once
///@file

#include "nix/expr/print.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-settings.hh"

namespace nix {

/**
 * Note: Various places expect the allocated memory to be zeroed.
 */
[[gnu::always_inline]]
inline void * allocBytes(size_t n)
{
    void * p;
#if NIX_USE_BOEHMGC
    p = GC_MALLOC(n);
#else
    p = calloc(n, 1);
#endif
    if (!p)
        throw std::bad_alloc();
    return p;
}

[[gnu::always_inline]]
ValueRef EvalState::allocValue()
{
    nrValues++;
    return VPtoVR(&values.emplace_back());
    // return values.size() - 1;
// #if NIX_USE_BOEHMGC
//     /* We use the boehm batch allocator to speed up allocations of Values (of which there are many).
//        GC_malloc_many returns a linked list of objects of the given size, where the first word
//        of each object is also the pointer to the next object in the list. This also means that we
//        have to explicitly clear the first word of every object we take. */
//     if (!*valueAllocCache) {
//         *valueAllocCache = GC_malloc_many(sizeof(Value));
//         if (!*valueAllocCache)
//             throw std::bad_alloc();
//     }

//     /* GC_NEXT is a convenience macro for accessing the first word of an object.
//        Take the first list item, advance the list to the next item, and clear the next pointer. */
//     void * p = *valueAllocCache;
//     *valueAllocCache = GC_NEXT(p);
//     GC_NEXT(p) = nullptr;
// #else
//     void * p = allocBytes(sizeof(Value));
// #endif

//     nrValues++;
//     return (Value *) p;
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
    if (VRtoV(v).isThunk()) {
        Env * env = VRtoV(v).thunk().env;
        assert(env || VRtoV(v).isBlackhole());
        Expr * expr = VRtoV(v).thunk().expr;
        try {
            VRtoV(v).mkBlackhole();
            // checkInterrupt();
            if (env) [[likely]]
                expr->eval(*this, *env, v);
            else
                ExprBlackHole::throwInfiniteRecursionError(*this, v);
        } catch (...) {
            VRtoV(v).mkThunk(env, expr);
            tryFixupBlackHolePos(v, pos);
            throw;
        }
    } else if (VRtoV(v).isApp())
        callFunction(VRtoV(v).app().left, VRtoV(v).app().right, v, pos);
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
    if (VRtoV(v).type() != nAttrs) {
        error<TypeError>("expected a set but found %1%: %2%", showType(*this, v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline void EvalState::forceList(ValueRef v, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v, pos);
    if (!VRtoV(v).isList()) {
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
