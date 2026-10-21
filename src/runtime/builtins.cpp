// SPDX-License-Identifier: Apache-2.0
// Split from runtime.cpp

#include "erelang/runtime_builtins.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/runtime_imports.hpp"
#include "erelang/parser.hpp"
#include "erelang/runtime.hpp"
#include "erelang/version.hpp"
#include "erelang/features/serialization.hpp"
#include "erelang/mini_ir_vm.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <rpc.h>
#endif

namespace erelang {

std::string __erelang_builtin_math_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_system_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_data_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_perm_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_network_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_crypto_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_regex_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_binary_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_performance_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_threads_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_monitor_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv);

std::string Runtime::eval_builtin_call(std::string_view name, const std::vector<ExprPtr>& args, const Runtime::Env& env, bool /*allowCollectionHelpers*/) const {
    // Local owning string for APIs expecting std::string
    std::string nameStr(name);
    auto warn_deprecated = [&](const char* builtin, const char* replacement) {
        const std::string key = std::string("deprecated:") + builtin;
        if (g_deprecationWarningsShown.insert(key).second) {
            std::cerr << "[warn] deprecated builtin '" << builtin << "' (prefer " << replacement << ")\n";
        }
    };
    // Legacy spellings -> canonical builtin names (one hash lookup instead of 33 compares).
    static const std::unordered_map<std::string_view, std::string_view> kBuiltinRenames = {
        {"bitcast", "bit_cast"},
        {"fopen", "file_open"},
        {"fclose", "file_close"},
        {"fread", "file_read"},
        {"fs.write", "write_text"},
        {"fs.append", "append_text"},
        {"fs.read", "read_text"},
        {"fs.exists", "file_exists"},
        {"fwrite", "file_write"},
        {"fseek", "file_seek"},
        {"ftell", "file_tell"},
        {"fflush", "file_flush"},
        {"string_buffer_new", "strbuf_new"},
        {"string_buffer_append", "strbuf_append"},
        {"string_buffer_clear", "strbuf_clear"},
        {"string_buffer_len", "strbuf_len"},
        {"string_buffer_to_string", "strbuf_to_string"},
        {"string_buffer_free", "strbuf_free"},
        {"string_buffer_reserve", "strbuf_reserve"},
        {"hashmap_new", "dict_new"},
        {"hashmap_set", "dict_set"},
        {"hashmap_put", "dict_set"},
        {"hashmap_get", "dict_get"},
        {"hashmap_has", "dict_has"},
        {"hashmap_contains", "dict_has"},
        {"hashmap_get_or", "dict_get_or"},
        {"hashmap_get_or_default", "dict_get_or"},
        {"hashmap_remove", "dict_remove"},
        {"hashmap_clear", "dict_clear"},
        {"hashmap_size", "dict_size"},
        {"hashmap_keys", "dict_keys"},
        {"hashmap_values", "dict_values"},
        {"hashmap_merge", "dict_merge"},
    };
    if (auto rn = kBuiltinRenames.find(nameStr); rn != kBuiltinRenames.end()) nameStr = std::string(rn->second);
    // Check both local and global vars for alias binding
    if (auto aliasIt = env.vars.find(nameStr); aliasIt != env.vars.end()) {
        const std::string& aliasTarget = aliasIt->second;
        if (aliasTarget.rfind(kBuiltinAliasPrefix.data(), 0) == 0) {
            nameStr = aliasTarget.substr(kBuiltinAliasPrefix.size());
        }
    } else if (auto aliasIt = globalVars_.find(nameStr); aliasIt != globalVars_.end()) {
        const std::string& aliasTarget = aliasIt->second;
        if (aliasTarget.rfind(kBuiltinAliasPrefix.data(), 0) == 0) {
            nameStr = aliasTarget.substr(kBuiltinAliasPrefix.size());
        }
    }

    // Script module alias resolution: alias.action(...) -> action(...)
    // Example: #include <modules/math.elan> as math ; math.sum(2,3) -> sum(2,3)
    if (currentProgram_) {
        const auto dot = nameStr.find('.');
        if (dot != std::string::npos && dot > 0 && dot + 1 < nameStr.size()) {
            const std::string alias = nameStr.substr(0, dot);
            const std::string method = nameStr.substr(dot + 1);
            for (const auto& importDecl : currentProgram_->imports) {
                if (!importDecl.alias || *importDecl.alias != alias || importDecl.path.empty()) continue;
                std::string normalizedPath = importDecl.path;
                for (auto& ch : normalizedPath) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const bool isBuiltin = (normalizedPath.rfind("builtin/", 0) == 0 || normalizedPath.rfind("builtin:", 0) == 0);
                if (isBuiltin) continue;
                if (find_action(*currentProgram_, method)) {
                    nameStr = method;
                }
                break;
            }
        }
    }

    auto argS = [&](size_t i){ return i < args.size() ? eval_string(*args[i], env) : std::string(); };
    auto argV = [&](size_t i) -> Value {
        return i < args.size() ? eval_value(*args[i], env) : Value::null_value();
    };
    auto fsPath = [&](size_t i) -> std::filesystem::path {
        return resolve_filesystem_path(argS(i), scriptDirectory_);
    };
    // If the name refers to a scripted action in the running program, execute it and return any ctx.returnValue
    if (currentProgram_) {
        if (const Action* sa = find_action(*currentProgram_, nameStr)) {
            if (currentProgram_->strict && sa->visibility != Visibility::Public) {
                // respect strict mode
            }
            ExecContext actCtx;
            Env calleeEnv;
            // bind args
            for (size_t i=0;i<sa->params.size() && i<args.size(); ++i) {
                const std::string paramName = sa->params[i].name;
                calleeEnv.vars[paramName] = eval_string(*args[i], env);
                if (std::holds_alternative<ExprIdent>(args[i]->node)) {
                    const auto& id = std::get<ExprIdent>(args[i]->node).name;
                    auto oit = env.objects.find(id);
                    if (oit != env.objects.end()) {
                        calleeEnv.objects[paramName] = oit->second;
                    }
                }
            }
            prepare_action_slots(calleeEnv, *sa);
            exec_block(sa->body, *currentProgram_, actCtx, calleeEnv);
            // propagate any changed globals back
            for (const auto& kv : calleeEnv.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
            return actCtx.returned ? actCtx.returnValue : std::string();
        }
    }
    // Compare as string_view: checks length first, so the long chain below rejects
    // non-matching names cheaply. nameStr is final from here on.
    const std::string_view nv = nameStr;
    // Deterministic scaffolding
    static bool s_deterministic = false;
    static uint64_t s_seed = 0;
    static bool s_seedInit = false;
    static uint64_t s_timeVirtual = 0; // milliseconds
    if (!s_seedInit) {
        // Acquire seed from CLI args if provided: --seed <n> and --deterministic flags
        for (size_t i=0;i<s_cliArgs.size();++i) {
            if (s_cliArgs[i] == "--deterministic") { s_deterministic = true; }
            if (s_cliArgs[i] == "--seed" && i+1 < s_cliArgs.size()) { s_seed = (uint64_t)std::stoull(s_cliArgs[i+1]); }
        }
        if (s_seed == 0) s_seed = 0xC0FFEEULL; // default constant seed if not provided
        s_seedInit = true;
    }
    auto deterministic_rng = [&]()->uint64_t {
        // simple xorshift64*
        static uint64_t x = 0;
        if (x == 0) x = (s_seed? s_seed : 0xBAD5EEDULL);
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27; return x * 2685821657736338717ULL;
    };
    
    // Builtins to control deterministic virtual time
    if (nv == "advance_time" && s_deterministic) {
        uint64_t delta = (uint64_t)to_int(eval_string(*args[0], env));
        s_timeVirtual += delta; return std::to_string((long long)s_timeVirtual);
    }
    if ((nv == "now_ms" || nv == "time.now_ms") && s_deterministic) {
        return std::to_string((long long)s_timeVirtual);
    }
    if (nv == "mini_lex_buffer") {
        const std::string source = argS(0);
        Value kinds = mem_make_buffer("Tok", 0);
        Value texts = mem_make_buffer("string", 0);
        Value starts = mem_make_buffer("int", 0);
        Value ends = mem_make_buffer("int", 0);
        Value lines = mem_make_buffer("int", 0);
        Value columns = mem_make_buffer("int", 0);

        auto emit = [&](std::string kind, std::string text, int start, int end, int line, int column) {
            (void)mem_buffer_method(kinds, "push", {Value::from_string(std::move(kind))});
            (void)mem_buffer_method(texts, "push", {Value::from_string(std::move(text))});
            (void)mem_buffer_method(starts, "push", {Value::from_int(start)});
            (void)mem_buffer_method(ends, "push", {Value::from_int(end)});
            (void)mem_buffer_method(lines, "push", {Value::from_int(line)});
            (void)mem_buffer_method(columns, "push", {Value::from_int(column)});
        };
        auto is_alpha = [](char ch) {
            const unsigned char c = static_cast<unsigned char>(ch);
            return std::isalpha(c) || ch == '_';
        };
        auto is_digit = [](char ch) {
            return std::isdigit(static_cast<unsigned char>(ch));
        };
        auto is_space = [](char ch) {
            return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
        };
        auto is_keyword = [](const std::string& text) {
            static const std::unordered_set<std::string> keywords = {
                "public", "action", "return", "enum", "struct", "match", "case",
                "if", "else", "while", "true", "false", "int", "string",
                "bool", "let", "print", "run"
            };
            return keywords.count(text) != 0;
        };
        auto esc = [](char ch) -> std::string {
            if (ch == 'n') return "\n";
            if (ch == 't') return "\t";
            if (ch == 'r') return "\r";
            if (ch == '\\') return "\\";
            if (ch == '"') return "\"";
            return std::string(1, ch);
        };

        int i = 0;
        int line = 1;
        int col = 1;
        const int n = static_cast<int>(source.size());
        while (i < n) {
            const char c = source[static_cast<size_t>(i)];
            if (c == '#' && col == 1 && i + 8 <= n && source.compare(static_cast<size_t>(i), 8, "#include") == 0) {
                while (i < n && source[static_cast<size_t>(i)] != '\n') { ++i; ++col; }
            } else if (is_space(c)) {
                if (c == '\n') { ++line; col = 1; } else { ++col; }
                ++i;
            } else if (c == '/' && i + 1 < n && source[static_cast<size_t>(i + 1)] == '/') {
                i += 2;
                col += 2;
                while (i < n && source[static_cast<size_t>(i)] != '\n') { ++i; ++col; }
            } else if (c == '/' && i + 1 < n && source[static_cast<size_t>(i + 1)] == '*') {
                i += 2;
                col += 2;
                while (i + 1 < n && !(source[static_cast<size_t>(i)] == '*' && source[static_cast<size_t>(i + 1)] == '/')) {
                    if (source[static_cast<size_t>(i)] == '\n') { ++line; col = 1; } else { ++col; }
                    ++i;
                }
                if (i + 1 < n) { i += 2; col += 2; }
            } else if (is_alpha(c)) {
                const int start = i;
                const int startCol = col;
                ++i;
                ++col;
                while (i < n && (is_alpha(source[static_cast<size_t>(i)]) || is_digit(source[static_cast<size_t>(i)]))) { ++i; ++col; }
                std::string text = source.substr(static_cast<size_t>(start), static_cast<size_t>(i - start));
                emit(is_keyword(text) ? "Keyword" : "Ident", text, start, i, line, startCol);
            } else if (is_digit(c)) {
                const int start = i;
                const int startCol = col;
                ++i;
                ++col;
                while (i < n && is_digit(source[static_cast<size_t>(i)])) { ++i; ++col; }
                emit("Number", source.substr(static_cast<size_t>(start), static_cast<size_t>(i - start)), start, i, line, startCol);
            } else if (c == '"') {
                const int start = i;
                const int startLine = line;
                const int startCol = col;
                ++i;
                ++col;
                std::string text;
                while (i < n && source[static_cast<size_t>(i)] != '"') {
                    const char d = source[static_cast<size_t>(i)];
                    if (d == '\\' && i + 1 < n) {
                        text += esc(source[static_cast<size_t>(i + 1)]);
                        i += 2;
                        col += 2;
                    } else {
                        text.push_back(d);
                        ++i;
                        if (d == '\n') { ++line; col = 1; } else { ++col; }
                    }
                }
                if (i < n && source[static_cast<size_t>(i)] == '"') {
                    ++i;
                    ++col;
                    emit("String", text, start, i, startLine, startCol);
                } else {
                    emit("Error", text, start, i, startLine, startCol);
                }
            } else {
                const int start = i;
                const int startCol = col;
                std::string kind;
                std::string text(1, c);
                if (c == '=' && i + 1 < n && source[static_cast<size_t>(i + 1)] == '=') { kind = "EqEq"; text = "=="; i += 2; col += 2; }
                else if (c == '!' && i + 1 < n && source[static_cast<size_t>(i + 1)] == '=') { kind = "BangEq"; text = "!="; i += 2; col += 2; }
                else if (c == '&' && i + 1 < n && source[static_cast<size_t>(i + 1)] == '&') { kind = "AndAnd"; text = "&&"; i += 2; col += 2; }
                else if (c == '|' && i + 1 < n && source[static_cast<size_t>(i + 1)] == '|') { kind = "OrOr"; text = "||"; i += 2; col += 2; }
                else {
                    switch (c) {
                        case '=': kind = "Assign"; break;
                        case '!': kind = "Bang"; break;
                        case '<': kind = "Lt"; break;
                        case '>': kind = "Lt"; break;
                        case '+': kind = "Plus"; break;
                        case '-': kind = "Minus"; break;
                        case '*': kind = "Star"; break;
                        case '/': kind = "Slash"; break;
                        case '(': kind = "LParen"; break;
                        case ')': kind = "RParen"; break;
                        case '{': kind = "LBrace"; break;
                        case '}': kind = "RBrace"; break;
                        case ';': kind = "Semi"; break;
                        case ',': kind = "Comma"; break;
                        case ':': kind = "Colon"; break;
                        case '.': kind = "Dot"; break;
                        case '@': kind = "At"; break;
                        case '#': kind = "Hash"; break;
                        default: kind = "Error"; break;
                    }
                    ++i;
                    ++col;
                }
                emit(std::move(kind), text, start, i, line, startCol);
            }
        }
        emit("Eof", "", n, n, line, col);
        return encode_enum_variant("Buffer", {
            to_display_string(kinds),
            to_display_string(texts),
            to_display_string(starts),
            to_display_string(ends),
            to_display_string(lines),
            to_display_string(columns),
            "0"
        });
    }
    if (nv == "plugin_core" || nv == "plugin.core") {
        if (args.size() < 2) {
            return {};
        }
        const std::string slug = eval_string(*args[0], env);
        const std::string query = eval_string(*args[1], env);
        if (slug.empty() || query.empty()) {
            return {};
        }
        const PluginRecord* record = nullptr;
        for (const auto& plugin : pluginRecords_) {
            if (plugin.slug == slug) {
                record = &plugin;
                break;
            }
        }
        if (!record) {
            return {};
        }
        const auto [fileName, keyName] = split_core_query(query);
        if (!fileName.empty() && keyName.empty()) {
            return {};
        }
        auto lookup = [&](const std::string& file, const std::string& key) -> std::string {
            if (key.empty()) {
                return {};
            }
            if (!file.empty()) {
                auto itFile = record->coreProperties.find(file);
                if (itFile == record->coreProperties.end()) {
                    return {};
                }
                auto itKey = itFile->second.find(key);
                if (itKey == itFile->second.end()) {
                    return {};
                }
                return itKey->second;
            }
            for (const auto& entry : record->coreProperties) {
                auto itKey = entry.second.find(key);
                if (itKey != entry.second.end()) {
                    return itKey->second;
                }
            }
            return {};
        };
        std::string targetKey;
        if (keyName.empty()) {
            targetKey = trim_copy(query);
        } else {
            targetKey = keyName;
        }
        return lookup(fileName, targetKey);
    }
    if (nv == "plugin_core_files" || nv == "plugin.core_files") {
        if (args.empty()) {
            return {};
        }
        const std::string slug = eval_string(*args[0], env);
        if (slug.empty()) {
            return {};
        }
        for (const auto& plugin : pluginRecords_) {
            if (plugin.slug == slug) {
                std::vector<std::string> files;
                files.reserve(plugin.coreProperties.size());
                for (const auto& kv : plugin.coreProperties) {
                    files.push_back(kv.first);
                }
                return join_strings(std::move(files));
            }
        }
        return {};
    }
    if (nv == "plugin_core_keys" || nv == "plugin.core_keys") {
        if (args.size() < 2) {
            return {};
        }
        const std::string slug = eval_string(*args[0], env);
        const std::string fileName = eval_string(*args[1], env);
        if (slug.empty() || fileName.empty()) {
            return {};
        }
        for (const auto& plugin : pluginRecords_) {
            if (plugin.slug == slug) {
                auto itFile = plugin.coreProperties.find(fileName);
                if (itFile == plugin.coreProperties.end()) {
                    return {};
                }
                std::vector<std::string> keys;
                keys.reserve(itFile->second.size());
                for (const auto& kv : itFile->second) {
                    keys.push_back(kv.first);
                }
                return join_strings(std::move(keys));
            }
        }
        return {};
    }
    // List/Dict built-ins (cross-platform)
    if (nv == "language_name" || nv == "lang.name") {
        return std::string("erelang / Erelang");
    }
    if (nv == "language_version" || nv == "lang.version") {
        return std::string(erelang::BuildInfo::version());
    }
    if (nv == "language_about" || nv == "lang.about") {
        return std::string("Erelang interpreter for .elan programs.\n");
    }
    if (nv == "language_limitations" || nv == "lang.limitations") {
        return std::string("Windows-first CLI runtime. No GUI. Most I/O is import-gated.\n");
    }
    // Conversion and type-check helpers
    if (nv == "toint" || nv == "int") {
        return std::to_string((long long)to_int(argS(0)));
    }
    if (nv == "toInt") {
        return std::to_string((long long)to_int(argS(0)));
    }
    if (nv == "tofloat" || nv == "float") {
        double v = to_double(argS(0));
        std::ostringstream ss; ss << v; return ss.str();
    }
    if (nv == "tostr" || nv == "toString" || nv == "string") {
        return argS(0);
    }
    if (nv == "tobool" || nv == "bool") {
        return is_truthy(argS(0)) ? std::string("true") : std::string("false");
    }
    // Type-check methods
    if (nv == "int.is") { return is_int_string(argS(0)) ? "true" : "false"; }
    if (nv == "string.is") { return argS(0).empty() ? "false" : "true"; }
    if (nv == "float.is") { return is_float_string(argS(0)) ? "true" : "false"; }
    auto normalize_type_name = [](std::string typeName) {
        std::string out;
        out.reserve(typeName.size());
        for (char ch : typeName) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
        }
        return out;
    };
    auto infer_runtime_type_value = [&](const Value& value) {
        if (value_is_handle(value, HandleKind::List)) return std::string("Array<any>");
        if (value_is_handle(value, HandleKind::Dict)) return std::string("Map<string, any>");
        if (value.kind == ValueKind::Bool) return std::string("bool");
        if (value.kind == ValueKind::Int) return std::string("int");
        if (value.kind == ValueKind::Float) return std::string("double");
        const std::string text = to_display_string(value);
        if (text.rfind("struct:", 0) == 0) return text;
        if (text == "true" || text == "false") return std::string("bool");
        if (is_int_string(text)) return std::string("int");
        if (is_float_string(text)) return std::string("double");
        if (auto it = env.objects.find(text); it != env.objects.end() && it->second) return it->second->typeName;
        return std::string("string");
    };
    auto infer_runtime_type = [&](const std::string& value) {
        return infer_runtime_type_value(value_from_legacy_string(value));
    };
    auto size_of_type = [&](const std::string& typeName) -> int {
        const std::string t = normalize_type_name(typeName);
        if (t == "bool") return 1;
        if (t == "char" || t == "byte") return 1;
        if (t == "short") return 2;
        if (t == "int") return 8;
        if (t == "float") return 4;
        if (t == "double") return 8;
        if (t == "string" || t == "str") return 24;
        if (t.rfind("array", 0) == 0) return 24;
        if (t.rfind("map", 0) == 0 || t == "dictionary") return 24;
        if (t.rfind("struct:", 0) == 0) return 24;
        return 8;
    };
    auto align_of_type = [&](const std::string& typeName) -> int {
        const std::string t = normalize_type_name(typeName);
        if (t == "bool" || t == "char" || t == "byte") return 1;
        if (t == "short") return 2;
        if (t == "float") return 4;
        return 8;
    };
    if (nv == "__builtin_typeof" || nv == "__builtin_decltype") {
        if (args.empty()) return "unknown";
        return infer_runtime_type_value(argV(0));
    }
    if (nv == "__builtin_sizeof") {
        if (args.empty()) return "0";
        std::string typeName;
        if (std::holds_alternative<ExprString>(args[0]->node)) {
            typeName = std::get<ExprString>(args[0]->node).v;
        } else {
            typeName = infer_runtime_type(eval_string(*args[0], env));
        }
        return std::to_string(size_of_type(typeName));
    }
    if (nv == "__builtin_alignof") {
        if (args.empty()) return "1";
        std::string typeName;
        if (std::holds_alternative<ExprString>(args[0]->node)) {
            typeName = std::get<ExprString>(args[0]->node).v;
        } else {
            typeName = infer_runtime_type(eval_string(*args[0], env));
        }
        return std::to_string(align_of_type(typeName));
    }
    if (nv == "__builtin_offsetof") {
        if (args.size() < 2) return "0";
        const std::string typeName = argS(0);
        const std::string fieldName = argS(1);
        if (currentProgram_) {
            int index = 0;
            for (const auto& sd : currentProgram_->structs) {
                if (sd.name != typeName) continue;
                for (const auto& f : sd.fields) {
                    if (f.name == fieldName) return std::to_string(index * 8);
                    ++index;
                }
                return "0";
            }
            for (const auto& ent : currentProgram_->entities) {
                if (ent.name != typeName) continue;
                index = 0;
                for (const auto& f : ent.fields) {
                    if (f.name == fieldName) return std::to_string(index * 8);
                    ++index;
                }
                return "0";
            }
        }
        return "0";
    }
    if (nv == "__builtin_is_base_of") {
        if (args.size() < 2 || !currentProgram_) return "false";
        const std::string baseType = argS(0);
        const std::string derivedType = argS(1);
        auto findEntity = [&](std::string_view n) -> const Entity* {
            for (const auto& e : currentProgram_->entities) if (e.name == n) return &e;
            return nullptr;
        };
        const Entity* current = findEntity(derivedType);
        while (current && !current->baseType.empty()) {
            if (current->baseType == baseType) return "true";
            current = findEntity(current->baseType);
        }
        return "false";
    }
    if (nv == "dynamic_cast") {
        if (args.size() < 2) return {};
        const std::string targetType = argS(0);
        std::string sourceName = argS(1);
        if (std::holds_alternative<ExprIdent>(args[1]->node)) {
            sourceName = std::get<ExprIdent>(args[1]->node).name;
        }
        auto sourceIt = env.objects.find(sourceName);
        if (sourceIt == env.objects.end()) {
            return {};
        }
        const auto& obj = sourceIt->second;
        if (!obj) return {};

        if (obj->typeName == targetType) return sourceName;
        if (currentProgram_) {
            auto findEntity = [&](std::string_view n) -> const Entity* {
                for (const auto& e : currentProgram_->entities) if (e.name == n) return &e;
                return nullptr;
            };
            const Entity* current = findEntity(obj->typeName);
            while (current && !current->baseType.empty()) {
                if (current->baseType == targetType) return sourceName;
                current = findEntity(current->baseType);
            }
        }
        return {};
    }
    if (nv == "reinterpret_cast") {
        if (args.size() < 2) return {};
        std::string targetType = argS(0);
        std::string value = argS(1);
        std::string low;
        low.reserve(targetType.size());
        for (char ch : targetType) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
        }
        const bool pointerLike = (!low.empty() && (low.back() == '*' || low.back() == '&')) ||
                                 (low.find("ptr") != std::string::npos) ||
                                 (low == "pointer");
        if (pointerLike) {
            if (parse_pointer_handle(value).has_value() || value.rfind("ref:", 0) == 0) return value;
            const int id = g_nextPtrId++;
            g_ptrs[id] = value;
            return format_pointer_handle(id);
        }
        if (auto idOpt = parse_pointer_handle(value); idOpt.has_value()) {
            const int id = *idOpt;
            auto it = g_ptrs.find(id);
            if (it != g_ptrs.end()) return it->second;
            return {};
        }
        if (value.rfind("ref:", 0) == 0) {
            const std::string varName = value.substr(4);
            if (auto it = env.vars.find(varName); it != env.vars.end()) return it->second;
            if (auto git = globalVars_.find(varName); git != globalVars_.end()) return git->second;
            return {};
        }
        return value;
    }
    if (nv == "bit_cast") {
        if (args.size() < 2) return {};
        std::string targetType = argS(0);
        std::string value = argS(1);
        std::string low;
        low.reserve(targetType.size());
        for (char ch : targetType) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
        }

        if (low == "double") {
            uint64_t bits = 0;
            if (auto idOpt = parse_pointer_handle(value); idOpt.has_value()) {
                bits = static_cast<uint64_t>(*idOpt);
            } else if (value.rfind("ref:", 0) == 0) {
                bits = static_cast<uint64_t>(std::hash<std::string>{}(value));
            } else {
                bits = static_cast<uint64_t>(to_int(value));
            }
            const double out = std::bit_cast<double>(bits);
            std::ostringstream oss;
            oss << out;
            return oss.str();
        }
        if (low == "float") {
            uint32_t bits = static_cast<uint32_t>(to_int(value));
            const float out = std::bit_cast<float>(bits);
            std::ostringstream oss;
            oss << out;
            return oss.str();
        }
        if (low == "int" || low == "int64" || low == "long" || low == "longlong") {
            if (is_float_string(value)) {
                const double dv = to_double(value);
                const uint64_t bits = std::bit_cast<uint64_t>(dv);
                return std::to_string(static_cast<long long>(bits));
            }
            if (auto idOpt = parse_pointer_handle(value); idOpt.has_value()) return std::to_string(*idOpt);
            if (value.rfind("ref:", 0) == 0) {
                return std::to_string(static_cast<long long>(std::hash<std::string>{}(value)));
            }
            return std::to_string(static_cast<long long>(to_int(value)));
        }
        if (low == "uint32" || low == "u32") {
            if (is_float_string(value)) {
                const float fv = static_cast<float>(to_double(value));
                const uint32_t bits = std::bit_cast<uint32_t>(fv);
                return std::to_string(static_cast<unsigned long long>(bits));
            }
            return std::to_string(static_cast<unsigned long long>(static_cast<uint32_t>(to_int(value))));
        }
        if (low == "uint64" || low == "u64") {
            if (is_float_string(value)) {
                const double dv = to_double(value);
                const uint64_t bits = std::bit_cast<uint64_t>(dv);
                return std::to_string(static_cast<unsigned long long>(bits));
            }
            return std::to_string(static_cast<unsigned long long>(to_int(value)));
        }
        const bool pointerLike = (!low.empty() && (low.back() == '*' || low.back() == '&')) ||
                                 (low.find("ptr") != std::string::npos) ||
                                 (low == "pointer");
        if (pointerLike) {
            if (parse_pointer_handle(value).has_value() || value.rfind("ref:", 0) == 0) return value;
            const int id = g_nextPtrId++;
            g_ptrs[id] = value;
            return format_pointer_handle(id);
        }
        return value;
    }
    if (nv == "ptr_new" || nv == "make_unique" || nv == "make_shared") {
        const int id = g_nextPtrId++;
        g_ptrs[id] = argS(0);
        return format_pointer_handle(id);
    }
    if (nv == "malloc") {
        const int id = g_nextPtrId++;
        const std::string sizeOrValue = argS(0);
        if (is_int_string(sizeOrValue)) {
            const long long n = std::max<long long>(0, to_int(sizeOrValue));
            g_ptrs[id] = std::string(static_cast<std::size_t>(n), '\0');
        } else {
            g_ptrs[id] = sizeOrValue;
        }
        return format_pointer_handle(id);
    }
    if (nv == "ptr_get") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return {};
        const int id = *idOpt;
        auto it = g_ptrs.find(id);
        if (it == g_ptrs.end()) return {};
        if (it->second.rfind("ref:", 0) == 0) {
            const std::string varName = it->second.substr(4);
            if (auto vit = env.vars.find(varName); vit != env.vars.end()) return vit->second;
            if (auto git = globalVars_.find(varName); git != globalVars_.end()) return git->second;
            return {};
        }
        return it->second;
    }
    if (nv == "ptr_set") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return {};
        const int id = *idOpt;
        auto it = g_ptrs.find(id);
        if (it == g_ptrs.end()) return {};
        if (it->second.rfind("ref:", 0) == 0) {
            const std::string varName = it->second.substr(4);
            const std::string newValue = argS(1);
            if (globalNames_.count(varName)) {
                globalVars_[varName] = newValue;
            }
            const_cast<Env&>(env).vars[varName] = newValue;
            return {};
        }
        it->second = argS(1);
        return {};
    }
    if (nv == "memcpy") {
        const std::string dst = argS(0);
        const std::string src = argS(1);
        auto dstId = parse_pointer_handle(dst);
        auto srcId = parse_pointer_handle(src);
        if (!dstId.has_value() || !srcId.has_value()) return {};
        auto dit = g_ptrs.find(*dstId);
        auto sit = g_ptrs.find(*srcId);
        if (dit == g_ptrs.end() || sit == g_ptrs.end()) return {};
        if (args.size() >= 3) {
            const long long requested = std::max<long long>(0, to_int(argS(2)));
            const std::size_t count = static_cast<std::size_t>(requested);
            if (dit->second.size() < count) {
                dit->second.resize(count, '\0');
            }
            const std::size_t available = std::min<std::size_t>(count, sit->second.size());
            if (available > 0) {
                std::copy_n(sit->second.begin(), available, dit->second.begin());
            }
            if (available < count) {
                std::fill(dit->second.begin() + available, dit->second.begin() + count, '\0');
            }
        } else {
            dit->second = sit->second;
        }
        return {};
    }
    if (nv == "realloc") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return {};
        auto it = g_ptrs.find(*idOpt);
        if (it == g_ptrs.end()) return {};
        const long long n = std::max<long long>(0, to_int(argS(1)));
        it->second.resize(static_cast<std::size_t>(n), '\0');
        return format_pointer_handle(*idOpt);
    }
    if (nv == "memset") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return {};
        auto it = g_ptrs.find(*idOpt);
        if (it == g_ptrs.end()) return {};
        int fillByte = 0;
        if (args.size() >= 2) {
            const std::string raw = argS(1);
            fillByte = is_int_string(raw) ? static_cast<int>(to_int(raw) & 0xFF) : (raw.empty() ? 0 : static_cast<unsigned char>(raw[0]));
        }
        std::size_t count = it->second.size();
        if (args.size() >= 3) {
            const long long requested = std::max<long long>(0, to_int(argS(2)));
            count = std::min<std::size_t>(count, static_cast<std::size_t>(requested));
        }
        std::fill_n(it->second.begin(), count, static_cast<char>(fillByte));
        return {};
    }
    if (nv == "ptr_free" || nv == "unique_reset" || nv == "shared_reset") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return {};
        const int id = *idOpt;
        g_ptrs.erase(id);
        return {};
    }
    if (nv == "free") {
        const std::string handle = argS(0);
        if (auto idOpt = parse_pointer_handle(handle); idOpt.has_value()) {
            const int id = *idOpt;
            g_ptrs.erase(id);
        }
        return {};
    }
    if (nv == "ptr_valid") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return "false";
        const int id = *idOpt;
        return g_ptrs.count(id) ? "true" : "false";
    }
    if (nv == "to_json" || nv == "json.encode") {
        if (args.empty()) return "null";
        const Value valueV = argV(0);
        const std::string value = to_display_string(valueV);
        if (value_is_handle(valueV, HandleKind::Dict)) return features::dict_handle_to_json(value);
        if (value_is_handle(valueV, HandleKind::List)) return features::list_handle_to_json(value);
        auto objIt = env.objects.find(value);
        if (objIt != env.objects.end() && objIt->second) {
            std::ostringstream oss;
            oss << '{';
            bool first = true;
            for (const auto& [k, v] : objIt->second->fields) {
                if (!first) oss << ',';
                first = false;
                oss << '"' << features::json_escape(k) << "\":\"" << features::json_escape(v) << '"';
            }
            oss << '}';
            return oss.str();
        }
        if (value.rfind("struct:", 0) == 0) {
            std::ostringstream oss;
            oss << '{';
            bool first = true;
            const std::string prefix = value + ".";
            for (const auto& [k, v] : env.vars) {
                if (k.rfind(prefix, 0) != 0) continue;
                const std::string field = k.substr(prefix.size());
                if (!first) oss << ',';
                first = false;
                oss << '"' << features::json_escape(field) << "\":\"" << features::json_escape(to_display_string(v)) << '"';
            }
            oss << '}';
            return oss.str();
        }
        return std::string("\"") + features::json_escape(value) + "\"";
    }
    if (nv == "from_json" || nv == "json.decode") {
        if (args.empty()) return {};
        return features::from_json_object_to_dict_handle(argS(0));
    }
    if (nv == "string.lstrip") {
        std::string s = argS(0);
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        return s.substr(i);
    }
    if (nv == "string.rstrip") {
        std::string s = argS(0);
        if (s.empty()) return s;
        size_t j = s.size();
        while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1])) != 0) --j;
        return s.substr(0, j);
    }
    if (nv == "string.strip") {
        std::string s = argS(0);
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        size_t j = s.size();
        while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1])) != 0) --j;
        return s.substr(i, j - i);
    }
    if (nv == "string.lower") {
        std::string s = argS(0);
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
    if (nv == "string.upper") {
        std::string s = argS(0);
        for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    }
    if (nv == "string.starts_with") {
        std::string s = argS(0);
        std::string prefix = argS(1);
        if (prefix.size() > s.size()) return std::string("false");
        return s.compare(0, prefix.size(), prefix) == 0 ? std::string("true") : std::string("false");
    }
    if (nv == "string.ends_with") {
        std::string s = argS(0);
        std::string suffix = argS(1);
        if (suffix.size() > s.size()) return std::string("false");
        return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0 ? std::string("true") : std::string("false");
    }
    if (nv == "string.find") {
        std::string s = argS(0);
        std::string needle = argS(1);
        const auto pos = s.find(needle);
        if (pos == std::string::npos) return std::string("-1");
        return std::to_string(static_cast<int>(pos));
    }
    if (nv == "string.substr") {
        std::string s = argS(0);
        int start = to_int(argS(1));
        if (start < 0) start = 0;
        if (start > static_cast<int>(s.size())) return {};
        if (args.size() >= 3) {
            int len = to_int(argS(2));
            if (len < 0) len = 0;
            return s.substr(static_cast<size_t>(start), static_cast<size_t>(len));
        }
        return s.substr(static_cast<size_t>(start));
    }
    if (nv == "string.len") {
        return std::to_string(static_cast<int>(argS(0).size()));
    }
    if (nv == "is_int") {
        return is_int_string(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nv == "is_float") {
        return is_float_string(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nv == "is_str") {
        // Everything is a string at runtime; consider non-empty as true
        return argS(0).empty() ? std::string("false") : std::string("true");
    }
    if (nv == "args_count") {
    return std::to_string((int)s_cliArgs.size());
    }
    if (nv == "args_get") {
    int idx = to_int(argS(0));
    if (idx >= 0 && idx < (int)s_cliArgs.size()) return s_cliArgs[idx];
    return {};
    }
    if (nv == "os.args") {
        int id = g_nextListId++;
        g_lists[id] = {};
        for (const auto& a : s_cliArgs) g_lists[id].push_back(value_from_legacy_string(a));
        return std::string("list:") + std::to_string(id);
    }
    if (nv == "os.args_count") {
        return std::to_string((int)s_cliArgs.size());
    }
    if (nv == "os.args_get") {
        int idx = to_int(argS(0));
        if (idx >= 0 && idx < (int)s_cliArgs.size()) return s_cliArgs[idx];
        return {};
    }
    if (nv == "exec") {
    // Execute arbitrary command (shell). Return exit code as string.
    std::string cmd = argS(0);
#ifdef _WIN32
    std::string full = std::string("cmd /c ") + cmd;
#else
    std::string full = cmd;
#endif
    std::cout.flush(); int code = std::system(full.c_str());
    return std::to_string(code);
    }
        if (nv == "os.exec") {
        std::string cmd = argS(0);
    #ifdef _WIN32
        std::string full = std::string("cmd /c ") + cmd;
    #else
        std::string full = cmd;
    #endif
        std::cout.flush(); int code = std::system(full.c_str());
        return std::to_string(code);
        }
        if (nv == "spawn") {
        std::string cmd = argS(0);
    #ifdef _WIN32
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        std::string commandLine = std::string("cmd /c ") + cmd;
        BOOL ok = CreateProcessA(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
            nullptr,
            nullptr,
            &si,
            &pi);
        if (!ok) return "-1";
        const DWORD pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return std::to_string((long long)pid);
    #else
        std::thread([command = std::move(cmd)](){ std::system(command.c_str()); }).detach();
        return "1";
    #endif
        }
        if (nv == "os.spawn") {
        std::string cmd = argS(0);
    #ifdef _WIN32
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        std::string commandLine = std::string("cmd /c ") + cmd;
        BOOL ok = CreateProcessA(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
            nullptr,
            nullptr,
            &si,
            &pi);
        if (!ok) return "-1";
        const DWORD pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return std::to_string((long long)pid);
    #else
        std::thread([command = std::move(cmd)](){ std::system(command.c_str()); }).detach();
        return "1";
    #endif
        }
        if (nv == "exit") {
        int code = (int)to_int(argS(0));
        std::exit(code);
        }
    if (nv == "mini_ir_run" || nv == "mini_ir.run") {
        return erelang::mini_ir::encode_result(erelang::mini_ir::run_text(argS(0)));
    }
    if (nv == "mini_ir_run_file" || nv == "mini_ir.run_file") {
        return erelang::mini_ir::encode_result(erelang::mini_ir::run_file(fsPath(0).string()));
    }
    if (nv == "run_file") {
    // Run a file with default OS association (like double-click). No output capture; returns empty.
    std::string fp = argS(0);
#ifdef _WIN32
    std::wstring w(fp.begin(), fp.end()); ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    return {};
    }
    if (nv == "run_bat") {
    std::string bat = argS(0);
#ifdef _WIN32
    std::string full = std::string("cmd /c \"") + bat + "\"";
    std::cout.flush(); std::system(full.c_str());
#endif
    return {};
    }
    if (nv == "read_line" || nv == "io.read_line") {
        std::string s; std::getline(std::cin, s); return s;
    }
    if (nv == "stdin_read" || nv == "io.stdin") {
        std::string s; std::getline(std::cin, s); return s;
    }
    if (nv == "stderr_print" || nv == "io.stderr") {
        std::cerr << argS(0) << "\n";
        return {};
    }
    if (nv == "input") {
        if (!args.empty()) {
            std::string msg = argS(0);
            std::cout << msg;
            std::cout.flush();
        }
        std::string s;
        std::getline(std::cin, s);
        return s;
    }
    if (nv == "prompt" || nv == "io.prompt" || nv == "io.input") {
        std::string msg = argS(0); std::cout << msg; std::cout.flush(); std::string s; std::getline(std::cin, s); return s;
    }
    // Filesystem utilities (relative paths resolve against entry script directory)
    if (nv == "read_text") {
        std::ifstream in(fsPath(0), std::ios::binary);
        if (!in) return {};
        std::ostringstream ss; ss << in.rdbuf();
        return ss.str();
    }
    if (nv == "write_text") {
        std::ofstream out(fsPath(0), std::ios::binary);
        if (!out) return {};
        out << argS(1);
        return {};
    }
    if (nv == "append_text") {
        std::ofstream out(fsPath(0), std::ios::binary | std::ios::app);
        if (!out) return {};
        out << argS(1);
        return {};
    }
    if (nv == "file_open") {
        std::string mode = args.size() >= 2 ? argS(1) : std::string("r+");
        if (mode.empty()) mode = "r+";
        std::ios::openmode openMode = std::ios::binary;
        if (mode == "r" || mode == "rb") {
            openMode |= std::ios::in;
        } else if (mode == "w" || mode == "wb") {
            openMode |= std::ios::out | std::ios::trunc;
        } else if (mode == "a" || mode == "ab") {
            openMode |= std::ios::out | std::ios::app;
        } else if (mode == "r+" || mode == "rb+") {
            openMode |= std::ios::in | std::ios::out;
        } else if (mode == "w+" || mode == "wb+") {
            openMode |= std::ios::in | std::ios::out | std::ios::trunc;
        } else if (mode == "a+" || mode == "ab+") {
            openMode |= std::ios::in | std::ios::out | std::ios::app;
        } else {
            return {};
        }
        auto file = std::make_unique<std::fstream>(fsPath(0), openMode);
        if (!(*file)) return {};
        const int id = g_nextFileId++;
        g_fileStreams[id] = std::move(file);
        g_fileBufs[id] = FileBufState{};
        return std::string("file:") + std::to_string(id);
    }
    if (nv == "file_close") {
        const std::string handle = argS(0);
        if (handle.rfind("file:", 0) != 0) return "false";
        const int id = to_int(handle.substr(5));
        auto it = g_fileStreams.find(id);
        if (it == g_fileStreams.end()) return "false";
        auto bit = g_fileBufs.find(id);
        if (bit != g_fileBufs.end() && it->second && it->second->is_open() && !bit->second.writePending.empty()) {
            it->second->write(bit->second.writePending.data(),
                              static_cast<std::streamsize>(bit->second.writePending.size()));
            it->second->flush();
            bit->second.writePending.clear();
        }
        if (it->second && it->second->is_open()) it->second->close();
        g_fileStreams.erase(it);
        g_fileBufs.erase(id);
        return "true";
    }
    if (nv == "file_buffer") {
        const std::string handle = argS(0);
        if (handle.rfind("file:", 0) != 0) return "false";
        const int id = to_int(handle.substr(5));
        if (!g_fileStreams.count(id)) return "false";
        long long n = args.size() >= 2 ? to_int(argS(1)) : 4096;
        if (n < 0) n = 0;
        auto& buf = g_fileBufs[id];
        if (!buf.writePending.empty()) {
            auto fit = g_fileStreams.find(id);
            if (fit != g_fileStreams.end() && fit->second) {
                fit->second->write(buf.writePending.data(),
                                   static_cast<std::streamsize>(buf.writePending.size()));
                fit->second->flush();
            }
            buf.writePending.clear();
        }
        buf.capacity = static_cast<std::size_t>(n);
        buf.readCache.clear();
        buf.readPos = 0;
        return "true";
    }
    if (nv == "file_read") {
        const std::string handle = argS(0);
        if (handle.rfind("file:", 0) != 0) return {};
        const int id = to_int(handle.substr(5));
        auto it = g_fileStreams.find(id);
        if (it == g_fileStreams.end() || !it->second) return {};
        auto& stream = *it->second;
        auto& buf = g_fileBufs[id];
        if (!stream.good()) {
            stream.clear();
        }
        auto refill = [&]() {
            if (buf.capacity == 0) return;
            if (buf.readPos < buf.readCache.size()) return;
            buf.readCache.assign(buf.capacity, '\0');
            stream.read(buf.readCache.data(), static_cast<std::streamsize>(buf.capacity));
            buf.readCache.resize(static_cast<std::size_t>(stream.gcount()));
            buf.readPos = 0;
        };
        if (args.size() >= 2) {
            const long long count = std::max<long long>(0, to_int(argS(1)));
            std::string out;
            out.reserve(static_cast<std::size_t>(count));
            while (static_cast<long long>(out.size()) < count) {
                if (buf.capacity == 0) {
                    const long long need = count - static_cast<long long>(out.size());
                    std::string chunk(static_cast<std::size_t>(need), '\0');
                    stream.read(chunk.data(), static_cast<std::streamsize>(need));
                    chunk.resize(static_cast<std::size_t>(stream.gcount()));
                    out += chunk;
                    break;
                }
                refill();
                if (buf.readPos >= buf.readCache.size()) break;
                const std::size_t avail = buf.readCache.size() - buf.readPos;
                const std::size_t take = std::min(avail, static_cast<std::size_t>(count - static_cast<long long>(out.size())));
                out.append(buf.readCache, buf.readPos, take);
                buf.readPos += take;
            }
            return out;
        }
        if (buf.capacity == 0) {
            std::ostringstream ss;
            ss << stream.rdbuf();
            return ss.str();
        }
        std::string out;
        for (;;) {
            refill();
            if (buf.readPos >= buf.readCache.size()) break;
            out.append(buf.readCache, buf.readPos, buf.readCache.size() - buf.readPos);
            buf.readPos = buf.readCache.size();
        }
        return out;
    }
    if (nv == "file_write") {
        const std::string handle = argS(0);
        if (handle.rfind("file:", 0) != 0) return "0";
        const int id = to_int(handle.substr(5));
        auto it = g_fileStreams.find(id);
        if (it == g_fileStreams.end() || !it->second) return "0";
        auto& stream = *it->second;
        auto& buf = g_fileBufs[id];
        const std::string data = argS(1);
        if (buf.capacity == 0) {
            stream.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!stream.good()) return "0";
            return std::to_string(static_cast<long long>(data.size()));
        }
        buf.writePending += data;
        while (buf.writePending.size() >= buf.capacity) {
            stream.write(buf.writePending.data(), static_cast<std::streamsize>(buf.capacity));
            if (!stream.good()) return "0";
            buf.writePending.erase(0, buf.capacity);
        }
        return std::to_string(static_cast<long long>(data.size()));
    }
    if (nv == "file_seek") {
        const std::string handle = argS(0);
        if (handle.rfind("file:", 0) != 0) return "false";
        const int id = to_int(handle.substr(5));
        auto it = g_fileStreams.find(id);
        if (it == g_fileStreams.end() || !it->second) return "false";
        auto& stream = *it->second;
        auto& buf = g_fileBufs[id];
        if (!buf.writePending.empty()) {
            stream.write(buf.writePending.data(), static_cast<std::streamsize>(buf.writePending.size()));
            stream.flush();
            buf.writePending.clear();
        }
        buf.readCache.clear();
        buf.readPos = 0;
        const long long offset = to_int(argS(1));
        std::string whence = args.size() >= 3 ? argS(2) : "set";
        std::ios::seekdir dir = std::ios::beg;
        if (whence == "cur" || whence == "current" || whence == "1") dir = std::ios::cur;
        else if (whence == "end" || whence == "2") dir = std::ios::end;
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), dir);
        stream.seekp(static_cast<std::streamoff>(offset), dir);
        return stream.fail() ? "false" : "true";
    }
    if (nv == "file_tell") {
        const std::string handle = argS(0);
        if (handle.rfind("file:", 0) != 0) return "-1";
        const int id = to_int(handle.substr(5));
        auto it = g_fileStreams.find(id);
        if (it == g_fileStreams.end() || !it->second) return "-1";
        auto& stream = *it->second;
        auto pos = stream.tellg();
        if (pos < 0) pos = stream.tellp();
        if (pos < 0) return "-1";
        auto bit = g_fileBufs.find(id);
        long long adj = static_cast<long long>(pos);
        if (bit != g_fileBufs.end()) {
            adj += static_cast<long long>(bit->second.writePending.size());
            adj -= static_cast<long long>(bit->second.readCache.size() - bit->second.readPos);
        }
        return std::to_string(adj);
    }
    if (nv == "file_flush") {
        const std::string handle = argS(0);
        if (handle.rfind("file:", 0) != 0) return "false";
        const int id = to_int(handle.substr(5));
        auto it = g_fileStreams.find(id);
        if (it == g_fileStreams.end() || !it->second) return "false";
        auto bit = g_fileBufs.find(id);
        if (bit != g_fileBufs.end() && !bit->second.writePending.empty()) {
            it->second->write(bit->second.writePending.data(),
                              static_cast<std::streamsize>(bit->second.writePending.size()));
            bit->second.writePending.clear();
        }
        it->second->flush();
        return it->second->good() ? "true" : "false";
    }

    // Logging module (std/log): level-filtered lines to stderr and optional file.
    {
        static int g_logLevel = 1; // 0=debug 1=info 2=warn 3=error
        static std::string g_logFilePath;
        static std::mutex g_logMu;
        auto level_rank = [](const std::string& s) -> int {
            std::string l = s;
            for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (l == "debug") return 0;
            if (l == "info") return 1;
            if (l == "warn" || l == "warning") return 2;
            if (l == "error") return 3;
            return 1;
        };
        auto level_name = [](int r) -> const char* {
            switch (r) {
                case 0: return "debug";
                case 1: return "info";
                case 2: return "warn";
                default: return "error";
            }
        };
        auto emit_log = [&](int rank, const std::string& msg) -> std::string {
            if (rank < g_logLevel) return {};
            std::ostringstream line;
            line << "[" << level_name(rank) << "] " << msg << "\n";
            const std::string text = line.str();
            std::lock_guard<std::mutex> lock(g_logMu);
            std::cerr << text;
            if (!g_logFilePath.empty()) {
                std::ofstream out(g_logFilePath, std::ios::binary | std::ios::app);
                if (out) out << text;
            }
            return {};
        };
        if (nv == "log_set_level") {
            g_logLevel = level_rank(argS(0));
            return "true";
        }
        if (nv == "log_to_file") {
            g_logFilePath = argS(0);
            return "true";
        }
        if (nv == "log_write") {
            return emit_log(level_rank(argS(0)), argS(1));
        }
        if (nv == "log_debug") return emit_log(0, argS(0));
        if (nv == "log_info") return emit_log(1, argS(0));
        if (nv == "log_warn") return emit_log(2, argS(0));
        if (nv == "log_error") return emit_log(3, argS(0));
    }

    if (nv == "file_exists") {
        std::error_code ec;
        return std::filesystem::exists(fsPath(0), ec) && !ec ? std::string("true") : std::string("false");
    }
    if (nv == "is_dir") {
        std::error_code ec;
        return std::filesystem::is_directory(fsPath(0), ec) && !ec ? std::string("true") : std::string("false");
    }
    if (nv == "is_file") {
        std::error_code ec;
        return std::filesystem::is_regular_file(fsPath(0), ec) && !ec ? std::string("true") : std::string("false");
    }
    if (nv == "file_size") {
        std::error_code ec;
        auto size = std::filesystem::file_size(fsPath(0), ec);
        if (ec) return "-1";
        return std::to_string(static_cast<long long>(size));
    }
    if (nv == "mkdirs") {
        std::error_code ec; std::filesystem::create_directories(fsPath(0), ec); return {};
    }
    if (nv == "copy_file") {
        std::error_code ec; bool ok = std::filesystem::copy_file(fsPath(0), fsPath(1), std::filesystem::copy_options::overwrite_existing, ec);
        return ok && !ec ? std::string("true") : std::string("false");
    }
    if (nv == "move_file") {
        std::error_code ec; std::filesystem::rename(fsPath(0), fsPath(1), ec);
        return !ec ? std::string("true") : std::string("false");
    }
    if (nv == "delete_file") {
        std::error_code ec; std::filesystem::remove(fsPath(0), ec);
        return !ec ? std::string("true") : std::string("false");
    }
    if (nv == "list_files" || nv == "list_dirs" || nv == "list_regular_files") {
        const bool dirsOnly = (nv == "list_dirs");
        const bool filesOnly = (nv == "list_regular_files");
        const auto root = fsPath(0);
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec) || ec) {
            throw std::runtime_error(std::string(nameStr) + ": not a directory: " + root.string());
        }
        int id = g_nextListId++;
        g_lists[id] = {};
        for (auto& e : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            std::error_code tec;
            if (dirsOnly) {
                if (!e.is_directory(tec) || tec) continue;
            } else if (filesOnly) {
                if (!e.is_regular_file(tec) || tec) continue;
            }
            g_lists[id].push_back(value_from_legacy_string(e.path().string()));
        }
        sort_list_by_display(g_lists[id]);
        return std::string("list:") + std::to_string(id);
    }
    if (nv == "load_elan") {
        return load_elan_file(fsPath(0));
    }
    if (nv == "load_elan_dir") {
        return load_elan_directory(fsPath(0));
    }
    if (nv == "call_action") {
        std::vector<std::string> callArgs;
        callArgs.reserve(args.size() > 0 ? args.size() - 1 : 0);
        for (size_t i = 1; i < args.size(); ++i) {
            callArgs.push_back(argS(i));
        }
        return call_action_by_name(argS(0), callArgs);
    }
    if (nv == "cwd") {
        return std::filesystem::current_path().string();
    }
    if (nv == "chdir") {
        std::error_code ec; std::filesystem::current_path(fsPath(0), ec); return !ec ? std::string("true") : std::string("false");
    }
    if (nv == "path_join") {
        if (args.empty()) return {};
        std::filesystem::path p = argS(0);
        for (std::size_t i = 1; i < args.size(); ++i) {
            p /= argS(i);
        }
        return p.string();
    }
    if (nv == "path_dirname") {
        return std::filesystem::path(argS(0)).parent_path().string();
    }
    if (nv == "path_basename") {
        return std::filesystem::path(argS(0)).filename().string();
    }
    if (nv == "path_ext") {
        return std::filesystem::path(argS(0)).extension().string();
    }
    if (nv == "file_mtime") {
        std::error_code ec;
        auto ft = std::filesystem::last_write_time(fsPath(0), ec);
        if (ec) return "0";
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
        return std::to_string((long long)sec);
    }
    if (nv == "color.red") return std::string("\x1b[31m") + argS(0) + "\x1b[0m";
    if (nv == "color.green") return std::string("\x1b[32m") + argS(0) + "\x1b[0m";
    if (nv == "color.yellow") return std::string("\x1b[33m") + argS(0) + "\x1b[0m";
    if (nv == "color.blue") return std::string("\x1b[34m") + argS(0) + "\x1b[0m";
    if (nv == "color.magenta") return std::string("\x1b[35m") + argS(0) + "\x1b[0m";
    if (nv == "color.cyan") return std::string("\x1b[36m") + argS(0) + "\x1b[0m";
    if (nv == "color.bold") return std::string("\x1b[1m") + argS(0) + "\x1b[0m";
    if (nv == "color.reset") return std::string("\x1b[0m");
    // Debug builtins (no-ops when compiled with --release)
    if (nv == "debug.section") {
        std::string label = argS(0);
        std::cerr << "\n=== " << label << " ===\n";
        return {};
    }
    if (nv == "debug.assert") {
        std::string cond = argS(0);
        std::string msg = args.size() >= 2 ? argS(1) : "assertion failed";
        if (!is_truthy(cond)) {
            std::cerr << "[ASSERT FAIL] " << msg << "\n";
            std::abort();
        }
        return cond;
    }
    if (nv == "debug.assert_eq") {
        std::string a = argS(0);
        std::string b = argS(1);
        std::string msg = args.size() >= 3 ? argS(2) : "assert_eq failed: " + a + " != " + b;
        if (a != b) {
            std::cerr << "[ASSERT FAIL] " << msg << "\n";
            std::abort();
        }
        return "true";
    }
    if (nv == "debug.log") {
        std::cerr << "[LOG] " << argS(0) << "\n";
        return {};
    }
    if (nv == "debug.trace") {
        std::string cat = argS(0);
        std::string msg = args.size() >= 2 ? argS(1) : std::string();
        std::cerr << "[TRACE][" << cat << "] " << msg << "\n";
        return {};
    }
    if (nv == "debug.warn") {
        std::cerr << "\x1b[33m[WARN]\x1b[0m " << argS(0) << "\n";
        return {};
    }
    if (nv == "debug.error") {
        std::cerr << "\x1b[31m[ERROR]\x1b[0m " << argS(0) << "\n";
        return {};
    }
    if (nv == "debug.ok") {
        std::cerr << "\x1b[32m[OK]\x1b[0m " << argS(0) << "\n";
        return {};
    }
    if (nv == "debug.dump") {
        for (const auto& kv : env.vars) {
            std::cerr << "  " << kv.first << " = " << kv.second << "\n";
        }
        return {};
    }
    if (nv == "debug.dump_var") {
        std::string varName = argS(0);
        auto it = env.vars.find(varName);
        if (it != env.vars.end()) {
            std::cerr << "  " << varName << " = " << it->second << " (len=" << it->second.size() << ")\n";
        } else {
            std::cerr << "  " << varName << " = <undefined>\n";
        }
        return {};
    }
    if (nv == "debug.stack") {
        std::cerr << "[STACK] (depth: N/A)\n";
        return {};
    }
    if (nv == "debug.heap") {
        std::cerr << "[HEAP] lists=" << g_lists.size() << " dicts=" << g_dicts.size()
                  << " sets=" << g_sets.size() << " queues=" << g_queues.size()
                  << " strs=" << g_strBuffers.size() << " ptrs=" << g_ptrs.size() << "\n";
        return {};
    }
    // Debug timers (static storage for simplicity)
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_debugTimers;
    if (nv == "debug.timer_start") {
        std::string label = argS(0);
        s_debugTimers[label] = std::chrono::steady_clock::now();
        return label;
    }
    if (nv == "debug.timer_stop") {
        std::string label = argS(0);
        auto it = s_debugTimers.find(label);
        if (it != s_debugTimers.end()) {
            using namespace std::chrono;
            auto elapsed = duration_cast<milliseconds>(steady_clock::now() - it->second).count();
            s_debugTimers.erase(it);
            std::cerr << "[TIMER " << label << "] " << elapsed << "ms\n";
            return std::to_string(elapsed);
        }
        return "0";
    }
    if (nv == "debug.breakpoint") {
#ifdef _WIN32
        __debugbreak();
#else
        __builtin_trap();
#endif
        return {};
    }
    // Level and guard are no-ops at runtime (typechecker enforces)
    if (nv == "debug.level.set") { return argS(0); }
    if (nv == "debug.guard") { return argS(0); }
    if (nv == "fail") {
        throw std::runtime_error(argS(0));
    }
    // Thread helpers (spawn/join/kill live in experimental threads module)
    if (nv == "thread.sleep" || nv == "thread_sleep") {
        int ms = static_cast<int>(to_int(argS(0)));
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return {};
    }
    // thread.result stub until result tracking is wired deeper
    if (nv == "thread.result" || nv == "thread_result") {
        return {};
    }
    if (nv == "strbuf_new") {
        const int id = g_nextStrBufId++;
        g_strBuffers[id] = args.empty() ? std::string() : argS(0);
        return std::string("strbuf:") + std::to_string(id);
    }
    if (nv == "strbuf_append") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        it->second += argS(1);
        return {};
    }
    if (nv == "strbuf_clear") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        it->second.clear();
        return {};
    }
    if (nv == "strbuf_len") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return "0";
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return "0";
        return std::to_string(static_cast<long long>(it->second.size()));
    }
    if (nv == "strbuf_to_string") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        return it->second;
    }
    if (nv == "strbuf_free") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        g_strBuffers.erase(id);
        return {};
    }
    if (nv == "strbuf_reserve") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        const long long cap = std::max<long long>(0, to_int(argS(1)));
        it->second.reserve(static_cast<std::size_t>(cap));
        return {};
    }
    if (nv == "set_new" || nv == "set_of") {
        const int id = g_nextSetId++;
        g_sets[id] = {};
        for (const auto& a : args) g_sets[id].insert(eval_string(*a, env));
        return std::string("set:") + std::to_string(id);
    }
    if (nv == "set_add") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "false";
        const int id = to_int(h.substr(4));
        const auto inserted = g_sets[id].insert(argS(1));
        return inserted.second ? "true" : "false";
    }
    if (nv == "set_has") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "false";
        const int id = to_int(h.substr(4));
        return g_sets[id].count(argS(1)) ? "true" : "false";
    }
    if (nv == "set_remove") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "false";
        const int id = to_int(h.substr(4));
        return g_sets[id].erase(argS(1)) > 0 ? "true" : "false";
    }
    if (nv == "set_size") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "0";
        const int id = to_int(h.substr(4));
        return std::to_string(static_cast<long long>(g_sets[id].size()));
    }
    if (nv == "set_values") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return {};
        const int id = to_int(h.substr(4));
        const int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& value : g_sets[id]) g_lists[lid].push_back(value_from_legacy_string(value));
        sort_list_by_display(g_lists[lid]);
        return std::string("list:") + std::to_string(lid);
    }
    if (nv == "set_union") {
        const std::string h1 = argS(0);
        const std::string h2 = argS(1);
        if (h1.rfind("set:", 0) != 0 || h2.rfind("set:", 0) != 0) return {};
        const int id1 = to_int(h1.substr(4));
        const int id2 = to_int(h2.substr(4));
        const int outId = g_nextSetId++;
        auto& out = g_sets[outId];
        auto it1 = g_sets.find(id1);
        if (it1 != g_sets.end()) out.insert(it1->second.begin(), it1->second.end());
        auto it2 = g_sets.find(id2);
        if (it2 != g_sets.end()) out.insert(it2->second.begin(), it2->second.end());
        return std::string("set:") + std::to_string(outId);
    }
    if (nv == "set_intersect") {
        const std::string h1 = argS(0);
        const std::string h2 = argS(1);
        if (h1.rfind("set:", 0) != 0 || h2.rfind("set:", 0) != 0) return {};
        const int id1 = to_int(h1.substr(4));
        const int id2 = to_int(h2.substr(4));
        const int outId = g_nextSetId++;
        auto& out = g_sets[outId];
        auto it1 = g_sets.find(id1);
        auto it2 = g_sets.find(id2);
        if (it1 == g_sets.end() || it2 == g_sets.end()) return std::string("set:") + std::to_string(outId);
        const auto& lhs = it1->second;
        const auto& rhs = it2->second;
        const auto& probe = lhs.size() <= rhs.size() ? lhs : rhs;
        const auto& other = lhs.size() <= rhs.size() ? rhs : lhs;
        for (const auto& v : probe) {
            if (other.count(v) > 0) out.insert(v);
        }
        return std::string("set:") + std::to_string(outId);
    }
    if (nv == "set_diff") {
        const std::string h1 = argS(0);
        const std::string h2 = argS(1);
        if (h1.rfind("set:", 0) != 0 || h2.rfind("set:", 0) != 0) return {};
        const int id1 = to_int(h1.substr(4));
        const int id2 = to_int(h2.substr(4));
        const int outId = g_nextSetId++;
        auto& out = g_sets[outId];
        auto it1 = g_sets.find(id1);
        if (it1 == g_sets.end()) return std::string("set:") + std::to_string(outId);
        auto it2 = g_sets.find(id2);
        if (it2 == g_sets.end()) {
            out.insert(it1->second.begin(), it1->second.end());
            return std::string("set:") + std::to_string(outId);
        }
        for (const auto& v : it1->second) {
            if (it2->second.count(v) == 0) out.insert(v);
        }
        return std::string("set:") + std::to_string(outId);
    }
    if (nv == "queue_new") {
        const int id = g_nextQueueId++;
        g_queues[id] = {};
        for (const auto& a : args) g_queues[id].push_back(eval_string(*a, env));
        return std::string("queue:") + std::to_string(id);
    }
    if (nv == "queue_push") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        g_queues[id].push_back(argS(1));
        return {};
    }
    if (nv == "queue_pop") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        auto& q = g_queues[id];
        if (q.empty()) return {};
        std::string out = q.front();
        q.pop_front();
        return out;
    }
    if (nv == "queue_peek") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        auto& q = g_queues[id];
        if (q.empty()) return {};
        return q.front();
    }
    if (nv == "queue_len") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return "0";
        const int id = to_int(h.substr(6));
        return std::to_string(static_cast<long long>(g_queues[id].size()));
    }
    if (nv == "queue_clear") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        g_queues[id].clear();
        return {};
    }
    if (nv == "chan_new") {
        const int id = g_nextChanId++;
        auto ch = std::make_shared<ChannelState>();
        if (!args.empty()) {
            int cap = static_cast<int>(to_int(argS(0)));
            if (cap < 0) cap = 0;
            ch->capacity = cap;
        }
        g_channels[id] = ch;
        return std::string("chan:") + std::to_string(id);
    }
    if (nv == "chan_send") {
        const std::string h = argS(0);
        if (h.rfind("chan:", 0) != 0) throw std::runtime_error("chan_send: invalid handle");
        const int id = to_int(h.substr(5));
        auto it = g_channels.find(id);
        if (it == g_channels.end() || !it->second) throw std::runtime_error("chan_send: unknown channel");
        auto ch = it->second;
        std::unique_lock<std::mutex> lock(ch->mu);
        while (true) {
            if (tls_current_future && tls_current_future->cancelled) {
                throw std::runtime_error("future cancelled");
            }
            if (ch->closed || ch->capacity == 0 || static_cast<int>(ch->q.size()) < ch->capacity) {
                break;
            }
            ch->cv.wait_for(lock, std::chrono::milliseconds(25));
        }
        if (tls_current_future && tls_current_future->cancelled) {
            throw std::runtime_error("future cancelled");
        }
        if (ch->closed) throw std::runtime_error("chan_send: channel closed");
        ch->q.push_back(argS(1));
        ch->cv.notify_all();
        return {};
    }
    if (nv == "chan_recv") {
        const std::string h = argS(0);
        if (h.rfind("chan:", 0) != 0) throw std::runtime_error("chan_recv: invalid handle");
        const int id = to_int(h.substr(5));
        auto it = g_channels.find(id);
        if (it == g_channels.end() || !it->second) throw std::runtime_error("chan_recv: unknown channel");
        auto ch = it->second;
        std::unique_lock<std::mutex> lock(ch->mu);
        while (true) {
            if (tls_current_future && tls_current_future->cancelled) {
                throw std::runtime_error("future cancelled");
            }
            if (ch->closed || !ch->q.empty()) {
                break;
            }
            ch->cv.wait_for(lock, std::chrono::milliseconds(25));
        }
        if (tls_current_future && tls_current_future->cancelled) {
            throw std::runtime_error("future cancelled");
        }
        if (ch->q.empty()) throw std::runtime_error("chan_recv: channel closed");
        std::string out = std::move(ch->q.front());
        ch->q.pop_front();
        ch->cv.notify_all();
        return out;
    }
    if (nv == "chan_try_send") {
        const std::string h = argS(0);
        if (h.rfind("chan:", 0) != 0) return "false";
        const int id = to_int(h.substr(5));
        auto it = g_channels.find(id);
        if (it == g_channels.end() || !it->second) return "false";
        auto ch = it->second;
        std::lock_guard<std::mutex> lock(ch->mu);
        if (ch->closed) return "false";
        if (ch->capacity > 0 && static_cast<int>(ch->q.size()) >= ch->capacity) return "false";
        ch->q.push_back(argS(1));
        ch->cv.notify_all();
        return "true";
    }
    if (nv == "chan_try_recv") {
        const std::string h = argS(0);
        if (h.rfind("chan:", 0) != 0) return {};
        const int id = to_int(h.substr(5));
        auto it = g_channels.find(id);
        if (it == g_channels.end() || !it->second) return {};
        auto ch = it->second;
        std::lock_guard<std::mutex> lock(ch->mu);
        if (ch->q.empty()) return {};
        std::string out = std::move(ch->q.front());
        ch->q.pop_front();
        ch->cv.notify_all();
        return out;
    }
    if (nv == "chan_close") {
        const std::string h = argS(0);
        if (h.rfind("chan:", 0) != 0) return {};
        const int id = to_int(h.substr(5));
        auto it = g_channels.find(id);
        if (it == g_channels.end() || !it->second) return {};
        {
            std::lock_guard<std::mutex> lock(it->second->mu);
            it->second->closed = true;
        }
        it->second->cv.notify_all();
        return {};
    }
    if (nv == "chan_len") {
        const std::string h = argS(0);
        if (h.rfind("chan:", 0) != 0) return "0";
        const int id = to_int(h.substr(5));
        auto it = g_channels.find(id);
        if (it == g_channels.end() || !it->second) return "0";
        std::lock_guard<std::mutex> lock(it->second->mu);
        return std::to_string(static_cast<long long>(it->second->q.size()));
    }
    if (nv == "mutex_new") {
        const int id = g_nextMutexId++;
        g_mutexes[id] = std::make_shared<ScriptMutex>();
        return std::string("mutex:") + std::to_string(id);
    }
    if (nv == "mutex_lock") {
        const std::string h = argS(0);
        if (h.rfind("mutex:", 0) != 0) throw std::runtime_error("mutex_lock: invalid handle");
        const int id = to_int(h.substr(6));
        auto it = g_mutexes.find(id);
        if (it == g_mutexes.end() || !it->second) throw std::runtime_error("mutex_lock: unknown mutex");
        it->second->mu.lock();
        return {};
    }
    if (nv == "mutex_unlock") {
        const std::string h = argS(0);
        if (h.rfind("mutex:", 0) != 0) throw std::runtime_error("mutex_unlock: invalid handle");
        const int id = to_int(h.substr(6));
        auto it = g_mutexes.find(id);
        if (it == g_mutexes.end() || !it->second) throw std::runtime_error("mutex_unlock: unknown mutex");
        it->second->mu.unlock();
        return {};
    }
    if (nv == "mutex_try_lock") {
        const std::string h = argS(0);
        if (h.rfind("mutex:", 0) != 0) return "false";
        const int id = to_int(h.substr(6));
        auto it = g_mutexes.find(id);
        if (it == g_mutexes.end() || !it->second) return "false";
        return it->second->mu.try_lock() ? "true" : "false";
    }
    if (nv == "future_cancel") {
        const std::string h = argS(0);
        if (h.rfind("future:", 0) != 0) return "false";
        const int id = to_int(h.substr(7));
        auto it = g_futures.find(id);
        if (it == g_futures.end() || !it->second) return "false";
        {
            std::lock_guard<std::mutex> lock(it->second->mu);
            if (it->second->done) return "false";
            it->second->cancelled = true;
            it->second->failed = true;
            it->second->error = "future cancelled";
            if (!it->second->done) {
                // leave done false until worker finishes or await observes cancel
            }
        }
        it->second->cv.notify_all();
        notify_channels_for_cancel();
        return "true";
    }
    if (nv == "char_is_digit" || nv == "char.is_digit") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        return std::isdigit(static_cast<unsigned char>(s[0])) ? "true" : "false";
    }
    if (nv == "char_is_space" || nv == "char.is_space") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        return std::isspace(static_cast<unsigned char>(s[0])) ? "true" : "false";
    }
    if (nv == "char_is_alpha" || nv == "char.is_alpha") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        return std::isalpha(static_cast<unsigned char>(s[0])) ? "true" : "false";
    }
    if (nv == "char_is_ident_start" || nv == "char.is_ident_start") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        const unsigned char ch = static_cast<unsigned char>(s[0]);
        return (std::isalpha(ch) || ch == '_') ? "true" : "false";
    }
    if (nv == "char_is_ident_part" || nv == "char.is_ident_part") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        const unsigned char ch = static_cast<unsigned char>(s[0]);
        return (std::isalnum(ch) || ch == '_') ? "true" : "false";
    }
    // Time utilities
    if (nv == "now_iso" || nv == "time.now_iso") {
        using namespace std::chrono;
        auto t = system_clock::now(); auto tt = system_clock::to_time_t(t);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[32]; std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    }
    if (nv == "rand_int" || nv == "time.rand_int") {
        int a = (int)to_int(argS(0)); int b = (int)to_int(argS(1)); if (a > b) std::swap(a,b);
        if (s_deterministic) {
            uint64_t r = deterministic_rng();
            int span = (b - a) + 1; if (span <= 0) span = 1;
            return std::to_string(a + (int)(r % (uint64_t)span));
        } else {
            static std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> dist(a, b);
            return std::to_string(dist(rng));
        }
    }
    if (nv == "uuid" || nv == "time.uuid") {
#ifdef _WIN32
        UUID u; if (UuidCreate(&u) == RPC_S_OK) {
            RPC_CSTR s = nullptr; if (UuidToStringA(&u, &s) == RPC_S_OK && s) {
                std::string out(reinterpret_cast<char*>(s)); RpcStringFreeA(&s); return out;
            }
        }
        return {};
#else
        // Fallback: pseudo UUID
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> d(0, 15);
        const char* hex = "0123456789abcdef"; std::string r(36, '-');
        int idxs[] = {8,13,18,23}; int p = 0;
        for (int i=0;i<36;++i) { if (p < 4 && i == idxs[p]) { ++p; continue; } r[i] = hex[d(rng)]; }
        return r;
#endif
    }
    // map: apply func to each element, return new list
    if (nv == "map") {
        if (args.size() < 2) return {};
        std::string listHandle = eval_string(*args[0], env);
        std::string funcHandle = eval_string(*args[1], env);
        // Parse list handle
        auto listIdStr = listHandle.substr(5);
        int listId = std::stoi(listIdStr);
        auto lit = g_lists.find(listId);
        if (lit == g_lists.end()) return {};
        // Parse func handle
        auto funcIdStr = funcHandle.substr(5);
        int funcId = std::stoi(funcIdStr);
        auto fit = g_closures.find(funcId);
        if (fit == g_closures.end() || !fit->second) return {};
        ClosureData* cd = fit->second;
        // Apply to each element
        int newListId = g_nextListId++;
        for (const auto& elem : lit->second) {
            Env callEnv;
            for (const auto& cv : cd->captured) {
                if (cv.cell) {
                    callEnv.cells[cv.name] = cv.cell;
                    callEnv.vars[cv.name] = *cv.cell;
                }
            }
            if (!cd->body.params.empty()) callEnv.vars[cd->body.params[0].name] = elem;
            ExecContext child;
            exec_block(cd->body.body, *currentProgram_, child, callEnv);
            g_lists[newListId].push_back(child.returned ? child.returnValue : elem);
        }
        return std::string("list:") + std::to_string(newListId);
    }
    // filter: keep elements where func returns truthy
    if (nv == "filter") {
        if (args.size() < 2) return {};
        std::string listHandle = eval_string(*args[0], env);
        std::string funcHandle = eval_string(*args[1], env);
        auto listIdStr = listHandle.substr(5);
        int listId = std::stoi(listIdStr);
        auto lit = g_lists.find(listId);
        if (lit == g_lists.end()) return {};
        auto funcIdStr = funcHandle.substr(5);
        int funcId = std::stoi(funcIdStr);
        auto fit = g_closures.find(funcId);
        if (fit == g_closures.end() || !fit->second) return {};
        ClosureData* cd = fit->second;
        int newListId = g_nextListId++;
        for (const auto& elem : lit->second) {
            Env callEnv;
            for (const auto& cv : cd->captured) {
                if (cv.cell) {
                    callEnv.cells[cv.name] = cv.cell;
                    callEnv.vars[cv.name] = *cv.cell;
                }
            }
            if (!cd->body.params.empty()) callEnv.vars[cd->body.params[0].name] = elem;
            ExecContext child;
            exec_block(cd->body.body, *currentProgram_, child, callEnv);
            std::string result = child.returned ? child.returnValue : elem;
            if (!result.empty() && result != "0" && result != "false") {
                g_lists[newListId].push_back(elem);
            }
        }
        return std::string("list:") + std::to_string(newListId);
    }
    // reduce: accumulate values
    if (nv == "reduce") {
        if (args.size() < 3) return {};
        std::string listHandle = eval_string(*args[0], env);
        std::string funcHandle = eval_string(*args[1], env);
        std::string initial = eval_string(*args[2], env);
        auto listIdStr = listHandle.substr(5);
        int listId = std::stoi(listIdStr);
        auto lit = g_lists.find(listId);
        if (lit == g_lists.end()) return initial;
        auto funcIdStr = funcHandle.substr(5);
        int funcId = std::stoi(funcIdStr);
        auto fit = g_closures.find(funcId);
        if (fit == g_closures.end() || !fit->second) return initial;
        ClosureData* cd = fit->second;
        std::string acc = initial;
        for (const auto& elem : lit->second) {
            Env callEnv;
            for (const auto& cv : cd->captured) {
                if (cv.cell) {
                    callEnv.cells[cv.name] = cv.cell;
                    callEnv.vars[cv.name] = *cv.cell;
                }
            }
            if (cd->body.params.size() > 0) callEnv.vars[cd->body.params[0].name] = acc;
            if (cd->body.params.size() > 1) callEnv.vars[cd->body.params[1].name] = elem;
            ExecContext child;
            exec_block(cd->body.body, *currentProgram_, child, callEnv);
            acc = child.returned ? child.returnValue : acc;
        }
        return acc;
    }
    if (nv == "list_new") {
        int id = g_nextListId++;
        g_lists[id].reserve(args.size());
        for (const auto& a : args) {
            g_lists[id].push_back(eval_arg(*a, env));
        }
        return to_display_string(make_handle_value(HandleKind::List, static_cast<uint32_t>(id)));
    }
    if (nv == "list_push") {
        warn_deprecated("list_push", "method call list.push(value)");
        Value hv = argV(0); std::string v = argS(1);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv)); g_lists[id].push_back(value_from_legacy_string(v));
        }
        return {};
    }
    if (nv == "list_get") {
        Value hv = argV(0); int idx = to_int(argS(1));
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv)); auto& vec = g_lists[id];
            if (idx >=0 && idx < (int)vec.size()) return vec[idx];
        }
        return {};
    }
    if (nv == "list_len") {
        Value hv = argV(0);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv));
            return std::to_string((int)g_lists[id].size());
        }
        return "0";
    }
    if (nv == "list_join") {
        Value hv = argV(0); std::string sep = argS(1);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv));
            std::ostringstream ss; const auto& v = g_lists[id];
            for (size_t i=0;i<v.size();++i) { if (i) ss << sep; ss << v[i]; }
            return ss.str();
        }
        return {};
    }
    if (nv == "list_clear") {
        Value hv = argV(0);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv)); g_lists[id].clear();
        }
        return {};
    }
    if (nv == "list_remove_at") {
        Value hv = argV(0); int idx = (int)to_int(argS(1));
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv));
            auto& v = g_lists[id];
            if (idx>=0 && idx<(int)v.size()) v.erase(v.begin()+idx);
        }
        return {};
    }
    if (nv == "dict_new") {
        int id = g_nextDictId++;
        g_dicts[id] = {};
        // Optional key/value varargs: dict_new("k1", v1, "k2", v2, ...)
        for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
            std::string key = eval_string(*args[i], env);
            g_dicts[id][key] = eval_arg(*args[i + 1], env);
        }
        return to_display_string(make_handle_value(HandleKind::Dict, static_cast<uint32_t>(id)));
    }
    auto join_builtin_path = [&](size_t from, size_t toExclusive) -> std::string {
        if (toExclusive <= from) return {};
        std::ostringstream oss;
        for (size_t i = from; i < toExclusive; ++i) {
            if (i > from) oss << '.';
            oss << argS(i);
        }
        return oss.str();
    };
    if (nv == "dict_set") {
        warn_deprecated("dict_set", "method call dict.set(key, value)");
        Value hv = argV(0);
        if (args.size() < 3) return {};
        std::string k = join_builtin_path(1, args.size() - 1);
        std::string v = argS(args.size() - 1);
        if (value_is_handle(hv, HandleKind::Dict)) {
            int id = static_cast<int>(value_handle_id(hv)); g_dicts[id][k] = v;
        }
        return {};
    }
    if (nv == "dict_get") {
        Value hv = argV(0);
        if (args.size() < 2) return {};
        std::string k = join_builtin_path(1, args.size());
        if (value_is_handle(hv, HandleKind::Dict)) {
            int id = static_cast<int>(value_handle_id(hv));
            auto it = g_dicts[id].find(k);
            if (it!=g_dicts[id].end()) return it->second;
        }
        return {};
    }
    if (nv == "dict_has") {
        Value hv = argV(0);
        if (args.size() < 2) return "false";
        std::string k = join_builtin_path(1, args.size());
        if (value_is_handle(hv, HandleKind::Dict)) {
            int id = static_cast<int>(value_handle_id(hv));
            return (g_dicts[id].count(k)?"true":"false");
        }
        return "false";
    }
    if (nv == "dict_keys") {
        Value hv = argV(0);
        if (!value_is_handle(hv, HandleKind::Dict)) return {};
        int id = static_cast<int>(value_handle_id(hv));
        int lid = g_nextListId++; g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(value_from_legacy_string(kv.first));
        sort_list_by_display(g_lists[lid]);
        return std::string("list:") + std::to_string(lid);
    }
    if (nv == "dict_values") {
        std::string h = argS(0); if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5)); int lid = g_nextListId++; g_lists[lid] = {};
        std::vector<std::pair<std::string, Value>> ordered;
        ordered.reserve(g_dicts[id].size());
        for (const auto& kv : g_dicts[id]) ordered.push_back(kv);
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
        for (const auto& kv : ordered) g_lists[lid].push_back(kv.second);
        return std::string("list:") + std::to_string(lid);
    }
    if (nv == "dict_get_or") {
        std::string h = argS(0);
        if (args.size() < 3) return {};
        std::string k = join_builtin_path(1, args.size() - 1);
        std::string def = argS(args.size() - 1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            auto it = g_dicts[id].find(k);
            if (it != g_dicts[id].end()) return it->second;
        }
        return def;
    }
    if (nv == "dict_remove") {
        std::string h = argS(0);
        if (args.size() < 2) return "false";
        std::string k = join_builtin_path(1, args.size());
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(k) ? "true" : "false";
        }
        return "false";
    }
    if (nv == "dict_clear") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id].clear();
        }
        return {};
    }
    if (nv == "dict_size") {
        Value hv = argV(0);
        if (value_is_handle(hv, HandleKind::Dict)) {
            int id = static_cast<int>(value_handle_id(hv));
            return std::to_string((int)g_dicts[id].size());
        }
        return "0";
    }
    if (nv == "dict_merge") {
        std::string target = argS(0);
        std::string source = argS(1);
        if (target.rfind("dict:", 0) == 0 && source.rfind("dict:", 0) == 0) {
            int targetId = to_int(target.substr(5));
            int sourceId = to_int(source.substr(5));
            for (const auto& kv : g_dicts[sourceId]) {
                g_dicts[targetId][kv.first] = kv.second;
            }
        }
        return {};
    }
    if (nv == "dict_clone") {
        std::string source = argS(0);
        int newId = g_nextDictId++;
        g_dicts[newId] = {};
        if (source.rfind("dict:", 0) == 0) {
            int sourceId = to_int(source.substr(5));
            g_dicts[newId] = g_dicts[sourceId];
        }
        return std::string("dict:") + std::to_string(newId);
    }
    if (nv == "dict_items" || nv == "dict_entries") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) {
            g_lists[lid].push_back(value_from_legacy_string(kv.first + "=" + to_display_string(kv.second)));
        }
        sort_list_by_display(g_lists[lid]);
        return std::string("list:") + std::to_string(lid);
    }
    if (nv == "dict_set_path") {
        std::string h = argS(0); std::string path = argS(1); std::string v = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id][path] = v;
        }
        return {};
    }
    if (nv == "dict_get_path") {
        std::string h = argS(0); std::string path = argS(1);
        std::string def = (args.size() > 2) ? argS(2) : std::string();
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            auto it = g_dicts[id].find(path);
            if (it != g_dicts[id].end()) return it->second;
        }
        return def;
    }
    if (nv == "dict_has_path") {
        std::string h = argS(0); std::string path = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].count(path) ? "true" : "false";
        }
        return "false";
    }
    if (nv == "dict_remove_path") {
        std::string h = argS(0); std::string path = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(path) ? "true" : "false";
        }
        return "false";
    }
    if (nv == "table_new") {
        int id = g_nextDictId++;
        g_dicts[id] = {};
        return std::string("dict:") + std::to_string(id);
    }
    if (nv == "table_put") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        std::string value = argS(3);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id][row + "." + col] = value;
        }
        return {};
    }
    if (nv == "table_get") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        std::string def = (args.size() > 3) ? argS(3) : std::string();
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            auto key = row + "." + col;
            auto it = g_dicts[id].find(key);
            if (it != g_dicts[id].end()) return it->second;
        }
        return def;
    }
    if (nv == "table_has") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].count(row + "." + col) ? "true" : "false";
        }
        return "false";
    }
    if (nv == "table_remove") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(row + "." + col) ? "true" : "false";
        }
        return "false";
    }
    if (nv == "table_rows") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        std::unordered_set<std::string> rows;
        for (const auto& kv : g_dicts[id]) {
            auto pos = kv.first.find('.');
            if (pos != std::string::npos) {
                rows.insert(kv.first.substr(0, pos));
            }
        }
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& row : rows) g_lists[lid].push_back(value_from_legacy_string(row));
        sort_list_by_display(g_lists[lid]);
        return std::string("list:") + std::to_string(lid);
    }
    if (nv == "table_columns") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        std::unordered_set<std::string> cols;
        for (const auto& kv : g_dicts[id]) {
            auto pos = kv.first.find('.');
            if (pos != std::string::npos && pos + 1 < kv.first.size()) {
                cols.insert(kv.first.substr(pos + 1));
            }
        }
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& col : cols) g_lists[lid].push_back(value_from_legacy_string(col));
        sort_list_by_display(g_lists[lid]);
        return std::string("list:") + std::to_string(lid);
    }
    if (nv == "table_row_keys") {
        std::string h = argS(0);
        std::string row = argS(1);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        std::string prefix = row + ".";
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) {
            if (kv.first.rfind(prefix, 0) == 0 && kv.first.size() > prefix.size()) {
                g_lists[lid].push_back(value_from_legacy_string(kv.first.substr(prefix.size())));
            }
        }
        sort_list_by_display(g_lists[lid]);
        return std::string("list:") + std::to_string(lid);
    }
    if (nv == "table_clear_row") {
        std::string h = argS(0);
        std::string row = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            std::string prefix = row + ".";
            std::vector<std::string> toErase;
            for (const auto& kv : g_dicts[id]) {
                if (kv.first.rfind(prefix, 0) == 0) toErase.push_back(kv.first);
            }
            for (const auto& key : toErase) g_dicts[id].erase(key);
        }
        return {};
    }
    if (nv == "table_count_row") {
        std::string h = argS(0);
        std::string row = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            std::string prefix = row + ".";
            int count = 0;
            for (const auto& kv : g_dicts[id]) {
                if (kv.first.rfind(prefix, 0) == 0) ++count;
            }
            return std::to_string(count);
        }
        return "0";
    }
    if (nv == "now_ms" || nv == "time.now_ms") {
        using namespace std::chrono;
        return std::to_string(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }
    if (nv == "env" || nv == "env.get") {
        std::string key = argS(0);
#ifdef _WIN32
        std::wstring wkey(key.begin(), key.end());
        DWORD len = GetEnvironmentVariableW(wkey.c_str(), nullptr, 0);
        if (len) {
            std::wstring buf; buf.resize(len);
            GetEnvironmentVariableW(wkey.c_str(), buf.data(), len);
            std::string out(buf.begin(), buf.end());
            if (!out.empty() && out.back()=='\0') out.pop_back();
            return out;
        }
        return {};
#else
        const char* v = std::getenv(key.c_str());
        return v ? std::string(v) : std::string();
#endif
    }
    if (nv == "dotenv_load" || nv == "env.load_dotenv") {
        std::string filepath = argS(0);
        if (filepath.empty()) {
            filepath = ".env";
        }
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return "false";
        }
        std::string line;
        int loaded = 0;
        while (std::getline(file, line)) {
            // Trim leading whitespace
            size_t start = 0;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t' || line[start] == '\r')) {
                ++start;
            }
            if (start >= line.size()) continue; // empty line
            if (line[start] == '#') continue;   // comment
            // Find '='
            size_t eq = line.find('=', start);
            if (eq == std::string::npos) continue;
            std::string key = line.substr(start, eq - start);
            // Trim trailing whitespace from key
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                key.pop_back();
            }
            if (key.empty()) continue;
            std::string value = line.substr(eq + 1);
            // Trim leading whitespace from value
            size_t vstart = 0;
            while (vstart < value.size() && (value[vstart] == ' ' || value[vstart] == '\t')) {
                ++vstart;
            }
            value = value.substr(vstart);
            // Trim trailing whitespace and \r from value
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
                value.pop_back();
            }
            // Strip surrounding quotes if present
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
#ifdef _WIN32
            _putenv_s(key.c_str(), value.c_str());
#else
            setenv(key.c_str(), value.c_str(), 1);
#endif
            ++loaded;
        }
        return std::to_string(loaded);
    }
    // Unified external built-in module dispatch (evaluate args only once if present)
    std::vector<std::string> argv;
    if (!args.empty()) {
        argv.reserve(args.size());
        for (const auto& a : args) argv.push_back(eval_string(*a, env));
    }
    const std::string& n = nameStr; // alias for dispatchers expecting std::string
    if (auto imported = dispatch_imported_builtin_modules(
            const_cast<Runtime*>(this), currentProgram_, n, argv)) {
        return *imported;
    }
    if (currentProgram_) {
        if (program_imports_module(currentProgram_, "builtin/math")) {
            if (auto r = __erelang_builtin_math_dispatch(n, argv); !r.empty()) return r;
        }
        if (program_imports_module(currentProgram_, "builtin/system")) {
            if (auto r = __erelang_builtin_system_dispatch(n, argv); !r.empty()) return r;
        }
        if (program_imports_module(currentProgram_, "builtin/data")) {
            if (auto r = __erelang_builtin_data_dispatch(n, argv); !r.empty()) return r;
        }
        if (program_imports_module(currentProgram_, "builtin/perm")) {
            if (auto r = __erelang_builtin_perm_dispatch(n, argv); !r.empty()) return r;
        }
    }
    return {};
}

std::optional<std::string> dispatch_imported_builtin_modules(
    Runtime* runtime,
    const Program* program,
    const std::string& name,
    const std::vector<std::string>& argv) {
    if (!program) {
        return std::nullopt;
    }

    if (program_imports_module(program, "builtin/network") || program_imports_module(program, "builtin/net")) {
        if (auto r = __erelang_builtin_network_dispatch(name, argv); !r.empty()) {
            return r;
        }
    }
    if (program_imports_module(program, "builtin/regex")) {
        if (auto r = __erelang_builtin_regex_dispatch(name, argv); !r.empty()) {
            return r;
        }
    }
    if (program_imports_module(program, "builtin/crypto")) {
        if (auto r = __erelang_builtin_crypto_dispatch(name, argv); !r.empty()) {
            return r;
        }
    }
    if (program_imports_module(program, "builtin/performance") || program_imports_module(program, "builtin/perf")) {
        if (auto r = __erelang_builtin_performance_dispatch(name, argv); !r.empty()) {
            return r;
        }
    }
    if (program_imports_module(program, "builtin/binary")) {
        if (auto r = __erelang_builtin_binary_dispatch(name, argv); !r.empty()) {
            return r;
        }
    }
    if (program_imports_module(program, "builtin/websocket") || program_imports_module(program, "builtin/ws")) {
        if (auto r = __erelang_builtin_websocket_dispatch(name, argv); !r.empty()) {
            return r;
        }
    }
    if (program_imports_module(program, "builtin/threads")) {
        if (auto r = __erelang_builtin_threads_dispatch(runtime, name, argv); !r.empty()) {
            return r;
        }
    }
    if (program_imports_module(program, "builtin/monitor")) {
        if (auto r = __erelang_builtin_monitor_dispatch(runtime, name, argv); !r.empty()) {
            return r;
        }
    }
    return std::nullopt;
}

} // namespace erelang
