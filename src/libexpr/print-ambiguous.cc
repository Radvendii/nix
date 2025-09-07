#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/print.hh"
#include "nix/util/signals.hh"
#include "nix/expr/eval.hh"

namespace nix {

// See: https://github.com/NixOS/nix/issues/9730
void printAmbiguous(
    EvalState & state, ValueRef v, const SymbolTable & symbols, std::ostream & str, std::set<size_t> * seen, int depth)
{
    checkInterrupt();

    if (depth <= 0) {
        str << "«too deep»";
        return;
    }
    switch (v.type(state.values)) {
    case nInt:
        str << v.integer(state.values);
        break;
    case nBool:
        printLiteralBool(str, v.boolean(state.values));
        break;
    case nString:
        printLiteralString(str, v.string_view(state.values));
        break;
    case nPath:
        str << v.path(state.values).to_string(); // !!! escaping?
        break;
    case nNull:
        str << "null";
        break;
    case nAttrs: {
        if (seen && !v.attrs(state.values)->empty() && !seen->insert((size_t) v.attrs(state.values)).second)
            str << "«repeated»";
        else {
            str << "{ ";
            for (auto & i : v.attrs(state.values)->lexicographicOrder(symbols)) {
                str << symbols[i->name] << " = ";
                printAmbiguous(state, i->value, symbols, str, seen, depth - 1);
                str << "; ";
            }
            str << "}";
        }
        break;
    }
    case nList:
        /* Use pointer to the Value instead of pointer to the elements, because
           that would need to explicitly handle the case of SmallList. */
        if (seen && v.listSize(state.values) && !seen->insert((size_t) v.ref).second)
            str << "«repeated»";
        else {
            str << "[ ";
            for (auto v2 : v.listView(state.values)) {
                if (v2)
                    printAmbiguous(state, v2, symbols, str, seen, depth - 1);
                else
                    str << "(nullptr)";
                str << " ";
            }
            str << "]";
        }
        break;
    case nThunk:
        if (!v.isBlackhole(state.values)) {
            str << "<CODE>";
        } else {
            // Although we know for sure that it's going to be an infinite recursion
            // when this value is accessed _in the current context_, it's likely
            // that the user will misinterpret a simpler «infinite recursion» output
            // as a definitive statement about the value, while in fact it may be
            // a valid value after `builtins.trace` and perhaps some other steps
            // have completed.
            str << "«potential infinite recursion»";
        }
        break;
    case nFunction:
        if (v.isLambda(state.values)) {
            str << "<LAMBDA>";
        } else if (v.isPrimOp(state.values)) {
            str << "<PRIMOP>";
        } else if (v.isPrimOpApp(state.values)) {
            str << "<PRIMOP-APP>";
        }
        break;
    case nExternal:
        str << *v.external(state.values);
        break;
    case nFloat:
        str << v.fpoint(state.values);
        break;
    default:
        printError("Nix evaluator internal error: printAmbiguous: invalid value type");
        unreachable();
    }
}

} // namespace nix
