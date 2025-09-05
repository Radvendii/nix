#pragma once
///@file

#include <cinttypes>
#include <functional>

namespace nix {

class EvalState;
struct Value;
class ValueRef;

class PosIdx
{
    friend void makePositionThunks(EvalState & state, const PosIdx pos, ValueRef line, ValueRef column);
    friend void prim_lineOfPos(EvalState & state, PosIdx pos, ValueRef * args, ValueRef v);
    friend void prim_columnOfPos(EvalState & state, PosIdx pos, ValueRef * args, ValueRef v);
    friend class PosTable;
    friend class std::hash<PosIdx>;

private:
    uint32_t id;

    explicit PosIdx(uint32_t id)
        : id(id)
    {
    }

public:
    PosIdx()
        : id(0)
    {
    }

    explicit operator bool() const
    {
        return id > 0;
    }

    auto operator<=>(const PosIdx other) const
    {
        return id <=> other.id;
    }

    bool operator==(const PosIdx other) const
    {
        return id == other.id;
    }

    size_t hash() const noexcept
    {
        return std::hash<uint32_t>{}(id);
    }
};

inline PosIdx noPos = {};

} // namespace nix

namespace std {

template<>
struct hash<nix::PosIdx>
{
    std::size_t operator()(nix::PosIdx pos) const noexcept
    {
        return pos.hash();
    }
};

} // namespace std
