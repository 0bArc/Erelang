// SPDX-License-Identifier: Apache-2.0
// Runtime Value and Handle types.

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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
    // Heap string when kind==String; otherwise null.
    std::string* str{nullptr};

    Value() : i(0) {}

    ~Value() { delete str; }

    Value(const Value& other) : kind(other.kind), i(0), str(nullptr) {
        switch (other.kind) {
            case ValueKind::Bool: b = other.b; break;
            case ValueKind::Int: i = other.i; break;
            case ValueKind::Float: f = other.f; break;
            case ValueKind::Handle: h = other.h; break;
            case ValueKind::String:
                if (other.str) str = new std::string(*other.str);
                break;
            case ValueKind::Null: i = 0; break;
        }
    }

    Value(Value&& other) noexcept : kind(other.kind), i(0), str(other.str) {
        switch (other.kind) {
            case ValueKind::Bool: b = other.b; break;
            case ValueKind::Int: i = other.i; break;
            case ValueKind::Float: f = other.f; break;
            case ValueKind::Handle: h = other.h; break;
            default: break;
        }
        other.str = nullptr;
        other.kind = ValueKind::Null;
        other.i = 0;
    }

    Value& operator=(const Value& other) {
        if (this == &other) return *this;
        Value tmp(other);
        *this = std::move(tmp);
        return *this;
    }

    Value& operator=(Value&& other) noexcept {
        if (this == &other) return *this;
        delete str;
        kind = other.kind;
        str = other.str;
        switch (other.kind) {
            case ValueKind::Bool: b = other.b; break;
            case ValueKind::Int: i = other.i; break;
            case ValueKind::Float: f = other.f; break;
            case ValueKind::Handle: h = other.h; break;
            default: i = 0; break;
        }
        other.str = nullptr;
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
            delete str;
            str = new std::string();
            kind = ValueKind::String;
        }
        return *str;
    }

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
        out.str = new std::string(std::move(v));
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

[[nodiscard]] inline uint32_t value_handle_id(const Value& v) {
    if (v.kind == ValueKind::Handle) return v.h.id;
    Handle h{};
    if (v.kind == ValueKind::String) (void)try_parse_legacy_handle(v.string_ref(), h);
    return h.id;
}

enum class ValueBinOp {
    Add, Sub, Mul, Div, Mod, Pow,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or, Coalesce
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

    AssignRef operator[](const std::string& key) { return AssignRef{data[key]}; }
    AssignRef operator[](std::string&& key) { return AssignRef{data[std::move(key)]}; }

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
    void clear() { data.clear(); }
    bool empty() const { return data.empty(); }
    std::size_t size() const { return data.size(); }
};

} // namespace erelang
