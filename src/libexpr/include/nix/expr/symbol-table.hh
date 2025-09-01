#pragma once
///@file

#include <memory_resource>
#include "nix/expr/value.hh"
#include "nix/util/error.hh"

#include <boost/version.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

/**
 * SymbolData is the deduplicated data associated with a Symbol. The primary
 * thing this stores is the string underlying it, but sometimes (e.g. via
 * builtins.attrNames), we construct Values pointing at those strings. We
 * would like to also deduplicate these Values, and to do so we must store a a
 * reference to the Value for this Symbol here. When we need to create another
 * Value for it, we can find the one that already exists and use that instead.
 */
 // XXX [speed]: do we actually need size? Can't we construct a string_view with just a c string pointer? is it super slow?
class SymbolData {
    ValueRef v = ValueRefNull;
    uint32_t size;
    // variable length string allocated after the SymbolData in memory
    char c_str[0];

public:
    friend class Symbol;
    friend class SymbolTable;
};

/**
 * A SymbolRef points to the canonical Value of a Symbol. The type is literally
 * a subtype of ValueRefs, but only the ones that point at Symbols. We use
 * SymbolRefs so we can refer to SymbolData by a smaller value (4 bytes, rather
 * than the 8 of Symbol). The tradeoff is that we need an EvalState around to
 * access anything, so the use of SymbolRefs is typically to construct a Symbol
 * that directly refers to the data.
 *
 * Once we have the Value pointed to by the SymbolRef, we can follow it to the
 * string, which is itself a known offset from the beginning of the SymbolData.
 *
 * SymbolRefs can also be compared directly for equality, since the Values they
 * point to have been deduplicated.
 */
typedef ValueRef SymbolRef;

/**
 * Symbols have the property that they can be compared efficiently (using an
 * equality test), because the symbol table stores only one copy of each string.
 *
 * We also define several convenience operators so that Symbols can be used as
 * though they were the underlying string in many contexts.
 *
 * Symbols are stored in the SymbolTable, which performs deduplication.
 */
class Symbol {

    friend class SymbolTable;

    SymbolData *data;

    public:
    explicit Symbol(SymbolData *data) noexcept
        : data(data)
    {
    }

    // Fast equality comparison of pointers, courtesy of deduplicated data.
    bool operator==(const Symbol other) const noexcept
    {
        return data == other.data;
    }

    /* Deduplication machinery */

    private:
    /**
     * This is a not-yet-created SymbolData. It contains all the information
     * needed to create one, should we need to. But before we do we use that
     * information to look in the unordered_flat_map and see if it's there
     * already.
     */
    struct Key
    {
        using HashType = boost::hash<std::string_view>;

        // used for creating new Values
        EvalState & es;
        std::string_view str;
        // [XXX] speed: is this used so we don't have to re-calculate the hash many times? Why doesn't unordered_flat_set do this for us?
        std::size_t hash;
        std::pmr::polymorphic_allocator<char> & alloc;

        Key(EvalState & es, std::string_view str, std::pmr::polymorphic_allocator<char> & stringAlloc)
            : es(es)
            , str(str)
            , hash(HashType{}(str))
            , alloc(stringAlloc)
        {
        }
    };

    /**
     * The Hash and Equal interfaces allow Symbol to be entered into the
     * unordered_hash_set, and use Symbol::Key to look them up
     */
    struct Hash
    {
        using is_transparent = void;
        using is_avalanching = std::true_type;

        std::size_t operator()(Symbol sym) const
        {
            return Key::HashType{}(sym);
        }

        std::size_t operator()(const Key & key) const noexcept
        {
            return key.hash;
        }
    };

    struct Equal
    {
        using is_transparent = void;

        bool operator()(Symbol a, Symbol b) const noexcept
        {
            // strings are unique, so that a pointer comparison is OK
            return a.data == b.data;
        }

        bool operator()(Symbol a, const Key & b) const noexcept
        {
            return a == b.str;
        }

        [[gnu::always_inline]]
        bool operator()(const Key & a, Symbol b) const noexcept
        {
            return operator()(b, a);
        }
    };


    public:
    /**
     * This is where we do the allocating and assinging of a new Value and
     * SymbolData based on the Key. It's called when we're inserting into the
     * unordered_hash_set, and the Key doesn't match any of the already-existing
     * Symbols.
     */
    Symbol(const Key & key);

    /* End of deduplication machinery */

    /* Convenience string conversions */

    [[gnu::always_inline]]
    const char * c_str() const noexcept
    {
        return data->c_str;
    }

    [[gnu::always_inline]] operator std::string_view() const noexcept
    {
        return {data->c_str, data->size};
    }

    bool operator==(std::string_view s2) const noexcept
    {
        std::string_view this_str = *this;
        return this_str == s2;
    }

    friend std::ostream & operator<<(std::ostream & os, const Symbol & symbol);

    [[gnu::always_inline]]
    bool empty() const noexcept
    {
        return data->size == 0;
    }

    [[gnu::always_inline]]
    size_t size() const noexcept
    {
        return data->size;
    }

    /* End of convenience string conversions */

    /* Get the Value associated with the Symbol */
    /* XXX [speed] [[gnu::always_inline]] */
    const Value * valuePtr(EvalState & es) const noexcept;
};

class SymbolTable {
    /**
     * SymbolTable is an append only data structure. During its lifetime the
     * monotonic buffer holds all SymbolDatas, including all strings.
     */
    std::pmr::monotonic_buffer_resource buffer;
    std::pmr::polymorphic_allocator<char> stringAlloc{&buffer};

    // Used for creating Values
    EvalState & es;

    constexpr static size_t chunkSize{8192};
    /*
     * Hash set which allows deduplication of SymbolData. We first see if a
     * string is already in here before creating a new one.
     */
    boost::unordered_flat_set<Symbol, Symbol::Hash, Symbol::Equal> symbols{chunkSize};

public:

    SymbolTable(EvalState & es)
        : es(es)
    {
    }

    /**
     * Converts a string into a symbol.
     */
    SymbolRef create(std::string_view s)
    {
        // Most symbols are looked up more than once, so we trade off insertion performance
        // for lookup performance.
        // FIXME: make this thread-safe.
        return symbols.insert(Symbol::Key{es, s, stringAlloc}).first->data->v;
    }

    // XXX [speed]: these don't actually need a SymbolTable, just an EvalState
    Symbol operator[](SymbolRef ref) const;

    std::vector<Symbol> resolve(const std::vector<SymbolRef> & symbols) const
    {
        std::vector<Symbol> result;
        result.reserve(symbols.size());
        for (auto sym : symbols)
            result.push_back((*this)[sym]);
        return result;
    }

    size_t totalSize() const;

    [[gnu::always_inline]]
    size_t size() const noexcept
    {
        return symbols.size();
    }

    template<typename T>
    void dump(T callback) const
    {
        for (auto & sym : symbols) {
            callback(sym);
        }
    }

};
} // namespace nix
