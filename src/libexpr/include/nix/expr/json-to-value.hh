#pragma once
///@file

#include "nix/expr/value.hh"
#include "nix/util/error.hh"

#include <string>

namespace nix {

class EvalState;
struct Value;

MakeError(JSONParseError, Error);

void parseJSON(EvalState & state, const std::string_view & s, ValueRef v);

} // namespace nix
