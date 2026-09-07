#pragma once
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


namespace lumen_script {

// Sanitizers have to see every box as an allocation of its own: a recycler
// hides use-after-free from them, which is exactly the bug they are there to
// catch.  With one active, boxes go straight to the allocator.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define LUMEN_NO_RECYCLER 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define LUMEN_NO_RECYCLER 1
#  endif
#endif

namespace detail {

// Recycler for boxes of one size, one per thread.
//
// Value boxes are fixed size and die the moment the refcount hits zero, so
// handing them back to the general allocator and asking for them again is work
// that can be skipped: building a string in a loop takes and releases the same
// box thousands of times.
//
// A box can be born on a database pool thread and die on the event loop one, so
// it ends up on the list of whichever thread releases it.  Hence the cap:
// without it, a producer thread and a consumer thread would leave all the
// memory parked on the second one's list.
template <size_t N>
class Recycler {
public:
    static_assert(N >= sizeof(void*), "the box must be able to hold the link");

    void* take() {
        if (!head_) return ::operator new(N);
        void* p = head_;
        head_  = *reinterpret_cast<void**>(p);
        --count_;
        return p;
    }

    void give_back(void* p) noexcept {
        if (count_ >= kCap) { ::operator delete(p); return; }
        *reinterpret_cast<void**>(p) = head_;
        head_ = p;
        ++count_;
    }

    ~Recycler() {
        while (head_) {
            void* p = head_;
            head_  = *reinterpret_cast<void**>(p);
            ::operator delete(p);
        }
    }

private:
    static constexpr size_t kCap = 256;

    void*  head_  = nullptr;
    size_t count_ = 0;
};

template <size_t N>
Recycler<N>& recycler() {
    thread_local Recycler<N> r;
    return r;
}

}  // namespace detail

// Runtime value of the VM.
//
// Takes 16 bytes: 8 of payload and 1 of tag.  A value is of exactly one type at
// a time, so the heap pointer shares room with the scalars in a union.  It used
// to be 72 —three shared_ptr, two of them always null— and every push and pop
// of the VM moved those 72 bytes.
class Value {
public:
    enum class Type { Null, Bool, Int, Float, Str, List, Dict };

    using List = std::vector<Value>;

    // Dictionary of the VM.
    //
    // It used to be a std::map: a red-black tree, with one allocation per node
    // and a pointer hop per access, for objects that almost always have between
    // 3 and 10 keys.  Measured with string keys, a linear scan over a contiguous
    // vector runs twice as fast below 8 keys and the tree does not win until 64.
    //
    // So this is always a vector, in insertion order, and past a threshold it
    // gains a hash index.  That way lookup is also O(1) in big dictionaries —an
    // accumulator, a word counter— where a linear scan would be a disaster, and
    // insertion is amortized O(1) at every size, which the tree never gave.
    //
    // The key order of the response does not change: it is fixed when the JSON
    // object is built, not by this container.
    class Dict {
    public:
        using Pair           = std::pair<std::string, Value>;
        using iterator       = std::vector<Pair>::iterator;
        using const_iterator = std::vector<Pair>::const_iterator;

        iterator       begin()       { return v_.begin(); }
        iterator       end()         { return v_.end();   }
        const_iterator begin() const { return v_.begin(); }
        const_iterator end()   const { return v_.end();   }

        size_t size()  const { return v_.size();  }
        bool   empty() const { return v_.empty(); }
        void   clear()       { v_.clear(); idx_.clear(); }

        // Whoever knows how many keys they will insert —the VM's MakeDict has
        // them counted— keeps the vector from growing in steps: without this a
        // 3-key dictionary does three allocations, one per reallocation.
        void   reserve(size_t n) { v_.reserve(n); }
        size_t count(std::string_view k) const { return find(k) == end() ? 0 : 1; }

        iterator       find(std::string_view k);
        const_iterator find(std::string_view k) const;
        Value&         operator[](std::string_view k);

    private:
        // Heterogeneous lookup: without it, every find with a const char* or a
        // string_view would build a std::string just to query.
        struct Hash {
            using is_transparent = void;
            size_t operator()(std::string_view s) const noexcept {
                return std::hash<std::string_view>{}(s);
            }
        };
        struct Equal {
            using is_transparent = void;
            bool operator()(std::string_view a, std::string_view b) const noexcept {
                return a == b;
            }
        };

        // Below the threshold there is no index: keeping it would cost more than
        // walking the whole vector.
        static constexpr size_t kIndexThreshold = 16;

        void build_index();

        std::vector<Pair>                                    v_;
        std::unordered_map<std::string, size_t, Hash, Equal> idx_;
    };

    Value() = default;
    ~Value() { release(); }

    Value(const Value& o) : type_(o.type_) { copy_payload(o); retain(); }
    Value(Value&& o) noexcept : type_(o.type_) { copy_payload(o); o.type_ = Type::Null; }

    Value& operator=(const Value& o) {
        if (this != &o) {
            o.retain();            // before release: it may be the same box
            release();
            type_ = o.type_;
            copy_payload(o);
        }
        return *this;
    }
    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            release();
            type_ = o.type_;
            copy_payload(o);
            o.type_ = Type::Null;
        }
        return *this;
    }

    static Value null()               { return Value(); }
    static Value boolean(bool b)      { Value v; v.type_ = Type::Bool;  v.b_ = b; return v; }
    static Value integer(long long i) { Value v; v.type_ = Type::Int;   v.i_ = i; return v; }
    static Value real(double d)       { Value v; v.type_ = Type::Float; v.d_ = d; return v; }

    static Value str(std::string s) {
        Value v; v.type_ = Type::Str;  v.o_ = new_box<CStr>(std::move(s));  return v;
    }
    static Value list(List l = {}) {
        Value v; v.type_ = Type::List; v.o_ = new_box<CList>(std::move(l)); return v;
    }
    static Value dict(Dict d = {}) {
        Value v; v.type_ = Type::Dict; v.o_ = new_box<CDict>(std::move(d)); return v;
    }

    Type type() const { return type_; }

    bool is_null()  const { return type_ == Type::Null; }
    bool is_bool()  const { return type_ == Type::Bool; }
    bool is_int()   const { return type_ == Type::Int; }
    bool is_float() const { return type_ == Type::Float; }
    bool is_num()   const { return is_int() || is_float(); }
    bool is_str()   const { return type_ == Type::Str; }
    bool is_list()  const { return type_ == Type::List; }
    bool is_dict()  const { return type_ == Type::Dict; }

    bool               as_bool()  const { return b_; }
    long long          as_int()   const { return i_; }
    double             as_float() const { return is_float() ? d_ : double(i_); }
    const std::string& as_str()   const { return static_cast<CStr*>(o_)->v;  }
    List&              as_list()  const { return static_cast<CList*>(o_)->v; }
    Dict&              as_dict()  const { return static_cast<CDict*>(o_)->v; }

    // Lumen Script truthiness, Python style: null, false, 0, 0.0, the empty
    // string and the empty containers are false.
    //
    // This is NOT the JavaScript coercion the README complains about: that one
    // converts types inside '==' behind your back.  There is no conversion here,
    // just a read in boolean context — the same one Python and C++ do, the two
    // languages Lumen Script comes from.
    bool truthy() const {
        switch (type_) {
            case Type::Null:  return false;
            case Type::Bool:  return b_;
            case Type::Int:   return i_ != 0;
            case Type::Float: return d_ != 0;
            case Type::Str:   return !as_str().empty();
            case Type::List:  return !as_list().empty();
            case Type::Dict:  return !as_dict().empty();
        }
        return false;
    }

    const char* type_name() const;
    std::string to_string() const;          // representation for text()/concatenation
    // Serialization: writes the JSON straight into the buffer, in one pass.
    void        write_json(std::string& out) const;
    std::string to_json_text() const { std::string s; write_json(s); return s; }

    // Parses JSON into a Value, with no intermediate tree.  Returns false for
    // anything that is not valid and complete JSON, trailing garbage included.
    // See src/lumen_script/json_parse.cpp.
    static bool parse_json(std::string_view text, Value& out);

    bool equals(const Value& o) const;

private:
    // Box with its own counter.  Atomic because a value can be born on a
    // database pool thread and be consumed on the event loop one: the handover
    // is synchronized, but the box can stay shared between the two.
    struct Box { std::atomic<unsigned> rc{1}; };
    template <typename T>
    struct BoxOf : Box { T v; explicit BoxOf(T x) : v(std::move(x)) {} };

    using CStr  = BoxOf<std::string>;
    using CList = BoxOf<List>;
    using CDict = BoxOf<Dict>;

    // Boxes go through the thread's recycler instead of new/delete.  The
    // reference count still decides when one dies: the only thing that changes
    // is where the memory goes afterwards.
    template <typename C, typename T>
    static Box* new_box(T x) {
#ifdef LUMEN_NO_RECYCLER
        return new C(std::move(x));
#else
        void* m = detail::recycler<sizeof(C)>().take();
        return new (m) C(std::move(x));
#endif
    }

    template <typename C>
    void release_box() {
#ifdef LUMEN_NO_RECYCLER
        delete static_cast<C*>(o_);
#else
        C* c = static_cast<C*>(o_);
        c->~C();
        detail::recycler<sizeof(C)>().give_back(c);
#endif
    }

    bool on_heap() const {
        return type_ == Type::Str || type_ == Type::List || type_ == Type::Dict;
    }
    void copy_payload(const Value& o) { std::memcpy(&i_, &o.i_, sizeof(i_)); }
    void retain() const {
        if (on_heap()) o_->rc.fetch_add(1, std::memory_order_relaxed);
    }
    void release() {
        if (!on_heap()) return;
        if (o_->rc.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        switch (type_) {
            case Type::Str:  release_box<CStr>();  break;
            case Type::List: release_box<CList>(); break;
            case Type::Dict: release_box<CDict>(); break;
            default: break;
        }
    }

    union {
        long long i_ = 0;
        bool      b_;
        double    d_;
        Box*     o_;
    };
    Type type_ = Type::Null;
};

// ─── Value::Dict ─────────────────────────────────────────────────────────────
// Outside the class because they need Value to be complete.

inline void Value::Dict::build_index() {
    idx_.clear();
    idx_.reserve(v_.size() * 2);
    for (size_t i = 0; i < v_.size(); ++i) idx_.emplace(v_[i].first, i);
}

inline Value::Dict::iterator Value::Dict::find(std::string_view k) {
    if (!idx_.empty()) {
        auto it = idx_.find(k);
        return it == idx_.end() ? v_.end()
                                : v_.begin() + static_cast<std::ptrdiff_t>(it->second);
    }
    for (auto it = v_.begin(); it != v_.end(); ++it)
        if (it->first == k) return it;
    return v_.end();
}

inline Value::Dict::const_iterator Value::Dict::find(std::string_view k) const {
    if (!idx_.empty()) {
        auto it = idx_.find(k);
        return it == idx_.end() ? v_.end()
                                : v_.begin() + static_cast<std::ptrdiff_t>(it->second);
    }
    for (auto it = v_.begin(); it != v_.end(); ++it)
        if (it->first == k) return it;
    return v_.end();
}

inline Value& Value::Dict::operator[](std::string_view k) {
    if (auto it = find(k); it != v_.end()) return it->second;

    v_.emplace_back(std::string(k), Value());
    if (!idx_.empty())            idx_.emplace(v_.back().first, v_.size() - 1);
    else if (v_.size() > kIndexThreshold) build_index();
    return v_.back().second;
}

} // namespace lumen_script
