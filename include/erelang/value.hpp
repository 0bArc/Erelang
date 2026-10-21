// SPDX-License-Identifier: Apache-2.0
// Runtime Value and Handle types.

#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace erelang {

struct Value;

enum class ValueKind : uint8_t {
    Null = 0,
    Bool,
    Int,
    Float,
    String,
    Handle
};

enum class HandleKind : uint8_t {
    List = 0,
    Dict,
    File,
    Func,
    Ptr,
    StrBuf,
    Set,
    Queue,
    Ws,
    Http,
    Own,
    Shared,
    Weak,
    Buffer,
    Unknown
};

struct Handle {
    HandleKind kind{HandleKind::Unknown};
    uint32_t id{0};
};

[[nodiscard]] std::string to_display_string(const Value& v);
[[nodiscard]] Value value_from_legacy_string(std::string s);

struct Value {
    ValueKind kind{ValueKind::Null};
    union {
        bool b;
        int64_t i;
        double f;
        Handle h;
    };
    // String payload when kind==String; otherwise null. Shared between copies (copying a
    // Value bumps a refcount instead of duplicating the text); string_mut() unshares first.
    std::shared_ptr<std::string> str;

    Value() : i(0) {}

    Value(const Value& other) : kind(other.kind), i(0), str(other.str) { copy_scalar(other); }

    Value(Value&& other) noexcept : kind(other.kind), i(0), str(std::move(other.str)) {
        copy_scalar(other);
        other.kind = ValueKind::Null;
        other.i = 0;
    }

    Value& operator=(const Value& other) {
        if (this == &other) return *this;
        kind = other.kind;
        str = other.str;
        copy_scalar(other);
        return *this;
    }

    Value& operator=(Value&& other) noexcept {
        if (this == &other) return *this;
        kind = other.kind;
        str = std::move(other.str);
        copy_scalar(other);
        other.kind = ValueKind::Null;
        other.i = 0;
        return *this;
    }

    [[nodiscard]] const std::string& string_ref() const {
        static const std::string empty;
        if (kind == ValueKind::String && str) return *str;
        return empty;
    }

    std::string& string_mut() {
        if (kind != ValueKind::String || !str) {
            str = std::make_shared<std::string>();
            kind = ValueKind::String;
        } else if (str.use_count() > 1) {
            str = std::make_shared<std::string>(*str);
        }
        return *str;
    }

private:
    void copy_scalar(const Value& other) {
        switch (other.kind) {
            case ValueKind::Bool: b = other.b; break;
            case ValueKind::Int: i = other.i; break;
            case ValueKind::Float: f = other.f; break;
            case ValueKind::Handle: h = other.h; break;
            default: i = 0; break;
        }
    }

public:

    static Value null_value() { return Value{}; }
    static Value from_bool(bool v) {
        Value out;
        out.kind = ValueKind::Bool;
        out.b = v;
        return out;
    }
    static Value from_int(int64_t v) {
        Value out;
        out.kind = ValueKind::Int;
        out.i = v;
        return out;
    }
    static Value from_float(double v) {
        Value out;
        out.kind = ValueKind::Float;
        out.f = v;
        return out;
    }
    static Value from_string(std::string v) {
        Value out;
        out.kind = ValueKind::String;
        out.str = std::make_shared<std::string>(std::move(v));
        return out;
    }
    static Value from_handle(Handle handle) {
        Value out;
        out.kind = ValueKind::Handle;
        out.h = handle;
        return out;
    }

    // Converts via value_from_legacy_string.
    Value& operator=(std::string s) {
        *this = value_from_legacy_string(std::move(s));
        return *this;
    }
    Value& operator=(const char* s) {
        *this = value_from_legacy_string(s ? std::string(s) : std::string{});
        return *this;
    }
    operator std::string() const { return to_display_string(*this); }

    [[nodiscard]] std::size_t size() const { return to_display_string(*this).size(); }
    [[nodiscard]] bool empty() const { return to_display_string(*this).empty(); }
    [[nodiscard]] std::size_t rfind(std::string_view needle, std::size_t pos = std::string::npos) const {
        return to_display_string(*this).rfind(std::string(needle), pos);
    }
    [[nodiscard]] std::string substr(std::size_t pos, std::size_t count = std::string::npos) const {
        return to_display_string(*this).substr(pos, count);
    }
    char operator[](std::size_t i) const { return to_display_string(*this)[i]; }
};

inline bool operator==(const Value& v, std::string_view s) { return to_display_string(v) == s; }
inline bool operator!=(const Value& v, std::string_view s) { return !(v == s); }
inline bool operator==(const Value& a, const Value& b) {
    if (a.kind != b.kind) return to_display_string(a) == to_display_string(b);
    switch (a.kind) {
        case ValueKind::Null: return true;
        case ValueKind::Bool: return a.b == b.b;
        case ValueKind::Int: return a.i == b.i;
        case ValueKind::Float: return a.f == b.f;
        case ValueKind::String: return a.string_ref() == b.string_ref();
        case ValueKind::Handle: return a.h.kind == b.h.kind && a.h.id == b.h.id;
    }
    return false;
}
inline bool operator!=(const Value& a, const Value& b) { return !(a == b); }

template <typename CharT, typename Traits>
std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, const Value& v) {
    return os << to_display_string(v);
}

[[nodiscard]] bool is_truthy(const Value& v);
// to_display_string / value_from_legacy_string declared above Value.
[[nodiscard]] HandleKind handle_kind_from_prefix(std::string_view prefix);
[[nodiscard]] std::string_view handle_kind_prefix(HandleKind kind);
[[nodiscard]] Value make_handle_value(HandleKind kind, uint32_t id);
[[nodiscard]] bool try_parse_legacy_handle(std::string_view s, Handle& out);

// Typed handle checks (Handle kind, or legacy string tags).
[[nodiscard]] inline bool value_is_handle(const Value& v, HandleKind kind) {
    if (v.kind == ValueKind::Handle) return v.h.kind == kind;
    if (v.kind == ValueKind::String) {
        Handle h;
        if (try_parse_legacy_handle(v.string_ref(), h)) return h.kind == kind;
    }
    return false;
}

// Struct instances are bound as the marker string "struct:<Type>" (fields live in "<name>.<field>").
[[nodiscard]] inline bool value_is_struct_marker(const Value& v) {
    return v.kind == ValueKind::String && v.str && v.str->compare(0, 7, "struct:") == 0;
}

// True when a "<name>.<field>" key could belong to a variable holding v (struct marker,
// struct baked into a dict, or unknown). Scalars, lists, files etc. never carry fields.
[[nodiscard]] inline bool value_may_carry_fields(const Value* v) {
    if (!v) return true;
    switch (v->kind) {
        case ValueKind::Int: case ValueKind::Float: case ValueKind::Bool: return false;
        case ValueKind::Handle: return v->h.kind == HandleKind::Dict;
        default: return true;
    }
}

[[nodiscard]] inline uint32_t value_handle_id(const Value& v) {
    if (v.kind == ValueKind::Handle) return v.h.id;
    Handle h{};
    if (v.kind == ValueKind::String) (void)try_parse_legacy_handle(v.string_ref(), h);
    return h.id;
}

enum class ValueBinOp {
    Add, Sub, Mul, Div, Mod, Pow,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or, Coalesce,
    BitAnd, BitXor, BitOr, Shl, Shr, StrictEq, StrictNe
};
enum class ValueUnOp { Neg, Not, BitNot };

[[nodiscard]] Value apply_binary(ValueBinOp op, const Value& left, const Value& right);
[[nodiscard]] Value apply_unary(ValueUnOp op, const Value& operand);

[[nodiscard]] int64_t value_as_int(const Value& v);
[[nodiscard]] double value_as_float(const Value& v);
[[nodiscard]] bool value_as_bool(const Value& v);
[[nodiscard]] const std::string& value_as_string_ref(const Value& v);

[[nodiscard]] inline std::string legacy_string(const Value& v) { return to_display_string(v); }

// string assignment goes through value_from_legacy_string.
struct ValueMap {
    std::unordered_map<std::string, Value> data;
    // Keys created through operator[], in insertion order. Lets a block scope find the
    // names it introduced without snapshotting every key on entry (see exec_block).
    std::vector<std::string> insertLog;

    struct AssignRef {
        Value& ref;
        AssignRef& operator=(const AssignRef& other) {
            ref = other.ref;
            return *this;
        }
        AssignRef& operator=(std::string s) {
            ref = value_from_legacy_string(std::move(s));
            return *this;
        }
        AssignRef& operator=(const char* s) { return *this = std::string(s ? s : ""); }
        AssignRef& operator=(Value v) {
            ref = std::move(v);
            return *this;
        }
        operator Value&() { return ref; }
        operator const Value&() const { return ref; }
        Value* operator->() { return &ref; }
        const Value* operator->() const { return &ref; }
    };

    AssignRef operator[](const std::string& key) {
        auto [it, inserted] = data.try_emplace(key);
        if (inserted) insertLog.push_back(key);
        return AssignRef{it->second};
    }
    AssignRef operator[](std::string&& key) {
        auto [it, inserted] = data.try_emplace(std::move(key));
        if (inserted) insertLog.push_back(it->first);
        return AssignRef{it->second};
    }

    auto find(const std::string& key) { return data.find(key); }
    auto find(const std::string& key) const { return data.find(key); }
    auto begin() { return data.begin(); }
    auto end() { return data.end(); }
    auto begin() const { return data.begin(); }
    auto end() const { return data.end(); }
    auto cbegin() const { return data.cbegin(); }
    auto cend() const { return data.cend(); }
    std::size_t count(const std::string& key) const { return data.count(key); }
    std::size_t erase(const std::string& key) { return data.erase(key); }
    void clear() { data.clear(); insertLog.clear(); }
    bool empty() const { return data.empty(); }
    std::size_t size() const { return data.size(); }
};

} // namespace erelang
