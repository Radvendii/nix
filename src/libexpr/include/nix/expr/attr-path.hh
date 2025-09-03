#pragma once
///@file

#include "nix/expr/eval.hh"

#include <string>
#include <map>

namespace nix {

MakeError(AttrPathNotFound, Error);
MakeError(NoPositionInfo, Error);

std::pair<ValueRef, PosIdx>
findAlongAttrPath(EvalState & state, const std::string & attrPath, Bindings & autoArgs, ValueRef vIn);

/**
 * Heuristic to find the filename and lineno or a nix value.
 */
std::pair<SourcePath, uint32_t> findPackageFilename(EvalState & state, ValueRef v, std::string what);

std::vector<SymbolRef> parseAttrPath(EvalState & state, std::string_view s);

} // namespace nix
