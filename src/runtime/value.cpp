// SPDX-License-Identifier: Apache-2.0
#include "erelang/value.hpp"

#include <cmath>
#include <stdexcept>

namespace erelang {
namespace {

bool is_int_digits(std::string_view s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

bool is_float_digits(std::string_view s) {
    if (s.empty()) return false;
    bool seenDot = false;
    bool seenDigit = false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') { seenDigit = true; continue; }
        if (c == '.' && !seenDot) { seenDot = true; continue; }
        return false;
    }
    return seenDigit && seenDot;
}

} // namespace

bool is_truthy(const Value& v) {
    switch (v.kind) {
        case ValueKind::Null: return false;
        case ValueKind::Bool: return v.b;
        case ValueKind::Int: return v.i != 0;
        case ValueKind::Float: return v.f != 0.0;
        case ValueKind::String: {
            const std::string& s = v.string_ref();
            return !s.empty() && s != "false" && s != "0";
        }
        case ValueKind::Handle: return true;
    }
    return false;
}

std::string to_display_string(const Value& v) {
    switch (v.kind) {
        case ValueKind::Null: return "nullptr";
        case ValueKind::Bool: return v.b ? "true" : "false";
        case ValueKind::Int: return std::to_string(v.i);
        case ValueKind::Float: return std::to_string(v.f);
        case ValueKind::String: return v.string_ref();
        case ValueKind::Handle: {
            std::string out(handle_kind_prefix(v.h.kind));
            out += std::to_string(v.h.id);
            return out;
        }
    }
    return {};
}

HandleKind handle_kind_from_prefix(std::string_view prefix) {
    if (prefix == "list") return HandleKind::List;
    if (prefix == "dict") return HandleKind::Dict;
    if (prefix == "file") return HandleKind::File;
    if (prefix == "func") return HandleKind::Func;
    if (prefix == "ptr") return HandleKind::Ptr;
    if (prefix == "strbuf") return HandleKind::StrBuf;
    if (prefix == "set") return HandleKind::Set;
    if (prefix == "queue") return HandleKind::Queue;
    if (prefix == "ws") return HandleKind::Ws;
    if (prefix == "http" || prefix == "req" || prefix == "res" || prefix == "resp") return HandleKind::Http;
    return HandleKind::Unknown;
}

std::string_view handle_kind_prefix(HandleKind kind) {
    switch (kind) {
        case HandleKind::List: return "list:";
        case HandleKind::Dict: return "dict:";
        case HandleKind::File: return "file:";
        case HandleKind::Func: return "func:";
        case HandleKind::Ptr: return "ptr:";
        case HandleKind::StrBuf: return "strbuf:";
        case HandleKind::Set: return "set:";
        case HandleKind::Queue: return "queue:";
        case HandleKind::Ws: return "ws:";
        case HandleKind::Http: return "http:";
        case HandleKind::Unknown: return "handle:";
    }
    return "handle:";
}

bool try_parse_legacy_handle(std::string_view s, Handle& out) {
    const auto colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const auto prefix = s.substr(0, colon);
    const auto idPart = s.substr(colon + 1);
    if (idPart.empty()) return false;
    for (char c : idPart) {
        if (c < '0' || c > '9') return false;
    }
    HandleKind hk = handle_kind_from_prefix(prefix);
    if (hk == HandleKind::Unknown && prefix != "handle") {
        if (prefix != "tcp" && prefix != "sse" && prefix != "ref") return false;
    }
    try {
        out.kind = hk;
        out.id = static_cast<uint32_t>(std::stoul(std::string(idPart)));
        return true;
    } catch (...) {
        return false;
    }
}

Value make_handle_value(HandleKind kind, uint32_t id) {
    return Value::from_handle(Handle{kind, id});
}

Value value_from_legacy_string(std::string s) {
    if (s == "true") return Value::from_bool(true);
    if (s == "false") return Value::from_bool(false);
    if (s == "nullptr" || s == "null") return Value::null_value();
    Handle h;
    if (try_parse_legacy_handle(s, h)) return Value::from_handle(h);
    if (is_int_digits(s)) {
        try { return Value::from_int(std::stoll(s)); } catch (...) {}
    }
    if (is_float_digits(s)) {
        try { return Value::from_float(std::stod(s)); } catch (...) {}
    }
    return Value::from_string(std::move(s));
}

int64_t value_as_int(const Value& v) {
    switch (v.kind) {
        case ValueKind::Int: return v.i;
        case ValueKind::Bool: return v.b ? 1 : 0;
        case ValueKind::Float: return static_cast<int64_t>(v.f);
        case ValueKind::String: {
            try { return std::stoll(v.string_ref()); } catch (...) { return 0; }
        }
        case ValueKind::Handle: return static_cast<int64_t>(v.h.id);
        case ValueKind::Null: return 0;
    }
    return 0;
}

double value_as_float(const Value& v) {
    switch (v.kind) {
        case ValueKind::Float: return v.f;
        case ValueKind::Int: return static_cast<double>(v.i);
        case ValueKind::Bool: return v.b ? 1.0 : 0.0;
        case ValueKind::String: {
            try { return std::stod(v.string_ref()); } catch (...) { return 0.0; }
        }
        default: return static_cast<double>(value_as_int(v));
    }
}

bool value_as_bool(const Value& v) { return is_truthy(v); }

const std::string& value_as_string_ref(const Value& v) {
    return v.string_ref();
}

static bool both_numeric(const Value& a, const Value& b) {
    auto num = [](ValueKind k) {
        return k == ValueKind::Int || k == ValueKind::Float || k == ValueKind::Bool;
    };
    return num(a.kind) && num(b.kind);
}

static bool any_float(const Value& a, const Value& b) {
    return a.kind == ValueKind::Float || b.kind == ValueKind::Float;
}

static bool is_stringy(const Value& v) {
    return v.kind == ValueKind::String || v.kind == ValueKind::Handle;
}

Value apply_binary(ValueBinOp op, const Value& left, const Value& right) {
    switch (op) {
        case ValueBinOp::And:
            return Value::from_bool(is_truthy(left) && is_truthy(right));
        case ValueBinOp::Or:
            return Value::from_bool(is_truthy(left) || is_truthy(right));
        case ValueBinOp::Coalesce:
            return (left.kind == ValueKind::Null) ? right : left;
        case ValueBinOp::Eq:
            if (left.kind == ValueKind::String && right.kind == ValueKind::String)
                return Value::from_bool(left.string_ref() == right.string_ref());
            if (both_numeric(left, right)) {
                if (any_float(left, right))
                    return Value::from_bool(value_as_float(left) == value_as_float(right));
                return Value::from_bool(value_as_int(left) == value_as_int(right));
            }
            return Value::from_bool(to_display_string(left) == to_display_string(right));
        case ValueBinOp::Ne: {
            Value eq = apply_binary(ValueBinOp::Eq, left, right);
            return Value::from_bool(!eq.b);
        }
        case ValueBinOp::Lt:
        case ValueBinOp::Le:
        case ValueBinOp::Gt:
        case ValueBinOp::Ge: {
            if (both_numeric(left, right)) {
                if (any_float(left, right)) {
                    const double a = value_as_float(left), b = value_as_float(right);
                    if (op == ValueBinOp::Lt) return Value::from_bool(a < b);
                    if (op == ValueBinOp::Le) return Value::from_bool(a <= b);
                    if (op == ValueBinOp::Gt) return Value::from_bool(a > b);
                    return Value::from_bool(a >= b);
                }
                const int64_t a = value_as_int(left), b = value_as_int(right);
                if (op == ValueBinOp::Lt) return Value::from_bool(a < b);
                if (op == ValueBinOp::Le) return Value::from_bool(a <= b);
                if (op == ValueBinOp::Gt) return Value::from_bool(a > b);
                return Value::from_bool(a >= b);
            }
            const std::string a = to_display_string(left), b = to_display_string(right);
            if (op == ValueBinOp::Lt) return Value::from_bool(a < b);
            if (op == ValueBinOp::Le) return Value::from_bool(a <= b);
            if (op == ValueBinOp::Gt) return Value::from_bool(a > b);
            return Value::from_bool(a >= b);
        }
        case ValueBinOp::Add: {
            if (is_stringy(left) || is_stringy(right) ||
                left.kind == ValueKind::String || right.kind == ValueKind::String) {
                if (left.kind == ValueKind::String || right.kind == ValueKind::String ||
                    left.kind == ValueKind::Handle || right.kind == ValueKind::Handle ||
                    !both_numeric(left, right)) {
                    std::string a = (left.kind == ValueKind::String) ? left.string_ref() : to_display_string(left);
                    std::string b = (right.kind == ValueKind::String) ? right.string_ref() : to_display_string(right);
                    std::string out;
                    out.reserve(a.size() + b.size());
                    out.append(a);
                    out.append(b);
                    return Value::from_string(std::move(out));
                }
            }
            if (both_numeric(left, right)) {
                if (any_float(left, right))
                    return Value::from_float(value_as_float(left) + value_as_float(right));
                return Value::from_int(value_as_int(left) + value_as_int(right));
            }
            std::string a = to_display_string(left), b = to_display_string(right);
            std::string out;
            out.reserve(a.size() + b.size());
            out.append(a);
            out.append(b);
            return Value::from_string(std::move(out));
        }
        case ValueBinOp::Sub:
        case ValueBinOp::Mul:
        case ValueBinOp::Div:
        case ValueBinOp::Mod:
        case ValueBinOp::Pow: {
            if (!both_numeric(left, right) && left.kind != ValueKind::String && right.kind != ValueKind::String) {
            }
            if (any_float(left, right) || left.kind == ValueKind::Float || right.kind == ValueKind::Float ||
                (left.kind == ValueKind::String && is_float_digits(left.string_ref())) ||
                (right.kind == ValueKind::String && is_float_digits(right.string_ref()))) {
                const double a = value_as_float(left), b = value_as_float(right);
                if (op == ValueBinOp::Sub) return Value::from_float(a - b);
                if (op == ValueBinOp::Mul) return Value::from_float(a * b);
                if (op == ValueBinOp::Div) {
                    if (b == 0.0) throw std::runtime_error("Division by zero");
                    return Value::from_float(a / b);
                }
                if (op == ValueBinOp::Mod) {
                    if (b == 0.0) throw std::runtime_error("Modulo by zero");
                    return Value::from_float(std::fmod(a, b));
                }
                return Value::from_float(std::pow(a, b));
            }
            const int64_t a = value_as_int(left), b = value_as_int(right);
            if (op == ValueBinOp::Sub) return Value::from_int(a - b);
            if (op == ValueBinOp::Mul) return Value::from_int(a * b);
            if (op == ValueBinOp::Div) {
                if (b == 0) throw std::runtime_error("Division by zero");
                return Value::from_int(a / b);
            }
            if (op == ValueBinOp::Mod) {
                if (b == 0) throw std::runtime_error("Modulo by zero");
                return Value::from_int(a % b);
            }
            if (b < 0) return Value::from_int(0);
            if (b > 62) throw std::runtime_error("Exponent too large for integer power");
            int64_t value = 1;
            for (int64_t i = 0; i < b; ++i) value *= a;
            return Value::from_int(value);
        }
        case ValueBinOp::BitAnd:
            return Value::from_int(value_as_int(left) & value_as_int(right));
        case ValueBinOp::BitOr:
            return Value::from_int(value_as_int(left) | value_as_int(right));
        case ValueBinOp::BitXor:
            return Value::from_int(value_as_int(left) ^ value_as_int(right));
        case ValueBinOp::Shl:
        case ValueBinOp::Shr: {
            const int64_t a = value_as_int(left);
            const int64_t b = value_as_int(right);
            if (b < 0 || b >= 64) throw std::runtime_error("Shift amount out of range");
            const auto shift = static_cast<unsigned>(b);
            if (op == ValueBinOp::Shl) return Value::from_int(a << shift);
            return Value::from_int(a >> shift);
        }
        case ValueBinOp::StrictEq:
            if (left.kind != right.kind) return Value::from_bool(false);
            return apply_binary(ValueBinOp::Eq, left, right);
        case ValueBinOp::StrictNe: {
            Value eq = apply_binary(ValueBinOp::StrictEq, left, right);
            return Value::from_bool(!eq.b);
        }
    }
    return Value::null_value();
}

Value apply_unary(ValueUnOp op, const Value& operand) {
    switch (op) {
        case ValueUnOp::Not:
            return Value::from_bool(!is_truthy(operand));
        case ValueUnOp::Neg:
            if (operand.kind == ValueKind::Float)
                return Value::from_float(-operand.f);
            return Value::from_int(-value_as_int(operand));
        case ValueUnOp::BitNot:
            return Value::from_int(~value_as_int(operand));
    }
    return Value::null_value();
}

} // namespace erelang
