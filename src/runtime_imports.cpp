#include "erelang/runtime_imports.hpp"

#include "erelang/modules.hpp"
#include "erelang/parser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace erelang {
namespace fs = std::filesystem;

namespace {

std::string lowercase_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string normalize_import_path(std::string path) {
    for (auto& ch : path) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    return lowercase_copy(std::move(path));
}

bool path_is(const std::string& normalized, std::initializer_list<const char*> options) {
    for (const char* opt : options) {
        if (normalized == opt) {
            return true;
        }
    }
    return false;
}

} // namespace

bool program_imports_module(const Program* program, std::string_view modulePath) {
    if (!program) {
        return false;
    }
    const std::string want = normalize_import_path(std::string(modulePath));
    for (const auto& importDecl : program->imports) {
        if (importDecl.path.empty()) {
            continue;
        }
        if (normalize_import_path(importDecl.path) == want) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> resolve_builtin_module_method(
    const Program& program,
    std::string_view alias,
    std::string_view method) {
    if (alias.empty() || method.empty()) {
        return std::nullopt;
    }

    const std::string methodName = lowercase_copy(std::string(method));
    for (const auto& importDecl : program.imports) {
        if (!importDecl.alias || *importDecl.alias != alias || importDecl.path.empty()) {
            continue;
        }

        const std::string normalizedPath = normalize_import_path(importDecl.path);

        if (path_is(normalizedPath, {"builtin/fs", "builtin/erefs"})) {
            if (methodName == "read") return std::string("read_text");
            if (methodName == "write") return std::string("write_text");
            if (methodName == "append") return std::string("append_text");
            if (methodName == "exists") return std::string("file_exists");
            if (methodName == "mkdir") return std::string("mkdirs");
            if (methodName == "copy") return std::string("copy_file");
            if (methodName == "move") return std::string("move_file");
            if (methodName == "remove") return std::string("delete_file");
            if (methodName == "list") return std::string("list_files");
            if (methodName == "cwd") return std::string("cwd");
            if (methodName == "chdir") return std::string("chdir");
            if (methodName == "join") return std::string("path_join");
            if (methodName == "parent" || methodName == "dirname") return std::string("path_dirname");
            if (methodName == "name" || methodName == "basename") return std::string("path_basename");
            if (methodName == "ext") return std::string("path_ext");
        }

        if (path_is(normalizedPath, {"builtin/path", "builtin/erepath"})) {
            if (methodName == "join") return std::string("path_join");
            if (methodName == "parent" || methodName == "dirname") return std::string("path_dirname");
            if (methodName == "name" || methodName == "basename") return std::string("path_basename");
            if (methodName == "ext") return std::string("path_ext");
            if (methodName == "exists") return std::string("file_exists");
        }

        if (path_is(normalizedPath, {"builtin/regex"})) {
            if (methodName == "match") return std::string("regex_match");
            if (methodName == "find") return std::string("regex_find");
            if (methodName == "replace") return std::string("regex_replace");
            if (methodName == "regex_match" || methodName == "regex_find" || methodName == "regex_replace") {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/crypto"})) {
            if (methodName == "hash" || methodName == "hash_fnv1a") return std::string("hash_fnv1a");
            if (methodName == "random_bytes") return std::string("random_bytes");
        }

        if (path_is(normalizedPath, {"builtin/network", "builtin/net"})) {
            if (methodName == "get") return std::string("http_get");
            if (methodName == "download") return std::string("http_download");
            if (methodName == "encode" || methodName == "url_encode") return std::string("url_encode");
            if (methodName.rfind("http_", 0) == 0 || methodName.rfind("network.", 0) == 0 ||
                methodName == "hls_download_best" || methodName == "url_encode") {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/binary"})) {
            if (methodName.rfind("bin_", 0) == 0) {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/threads"})) {
            if (methodName.rfind("thread_", 0) == 0) {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/monitor"})) {
            if (methodName.rfind("monitor_", 0) == 0) {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/math"})) {
            if (methodName.rfind("collatz_", 0) == 0 || methodName == "add" || methodName == "sub" ||
                methodName == "mul" || methodName == "div" || methodName == "mod" || methodName == "min" ||
                methodName == "max" || methodName == "abs" || methodName == "sin" || methodName == "cos" ||
                methodName == "tan" || methodName == "sqrt" || methodName == "pow") {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/data"})) {
            if (methodName.rfind("data_", 0) == 0) {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/perm"})) {
            if (methodName.rfind("perm_", 0) == 0) {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/system"})) {
            if (methodName.rfind("system.", 0) == 0) {
                return methodName;
            }
        }
    }

    return std::nullopt;
}

void bind_builtin_module_aliases(const Program& program, std::unordered_map<std::string, std::string>& vars) {
    auto bind_alias = [&](const std::string& alias, const char* method, const char* builtin) {
        vars[alias + "." + method] = std::string(kBuiltinAliasPrefix) + builtin;
    };
    auto bind_same = [&](const std::string& alias, std::initializer_list<const char*> names) {
        for (const char* name : names) {
            bind_alias(alias, name, name);
        }
    };

    for (const auto& importDecl : program.imports) {
        if (!importDecl.alias || importDecl.path.empty()) {
            continue;
        }

        const std::string normalizedPath = normalize_import_path(importDecl.path);
        const std::string& alias = *importDecl.alias;

        if (path_is(normalizedPath, {"builtin/fs", "builtin/erefs"})) {
            bind_alias(alias, "read", "read_text");
            bind_alias(alias, "write", "write_text");
            bind_alias(alias, "append", "append_text");
            bind_alias(alias, "exists", "file_exists");
            bind_alias(alias, "mkdir", "mkdirs");
            bind_alias(alias, "copy", "copy_file");
            bind_alias(alias, "move", "move_file");
            bind_alias(alias, "remove", "delete_file");
            bind_alias(alias, "list", "list_files");
            bind_alias(alias, "cwd", "cwd");
            bind_alias(alias, "chdir", "chdir");
            bind_alias(alias, "join", "path_join");
            bind_alias(alias, "parent", "path_dirname");
            bind_alias(alias, "dirname", "path_dirname");
            bind_alias(alias, "name", "path_basename");
            bind_alias(alias, "basename", "path_basename");
            bind_alias(alias, "ext", "path_ext");
        }

        if (path_is(normalizedPath, {"builtin/path", "builtin/erepath"})) {
            bind_alias(alias, "join", "path_join");
            bind_alias(alias, "parent", "path_dirname");
            bind_alias(alias, "dirname", "path_dirname");
            bind_alias(alias, "name", "path_basename");
            bind_alias(alias, "basename", "path_basename");
            bind_alias(alias, "ext", "path_ext");
            bind_alias(alias, "exists", "file_exists");
        }

        if (path_is(normalizedPath, {"builtin/regex"})) {
            bind_alias(alias, "match", "regex_match");
            bind_alias(alias, "find", "regex_find");
            bind_alias(alias, "replace", "regex_replace");
            bind_same(alias, {"regex_match", "regex_find", "regex_replace"});
        }

        if (path_is(normalizedPath, {"builtin/crypto"})) {
            bind_alias(alias, "hash", "hash_fnv1a");
            bind_same(alias, {"hash_fnv1a", "random_bytes"});
        }

        if (path_is(normalizedPath, {"builtin/network", "builtin/net"})) {
            bind_alias(alias, "get", "http_get");
            bind_alias(alias, "download", "http_download");
            bind_alias(alias, "encode", "url_encode");
            bind_same(alias, {
                "http_get", "http_download", "hls_download_best", "url_encode",
                "network.ip.flush", "network.ip.release", "network.ip.renew", "network.ip.registerdns",
                "network.debug.enable", "network.debug.disable", "network.debug.status",
                "network.debug.last", "network.debug.clear", "network.debug.log_tail",
            });
        }

        if (path_is(normalizedPath, {"builtin/binary"})) {
            bind_same(alias, {"bin_new", "bin_from_hex", "bin_len", "bin_hex", "bin_push_u8", "bin_get_u8"});
        }

        if (path_is(normalizedPath, {"builtin/threads"})) {
            bind_same(alias, {
                "thread_run", "thread_join", "thread_join_timeout", "thread_done", "thread_list",
                "thread_wait_all", "thread_count", "thread_yield", "thread_gc", "thread_gc_all",
                "thread_purge", "thread_remove", "thread_state",
            });
        }

        if (path_is(normalizedPath, {"builtin/monitor"})) {
            bind_same(alias, {
                "monitor_add", "monitor_remove", "monitor_list", "monitor_info",
                "monitor_last_change", "monitor_set_interval",
            });
        }

        if (path_is(normalizedPath, {"builtin/math"})) {
            bind_same(alias, {
                "add", "sub", "mul", "div", "mod", "min", "max", "abs",
                "sin", "cos", "tan", "sqrt", "pow",
                "collatz_len", "collatz_sweep", "collatz_best_n", "collatz_best_steps",
                "collatz_total_steps", "collatz_avg_steps",
            });
        }

        if (path_is(normalizedPath, {"builtin/data"})) {
            bind_same(alias, {
                "data_new", "data_set", "data_get", "data_has", "data_keys", "data_save", "data_load",
            });
        }

        if (path_is(normalizedPath, {"builtin/perm"})) {
            bind_same(alias, {"perm_grant", "perm_revoke", "perm_has", "perm_list"});
        }

        if (path_is(normalizedPath, {"builtin/system"})) {
            bind_same(alias, {
                "system.cmd", "system.execute", "system.output", "system.last_exit", "system.ip.flush",
            });
        }
    }
}

} // namespace erelang
