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
    if (nameStr == "bitcast") nameStr = "bit_cast";
    if (nameStr == "fopen") nameStr = "file_open";
    if (nameStr == "fclose") nameStr = "file_close";
    if (nameStr == "fread") nameStr = "file_read";
    if (nameStr == "fwrite") nameStr = "file_write";
    if (nameStr == "fseek") nameStr = "file_seek";
    if (nameStr == "ftell") nameStr = "file_tell";
    if (nameStr == "fflush") nameStr = "file_flush";
    if (nameStr == "string_buffer_new") nameStr = "strbuf_new";
    if (nameStr == "string_buffer_append") nameStr = "strbuf_append";
    if (nameStr == "string_buffer_clear") nameStr = "strbuf_clear";
    if (nameStr == "string_buffer_len") nameStr = "strbuf_len";
    if (nameStr == "string_buffer_to_string") nameStr = "strbuf_to_string";
    if (nameStr == "string_buffer_free") nameStr = "strbuf_free";
    if (nameStr == "string_buffer_reserve") nameStr = "strbuf_reserve";
    if (nameStr == "hashmap_new") nameStr = "dict_new";
    if (nameStr == "hashmap_set") nameStr = "dict_set";
    if (nameStr == "hashmap_put") nameStr = "dict_set";
    if (nameStr == "hashmap_get") nameStr = "dict_get";
    if (nameStr == "hashmap_has") nameStr = "dict_has";
    if (nameStr == "hashmap_contains") nameStr = "dict_has";
    if (nameStr == "hashmap_get_or") nameStr = "dict_get_or";
    if (nameStr == "hashmap_get_or_default") nameStr = "dict_get_or";
    if (nameStr == "hashmap_remove") nameStr = "dict_remove";
    if (nameStr == "hashmap_clear") nameStr = "dict_clear";
    if (nameStr == "hashmap_size") nameStr = "dict_size";
    if (nameStr == "hashmap_keys") nameStr = "dict_keys";
    if (nameStr == "hashmap_values") nameStr = "dict_values";
    if (nameStr == "hashmap_merge") nameStr = "dict_merge";
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
            for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
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
    if (nameStr == "advance_time" && s_deterministic) {
        uint64_t delta = (uint64_t)to_int(eval_string(*args[0], env));
        s_timeVirtual += delta; return std::to_string((long long)s_timeVirtual);
    }
    if ((nameStr == "now_ms" || nameStr == "time.now_ms") && s_deterministic) {
        return std::to_string((long long)s_timeVirtual);
    }
    if (nameStr == "plugin_core" || nameStr == "plugin.core") {
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
    if (nameStr == "plugin_core_files" || nameStr == "plugin.core_files") {
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
    if (nameStr == "plugin_core_keys" || nameStr == "plugin.core_keys") {
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
    if (nameStr == "language_name" || nameStr == "lang.name") {
        return std::string("erelang / Erelang");
    }
    if (nameStr == "language_version" || nameStr == "lang.version") {
        return std::string(erelang::BuildInfo::version());
    }
    if (nameStr == "language_about" || nameStr == "lang.about") {
        return std::string("Erelang interpreter for .elan programs.\n");
    }
    if (nameStr == "language_limitations" || nameStr == "lang.limitations") {
        return std::string("Windows-first CLI runtime. No GUI. Most I/O is import-gated.\n");
    }
    // Conversion and type-check helpers
    if (nameStr == "toint" || nameStr == "int") {
        return std::to_string((long long)to_int(argS(0)));
    }
    if (nameStr == "toInt") {
        return std::to_string((long long)to_int(argS(0)));
    }
    if (nameStr == "tofloat" || nameStr == "float") {
        double v = to_double(argS(0));
        std::ostringstream ss; ss << v; return ss.str();
    }
    if (nameStr == "tostr" || nameStr == "toString" || nameStr == "string") {
        return argS(0);
    }
    if (nameStr == "tobool" || nameStr == "bool") {
        return is_truthy(argS(0)) ? std::string("true") : std::string("false");
    }
    // Type-check methods
    if (nameStr == "int.is") { return is_int_string(argS(0)) ? "true" : "false"; }
    if (nameStr == "string.is") { return argS(0).empty() ? "false" : "true"; }
    if (nameStr == "float.is") { return is_float_string(argS(0)) ? "true" : "false"; }
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
    if (nameStr == "__builtin_typeof" || nameStr == "__builtin_decltype") {
        if (args.empty()) return "unknown";
        return infer_runtime_type_value(argV(0));
    }
    if (nameStr == "__builtin_sizeof") {
        if (args.empty()) return "0";
        std::string typeName;
        if (std::holds_alternative<ExprString>(args[0]->node)) {
            typeName = std::get<ExprString>(args[0]->node).v;
        } else {
            typeName = infer_runtime_type(eval_string(*args[0], env));
        }
        return std::to_string(size_of_type(typeName));
    }
    if (nameStr == "__builtin_alignof") {
        if (args.empty()) return "1";
        std::string typeName;
        if (std::holds_alternative<ExprString>(args[0]->node)) {
            typeName = std::get<ExprString>(args[0]->node).v;
        } else {
            typeName = infer_runtime_type(eval_string(*args[0], env));
        }
        return std::to_string(align_of_type(typeName));
    }
    if (nameStr == "__builtin_offsetof") {
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
    if (nameStr == "__builtin_is_base_of") {
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
    if (nameStr == "dynamic_cast") {
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
    if (nameStr == "reinterpret_cast") {
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
    if (nameStr == "bit_cast") {
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
    if (nameStr == "ptr_new" || nameStr == "make_unique" || nameStr == "make_shared") {
        const int id = g_nextPtrId++;
        g_ptrs[id] = argS(0);
        return format_pointer_handle(id);
    }
    if (nameStr == "malloc") {
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
    if (nameStr == "ptr_get") {
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
    if (nameStr == "ptr_set") {
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
    if (nameStr == "memcpy") {
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
    if (nameStr == "realloc") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return {};
        auto it = g_ptrs.find(*idOpt);
        if (it == g_ptrs.end()) return {};
        const long long n = std::max<long long>(0, to_int(argS(1)));
        it->second.resize(static_cast<std::size_t>(n), '\0');
        return format_pointer_handle(*idOpt);
    }
    if (nameStr == "memset") {
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
    if (nameStr == "ptr_free" || nameStr == "unique_reset" || nameStr == "shared_reset") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return {};
        const int id = *idOpt;
        g_ptrs.erase(id);
        return {};
    }
    if (nameStr == "free") {
        const std::string handle = argS(0);
        if (auto idOpt = parse_pointer_handle(handle); idOpt.has_value()) {
            const int id = *idOpt;
            g_ptrs.erase(id);
        }
        return {};
    }
    if (nameStr == "ptr_valid") {
        const std::string handle = argS(0);
        auto idOpt = parse_pointer_handle(handle);
        if (!idOpt.has_value()) return "false";
        const int id = *idOpt;
        return g_ptrs.count(id) ? "true" : "false";
    }
    if (nameStr == "to_json" || nameStr == "json.encode") {
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
    if (nameStr == "from_json" || nameStr == "json.decode") {
        if (args.empty()) return {};
        return features::from_json_object_to_dict_handle(argS(0));
    }
    if (nameStr == "string.lstrip") {
        std::string s = argS(0);
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        return s.substr(i);
    }
    if (nameStr == "string.rstrip") {
        std::string s = argS(0);
        if (s.empty()) return s;
        size_t j = s.size();
        while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1])) != 0) --j;
        return s.substr(0, j);
    }
    if (nameStr == "string.strip") {
        std::string s = argS(0);
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        size_t j = s.size();
        while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1])) != 0) --j;
        return s.substr(i, j - i);
    }
    if (nameStr == "string.lower") {
        std::string s = argS(0);
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
    if (nameStr == "string.upper") {
        std::string s = argS(0);
        for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    }
    if (nameStr == "string.starts_with") {
        std::string s = argS(0);
        std::string prefix = argS(1);
        if (prefix.size() > s.size()) return std::string("false");
        return s.compare(0, prefix.size(), prefix) == 0 ? std::string("true") : std::string("false");
    }
    if (nameStr == "string.ends_with") {
        std::string s = argS(0);
        std::string suffix = argS(1);
        if (suffix.size() > s.size()) return std::string("false");
        return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0 ? std::string("true") : std::string("false");
    }
    if (nameStr == "string.find") {
        std::string s = argS(0);
        std::string needle = argS(1);
        const auto pos = s.find(needle);
        if (pos == std::string::npos) return std::string("-1");
        return std::to_string(static_cast<int>(pos));
    }
    if (nameStr == "string.substr") {
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
    if (nameStr == "string.len") {
        return std::to_string(static_cast<int>(argS(0).size()));
    }
    if (nameStr == "is_int") {
        return is_int_string(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nameStr == "is_float") {
        return is_float_string(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nameStr == "is_str") {
        // Everything is a string at runtime; consider non-empty as true
        return argS(0).empty() ? std::string("false") : std::string("true");
    }
    if (nameStr == "args_count") {
    return std::to_string((int)s_cliArgs.size());
    }
    if (nameStr == "args_get") {
    int idx = to_int(argS(0));
    if (idx >= 0 && idx < (int)s_cliArgs.size()) return s_cliArgs[idx];
    return {};
    }
    if (nameStr == "os.args") {
        int id = g_nextListId++;
        g_lists[id] = {};
        for (const auto& a : s_cliArgs) g_lists[id].push_back(a);
        return std::string("list:") + std::to_string(id);
    }
    if (nameStr == "os.args_count") {
        return std::to_string((int)s_cliArgs.size());
    }
    if (nameStr == "os.args_get") {
        int idx = to_int(argS(0));
        if (idx >= 0 && idx < (int)s_cliArgs.size()) return s_cliArgs[idx];
        return {};
    }
    if (nameStr == "exec") {
    // Execute arbitrary command (shell). Return exit code as string.
    std::string cmd = argS(0);
#ifdef _WIN32
    std::string full = std::string("cmd /c ") + cmd;
#else
    std::string full = cmd;
#endif
    int code = std::system(full.c_str());
    return std::to_string(code);
    }
        if (nameStr == "os.exec") {
        std::string cmd = argS(0);
    #ifdef _WIN32
        std::string full = std::string("cmd /c ") + cmd;
    #else
        std::string full = cmd;
    #endif
        int code = std::system(full.c_str());
        return std::to_string(code);
        }
        if (nameStr == "spawn") {
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
        if (nameStr == "os.spawn") {
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
        if (nameStr == "exit") {
        int code = (int)to_int(argS(0));
        std::exit(code);
        }
    if (nameStr == "mini_ir_run" || nameStr == "mini_ir.run") {
        return erelang::mini_ir::encode_result(erelang::mini_ir::run_text(argS(0)));
    }
    if (nameStr == "mini_ir_run_file" || nameStr == "mini_ir.run_file") {
        return erelang::mini_ir::encode_result(erelang::mini_ir::run_file(fsPath(0).string()));
    }
    if (nameStr == "run_file") {
    // Run a file with default OS association (like double-click). No output capture; returns empty.
    std::string fp = argS(0);
#ifdef _WIN32
    std::wstring w(fp.begin(), fp.end()); ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    return {};
    }
    if (nameStr == "run_bat") {
    std::string bat = argS(0);
#ifdef _WIN32
    std::string full = std::string("cmd /c \"") + bat + "\"";
    std::system(full.c_str());
#endif
    return {};
    }
    if (nameStr == "read_line" || nameStr == "io.read_line") {
        std::string s; std::getline(std::cin, s); return s;
    }
    if (nameStr == "stdin_read" || nameStr == "io.stdin") {
        std::string s; std::getline(std::cin, s); return s;
    }
    if (nameStr == "stderr_print" || nameStr == "io.stderr") {
        std::cerr << argS(0) << "\n";
        return {};
    }
    if (nameStr == "input") {
        if (!args.empty()) {
            std::string msg = argS(0);
            std::cout << msg;
            std::cout.flush();
        }
        std::string s;
        std::getline(std::cin, s);
        return s;
    }
    if (nameStr == "prompt" || nameStr == "io.prompt" || nameStr == "io.input") {
        std::string msg = argS(0); std::cout << msg; std::cout.flush(); std::string s; std::getline(std::cin, s); return s;
    }
    // Filesystem utilities (relative paths resolve against entry script directory)
    if (nameStr == "read_text") {
        std::ifstream in(fsPath(0), std::ios::binary);
        if (!in) return {};
        std::ostringstream ss; ss << in.rdbuf();
        return ss.str();
    }
    if (nameStr == "write_text") {
        std::ofstream out(fsPath(0), std::ios::binary);
        if (!out) return {};
        out << argS(1);
        return {};
    }
    if (nameStr == "append_text") {
        std::ofstream out(fsPath(0), std::ios::binary | std::ios::app);
        if (!out) return {};
        out << argS(1);
        return {};
    }
    if (nameStr == "file_open") {
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
    if (nameStr == "file_close") {
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
    if (nameStr == "file_buffer") {
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
    if (nameStr == "file_read") {
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
    if (nameStr == "file_write") {
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
    if (nameStr == "file_seek") {
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
    if (nameStr == "file_tell") {
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
    if (nameStr == "file_flush") {
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
        if (nameStr == "log_set_level") {
            g_logLevel = level_rank(argS(0));
            return "true";
        }
        if (nameStr == "log_to_file") {
            g_logFilePath = argS(0);
            return "true";
        }
        if (nameStr == "log_write") {
            return emit_log(level_rank(argS(0)), argS(1));
        }
        if (nameStr == "log_debug") return emit_log(0, argS(0));
        if (nameStr == "log_info") return emit_log(1, argS(0));
        if (nameStr == "log_warn") return emit_log(2, argS(0));
        if (nameStr == "log_error") return emit_log(3, argS(0));
    }

    if (nameStr == "file_exists") {
        std::error_code ec;
        return std::filesystem::exists(fsPath(0), ec) && !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "is_dir") {
        std::error_code ec;
        return std::filesystem::is_directory(fsPath(0), ec) && !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "is_file") {
        std::error_code ec;
        return std::filesystem::is_regular_file(fsPath(0), ec) && !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "file_size") {
        std::error_code ec;
        auto size = std::filesystem::file_size(fsPath(0), ec);
        if (ec) return "-1";
        return std::to_string(static_cast<long long>(size));
    }
    if (nameStr == "mkdirs") {
        std::error_code ec; std::filesystem::create_directories(fsPath(0), ec); return {};
    }
    if (nameStr == "copy_file") {
        std::error_code ec; bool ok = std::filesystem::copy_file(fsPath(0), fsPath(1), std::filesystem::copy_options::overwrite_existing, ec);
        return ok && !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "move_file") {
        std::error_code ec; std::filesystem::rename(fsPath(0), fsPath(1), ec);
        return !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "delete_file") {
        std::error_code ec; std::filesystem::remove(fsPath(0), ec);
        return !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "list_files" || nameStr == "list_dirs" || nameStr == "list_regular_files") {
        const bool dirsOnly = (nameStr == "list_dirs");
        const bool filesOnly = (nameStr == "list_regular_files");
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
            g_lists[id].push_back(e.path().string());
        }
        std::sort(g_lists[id].begin(), g_lists[id].end());
        return std::string("list:") + std::to_string(id);
    }
    if (nameStr == "load_elan") {
        return load_elan_file(fsPath(0));
    }
    if (nameStr == "load_elan_dir") {
        return load_elan_directory(fsPath(0));
    }
    if (nameStr == "call_action") {
        std::vector<std::string> callArgs;
        callArgs.reserve(args.size() > 0 ? args.size() - 1 : 0);
        for (size_t i = 1; i < args.size(); ++i) {
            callArgs.push_back(argS(i));
        }
        return call_action_by_name(argS(0), callArgs);
    }
    if (nameStr == "cwd") {
        return std::filesystem::current_path().string();
    }
    if (nameStr == "chdir") {
        std::error_code ec; std::filesystem::current_path(fsPath(0), ec); return !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "path_join") {
        if (args.empty()) return {};
        std::filesystem::path p = argS(0);
        for (std::size_t i = 1; i < args.size(); ++i) {
            p /= argS(i);
        }
        return p.string();
    }
    if (nameStr == "path_dirname") {
        return std::filesystem::path(argS(0)).parent_path().string();
    }
    if (nameStr == "path_basename") {
        return std::filesystem::path(argS(0)).filename().string();
    }
    if (nameStr == "path_ext") {
        return std::filesystem::path(argS(0)).extension().string();
    }
    if (nameStr == "file_mtime") {
        std::error_code ec;
        auto ft = std::filesystem::last_write_time(fsPath(0), ec);
        if (ec) return "0";
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
        return std::to_string((long long)sec);
    }
    if (nameStr == "color.red") return std::string("\x1b[31m") + argS(0) + "\x1b[0m";
    if (nameStr == "color.green") return std::string("\x1b[32m") + argS(0) + "\x1b[0m";
    if (nameStr == "color.yellow") return std::string("\x1b[33m") + argS(0) + "\x1b[0m";
    if (nameStr == "color.blue") return std::string("\x1b[34m") + argS(0) + "\x1b[0m";
    if (nameStr == "color.magenta") return std::string("\x1b[35m") + argS(0) + "\x1b[0m";
    if (nameStr == "color.cyan") return std::string("\x1b[36m") + argS(0) + "\x1b[0m";
    if (nameStr == "color.bold") return std::string("\x1b[1m") + argS(0) + "\x1b[0m";
    if (nameStr == "color.reset") return std::string("\x1b[0m");
    // Debug builtins (no-ops when compiled with --release)
    if (nameStr == "debug.section") {
        std::string label = argS(0);
        std::cerr << "\n=== " << label << " ===\n";
        return {};
    }
    if (nameStr == "debug.assert") {
        std::string cond = argS(0);
        std::string msg = args.size() >= 2 ? argS(1) : "assertion failed";
        if (!is_truthy(cond)) {
            std::cerr << "[ASSERT FAIL] " << msg << "\n";
            std::abort();
        }
        return cond;
    }
    if (nameStr == "debug.assert_eq") {
        std::string a = argS(0);
        std::string b = argS(1);
        std::string msg = args.size() >= 3 ? argS(2) : "assert_eq failed: " + a + " != " + b;
        if (a != b) {
            std::cerr << "[ASSERT FAIL] " << msg << "\n";
            std::abort();
        }
        return "true";
    }
    if (nameStr == "debug.log") {
        std::cerr << "[LOG] " << argS(0) << "\n";
        return {};
    }
    if (nameStr == "debug.trace") {
        std::string cat = argS(0);
        std::string msg = args.size() >= 2 ? argS(1) : std::string();
        std::cerr << "[TRACE][" << cat << "] " << msg << "\n";
        return {};
    }
    if (nameStr == "debug.warn") {
        std::cerr << "\x1b[33m[WARN]\x1b[0m " << argS(0) << "\n";
        return {};
    }
    if (nameStr == "debug.error") {
        std::cerr << "\x1b[31m[ERROR]\x1b[0m " << argS(0) << "\n";
        return {};
    }
    if (nameStr == "debug.ok") {
        std::cerr << "\x1b[32m[OK]\x1b[0m " << argS(0) << "\n";
        return {};
    }
    if (nameStr == "debug.dump") {
        for (const auto& kv : env.vars) {
            std::cerr << "  " << kv.first << " = " << kv.second << "\n";
        }
        return {};
    }
    if (nameStr == "debug.dump_var") {
        std::string varName = argS(0);
        auto it = env.vars.find(varName);
        if (it != env.vars.end()) {
            std::cerr << "  " << varName << " = " << it->second << " (len=" << it->second.size() << ")\n";
        } else {
            std::cerr << "  " << varName << " = <undefined>\n";
        }
        return {};
    }
    if (nameStr == "debug.stack") {
        std::cerr << "[STACK] (depth: N/A)\n";
        return {};
    }
    if (nameStr == "debug.heap") {
        std::cerr << "[HEAP] lists=" << g_lists.size() << " dicts=" << g_dicts.size()
                  << " sets=" << g_sets.size() << " queues=" << g_queues.size()
                  << " strs=" << g_strBuffers.size() << " ptrs=" << g_ptrs.size() << "\n";
        return {};
    }
    // Debug timers (static storage for simplicity)
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_debugTimers;
    if (nameStr == "debug.timer_start") {
        std::string label = argS(0);
        s_debugTimers[label] = std::chrono::steady_clock::now();
        return label;
    }
    if (nameStr == "debug.timer_stop") {
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
    if (nameStr == "debug.breakpoint") {
#ifdef _WIN32
        __debugbreak();
#else
        __builtin_trap();
#endif
        return {};
    }
    // Level and guard are no-ops at runtime (typechecker enforces)
    if (nameStr == "debug.level.set") { return argS(0); }
    if (nameStr == "debug.guard") { return argS(0); }
    if (nameStr == "fail") {
        throw std::runtime_error(argS(0));
    }
    // Thread helpers (spawn/join/kill live in experimental threads module)
    if (nameStr == "thread.sleep" || nameStr == "thread_sleep") {
        int ms = static_cast<int>(to_int(argS(0)));
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return {};
    }
    // thread.result stub until result tracking is wired deeper
    if (nameStr == "thread.result" || nameStr == "thread_result") {
        return {};
    }
    if (nameStr == "strbuf_new") {
        const int id = g_nextStrBufId++;
        g_strBuffers[id] = args.empty() ? std::string() : argS(0);
        return std::string("strbuf:") + std::to_string(id);
    }
    if (nameStr == "strbuf_append") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        it->second += argS(1);
        return {};
    }
    if (nameStr == "strbuf_clear") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        it->second.clear();
        return {};
    }
    if (nameStr == "strbuf_len") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return "0";
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return "0";
        return std::to_string(static_cast<long long>(it->second.size()));
    }
    if (nameStr == "strbuf_to_string") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        return it->second;
    }
    if (nameStr == "strbuf_free") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        g_strBuffers.erase(id);
        return {};
    }
    if (nameStr == "strbuf_reserve") {
        const std::string handle = argS(0);
        if (handle.rfind("strbuf:", 0) != 0) return {};
        const int id = to_int(handle.substr(7));
        auto it = g_strBuffers.find(id);
        if (it == g_strBuffers.end()) return {};
        const long long cap = std::max<long long>(0, to_int(argS(1)));
        it->second.reserve(static_cast<std::size_t>(cap));
        return {};
    }
    if (nameStr == "set_new" || nameStr == "set_of") {
        const int id = g_nextSetId++;
        g_sets[id] = {};
        for (const auto& a : args) g_sets[id].insert(eval_string(*a, env));
        return std::string("set:") + std::to_string(id);
    }
    if (nameStr == "set_add") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "false";
        const int id = to_int(h.substr(4));
        const auto inserted = g_sets[id].insert(argS(1));
        return inserted.second ? "true" : "false";
    }
    if (nameStr == "set_has") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "false";
        const int id = to_int(h.substr(4));
        return g_sets[id].count(argS(1)) ? "true" : "false";
    }
    if (nameStr == "set_remove") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "false";
        const int id = to_int(h.substr(4));
        return g_sets[id].erase(argS(1)) > 0 ? "true" : "false";
    }
    if (nameStr == "set_size") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return "0";
        const int id = to_int(h.substr(4));
        return std::to_string(static_cast<long long>(g_sets[id].size()));
    }
    if (nameStr == "set_values") {
        const std::string h = argS(0);
        if (h.rfind("set:", 0) != 0) return {};
        const int id = to_int(h.substr(4));
        const int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& value : g_sets[id]) g_lists[lid].push_back(value);
        std::sort(g_lists[lid].begin(), g_lists[lid].end());
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "set_union") {
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
    if (nameStr == "set_intersect") {
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
    if (nameStr == "set_diff") {
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
    if (nameStr == "queue_new") {
        const int id = g_nextQueueId++;
        g_queues[id] = {};
        for (const auto& a : args) g_queues[id].push_back(eval_string(*a, env));
        return std::string("queue:") + std::to_string(id);
    }
    if (nameStr == "queue_push") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        g_queues[id].push_back(argS(1));
        return {};
    }
    if (nameStr == "queue_pop") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        auto& q = g_queues[id];
        if (q.empty()) return {};
        std::string out = q.front();
        q.pop_front();
        return out;
    }
    if (nameStr == "queue_peek") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        auto& q = g_queues[id];
        if (q.empty()) return {};
        return q.front();
    }
    if (nameStr == "queue_len") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return "0";
        const int id = to_int(h.substr(6));
        return std::to_string(static_cast<long long>(g_queues[id].size()));
    }
    if (nameStr == "queue_clear") {
        const std::string h = argS(0);
        if (h.rfind("queue:", 0) != 0) return {};
        const int id = to_int(h.substr(6));
        g_queues[id].clear();
        return {};
    }
    if (nameStr == "chan_new") {
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
    if (nameStr == "chan_send") {
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
    if (nameStr == "chan_recv") {
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
    if (nameStr == "chan_try_send") {
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
    if (nameStr == "chan_try_recv") {
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
    if (nameStr == "chan_close") {
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
    if (nameStr == "chan_len") {
        const std::string h = argS(0);
        if (h.rfind("chan:", 0) != 0) return "0";
        const int id = to_int(h.substr(5));
        auto it = g_channels.find(id);
        if (it == g_channels.end() || !it->second) return "0";
        std::lock_guard<std::mutex> lock(it->second->mu);
        return std::to_string(static_cast<long long>(it->second->q.size()));
    }
    if (nameStr == "mutex_new") {
        const int id = g_nextMutexId++;
        g_mutexes[id] = std::make_shared<ScriptMutex>();
        return std::string("mutex:") + std::to_string(id);
    }
    if (nameStr == "mutex_lock") {
        const std::string h = argS(0);
        if (h.rfind("mutex:", 0) != 0) throw std::runtime_error("mutex_lock: invalid handle");
        const int id = to_int(h.substr(6));
        auto it = g_mutexes.find(id);
        if (it == g_mutexes.end() || !it->second) throw std::runtime_error("mutex_lock: unknown mutex");
        it->second->mu.lock();
        return {};
    }
    if (nameStr == "mutex_unlock") {
        const std::string h = argS(0);
        if (h.rfind("mutex:", 0) != 0) throw std::runtime_error("mutex_unlock: invalid handle");
        const int id = to_int(h.substr(6));
        auto it = g_mutexes.find(id);
        if (it == g_mutexes.end() || !it->second) throw std::runtime_error("mutex_unlock: unknown mutex");
        it->second->mu.unlock();
        return {};
    }
    if (nameStr == "mutex_try_lock") {
        const std::string h = argS(0);
        if (h.rfind("mutex:", 0) != 0) return "false";
        const int id = to_int(h.substr(6));
        auto it = g_mutexes.find(id);
        if (it == g_mutexes.end() || !it->second) return "false";
        return it->second->mu.try_lock() ? "true" : "false";
    }
    if (nameStr == "future_cancel") {
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
    if (nameStr == "char_is_digit" || nameStr == "char.is_digit") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        return std::isdigit(static_cast<unsigned char>(s[0])) ? "true" : "false";
    }
    if (nameStr == "char_is_space" || nameStr == "char.is_space") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        return std::isspace(static_cast<unsigned char>(s[0])) ? "true" : "false";
    }
    if (nameStr == "char_is_alpha" || nameStr == "char.is_alpha") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        return std::isalpha(static_cast<unsigned char>(s[0])) ? "true" : "false";
    }
    if (nameStr == "char_is_ident_start" || nameStr == "char.is_ident_start") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        const unsigned char ch = static_cast<unsigned char>(s[0]);
        return (std::isalpha(ch) || ch == '_') ? "true" : "false";
    }
    if (nameStr == "char_is_ident_part" || nameStr == "char.is_ident_part") {
        const std::string s = argS(0);
        if (s.empty()) return "false";
        const unsigned char ch = static_cast<unsigned char>(s[0]);
        return (std::isalnum(ch) || ch == '_') ? "true" : "false";
    }
    // Time utilities
    if (nameStr == "now_iso" || nameStr == "time.now_iso") {
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
    if (nameStr == "rand_int" || nameStr == "time.rand_int") {
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
    if (nameStr == "uuid" || nameStr == "time.uuid") {
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
    if (nameStr == "map") {
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
    if (nameStr == "filter") {
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
    if (nameStr == "reduce") {
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
    if (nameStr == "list_new") {
        int id = g_nextListId++;
        g_lists[id] = {};
        for (const auto& a : args) {
            g_lists[id].push_back(eval_string(*a, env));
        }
        return to_display_string(make_handle_value(HandleKind::List, static_cast<uint32_t>(id)));
    }
    if (nameStr == "list_push") {
        warn_deprecated("list_push", "method call list.push(value)");
        Value hv = argV(0); std::string v = argS(1);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv)); g_lists[id].push_back(v);
        }
        return {};
    }
    if (nameStr == "list_get") {
        Value hv = argV(0); int idx = to_int(argS(1));
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv)); auto& vec = g_lists[id];
            if (idx >=0 && idx < (int)vec.size()) return vec[idx];
        }
        return {};
    }
    if (nameStr == "list_len") {
        Value hv = argV(0);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv));
            return std::to_string((int)g_lists[id].size());
        }
        return "0";
    }
    if (nameStr == "list_join") {
        Value hv = argV(0); std::string sep = argS(1);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv));
            std::ostringstream ss; const auto& v = g_lists[id];
            for (size_t i=0;i<v.size();++i) { if (i) ss << sep; ss << v[i]; }
            return ss.str();
        }
        return {};
    }
    if (nameStr == "list_clear") {
        Value hv = argV(0);
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv)); g_lists[id].clear();
        }
        return {};
    }
    if (nameStr == "list_remove_at") {
        Value hv = argV(0); int idx = (int)to_int(argS(1));
        if (value_is_handle(hv, HandleKind::List)) {
            int id = static_cast<int>(value_handle_id(hv));
            auto& v = g_lists[id];
            if (idx>=0 && idx<(int)v.size()) v.erase(v.begin()+idx);
        }
        return {};
    }
    if (nameStr == "dict_new") {
        int id = g_nextDictId++;
        g_dicts[id] = {};
        // Optional key/value varargs: dict_new("k1", v1, "k2", v2, ...)
        for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
            std::string key = eval_string(*args[i], env);
            std::string value = eval_string(*args[i + 1], env);
            g_dicts[id][key] = value;
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
    if (nameStr == "dict_set") {
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
    if (nameStr == "dict_get") {
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
    if (nameStr == "dict_has") {
        Value hv = argV(0);
        if (args.size() < 2) return "false";
        std::string k = join_builtin_path(1, args.size());
        if (value_is_handle(hv, HandleKind::Dict)) {
            int id = static_cast<int>(value_handle_id(hv));
            return (g_dicts[id].count(k)?"true":"false");
        }
        return "false";
    }
    if (nameStr == "dict_keys") {
        Value hv = argV(0);
        if (!value_is_handle(hv, HandleKind::Dict)) return {};
        int id = static_cast<int>(value_handle_id(hv));
        int lid = g_nextListId++; g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.first);
        std::sort(g_lists[lid].begin(), g_lists[lid].end());
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "dict_values") {
        std::string h = argS(0); if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5)); int lid = g_nextListId++; g_lists[lid] = {};
        std::vector<std::pair<std::string, std::string>> ordered;
        ordered.reserve(g_dicts[id].size());
        for (const auto& kv : g_dicts[id]) ordered.push_back(kv);
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
        for (const auto& kv : ordered) g_lists[lid].push_back(kv.second);
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "dict_get_or") {
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
    if (nameStr == "dict_remove") {
        std::string h = argS(0);
        if (args.size() < 2) return "false";
        std::string k = join_builtin_path(1, args.size());
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(k) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "dict_clear") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id].clear();
        }
        return {};
    }
    if (nameStr == "dict_size") {
        Value hv = argV(0);
        if (value_is_handle(hv, HandleKind::Dict)) {
            int id = static_cast<int>(value_handle_id(hv));
            return std::to_string((int)g_dicts[id].size());
        }
        return "0";
    }
    if (nameStr == "dict_merge") {
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
    if (nameStr == "dict_clone") {
        std::string source = argS(0);
        int newId = g_nextDictId++;
        g_dicts[newId] = {};
        if (source.rfind("dict:", 0) == 0) {
            int sourceId = to_int(source.substr(5));
            g_dicts[newId] = g_dicts[sourceId];
        }
        return std::string("dict:") + std::to_string(newId);
    }
    if (nameStr == "dict_items" || nameStr == "dict_entries") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) {
            g_lists[lid].push_back(kv.first + "=" + kv.second);
        }
        std::sort(g_lists[lid].begin(), g_lists[lid].end());
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "dict_set_path") {
        std::string h = argS(0); std::string path = argS(1); std::string v = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id][path] = v;
        }
        return {};
    }
    if (nameStr == "dict_get_path") {
        std::string h = argS(0); std::string path = argS(1);
        std::string def = (args.size() > 2) ? argS(2) : std::string();
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            auto it = g_dicts[id].find(path);
            if (it != g_dicts[id].end()) return it->second;
        }
        return def;
    }
    if (nameStr == "dict_has_path") {
        std::string h = argS(0); std::string path = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].count(path) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "dict_remove_path") {
        std::string h = argS(0); std::string path = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(path) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "table_new") {
        int id = g_nextDictId++;
        g_dicts[id] = {};
        return std::string("dict:") + std::to_string(id);
    }
    if (nameStr == "table_put") {
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
    if (nameStr == "table_get") {
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
    if (nameStr == "table_has") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].count(row + "." + col) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "table_remove") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(row + "." + col) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "table_rows") {
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
        for (const auto& row : rows) g_lists[lid].push_back(row);
        std::sort(g_lists[lid].begin(), g_lists[lid].end());
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "table_columns") {
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
        for (const auto& col : cols) g_lists[lid].push_back(col);
        std::sort(g_lists[lid].begin(), g_lists[lid].end());
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "table_row_keys") {
        std::string h = argS(0);
        std::string row = argS(1);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        std::string prefix = row + ".";
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) {
            if (kv.first.rfind(prefix, 0) == 0 && kv.first.size() > prefix.size()) {
                g_lists[lid].push_back(kv.first.substr(prefix.size()));
            }
        }
        std::sort(g_lists[lid].begin(), g_lists[lid].end());
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "table_clear_row") {
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
    if (nameStr == "table_count_row") {
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
    if (nameStr == "now_ms" || nameStr == "time.now_ms") {
        using namespace std::chrono;
        return std::to_string(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }
    if (nameStr == "env" || nameStr == "env.get") {
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
    if (nameStr == "dotenv_load" || nameStr == "env.load_dotenv") {
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
