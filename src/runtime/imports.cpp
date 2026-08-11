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
            if (methodName == "load") return std::string("load_elan");
            if (methodName == "load_dir") return std::string("load_elan_dir");
            if (methodName == "cwd") return std::string("cwd");
            if (methodName == "chdir") return std::string("chdir");
            if (methodName == "join") return std::string("path_join");
            if (methodName == "parent" || methodName == "dirname") return std::string("path_dirname");
            if (methodName == "name" || methodName == "basename") return std::string("path_basename");
            if (methodName == "ext") return std::string("path_ext");
            if (methodName == "load_elan" || methodName == "load_elan_dir" || methodName == "call_action") {
                return methodName;
            }
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
            if (methodName == "find_all") return std::string("regex_find_all");
            if (methodName == "replace") return std::string("regex_replace");
            if (methodName == "split") return std::string("regex_split");
            if (methodName == "capture") return std::string("regex_capture");
            if (methodName == "group") return std::string("regex_group");
            if (methodName == "compile") return std::string("regex_compile");
            if (methodName == "free") return std::string("regex_free");
            if (methodName == "regex_match" || methodName == "regex_find" || methodName == "regex_replace") {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/performance", "builtin/perf"})) {
            if (methodName == "profile.begin") return std::string("perf.profile.begin");
            if (methodName == "profile.end") return std::string("perf.profile.end");
            if (methodName == "profile.duration") return std::string("perf.profile.duration");
            if (methodName == "profile.calls") return std::string("perf.profile.calls");
            if (methodName == "profile.report") return std::string("perf.profile.report");
            if (methodName == "mem.usage") return std::string("perf.mem.usage");
            if (methodName == "mem.peak") return std::string("perf.mem.peak");
            if (methodName == "gc.collect") return std::string("perf.gc.collect");
            if (methodName == "gc.threshold") return std::string("perf.gc.threshold");
            if (methodName == "gc.pause") return std::string("perf.gc.pause");
            if (methodName == "gc.resume") return std::string("perf.gc.resume");
            if (methodName.rfind("perf.", 0) == 0) return methodName;
        }

        if (path_is(normalizedPath, {"builtin/crypto"})) {
            if (methodName == "hash" || methodName == "hash_fnv1a") return std::string("hash_fnv1a");
            if (methodName == "random_bytes") return std::string("random_bytes");
        }

        if (path_is(normalizedPath, {"builtin/network", "builtin/net"})) {
            if (methodName == "get") return std::string("http_get");
            if (methodName == "get_auth") return std::string("http_get_auth");
            if (methodName == "post") return std::string("http_post");
            if (methodName == "post_auth") return std::string("http_post_auth");
            if (methodName == "put") return std::string("http_put_auth");
            if (methodName == "put_auth") return std::string("http_put_auth");
            if (methodName == "patch") return std::string("http_patch_auth");
            if (methodName == "patch_auth") return std::string("http_patch_auth");
            if (methodName == "delete") return std::string("http_delete_auth");
            if (methodName == "delete_auth") return std::string("http_delete_auth");
            if (methodName == "status") return std::string("http_status");
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
            // modular aliases
            if (methodName == "new") return std::string("bin_new");
            if (methodName == "from_hex") return std::string("bin_from_hex");
            if (methodName == "len") return std::string("bin_len");
            if (methodName == "to_hex") return std::string("bin_hex");
            if (methodName == "push_u8") return std::string("bin_push_u8");
            if (methodName == "get_u8") return std::string("bin_get_u8");
        }

        if (path_is(normalizedPath, {"builtin/threads"})) {
            if (methodName.rfind("thread_", 0) == 0) {
                return methodName;
            }
            // modular aliases
            if (methodName == "spawn") return std::string("thread_run");
            if (methodName == "spawn_detached") return std::string("thread_run");
            if (methodName == "sleep") return std::string("thread_sleep");
            if (methodName == "result") return std::string("thread_result");
            if (methodName == "join") return std::string("thread_join");
            if (methodName == "kill") return std::string("thread_purge");
            if (methodName == "active") return std::string("thread_count");
            if (methodName == "wait_all") return std::string("thread_wait_all");
            if (methodName == "done") return std::string("thread_done");
            if (methodName == "list") return std::string("thread_list");
        }

        if (path_is(normalizedPath, {"builtin/monitor"})) {
            if (methodName.rfind("monitor_", 0) == 0) {
                return methodName;
            }
            // modular aliases
            if (methodName == "add") return std::string("monitor_add");
            if (methodName == "remove") return std::string("monitor_remove");
            if (methodName == "list") return std::string("monitor_list");
            if (methodName == "info") return std::string("monitor_info");
            if (methodName == "last_change") return std::string("monitor_last_change");
            if (methodName == "set_interval") return std::string("monitor_set_interval");
        }

        if (path_is(normalizedPath, {"builtin/math"})) {
            if (methodName.rfind("collatz_", 0) == 0 || methodName == "add" || methodName == "sub" ||
                methodName == "mul" || methodName == "div" || methodName == "mod" || methodName == "min" ||
                methodName == "max" || methodName == "abs" || methodName == "sin" || methodName == "cos" ||
                methodName == "tan" || methodName == "sqrt" || methodName == "pow") {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/websocket", "builtin/ws"})) {
            if (methodName == "connect") return std::string("ws_connect");
            if (methodName == "send") return std::string("ws_send");
            if (methodName == "recv") return std::string("ws_recv");
            if (methodName == "recv_timeout") return std::string("ws_recv_timeout");
            if (methodName == "close") return std::string("ws_close");
            if (methodName.rfind("ws_", 0) == 0) return methodName;
        }

        if (path_is(normalizedPath, {"builtin/data"})) {
            if (methodName.rfind("data_", 0) == 0) {
                return methodName;
            }
            // modular aliases
            if (methodName == "new") return std::string("data_new");
            if (methodName == "set") return std::string("data_set");
            if (methodName == "get") return std::string("data_get");
            if (methodName == "has") return std::string("data_has");
            if (methodName == "keys") return std::string("data_keys");
            if (methodName == "save") return std::string("data_save");
            if (methodName == "load") return std::string("data_load");
        }

        if (path_is(normalizedPath, {"builtin/perm"})) {
            if (methodName.rfind("perm_", 0) == 0) {
                return methodName;
            }
        }

        if (path_is(normalizedPath, {"builtin/system"})) {
            if (methodName == "execute") return std::string("system.execute");
            if (methodName == "output")  return std::string("system.output");
            if (methodName == "last_exit") return std::string("system.last_exit");
            if (methodName == "cmd")     return std::string("system.cmd");
            if (methodName.rfind("system.", 0) == 0) return methodName;
        }
        if (path_is(normalizedPath, {"builtin/process", "builtin/proc"})) {
            if (methodName == "execute") return std::string("system.execute");
            if (methodName == "shell") return std::string("system.cmd");
            if (methodName == "spawn") return std::string("system.execute");
            if (methodName == "output") return std::string("system.output");
            if (methodName == "exit_code") return std::string("system.last_exit");
            if (methodName == "opts") return {};
            if (methodName == "kill" || methodName == "alive" || methodName == "wait") return {};
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
            bind_alias(alias, "load", "load_elan");
            bind_alias(alias, "load_dir", "load_elan_dir");
            bind_alias(alias, "cwd", "cwd");
            bind_alias(alias, "chdir", "chdir");
            bind_alias(alias, "join", "path_join");
            bind_alias(alias, "parent", "path_dirname");
            bind_alias(alias, "dirname", "path_dirname");
            bind_alias(alias, "name", "path_basename");
            bind_alias(alias, "basename", "path_basename");
            bind_alias(alias, "ext", "path_ext");
            bind_same(alias, {"load_elan", "load_elan_dir", "call_action", "list_files", "read_text", "write_text"});
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
            bind_alias(alias, "find_all", "regex_find_all");
            bind_alias(alias, "replace", "regex_replace");
            bind_alias(alias, "split", "regex_split");
            bind_alias(alias, "capture", "regex_capture");
            bind_alias(alias, "group", "regex_group");
            bind_alias(alias, "compile", "regex_compile");
            bind_alias(alias, "free", "regex_free");
            bind_alias(alias, "test", "regex_match");
            bind_same(alias, {"regex_match", "regex_find", "regex_replace"});
        }

        if (path_is(normalizedPath, {"builtin/performance", "builtin/perf"})) {
            bind_alias(alias, "profile.begin", "perf.profile.begin");
            bind_alias(alias, "profile.end", "perf.profile.end");
            bind_alias(alias, "profile.duration", "perf.profile.duration");
            bind_alias(alias, "profile.calls", "perf.profile.calls");
            bind_alias(alias, "profile.report", "perf.profile.report");
            bind_alias(alias, "mem.usage", "perf.mem.usage");
            bind_alias(alias, "mem.peak", "perf.mem.peak");
            bind_alias(alias, "gc.collect", "perf.gc.collect");
            bind_alias(alias, "gc.threshold", "perf.gc.threshold");
            bind_alias(alias, "gc.pause", "perf.gc.pause");
            bind_alias(alias, "gc.resume", "perf.gc.resume");
            bind_same(alias, {"perf.profile.begin", "perf.profile.end", "perf.profile.duration",
                              "perf.profile.calls", "perf.profile.report", "perf.mem.usage",
                              "perf.mem.peak", "perf.gc.collect", "perf.gc.threshold",
                              "perf.gc.pause", "perf.gc.resume"});
        }

        if (path_is(normalizedPath, {"builtin/crypto"})) {
            bind_alias(alias, "hash", "hash_fnv1a");
            bind_same(alias, {"hash_fnv1a", "random_bytes"});
        }

        if (path_is(normalizedPath, {"builtin/network", "builtin/net"})) {
            bind_alias(alias, "get", "http_get");
            bind_alias(alias, "get_auth", "http_get_auth");
            bind_alias(alias, "post", "http_post");
            bind_alias(alias, "post_auth", "http_post_auth");
            bind_alias(alias, "put", "http_put_auth");
            bind_alias(alias, "put_auth", "http_put_auth");
            bind_alias(alias, "patch", "http_patch_auth");
            bind_alias(alias, "patch_auth", "http_patch_auth");
            bind_alias(alias, "delete", "http_delete_auth");
            bind_alias(alias, "delete_auth", "http_delete_auth");
            bind_alias(alias, "status", "http_status");
            bind_alias(alias, "download", "http_download");
            bind_alias(alias, "encode", "url_encode");
            bind_same(alias, {
                "http_get", "http_get_auth", "http_post", "http_post_auth",
                "http_put_auth", "http_patch_auth", "http_delete_auth",
                "http_status", "http_download",
                "hls_download_best", "url_encode",
                "network.ip.flush", "network.ip.release", "network.ip.renew", "network.ip.registerdns",
            });
        }

        if (path_is(normalizedPath, {"builtin/binary"})) {
            bind_alias(alias, "new", "bin_new");
            bind_alias(alias, "from_hex", "bin_from_hex");
            bind_alias(alias, "len", "bin_len");
            bind_alias(alias, "to_hex", "bin_hex");
            bind_alias(alias, "push_u8", "bin_push_u8");
            bind_alias(alias, "get_u8", "bin_get_u8");
            // keep old flat names working
            bind_same(alias, {"bin_new", "bin_from_hex", "bin_len", "bin_hex", "bin_push_u8", "bin_get_u8"});
        }

        if (path_is(normalizedPath, {"builtin/threads"})) {
            bind_alias(alias, "spawn", "thread_run");
            bind_alias(alias, "spawn_detached", "thread_run");
            bind_alias(alias, "sleep", "thread_sleep");
            bind_alias(alias, "result", "thread_result");
            bind_alias(alias, "join", "thread_join");
            bind_alias(alias, "join_timeout", "thread_join_timeout");
            bind_alias(alias, "kill", "thread_purge");
            bind_alias(alias, "active", "thread_count");
            bind_alias(alias, "wait_all", "thread_wait_all");
            bind_alias(alias, "done", "thread_done");
            bind_alias(alias, "list", "thread_list");
            bind_alias(alias, "yield", "thread_yield");
            bind_alias(alias, "gc", "thread_gc");
            // pool control
            bind_alias(alias, "pool.max", "thread_pool_max");
            bind_alias(alias, "pool.stop", "thread_pool_stop");
            // keep old flat names working
            bind_same(alias, {
                "thread_run", "thread_join", "thread_join_timeout", "thread_done", "thread_list",
                "thread_wait_all", "thread_count", "thread_yield", "thread_gc", "thread_gc_all",
                "thread_purge", "thread_remove", "thread_state",
                "thread_sleep", "thread_result",
            });
        }

        if (path_is(normalizedPath, {"builtin/monitor"})) {
            bind_alias(alias, "add", "monitor_add");
            bind_alias(alias, "remove", "monitor_remove");
            bind_alias(alias, "list", "monitor_list");
            bind_alias(alias, "info", "monitor_info");
            bind_alias(alias, "last_change", "monitor_last_change");
            bind_alias(alias, "set_interval", "monitor_set_interval");
            // keep old flat names working
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
            bind_alias(alias, "new", "data_new");
            bind_alias(alias, "set", "data_set");
            bind_alias(alias, "get", "data_get");
            bind_alias(alias, "has", "data_has");
            bind_alias(alias, "keys", "data_keys");
            bind_alias(alias, "save", "data_save");
            bind_alias(alias, "load", "data_load");
            // keep old flat names working
            bind_same(alias, {"data_new", "data_set", "data_get", "data_has", "data_keys", "data_save", "data_load"});
        }

        if (path_is(normalizedPath, {"builtin/perm"})) {
            bind_same(alias, {"perm_grant", "perm_revoke", "perm_has", "perm_list"});
        }

        if (path_is(normalizedPath, {"builtin/websocket", "builtin/ws"})) {
            bind_alias(alias, "connect", "ws_connect");
            bind_alias(alias, "send", "ws_send");
            bind_alias(alias, "recv", "ws_recv");
            bind_alias(alias, "recv_timeout", "ws_recv_timeout");
            bind_alias(alias, "close", "ws_close");
            bind_same(alias, {"ws_connect", "ws_send", "ws_recv", "ws_recv_timeout", "ws_close"});
        }

        if (path_is(normalizedPath, {"builtin/system"})) {
            bind_alias(alias, "execute",   "system.execute");
            bind_alias(alias, "output",    "system.output");
            bind_alias(alias, "last_exit", "system.last_exit");
            bind_alias(alias, "cmd",       "system.cmd");
        }
        if (path_is(normalizedPath, {"builtin/process", "builtin/proc"})) {
            bind_alias(alias, "execute",   "system.execute");
            bind_alias(alias, "shell",     "system.cmd");
            bind_alias(alias, "spawn",     "system.execute");
            bind_alias(alias, "output",    "system.output");
            bind_alias(alias, "exit_code", "system.last_exit");
            bind_alias(alias, "opts",      "sys_opts");
            bind_alias(alias, "kill",      "sys_kill");
            bind_alias(alias, "wait",      "sys_wait");
            bind_alias(alias, "alive",     "sys_alive");
        }
    }
}

} // namespace erelang
