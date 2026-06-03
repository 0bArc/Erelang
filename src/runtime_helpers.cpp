// SPDX-License-Identifier: Apache-2.0
// Shared runtime helpers and global container state.

#include "erelang/runtime_helpers.hpp"
#include "erelang/parser.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace erelang {
namespace fs = std::filesystem;

int g_nextListId = 1;
std::unordered_map<int, std::vector<std::string>> g_lists;
int g_nextDictId = 1;
std::unordered_map<int, std::unordered_map<std::string, std::string>> g_dicts;
int g_nextPtrId = 1;
std::unordered_map<int, std::string> g_ptrs;
int g_nextFileId = 1;
std::unordered_map<int, std::unique_ptr<std::fstream>> g_fileStreams;
int g_nextStrBufId = 1;
std::unordered_map<int, std::string> g_strBuffers;
std::unordered_set<std::string> g_deprecationWarningsShown;
int g_nextSetId = 1;
std::unordered_map<int, std::unordered_set<std::string>> g_sets;
int g_nextQueueId = 1;
std::unordered_map<int, std::deque<std::string>> g_queues;

std::string slurp_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim_copy(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string{value.substr(begin, end - begin + 1)};
}

std::string join_strings(std::vector<std::string> items, char separator) {
    if (items.empty()) {
        return {};
    }
    std::sort(items.begin(), items.end());
    std::ostringstream oss;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) {
            oss << separator;
        }
        oss << items[i];
    }
    return oss.str();
}

std::pair<std::string, std::string> split_core_query(const std::string& query) {
    const auto trimmed = trim_copy(query);
    if (trimmed.empty()) {
        return {"", ""};
    }
    auto pos = trimmed.find(':');
    if (pos == std::string::npos) pos = trimmed.find('.');
    if (pos == std::string::npos) pos = trimmed.find('/');
    if (pos == std::string::npos) {
        return {"", trimmed};
    }
    auto left = trim_copy(trimmed.substr(0, pos));
    auto right = trim_copy(trimmed.substr(pos + 1));
    return {left, right};
}

int64_t to_int(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return 0;
    }
}

std::string format_pointer_handle(int id) {
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << static_cast<unsigned int>(id);
    return oss.str();
}

std::optional<int> parse_pointer_handle(const std::string& handle) {
    if (handle.rfind("ptr:", 0) == 0) {
        return static_cast<int>(to_int(handle.substr(4)));
    }
    if (handle.size() > 2 && handle[0] == '0' && (handle[1] == 'x' || handle[1] == 'X')) {
        try {
            std::size_t idx = 0;
            unsigned long parsed = std::stoul(handle, &idx, 16);
            if (idx == handle.size()) return static_cast<int>(parsed);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::filesystem::path path_from_u8(const std::string& s) {
    const auto* first = reinterpret_cast<const char8_t*>(s.data());
    const auto* last = first + s.size();
    return std::filesystem::path(std::u8string(first, last));
}

std::filesystem::path infer_entry_script_directory(const Program& program) {
    auto parent_of = [](const std::string& sourcePath) -> std::filesystem::path {
        if (sourcePath.empty()) {
            return {};
        }
        return std::filesystem::path(sourcePath).parent_path();
    };
    const std::string entry = program.runTarget.value_or("main");
    for (const auto& action : program.actions) {
        if (action.name == entry && !action.sourcePath.empty()) {
            return parent_of(action.sourcePath);
        }
    }
    for (const auto& action : program.actions) {
        if (action.name == "main" && !action.sourcePath.empty()) {
            return parent_of(action.sourcePath);
        }
    }
    for (const auto& action : program.actions) {
        if (!action.sourcePath.empty()) {
            return parent_of(action.sourcePath);
        }
    }
    return {};
}

std::filesystem::path resolve_filesystem_path(
    const std::string& raw,
    const std::filesystem::path& scriptDirectory) {
    std::filesystem::path p = path_from_u8(raw);
    if (p.is_absolute()) {
        return p.lexically_normal();
    }
    if (!scriptDirectory.empty()) {
        return (scriptDirectory / p).lexically_normal();
    }
    return (fs::current_path() / p).lexically_normal();
}

double to_double(const std::string& s) {
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

bool is_float_string(const std::string& s) {
    if (s.empty()) return false;
    bool hasPoint = false;
    bool hasExp = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.') hasPoint = true;
        if (c == 'e' || c == 'E') hasExp = true;
        if ((c == '+' || c == '-') && i != 0 && !(s[i - 1] == 'e' || s[i - 1] == 'E')) return false;
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E')) return false;
    }
    if (!hasPoint && !hasExp) return false;
    try {
        std::size_t idx;
        std::stod(s, &idx);
        return idx == s.size();
    } catch (...) {
        return false;
    }
}

std::string normalize_runtime_type_name(std::string typeName) {
    std::string out;
    out.reserve(typeName.size());
    for (char ch : typeName) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

bool parse_runtime_array_type(const std::string& typeName, std::string& elementType) {
    constexpr const char* prefix = "array<";
    if (typeName.rfind(prefix, 0) != 0 || typeName.back() != '>') return false;
    elementType = typeName.substr(6, typeName.size() - 7);
    return !elementType.empty();
}

bool parse_runtime_map_type(const std::string& typeName, std::string& keyType, std::string& valueType) {
    constexpr const char* prefix = "map<";
    if (typeName.rfind(prefix, 0) != 0 || typeName.back() != '>') return false;
    const std::string inner = typeName.substr(4, typeName.size() - 5);
    int depth = 0;
    for (size_t index = 0; index < inner.size(); ++index) {
        const char ch = inner[index];
        if (ch == '<') {
            ++depth;
            continue;
        }
        if (ch == '>') {
            --depth;
            continue;
        }
        if (ch == ',' && depth == 0) {
            keyType = inner.substr(0, index);
            valueType = inner.substr(index + 1);
            return !keyType.empty() && !valueType.empty();
        }
    }
    return false;
}

bool runtime_generic_compatible(const std::string& expected, const std::string& actual) {
    if (expected == "any" || expected == "unknown") return true;
    if (actual == "any" || actual == "unknown") return true;
    return expected == actual;
}

bool runtime_declared_type_matches(const std::string& declaredTypeRaw, const std::string& actualTypeRaw) {
    const std::string declaredType = normalize_runtime_type_name(declaredTypeRaw);
    const std::string actualType = normalize_runtime_type_name(actualTypeRaw);
    if (declaredType.empty() || declaredType == "auto" || declaredType == "any") return true;
    if (declaredType == actualType) return true;

    if (declaredType == "string" || declaredType == "str" || declaredType == "char") {
        return actualType == "string";
    }
    if (declaredType == "bool") return actualType == "bool";
    if (declaredType == "int" || declaredType == "u8" || declaredType == "u16" || declaredType == "u32" || declaredType == "u64" ||
        declaredType == "i8" || declaredType == "i16" || declaredType == "i32" || declaredType == "i64" ||
        declaredType == "uint" || declaredType == "unsigned" || declaredType == "unsignedint") {
        return actualType == "int";
    }
    if (declaredType == "double" || declaredType == "float") {
        return actualType == "double" || actualType == "int";
    }

    if (declaredType == "array") return actualType.rfind("array", 0) == 0;
    std::string declaredArrayElem;
    if (parse_runtime_array_type(declaredType, declaredArrayElem)) {
        std::string actualArrayElem;
        if (!parse_runtime_array_type(actualType, actualArrayElem)) return false;
        return runtime_generic_compatible(declaredArrayElem, actualArrayElem);
    }

    if (declaredType == "map" || declaredType == "dictionary") return actualType.rfind("map", 0) == 0;
    std::string declaredMapKey;
    std::string declaredMapValue;
    if (parse_runtime_map_type(declaredType, declaredMapKey, declaredMapValue)) {
        std::string actualMapKey;
        std::string actualMapValue;
        if (!parse_runtime_map_type(actualType, actualMapKey, actualMapValue)) return false;
        return runtime_generic_compatible(declaredMapKey, actualMapKey) && runtime_generic_compatible(declaredMapValue, actualMapValue);
    }

    return true;
}

bool is_int_string(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool is_truthy(const std::string& v) {
    if (v == "true") return true;
    if (v == "false") return false;
    return to_int(v) != 0 || !v.empty();
}

bool is_identifier_text(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    auto is_alpha_or_underscore = [](char ch) {
        return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
    };
    auto is_alnum_or_underscore = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    };
    if (!is_alpha_or_underscore(text.front())) {
        return false;
    }
    for (size_t i = 1; i < text.size(); ++i) {
        if (!is_alnum_or_underscore(text[i])) {
            return false;
        }
    }
    return true;
}

const StructDecl* find_struct_decl(const Program& program, std::string_view name) {
    for (const auto& s : program.structs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const Action* find_struct_method(const StructDecl& decl, std::string_view name) {
    for (const auto& m : decl.methods) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

} // namespace erelang
