#include "erelang/features/serialization.hpp"
#include "erelang/runtime_internals.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace erelang::features {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    static constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(ch >> 4) & 0xF]);
                    out.push_back(kHex[ch & 0xF]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

namespace {

// JSON for one element: numbers, bools and null keep their kind, lists and maps nest, the rest is
// a quoted string.
void value_to_json(std::ostringstream& oss, const Value& v, int depth) {
    if (depth > 256) { oss << "null"; return; }
    switch (v.kind) {
        case ValueKind::Null: oss << "null"; return;
        case ValueKind::Bool: oss << (v.b ? "true" : "false"); return;
        case ValueKind::Int: oss << v.i; return;
        case ValueKind::Float: {
            if (v.f != v.f || v.f == 1.0 / 0.0 || v.f == -1.0 / 0.0) { oss << "null"; return; }
            std::ostringstream num;
            num.precision(17);
            num << v.f;
            oss << num.str();
            return;
        }
        default: break;
    }
    if (value_is_handle(v, HandleKind::List)) {
        const auto it = g_lists.find(static_cast<int>(v.h.id));
        oss << '[';
        if (it != g_lists.end()) {
            bool first = true;
            for (const auto& item : it->second) {
                if (!first) oss << ',';
                first = false;
                value_to_json(oss, item, depth + 1);
            }
        }
        oss << ']';
        return;
    }
    if (value_is_handle(v, HandleKind::Dict)) {
        const auto it = g_dicts.find(static_cast<int>(v.h.id));
        oss << '{';
        if (it != g_dicts.end()) {
            bool first = true;
            for (const auto& [key, value] : it->second) {
                if (!first) oss << ',';
                first = false;
                oss << '"' << json_escape(key) << "\":";
                value_to_json(oss, value, depth + 1);
            }
        }
        oss << '}';
        return;
    }
    oss << '"' << json_escape(to_display_string(v)) << '"';
}

int handle_number(std::string_view handle, std::size_t prefixLen) {
    try { return std::stoi(std::string(handle.substr(prefixLen))); } catch (...) { return -1; }
}

}  // namespace

std::string list_handle_to_json(std::string_view listHandle) {
    if (listHandle.rfind("list:", 0) != 0) return "[]";
    const int id = handle_number(listHandle, 5);
    if (id < 0 || g_lists.find(id) == g_lists.end()) return "[]";
    std::ostringstream oss;
    value_to_json(oss, make_handle_value(HandleKind::List, static_cast<uint32_t>(id)), 0);
    return oss.str();
}

std::string dict_handle_to_json(std::string_view dictHandle) {
    if (dictHandle.rfind("dict:", 0) != 0) return "{}";
    const int id = handle_number(dictHandle, 5);
    if (id < 0 || g_dicts.find(id) == g_dicts.end()) return "{}";
    std::ostringstream oss;
    value_to_json(oss, make_handle_value(HandleKind::Dict, static_cast<uint32_t>(id)), 0);
    return oss.str();
}

// Top-level JSON array: a list handle. Nested objects and arrays stay as their JSON text (pass them
// to from_json again), strings are unescaped, other scalars become typed values.
std::string from_json_array_to_list_handle(std::string_view json) {
    const std::string s(json);
    std::size_t i = 0;
    auto skip_ws = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i; };
    skip_ws();
    if (i >= s.size() || s[i] != '[') return {};
    ++i;
    std::vector<Value> items;
    skip_ws();
    if (i < s.size() && s[i] == ']') {
        ++i;
    } else {
        for (;;) {
            skip_ws();
            if (i >= s.size()) return {};
            const std::size_t start = i;
            if (s[i] == '{' || s[i] == '[') {
                int depth = 0;
                bool inString = false;
                while (i < s.size()) {
                    const char c = s[i++];
                    if (inString) {
                        if (c == '\\' && i < s.size()) ++i;
                        else if (c == '"') inString = false;
                    } else if (c == '"') {
                        inString = true;
                    } else if (c == '{' || c == '[') {
                        ++depth;
                    } else if (c == '}' || c == ']') {
                        if (--depth == 0) break;
                    }
                }
                if (depth != 0) return {};
                items.push_back(Value::from_string(s.substr(start, i - start)));
            } else if (s[i] == '"') {
                std::string text;
                ++i;
                bool closed = false;
                while (i < s.size()) {
                    const char c = s[i++];
                    if (c == '"') { closed = true; break; }
                    if (c != '\\' || i >= s.size()) { text.push_back(c); continue; }
                    const char e = s[i++];
                    switch (e) {
                        case 'n': text.push_back('\n'); break;
                        case 't': text.push_back('\t'); break;
                        case 'r': text.push_back('\r'); break;
                        case 'b': text.push_back('\b'); break;
                        case 'f': text.push_back('\f'); break;
                        default: text.push_back(e); break;
                    }
                }
                if (!closed) return {};
                items.push_back(Value::from_string(std::move(text)));
            } else {
                while (i < s.size() && s[i] != ',' && s[i] != ']' && std::isspace(static_cast<unsigned char>(s[i])) == 0) ++i;
                const std::string raw = s.substr(start, i - start);
                if (raw == "null") items.push_back(Value::null_value());
                else if (raw == "true" || raw == "false") items.push_back(Value::from_bool(raw == "true"));
                else items.push_back(value_from_legacy_string(raw));
            }
            skip_ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; break; }
            return {};
        }
    }
    skip_ws();
    if (i != s.size()) return {};
    const int id = g_nextListId++;
    g_lists[id] = std::move(items);
    return std::string("list:") + std::to_string(id);
}

std::string from_json_object_to_dict_handle(std::string_view json) {
    std::string s(json);
    std::size_t i = 0;
    auto skip_ws = [&]() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
    };
    auto parse_string = [&]() -> std::string {
        std::string out;
        if (i >= s.size() || s[i] != '"') return out;
        ++i;
        bool unterminated = true;
        while (i < s.size()) {
            char ch = s[i++];
            if (ch == '"') {
                unterminated = false;
                break;
            }
            if (ch == '\\' && i < s.size()) {
                char esc = s[i++];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '/': out.push_back('/'); break;
                    case 'u': {
                        if (i + 4 > s.size()) break;
                        unsigned int code = 0;
                        bool ok = true;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned int>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned int>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned int>(h - 'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok && code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (ok && code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else if (ok) {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(ch);
            }
        }
        if (unterminated) {
            return {};
        }
        return out;
    };

    skip_ws();
    if (i >= s.size() || s[i] != '{') return {};
    ++i;

    int id = g_nextDictId++;
    auto& dict = g_dicts[id];
    dict.clear();

    bool valid = true;
    skip_ws();
    if (i < s.size() && s[i] == '}') {
        ++i;
        return std::string("dict:") + std::to_string(id);
    }

    while (i < s.size()) {
        skip_ws();
        std::string key = parse_string();
        // A string that ended without a closing quote produces an empty key;
        // reject the whole object instead of committing a partial entry.
        if (key.empty() && (i >= s.size() || s[i - 1] != '"')) {
            valid = false;
            break;
        }

        skip_ws();
        if (i >= s.size() || s[i] != ':') {
            valid = false;
            break;
        }
        ++i;
        skip_ws();

        std::string value;
        if (i < s.size() && s[i] == '"') {
            value = parse_string();
            if (value.empty() && (i >= s.size() || s[i - 1] != '"')) {
                valid = false;
                break;
            }
        } else {
            std::size_t start = i;
            if (i < s.size() && (s[i] == '{' || s[i] == '[')) {
                // Nested object/array: keep the raw JSON text (from_json can parse it again).
                // Track depth and skip over string contents so braces inside text do not count.
                int depth = 0;
                bool inString = false;
                while (i < s.size()) {
                    const char c = s[i++];
                    if (inString) {
                        if (c == '\\' && i < s.size()) ++i;
                        else if (c == '"') inString = false;
                    } else if (c == '"') {
                        inString = true;
                    } else if (c == '{' || c == '[') {
                        ++depth;
                    } else if (c == '}' || c == ']') {
                        if (--depth == 0) break;
                    }
                }
                if (depth != 0) { valid = false; break; }
            } else {
                while (i < s.size() && s[i] != ',' && s[i] != '}') ++i;
            }
            value = s.substr(start, i - start);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.pop_back();
            std::size_t startTrim = 0;
            while (startTrim < value.size() && std::isspace(static_cast<unsigned char>(value[startTrim])) != 0) ++startTrim;
            value = value.substr(startTrim);
        }

        dict[key] = value;
        skip_ws();
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        if (i < s.size() && s[i] == '}') {
            ++i;
            break;
        }
        valid = false;
        break;
    }

    if (!valid) {
        // Roll back the partially-filled dict so callers never observe
        // a half-parsed object from malformed JSON.
        g_dicts.erase(id);
        return {};
    }

    return std::string("dict:") + std::to_string(id);
}

} // namespace erelang::features
