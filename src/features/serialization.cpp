#include "erelang/features/serialization.hpp"
#include "erelang/runtime_internals.hpp"

#include <cctype>
#include <sstream>
#include <string>

namespace erelang::features {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string list_handle_to_json(std::string_view listHandle) {
    if (listHandle.rfind("list:", 0) != 0) return "[]";
    int id = 0;
    try { id = std::stoi(std::string(listHandle.substr(5))); } catch (...) { return "[]"; }
    auto it = g_lists.find(id);
    if (it == g_lists.end()) return "[]";

    std::ostringstream oss;
    oss << '[';
    bool first = true;
    for (const auto& item : it->second) {
        if (!first) oss << ',';
        first = false;
        oss << '"' << json_escape(item) << '"';
    }
    oss << ']';
    return oss.str();
}

std::string dict_handle_to_json(std::string_view dictHandle) {
    if (dictHandle.rfind("dict:", 0) != 0) return "{}";
    int id = 0;
    try { id = std::stoi(std::string(dictHandle.substr(5))); } catch (...) { return "{}"; }
    auto it = g_dicts.find(id);
    if (it == g_dicts.end()) return "{}";

    std::ostringstream oss;
    oss << '{';
    bool first = true;
    for (const auto& [key, value] : it->second) {
        if (!first) oss << ',';
        first = false;
        oss << '"' << json_escape(key) << "\":\"" << json_escape(value) << '"';
    }
    oss << '}';
    return oss.str();
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
        while (i < s.size()) {
            char ch = s[i++];
            if (ch == '"') break;
            if (ch == '\\' && i < s.size()) {
                char esc = s[i++];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(ch);
            }
        }
        return out;
    };

    skip_ws();
    if (i >= s.size() || s[i] != '{') return {};
    ++i;

    int id = g_nextDictId++;
    auto& dict = g_dicts[id];
    dict.clear();

    skip_ws();
    if (i < s.size() && s[i] == '}') {
        ++i;
        return std::string("dict:") + std::to_string(id);
    }

    while (i < s.size()) {
        skip_ws();
        std::string key = parse_string();
        if (key.empty() && (i >= s.size() || s[i - 1] != '"')) break;

        skip_ws();
        if (i >= s.size() || s[i] != ':') break;
        ++i;
        skip_ws();

        std::string value;
        if (i < s.size() && s[i] == '"') {
            value = parse_string();
        } else {
            std::size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}') ++i;
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
        break;
    }

    return std::string("dict:") + std::to_string(id);
}

} // namespace erelang::features
