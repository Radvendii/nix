#pragma once
///@file

#include <cassert>
#include <span>
#include <type_traits>
#include <concepts>

#include "nix/expr/eval-gc.hh"
#include "nix/expr/value/context.hh"
#include "nix/util/source-path.hh"
#include "nix/expr/print-options.hh"
#include "nix/util/checked-arithmetic.hh"

#include <nlohmann/json_fwd.hpp>

namespace nix {

// XXX [speed]: keeping track of how many of each type of Value we make, so we
// can see how impactful different refactors would be.
// NOTE: if something gets set and then reset (looking at you tThunk) it will count for both
extern unsigned long nrUninitialized;
extern unsigned long nrInt;
extern unsigned long nrBool;
extern unsigned long nrNull;
extern unsigned long nrFloat;
extern unsigned long nrExternal;
extern unsigned long nrPrimOp;
extern unsigned long nrAttrs;
extern unsigned long nrListSmall;
extern unsigned long nrPrimOpApp;
extern unsigned long nrApp;
extern unsigned long nrThunk;
extern unsigned long nrLambda;
extern unsigned long nrListN;
extern unsigned long nrString;
extern unsigned long nrPath;

struct Value;
class Values;
class ListBuilder;
class ExternalValueBase;

// XXX [speed]: this might not be needed when we're done
inline void * allocBytes(size_t n);

class BindingsBuilder;

typedef enum {
    tUninitialized = 0,
    tInt = 1,
    tBool,
    tString,
    tPath,
    tNull,
    tAttrs,
    tListSmall,
    tListN,
    tThunk,
    tApp,
    tLambda,
    tPrimOp,
    tPrimOpApp,
    tExternal,
    tFloat
} InternalType;

/**
 * This type abstracts over all actual value types in the language,
 * grouping together implementation details like tList*, different function
 * types, and types in non-normal form (so thunks and co.)
 */
typedef enum {
    nThunk,
    nInt,
    nFloat,
    nBool,
    nString,
    nPath,
    nNull,
    nAttrs,
    nList,
    nFunction,
    nExternal,
} ValueType;

class Bindings;
struct Env;
struct Expr;
struct ExprLambda;
struct ExprBlackHole;
struct PrimOp;
class SymbolRef;
class Symbol;
class PosIdx;
struct Pos;
class StorePath;
class EvalState;
class XMLWriter;
class Printer;

using NixInt = checked::Checked<int64_t>;
using NixFloat = double;

class ValueRef {
    public:

    static ValueRef null;

    uint32_t ref;

    constexpr ValueRef() = default;

    constexpr explicit ValueRef(uint32_t ref)
        : ref(ref)
    {
    }

    [[gnu::always_inline]]
    constexpr explicit operator bool() const noexcept {
        return ref;
    }

    constexpr auto operator<=>(const ValueRef & other) const noexcept = default;



    // Functions needed to distinguish the type
    // These should be removed eventually, by putting the functionality that's
    // needed by callers into methods of this type

    inline bool isThunk(Values & values) const;
    inline bool isApp(Values & values) const;
    inline bool isBlackhole(Values & values) const;
    inline bool isLambda(Values & values) const;
    inline bool isPrimOp(Values & values) const;
    inline bool isPrimOpApp(Values & values) const;

    /**
     * Returns the normal type of a Value. This only returns nThunk if
     * the Value hasn't been forceValue'd
     *
     * @param invalidIsThunk Instead of aborting an an invalid (probably
     * 0, so uninitialized) internal type, return `nThunk`.
     */
    inline ValueType type(Values & values, bool invalidIsThunk = false) const;

    inline void mkInt(Values & values, NixInt::Inner n) noexcept;
    inline void mkInt(Values & values, NixInt n) noexcept;
    inline void mkBool(Values & values, bool b) noexcept;
    inline void mkString(Values & values, const char * s, const char ** context = 0) noexcept;
    void mkString(Values & values, std::string_view s);
    void mkString(Values & values, std::string_view s, const NixStringContext & context);
    void mkStringMove(Values & values, const char * s, const NixStringContext & context);
    void mkPath(Values & values, const SourcePath & path);
    inline void mkPath(Values & values, SourceAccessor * accessor, const char * path) noexcept;
    inline void mkNull(Values & values) noexcept;
    inline void mkAttrs(Values & values, Bindings * a) noexcept;
    void mkAttrs(Values & values, BindingsBuilder & bindings);
    void mkList(Values & values, const ListBuilder & builder) noexcept;
    inline void mkThunk(Values & values, Env * e, Expr * ex) noexcept;
    inline void mkApp(Values & values, ValueRef l, ValueRef r) noexcept;
    inline void mkLambda(Values & values, Env * e, ExprLambda * f) noexcept;
    inline void mkBlackhole(Values & values);
    void mkPrimOp(Values & values, PrimOp * p);
    inline void mkPrimOpApp(Values & values, ValueRef l, ValueRef r) noexcept;
    /**
     * For a `tPrimOpApp` value, get the original `PrimOp` value.
     */
    const PrimOp * primOpAppPrimOp(Values & values) const;
    inline void mkExternal(Values & values, ExternalValueBase * e) noexcept;
    inline void mkFloat(Values & values, NixFloat n) noexcept;
};

/**
 * External values must descend from ExternalValueBase, so that
 * type-agnostic nix functions (e.g. showType) can be implemented
 */
class ExternalValueBase
{
    friend std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v);
    friend class Printer;
protected:
    /**
     * Print out the value
     */
    virtual std::ostream & print(std::ostream & str) const = 0;

public:
    /**
     * Return a simple string describing the type
     */
    virtual std::string showType() const = 0;

    /**
     * Return a string to be used in builtins.typeOf
     */
    virtual std::string typeOf() const = 0;

    /**
     * Coerce the value to a string. Defaults to uncoercable, i.e. throws an
     * error.
     */
    virtual std::string coerceToString(
        EvalState & state, const PosIdx & pos, NixStringContext & context, bool copyMore, bool copyToStore) const;

    /**
     * Compare to another value of the same type. Defaults to uncomparable,
     * i.e. always false.
     */
    virtual bool operator==(const ExternalValueBase & b) const noexcept;

    /**
     * Print the value as JSON. Defaults to unconvertable, i.e. throws an error
     */
    virtual nlohmann::json
    printValueAsJSON(EvalState & state, bool strict, NixStringContext & context, bool copyToStore = true) const;

    /**
     * Print the value as XML. Defaults to unevaluated
     */
    virtual void printValueAsXML(
        EvalState & state,
        bool strict,
        bool location,
        XMLWriter & doc,
        NixStringContext & context,
        PathSet & drvsSeen,
        const PosIdx pos) const;

    virtual ~ExternalValueBase() {};
};

std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v);

class ListBuilder
{
    const size_t size;
    ValueRef inlineElems[2] = {ValueRef::null, ValueRef::null};
public:
    ValueRef * elems;
    ListBuilder(EvalState & state, size_t size);

    // NOTE: Can be noexcept because we are just copying integral values and
    // raw pointers.
    ListBuilder(ListBuilder && x) noexcept
        : size(x.size)
        , inlineElems{x.inlineElems[0], x.inlineElems[1]}
        , elems(size <= 2 ? inlineElems : x.elems)
    {
    }

    ValueRef & operator[](size_t n)
    {
        return elems[n];
    }

    typedef ValueRef * iterator;

    iterator begin()
    {
        return &elems[0];
    }

    iterator end()
    {
        return &elems[size];
    }

    friend struct Value;
    friend class ValueRef;
};

namespace detail {

/**
 * Implementation mixin class for defining the public types
 * In can be inherited from by the actual ValueStorage implementations
 * for free due to Empty Base Class Optimization (EBCO).
 */
struct ValueBase
{
    /**
     * Strings in the evaluator carry a so-called `context` which
     * is a list of strings representing store paths.  This is to
     * allow users to write things like
     *
     *   "--with-freetype2-library=" + freetype + "/lib"
     *
     * where `freetype` is a derivation (or a source to be copied
     * to the store).  If we just concatenated the strings without
     * keeping track of the referenced store paths, then if the
     * string is used as a derivation attribute, the derivation
     * will not have the correct dependencies in its inputDrvs and
     * inputSrcs.

     * The semantics of the context is as follows: when a string
     * with context C is used as a derivation attribute, then the
     * derivations in C will be added to the inputDrvs of the
     * derivation, and the other store paths in C will be added to
     * the inputSrcs of the derivations.

     * For canonicity, the store paths should be in sorted order.
     */
    struct StringWithContext
    {
        const char * c_str;
        const char ** context; // must be in sorted order
    };

    struct Path
    {
        SourceAccessor * accessor;
        const char * path;
    };

    struct Null
    {};

    struct ClosureThunk
    {
        Env * env;
        Expr * expr;
    };

    struct FunctionApplicationThunk
    {
        ValueRef left, right;
    };

    /**
     * Like FunctionApplicationThunk, but must be a distinct type in order to
     * resolve overloads to `tPrimOpApp` instead of `tApp`.
     * This type helps with the efficient implementation of arity>=2 primop calls.
     */
    struct PrimOpApplicationThunk
    {
        ValueRef left, right;
    };

    struct Lambda
    {
        Env * env;
        ExprLambda * fun;
    };

    using SmallList = std::array<ValueRef, 2>;

    struct List
    {
        size_t size;
        ValueRef const * elems;
    };
};

template<typename T>
struct PayloadTypeToInternalType
{};

/**
 * All stored types must be distinct (not type aliases) for the purposes of
 * overload resolution in setStorage. This ensures there's a bijection from
 * InternalType <-> C++ type.
 */
#define NIX_VALUE_STORAGE_FOR_EACH_FIELD(MACRO)                     \
    MACRO(NixInt, integer, tInt)                                    \
    MACRO(bool, boolean, tBool)                                     \
    MACRO(ValueBase::StringWithContext, string, tString)            \
    MACRO(ValueBase::Path, path, tPath)                             \
    MACRO(ValueBase::Null, null_, tNull)                            \
    MACRO(Bindings *, attrs, tAttrs)                                \
    MACRO(ValueBase::List, bigList, tListN)                         \
    MACRO(ValueBase::SmallList, smallList, tListSmall)              \
    MACRO(ValueBase::ClosureThunk, thunk, tThunk)                   \
    MACRO(ValueBase::FunctionApplicationThunk, app, tApp)           \
    MACRO(ValueBase::Lambda, lambda, tLambda)                       \
    MACRO(PrimOp *, primOp, tPrimOp)                                \
    MACRO(ValueBase::PrimOpApplicationThunk, primOpApp, tPrimOpApp) \
    MACRO(ExternalValueBase *, external, tExternal)                 \
    MACRO(NixFloat, fpoint, tFloat)

#define NIX_VALUE_PAYLOAD_TYPE(T, FIELD_NAME, DISCRIMINATOR) \
    template<>                                               \
    struct PayloadTypeToInternalType<T>                      \
    {                                                        \
        static constexpr InternalType value = DISCRIMINATOR; \
    };

NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_PAYLOAD_TYPE)

#undef NIX_VALUE_PAYLOAD_TYPE

template<typename T>
inline constexpr InternalType payloadTypeToInternalType = PayloadTypeToInternalType<T>::value;

} // namespace detail

/**
 * Discriminated union of types stored in the value.
 * The union discriminator is @ref InternalType enumeration.
 *
 * This class can be specialized with a non-type template parameter
 * of pointer size for more optimized data layouts on when pointer alignment
 * bits can be used for storing the discriminator.
 *
 * All specializations of this type need to implement getStorage, setStorage and
 * getInternalType methods.
 */
template<std::size_t ptrSize>
class ValueStorage : public detail::ValueBase
{
protected:
    using Payload = union
    {
#define NIX_VALUE_STORAGE_DEFINE_FIELD(T, FIELD_NAME, DISCRIMINATOR) T FIELD_NAME;
        NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_STORAGE_DEFINE_FIELD)
#undef NIX_VALUE_STORAGE_DEFINE_FIELD
    };

private:
    InternalType internalType = tUninitialized;
    Payload payload;

protected:
#define NIX_VALUE_STORAGE_GET_IMPL(K, FIELD_NAME, DISCRIMINATOR) \
    void getStorage(K & val) const noexcept                      \
    {                                                            \
        assert(internalType == DISCRIMINATOR);                   \
        val = payload.FIELD_NAME;                                \
    }

#define NIX_VALUE_STORAGE_SET_IMPL(K, FIELD_NAME, DISCRIMINATOR) \
    void setStorage(K val) noexcept                              \
    {                                                            \
        payload.FIELD_NAME = val;                                \
        internalType = DISCRIMINATOR;                            \
    }

    NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_STORAGE_GET_IMPL)
    NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_STORAGE_SET_IMPL)

#undef NIX_VALUE_STORAGE_SET_IMPL
#undef NIX_VALUE_STORAGE_GET_IMPL
#undef NIX_VALUE_STORAGE_FOR_EACH_FIELD

    /** Get internal type currently occupying the storage. */
    InternalType getInternalType() const noexcept
    {
        return internalType;
    }
};

/**
 * View into a list of ValueRef that is itself immutable.
 *
 * Since not all representations of ValueStorage can provide
 * a pointer to a const array of ValueRef this proxy class either
 * stores the small list inline or points to the big list.
 */
class ListView
{
    using SpanType = std::span<ValueRef const>;
    using SmallList = detail::ValueBase::SmallList;
    using List = detail::ValueBase::List;

    std::variant<SmallList, List> raw;

public:

    // XXX [speed]: default-constructing this is generally a bad idea, but in prim_concatMap we actually don't need it to be initialized at all.
    ListView() {};

    ListView(SmallList list)
        : raw(list)
    {
    }

    ListView(List list)
        : raw(list)
    {
    }

    ValueRef const * data() const & noexcept
    {
        return std::visit(
            overloaded{
                [](const SmallList & list) {
                    return list.data();
                },
                [](const List & list) {
                    return list.elems;
                }},
            raw);
    }

    std::size_t size() const noexcept
    {
        return std::visit(
            overloaded{
                [](const SmallList & list) -> std::size_t { return list.back() == ValueRef::null ? 1 : 2; },
                [](const List & list) -> std::size_t { return list.size; }},
            raw);
    }

    ValueRef operator[](std::size_t i) const noexcept
    {
        return data()[i];
    }

    SpanType span() const &
    {
        return SpanType(data(), size());
    }

    /* Ensure that no dangling views can be created accidentally, as that
       would lead to hard to diagnose bugs that only affect small lists. */
    SpanType span() && = delete;
    ValueRef const * data() && noexcept = delete;

    /**
     * Random-access iterator that only allows iterating over a constant range
     * of mutable Value pointers.
     *
     * @note Not a pointer to minimize potential misuses and implicitly relying
     * on the iterator being a pointer.
     **/
    class iterator
    {
    public:
        using value_type = ValueRef;
        using pointer = const value_type *;
        using reference = const value_type &;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::random_access_iterator_tag;

    private:
        pointer ptr = nullptr;

        friend class ListView;

        iterator(pointer ptr)
            : ptr(ptr)
        {
        }

    public:
        iterator() = default;

        reference operator*() const
        {
            return *ptr;
        }

        const value_type * operator->() const
        {
            return ptr;
        }

        reference operator[](difference_type diff) const
        {
            return ptr[diff];
        }

        iterator & operator++()
        {
            ++ptr;
            return *this;
        }

        iterator operator++(int)
        {
            pointer tmp = ptr;
            ++*this;
            return iterator(tmp);
        }

        iterator & operator--()
        {
            --ptr;
            return *this;
        }

        iterator operator--(int)
        {
            pointer tmp = ptr;
            --*this;
            return iterator(tmp);
        }

        iterator & operator+=(difference_type diff)
        {
            ptr += diff;
            return *this;
        }

        iterator operator+(difference_type diff) const
        {
            return iterator(ptr + diff);
        }

        friend iterator operator+(difference_type diff, const iterator & rhs)
        {
            return iterator(diff + rhs.ptr);
        }

        iterator & operator-=(difference_type diff)
        {
            ptr -= diff;
            return *this;
        }

        iterator operator-(difference_type diff) const
        {
            return iterator(ptr - diff);
        }

        difference_type operator-(const iterator & rhs) const
        {
            return ptr - rhs.ptr;
        }

        std::strong_ordering operator<=>(const iterator & rhs) const = default;
    };

    using const_iterator = iterator;

    iterator begin()  const &
    {
        return data();
    }

    iterator end() const &
    {
        return data() + size();
    }

    /* Ensure that no dangling iterators can be created accidentally, as that
       would lead to hard to diagnose bugs that only affect small lists. */
    iterator begin() && = delete;
    iterator end() && = delete;
};

static_assert(std::random_access_iterator<ListView::iterator>);

struct Value : public ValueStorage<sizeof(void *)>
{
    friend std::string showType(EvalState & state, const ValueRef v);
    friend class ValueRef;

    template<InternalType... discriminator>
    bool isa() const noexcept
    {
        return ((getInternalType() == discriminator) || ...);
    }

    template<typename T>
    T getStorage() const noexcept
    {
        if (getInternalType() != detail::payloadTypeToInternalType<T>) [[unlikely]]
            unreachable();
        T out;
        ValueStorage::getStorage(out);
        return out;
    }

public:

    void print(EvalState & state, std::ostream & str, PrintOptions options = PrintOptions{});

    // Functions needed to distinguish the type
    // These should be removed eventually, by putting the functionality that's
    // needed by callers into methods of this type

    // type() == nThunk
    inline bool isThunk() const
    {
        return isa<tThunk>();
    };

    inline bool isApp() const
    {
        return isa<tApp>();
    };

    inline bool isBlackhole() const;

    // type() == nFunction
    inline bool isLambda() const
    {
        return isa<tLambda>();
    };

    inline bool isPrimOp() const
    {
        return isa<tPrimOp>();
    };

    inline bool isPrimOpApp() const
    {
        return isa<tPrimOpApp>();
    };

    /**
     * Returns the normal type of a Value. This only returns nThunk if
     * the Value hasn't been forceValue'd
     *
     * @param invalidIsThunk Instead of aborting an an invalid (probably
     * 0, so uninitialized) internal type, return `nThunk`.
     */
    inline ValueType type(bool invalidIsThunk = false) const
    {
        switch (getInternalType()) {
        case tUninitialized:
            break;
        case tInt:
            return nInt;
        case tBool:
            return nBool;
        case tString:
            return nString;
        case tPath:
            return nPath;
        case tNull:
            return nNull;
        case tAttrs:
            return nAttrs;
        case tListSmall:
        case tListN:
            return nList;
        case tLambda:
        case tPrimOp:
        case tPrimOpApp:
            return nFunction;
        case tExternal:
            return nExternal;
        case tFloat:
            return nFloat;
        case tThunk:
        case tApp:
            return nThunk;
        }
        if (invalidIsThunk)
            return nThunk;
        else
            unreachable();
    }

    /**
     * A value becomes valid when it is initialized. We don't use this
     * in the evaluator; only in the bindings, where the slight extra
     * cost is warranted because of inexperienced callers.
     */
    inline bool isValid() const noexcept
    {
        return !isa<tUninitialized>();
    }

    inline void mkInt(NixInt::Inner n) noexcept
    {
        mkInt(NixInt{n});
    }

    inline void mkInt(NixInt n) noexcept
    {
        setStorage(NixInt{n});
        nrInt++;
    }

    inline void mkBool(bool b) noexcept
    {
        setStorage(b);
        nrBool++;
    }

    inline void mkString(const char * s, const char ** context = 0) noexcept
    {
        setStorage(StringWithContext{.c_str = s, .context = context});
        nrString++;
    }

    void mkString(std::string_view s);

    void mkString(std::string_view s, const NixStringContext & context);

    void mkStringMove(const char * s, const NixStringContext & context);

    void mkPath(const SourcePath & path);
    // YYY [speed]: this is never used or defined
    void mkPath(std::string_view path);

    inline void mkPath(SourceAccessor * accessor, const char * path) noexcept
    {
        setStorage(Path{.accessor = accessor, .path = path});
        nrPath++;
    }

    inline void mkNull() noexcept
    {
        setStorage(Null{});
        nrNull++;
    }

    inline void mkAttrs(Bindings * a) noexcept
    {
        setStorage(a);
        nrAttrs++;
    }

    void mkAttrs(BindingsBuilder & bindings);

    void mkList(const ListBuilder & builder) noexcept
    {
        if (builder.size == 1) {
            setStorage(std::array<ValueRef, 2>{builder.inlineElems[0], ValueRef::null});
            nrListSmall++;
        }
        else if (builder.size == 2) {
            setStorage(std::array<ValueRef, 2>{builder.inlineElems[0], builder.inlineElems[1]});
            nrListSmall++;
        }
        else {
            setStorage(List{.size = builder.size, .elems = builder.elems});
            nrListN++;
        }
    }

    inline void mkThunk(Env * e, Expr * ex) noexcept
    {
        setStorage(ClosureThunk{.env = e, .expr = ex});
        nrThunk++;
    }

    inline void mkApp(ValueRef l, ValueRef r) noexcept
    {
        setStorage(FunctionApplicationThunk{.left = l, .right = r});
        nrApp++;
    }


    inline void mkLambda(Env * e, ExprLambda * f) noexcept
    {
        setStorage(Lambda{.env = e, .fun = f});
        nrLambda++;
    }

    inline void mkBlackhole();

    void mkPrimOp(PrimOp * p);

    inline void mkPrimOpApp(ValueRef l, ValueRef r) noexcept
    {
        setStorage(PrimOpApplicationThunk{.left = l, .right = r});
        nrPrimOpApp++;
    }


    /**
     * For a `tPrimOpApp` value, get the original `PrimOp` value.
     */
    const PrimOp * primOpAppPrimOp(Values & values) const;

    inline void mkExternal(ExternalValueBase * e) noexcept
    {
        setStorage(e);
        nrExternal++;
    }

    inline void mkFloat(NixFloat n) noexcept
    {
        setStorage(n);
        nrFloat++;
    }

    bool isList() const noexcept
    {
        return isa<tListSmall, tListN>();
    }

    ListView listView() const noexcept
    {
        return isa<tListSmall>() ? ListView(getStorage<SmallList>()) : ListView(getStorage<List>());
    }

    size_t listSize() const noexcept
    {
        return isa<tListSmall>() ? (getStorage<SmallList>()[1] == ValueRef::null ? 1 : 2) : getStorage<List>().size;
    }

    PosIdx determinePos(EvalState & es, const PosIdx pos) const;

    /**
     * Check whether forcing this value requires a trivial amount of
     * computation. In particular, function applications are
     * non-trivial.
     */
    bool isTrivial() const;

    SourcePath path() const
    {
        return SourcePath(ref(pathAccessor()->shared_from_this()), CanonPath(CanonPath::unchecked_t(), pathStr()));
    }

    std::string_view string_view() const noexcept
    {
        return std::string_view(getStorage<StringWithContext>().c_str);
    }

    const char * c_str() const noexcept
    {
        return getStorage<StringWithContext>().c_str;
    }

    const char ** context() const noexcept
    {
        return getStorage<StringWithContext>().context;
    }

    ExternalValueBase * external() const noexcept
    {
        return getStorage<ExternalValueBase *>();
    }

    const Bindings * attrs() const noexcept
    {
        return getStorage<Bindings *>();
    }

    const PrimOp * primOp() const noexcept
    {
        return getStorage<PrimOp *>();
    }

    bool boolean() const noexcept
    {
        return getStorage<bool>();
    }

    NixInt integer() const noexcept
    {
        return getStorage<NixInt>();
    }

    NixFloat fpoint() const noexcept
    {
        return getStorage<NixFloat>();
    }

    Lambda lambda() const noexcept
    {
        return getStorage<Lambda>();
    }

    ClosureThunk thunk() const noexcept
    {
        return getStorage<ClosureThunk>();
    }

    PrimOpApplicationThunk primOpApp() const noexcept
    {
        return getStorage<PrimOpApplicationThunk>();
    }

    FunctionApplicationThunk app() const noexcept
    {
        return getStorage<FunctionApplicationThunk>();
    }

    const char * pathStr() const noexcept
    {
        return getStorage<Path>().path;
    }

    SourceAccessor * pathAccessor() const noexcept
    {
        return getStorage<Path>().accessor;
    }
};

extern ExprBlackHole eBlackHole;

bool Value::isBlackhole() const
{
    return isThunk() && thunk().expr == (Expr *) &eBlackHole;
}

void Value::mkBlackhole()
{
    mkThunk(nullptr, (Expr *) &eBlackHole);
}

typedef std::vector<ValueRef, traceable_allocator<ValueRef>> ValueVector;
typedef std::unordered_map<
    SymbolRef,
    ValueRef,
    std::hash<SymbolRef>,
    std::equal_to<SymbolRef>,
    traceable_allocator<std::pair<const SymbolRef, ValueRef>>>
    ValueMap;
typedef std::map<SymbolRef, ValueVector, std::less<SymbolRef>, traceable_allocator<std::pair<const SymbolRef, ValueVector>>>
    ValueVectorMap;

/**
 * A value allocated in traceable memory.
 */
// XXX [speed]: I will have to revisit this with someone who actually knows what's going on. For now I'm going to modify it in ways that I know make no sense. But they compile and leave most of the code in place in case we need it (easier to rip it out later than try to restore it)
typedef std::shared_ptr<ValueRef> RootValue;

RootValue allocRootValue(ValueRef v);

void forceNoNullByte(std::string_view s, std::function<Pos()> = nullptr);

class Values {
    public:

    std::vector<Value> values;

    /**
     * In order to refer to Values allocated on the stack in a ValueRef (32
     * bits), we need a stable pointer to somewhere in the stack from which to
     * offset. This is that pointer.
     *
     * XXX [speed]: figure out what to call this and where to put it
     * XXX [speed]: figure out how this works with multiple threads?
     */
    size_t stackPtr;

    Values() {
        // grab a pointer to somewhere in the stack for later
        // XXX [speed] if we make this a (Value *), we can maybe use alignment to make the indexable space even larger
        char *stackValue;
        stackPtr = (size_t) &stackValue;
    }

    Value * VRtoVP(ValueRef ref) {
        if (!ref)
            return nullptr;
        // XXX [speed] make sure this branching statement gets optimzied out
        if (ref.ref & 0x1) {
            // use arithmetic shift to preserve sign bit
            int32_t offset = (int32_t)ref.ref >> 1;
            return (Value *) (stackPtr + offset);
        }
        // XXX [speed]: we could save a pointer to &Values.front() - 1, so we don't have to offset by 1 every time
        return &values[(ref.ref >> 1) - 1];
    }
    Value & VRtoV(ValueRef ref) {
        return *VRtoVP(ref);
    }
    ValueRef VPtoVR(Value *v) {
        if (v == nullptr)
            return ValueRef::null;
        if (v < &values.front() || v > &values.back()) {
            // assume stack pointer
            // XXX [speed]: would really be nice if we could error check this properly (i.e. is it on the stack)
            size_t offset_64 = (size_t) v - stackPtr;
            size_t int31_max = 0x3FFFFFFF;
            if (offset_64 > int31_max && -offset_64 > int31_max)
            {
              std::cout << "value pointer out of range: " << std::hex << v << " (" << stackPtr << ")" << "\n";
            }
            int32_t offset = (size_t) v - stackPtr;
            ValueRef ret{(uint32_t) offset << 1 | 0x1};
            return ret;
        }
        // XXX [speed]: do we have to convert to size_t first?
        // Offset by 1 so we don't overlap with ValueRef::null
        return ValueRef{(uint32_t)((v - &values.front() + 1) << 1)};
    }
};

// type() == nThunk
inline bool ValueRef::isThunk(Values & values) const
{
    return values.VRtoV(*this).isa<tThunk>();
};

inline bool ValueRef::isApp(Values & values) const
{
    return values.VRtoV(*this).isa<tApp>();
};

bool ValueRef::isBlackhole(Values & values) const
{
    return isThunk(values) && values.VRtoV(*this).thunk().expr == (Expr *) &eBlackHole;
}

// type() == nFunction
inline bool ValueRef::isLambda(Values & values) const
{
    return values.VRtoV(*this).isa<tLambda>();
};

inline bool ValueRef::isPrimOp(Values & values) const
{
    return values.VRtoV(*this).isa<tPrimOp>();
};

inline bool ValueRef::isPrimOpApp(Values & values) const
{
    return values.VRtoV(*this).isa<tPrimOpApp>();
};
/**
 * Returns the normal type of a Value. This only returns nThunk if
 * the Value hasn't been forceValue'd
 *
 * @param invalidIsThunk Instead of aborting an an invalid (probably
 * 0, so uninitialized) internal type, return `nThunk`.
 */
inline ValueType ValueRef::type(Values & values, bool invalidIsThunk) const
{
    switch (values.VRtoV(*this).getInternalType()) {
    case tUninitialized:
        break;
    case tInt:
        return nInt;
    case tBool:
        return nBool;
    case tString:
        return nString;
    case tPath:
        return nPath;
    case tNull:
        return nNull;
    case tAttrs:
        return nAttrs;
    case tListSmall:
    case tListN:
        return nList;
    case tLambda:
    case tPrimOp:
    case tPrimOpApp:
        return nFunction;
    case tExternal:
        return nExternal;
    case tFloat:
        return nFloat;
    case tThunk:
    case tApp:
        return nThunk;
    }
    if (invalidIsThunk)
        return nThunk;
    else
        unreachable();
}

inline void ValueRef::mkInt(Values & values, NixInt::Inner n) noexcept
{
    mkInt(values, NixInt{n});
}

inline void ValueRef::mkInt(Values & values, NixInt n) noexcept
{
    values.VRtoV(*this).setStorage(NixInt{n});
    nrInt++;
}

inline void ValueRef::mkBool(Values & values, bool b) noexcept
{
    values.VRtoV(*this).setStorage(b);
    nrBool++;
}

inline void ValueRef::mkString(Values & values, const char * s, const char ** context) noexcept
{
    values.VRtoV(*this).setStorage(detail::ValueBase::StringWithContext{.c_str = s, .context = context});
    nrString++;
}

inline void ValueRef::mkPath(Values & values, SourceAccessor * accessor, const char * path) noexcept
{
    values.VRtoV(*this).setStorage(detail::ValueBase::Path{.accessor = accessor, .path = path});
    nrPath++;
}

inline void ValueRef::mkNull(Values & values) noexcept
{
    values.VRtoV(*this).setStorage(detail::ValueBase::Null{});
    nrNull++;
}

inline void ValueRef::mkAttrs(Values & values, Bindings * a) noexcept
{
    values.VRtoV(*this).setStorage(a);
    nrAttrs++;
}

inline void ValueRef::mkThunk(Values & values, Env * e, Expr * ex) noexcept
{
    values.VRtoV(*this).setStorage(detail::ValueBase::ClosureThunk{.env = e, .expr = ex});
    nrThunk++;
}

inline void ValueRef::mkApp(Values & values, ValueRef l, ValueRef r) noexcept
{
    values.VRtoV(*this).setStorage(detail::ValueBase::FunctionApplicationThunk{.left = l, .right = r});
    nrApp++;
}


inline void ValueRef::mkLambda(Values & values, Env * e, ExprLambda * f) noexcept
{
    values.VRtoV(*this).setStorage(detail::ValueBase::Lambda{.env = e, .fun = f});
    nrLambda++;
}

inline void ValueRef::mkBlackhole(Values & values)
{
    mkThunk(values, nullptr, (Expr *) &eBlackHole);
}

inline void ValueRef::mkPrimOpApp(Values & values, ValueRef l, ValueRef r) noexcept
{
    values.VRtoV(*this).setStorage(detail::ValueBase::PrimOpApplicationThunk{.left = l, .right = r});
    nrPrimOpApp++;
}

inline void ValueRef::mkExternal(Values & values, ExternalValueBase * e) noexcept
{
    values.VRtoV(*this).setStorage(e);
    nrExternal++;
}

inline void ValueRef::mkFloat(Values & values, NixFloat n) noexcept
{
    values.VRtoV(*this).setStorage(n);
    nrFloat++;
}
} // namespace nix
