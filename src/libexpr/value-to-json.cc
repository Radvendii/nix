#include "nix/expr/value-to-json.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/util/signals.hh"

#include <cstdlib>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace nix {
using json = nlohmann::json;

// TODO: rename. It doesn't print.
json printValueAsJSON(
    EvalState & state, bool strict, ValueRef v, const PosIdx pos, NixStringContext & context, bool copyToStore)
{
    checkInterrupt();

    if (strict)
        state.forceValue(v, pos);

    json out;

    switch (state.VRtoV(v).type()) {

    case nInt:
        out = state.VRtoV(v).integer().value;
        break;

    case nBool:
        out = state.VRtoV(v).boolean();
        break;

    case nString:
        copyContext(state, v, context);
        out = state.VRtoV(v).c_str();
        break;

    case nPath:
        if (copyToStore)
            out = state.store->printStorePath(state.copyPathToStore(context, state.VRtoV(v).path()));
        else
            out = state.VRtoV(v).path().path.abs();
        break;

    case nNull:
        // already initialized as null
        break;

    case nAttrs: {
        auto maybeString = state.tryAttrsToString(pos, v, context, false, false);
        if (maybeString) {
            out = *maybeString;
            break;
        }
        if (auto i = state.VRtoV(v).attrs()->get(state.sOutPath))
            return printValueAsJSON(state, strict, i->value, i->pos, context, copyToStore);
        else {
            out = json::object();
            for (auto & a : state.VRtoV(v).attrs()->lexicographicOrder(state.symbols)) {
                try {
                    out.emplace(
                        state.symbols[a->name],
                        printValueAsJSON(state, strict, a->value, a->pos, context, copyToStore));
                } catch (Error & e) {
                    e.addTrace(
                        state.positions[a->pos], HintFmt("while evaluating attribute '%1%'", state.symbols[a->name]));
                    throw;
                }
            }
        }
        break;
    }

    case nList: {
        out = json::array();
        int i = 0;
        for (auto elem : state.VRtoV(v).listView()) {
            try {
                out.push_back(printValueAsJSON(state, strict, elem, pos, context, copyToStore));
            } catch (Error & e) {
                e.addTrace(state.positions[pos], HintFmt("while evaluating list element at index %1%", i));
                throw;
            }
            i++;
        }
        break;
    }

    case nExternal:
        return state.VRtoV(v).external()->printValueAsJSON(state, strict, context, copyToStore);
        break;

    case nFloat:
        out = state.VRtoV(v).fpoint();
        break;

    case nThunk:
    case nFunction:
        state.error<TypeError>("cannot convert %1% to JSON", showType(state, v)).atPos(state.VRtoV(v).determinePos(state, pos)).debugThrow();
    }
    return out;
}

void printValueAsJSON(
    EvalState & state,
    bool strict,
    ValueRef v,
    const PosIdx pos,
    std::ostream & str,
    NixStringContext & context,
    bool copyToStore)
{
    try {
        str << printValueAsJSON(state, strict, v, pos, context, copyToStore);
    } catch (nlohmann::json::exception & e) {
        throw JSONSerializationError("JSON serialization error: %s", e.what());
    }
}

json ExternalValueBase::printValueAsJSON(
    EvalState & state, bool strict, NixStringContext & context, bool copyToStore) const
{
    state.error<TypeError>("cannot convert %1% to JSON", showType()).debugThrow();
}

} // namespace nix
