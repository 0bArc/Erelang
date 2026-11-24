// builtin/json: a full JSON parser and printer.
//
//   json.parse(text)          -> map, array or plain value. Objects become map handles, arrays list
//                                handles, numbers int/float, true/false bool, null null.
//   json.stringify(v, indent?) -> JSON text of a map/list handle or a plain value.
//   json.valid(text)          -> true when text is one complete JSON value.
//   json.encode(s) / decode(s) -> quote and unquote one string.
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/value.hpp"

namespace erelang {

namespace {

class JsonParser {
public:
    // Holds a view: `text` must outlive the parser.
    explicit JsonParser(std::string_view text) : s_(text) {}

    bool parse(Value& out) {
        skip_ws();
        if (!value(out, 0)) return false;
        skip_ws();
        return pos_ == s_.size();
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool number(Value& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        bool isFloat = false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            isFloat = true;
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            isFloat = true;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        const std::string text(s_.substr(start, pos_ - start));
        try {
            if (!isFloat) {
                out = Value::from_int(std::stoll(text));
                return true;
            }
        } catch (...) { /* too large for int64: fall through to double */ }
        try { out = Value::from_float(std::stod(text)); } catch (...) { return false; }
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > 512) return false;
        skip_ws();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            std::unordered_map<std::string, Value> obj;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; }
            else {
                for (;;) {
                    skip_ws();
                    std::string key;
                    if (!string(key)) return false;
                    skip_ws();
                    if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                    ++pos_;
                    Value v;
                    if (!value(v, depth + 1)) return false;
                    obj[std::move(key)] = std::move(v);
                    skip_ws();
                    if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                    if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; break; }
                    return false;
                }
            }
            const int id = g_nextDictId++;
            g_dicts[id] = std::move(obj);
            out = make_handle_value(HandleKind::Dict, static_cast<uint32_t>(id));
            return true;
        }
        if (c == '[') {
            ++pos_;
            std::vector<Value> arr;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; }
            else {
                for (;;) {
                    Value v;
                    if (!value(v, depth + 1)) return false;
                    arr.push_back(std::move(v));
                    skip_ws();
                    if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                    if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; break; }
                    return false;
                }
            }
            const int id = g_nextListId++;
            g_lists[id] = std::move(arr);
            out = make_handle_value(HandleKind::List, static_cast<uint32_t>(id));
            return true;
        }
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = Value::from_string(std::move(str));
            return true;
        }
        if (literal("true")) { out = Value::from_bool(true); return true; }
        if (literal("false")) { out = Value::from_bool(false); return true; }
        if (literal("null")) { out = Value::null_value(); return true; }
        return number(out);
    }
};

void escape_into(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void newline(std::string& out, int indent, int depth) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}

void stringify(std::string& out, const Value& v, int indent, int depth) {
    if (depth > 512) { out += "null"; return; }
    switch (v.kind) {
        case ValueKind::Null: out += "null"; return;
        case ValueKind::Bool: out += v.b ? "true" : "false"; return;
        case ValueKind::Int: out += std::to_string(v.i); return;
        case ValueKind::Float: {
            if (!std::isfinite(v.f)) { out += "null"; return; }
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", v.f);
            out += buf;
            return;
        }
        default: break;
    }
    if (value_is_handle(v, HandleKind::Dict)) {
        const auto it = g_dicts.find(static_cast<int>(v.h.id));
        if (it == g_dicts.end()) { out += "{}"; return; }
        // Stable output: keys sorted.
        std::vector<const std::string*> keys;
        keys.reserve(it->second.size());
        for (const auto& kv : it->second) keys.push_back(&kv.first);
        std::sort(keys.begin(), keys.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
        out += '{';
        bool first = true;
        for (const std::string* k : keys) {
            if (!first) out += ',';
            first = false;
            newline(out, indent, depth + 1);
            escape_into(out, *k);
            out += indent > 0 ? ": " : ":";
            stringify(out, it->second.at(*k), indent, depth + 1);
        }
        if (!first) newline(out, indent, depth);
        out += '}';
        return;
    }
    if (value_is_handle(v, HandleKind::List)) {
        const auto it = g_lists.find(static_cast<int>(v.h.id));
        if (it == g_lists.end()) { out += "[]"; return; }
        out += '[';
        bool first = true;
        for (const auto& item : it->second) {
            if (!first) out += ',';
            first = false;
            newline(out, indent, depth + 1);
            stringify(out, item, indent, depth + 1);
        }
        if (!first) newline(out, indent, depth);
        out += ']';
        return;
    }
    escape_into(out, to_display_string(v));
}

// Builtin args arrive as display text; handles ("dict:3", "list:4") come back to typed values.
Value arg_value(const std::string& s) {
    return value_from_legacy_string(s);
}

}  // namespace

std::string __erelang_builtin_json_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    const auto argS = [&](std::size_t i) { return i < argv.size() ? argv[i] : std::string(); };
    if (name == "json_parse") {
        Value out;
        const std::string text = argS(0);  // the parser keeps a view: the text must outlive it
        JsonParser p(text);
        if (!p.parse(out)) throw std::runtime_error("json.parse: invalid JSON");
        if (out.kind == ValueKind::Null) return "null";
        return to_display_string(out);
    }
    if (name == "json_valid") {
        Value out;
        const std::string text = argS(0);
        JsonParser p(text);
        return p.parse(out) ? "true" : "false";
    }
    if (name == "json_stringify") {
        int indent = 0;
        if (argv.size() > 1) {
            try { indent = std::stoi(argS(1)); } catch (...) { indent = 0; }
        }
        std::string out;
        stringify(out, arg_value(argS(0)), indent, 0);
        return out;
    }
    if (name == "json_encode") {
        std::string out;
        escape_into(out, argS(0));
        return out;
    }
    if (name == "json_decode") {
        const std::string s = argS(0);
        std::string out;
        JsonParser p(s);
        Value v;
        if (!s.empty() && s.front() == '"' && p.parse(v)) return to_display_string(v);
        return s;
    }
    return {};
}

}  // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("json",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_json_dispatch(name, argv);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(json)
