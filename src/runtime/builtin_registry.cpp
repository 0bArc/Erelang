// The builtin registry: every module, its script names, arity and return types, in one table.
// See include/erelang/builtin_registry.hpp. Keep entries sorted by how a reader meets them.
#include "erelang/builtin_registry.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace erelang::registry {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

using M = Member;

std::vector<Module> build_modules() {
    std::vector<Module> mods;

    mods.push_back({"fs", {"builtin/fs", "builtin/erefs", "std/fs"}, "fs", "Files and directories", {
        M{"read", "read_text", 1, 1, "string", "Whole file as text"},
        M{"read_text", "read_text", 1, 1, "string", "Whole file as text", true},
        M{"write", "write_text", 2, 2, "void", "Replace a file with text"},
        M{"write_text", "write_text", 2, 2, "void", "Replace a file with text", true},
        M{"append", "append_text", 2, 2, "void", "Append text to a file"},
        M{"exists", "file_exists", 1, 1, "bool", "File or directory exists"},
        M{"file_exists", "file_exists", 1, 1, "bool", "File or directory exists", true},
        M{"is_dir", "is_dir", 1, 1, "bool", "Path is a directory"},
        M{"is_file", "is_file", 1, 1, "bool", "Path is a regular file"},
        M{"mkdir", "mkdirs", 1, 1, "void", "Create a directory and its parents"},
        M{"copy", "copy_file", 2, 2, "bool", "Copy a file"},
        M{"move", "move_file", 2, 2, "bool", "Move or rename a file"},
        M{"remove", "delete_file", 1, 1, "bool", "Delete a file"},
        M{"list", "list_files", 1, 1, "array<string>", "Entries in a directory"},
        M{"dirs", "list_dirs", 1, 1, "array<string>", "Subdirectories of a directory"},
        M{"files", "list_regular_files", 1, 1, "array<string>", "Regular files in a directory"},
        M{"size", "file_size", 1, 1, "int", "File size in bytes"},
        M{"mtime", "file_mtime", 1, 1, "int", "Modification time, Unix seconds"},
        M{"load", "load_elan", 1, 1, "string", "Load an .elan file into the running program"},
        M{"load_dir", "load_elan_dir", 1, 1, "any", "Load every .elan file in a directory"},
        M{"load_elan", "load_elan", 1, 1, "string", "Load an .elan file", true},
        M{"load_elan_dir", "load_elan_dir", 1, 1, "any", "Load a directory of .elan files", true},
        M{"call_action", "call_action", 1, -1, "any", "Call an action by name", true},
        M{"cwd", "cwd", 0, 0, "string", "Current working directory"},
        M{"chdir", "chdir", 1, 1, "bool", "Change the working directory"},
        M{"join", "path_join", 1, -1, "string", "Join path parts"},
        M{"parent", "path_dirname", 1, 1, "string", "Parent directory of a path"},
        M{"dirname", "path_dirname", 1, 1, "string", "Parent directory of a path", true},
        M{"name", "path_basename", 1, 1, "string", "Last component of a path"},
        M{"basename", "path_basename", 1, 1, "string", "Last component of a path", true},
        M{"ext", "path_ext", 1, 1, "string", "Extension including the dot"},
        M{"open", "file_open", 1, 2, "string", "Open a file handle (mode r, w, a, rw)"},
        M{"close", "file_close", 1, 1, "bool", "Close a file handle"},
        M{"read_bytes", "file_read", 1, 2, "string", "Read from a file handle"},
        M{"read_handle", "file_read", 1, 2, "string", "Read from a file handle", true},
        M{"write_bytes", "file_write", 2, 2, "string", "Write to a file handle"},
        M{"write_handle", "file_write", 2, 2, "string", "Write to a file handle", true},
        M{"seek", "file_seek", 2, 3, "bool", "Move a file handle position"},
        M{"tell", "file_tell", 1, 1, "int", "Position of a file handle"},
        M{"flush", "file_flush", 1, 1, "bool", "Flush a file handle"},
    }});

    mods.push_back({"path", {"builtin/path", "builtin/erepath", "std/path"}, "path", "Path manipulation", {
        M{"join", "path_join", 1, -1, "string", "Join path parts"},
        M{"parent", "path_dirname", 1, 1, "string", "Parent directory of a path"},
        M{"dirname", "path_dirname", 1, 1, "string", "Parent directory of a path", true},
        M{"name", "path_basename", 1, 1, "string", "Last component of a path"},
        M{"basename", "path_basename", 1, 1, "string", "Last component of a path", true},
        M{"ext", "path_ext", 1, 1, "string", "Extension including the dot"},
        M{"exists", "file_exists", 1, 1, "bool", "Path exists"},
        M{"file_exists", "file_exists", 1, 1, "bool", "Path exists", true},
    }});

    mods.push_back({"log", {"builtin/log", "std/log"}, "log", "Leveled logging", {
        M{"set_level", "log_set_level", 1, 1, "bool", "Minimum level: debug, info, warn, error"},
        M{"to_file", "log_to_file", 1, 1, "bool", "Also write log lines to a file"},
        M{"write", "log_write", 2, 2, "void", "Log at a named level"},
        M{"debug", "log_debug", 1, 1, "void", "Log at debug level"},
        M{"info", "log_info", 1, 1, "void", "Log at info level"},
        M{"warn", "log_warn", 1, 1, "void", "Log at warn level"},
        M{"error", "log_error", 1, 1, "void", "Log at error level"},
    }, {"log_"}});

    mods.push_back({"ai", {"builtin/ai"}, "ai", "Infer, generate, explain and optimize patterns offline", {
        M{"auto", "ai_auto", 1, 1, "string", "Regex for samples; known formats give their canonical regex"},
        M{"infer.regex", "ai_infer_regex", 1, 1, "string", "Regex inferred from samples"},
        M{"infer.pattern", "ai_infer_pattern", 1, 1, "string", "Expect pattern inferred from samples"},
        M{"pattern", "ai_pattern", 1, 1, "string", "Expect pattern for one sample"},
        M{"kind", "ai_kind", 1, 1, "string", "What the text looks like (email, date, int, ...)"},
        M{"kind_all", "ai_kind_all", 1, 1, "string", "Most common kind among samples"},
        M{"names", "ai_names", 0, 0, "array<string>", "Known kinds"},
        M{"describe", "ai_describe", 1, 1, "string", "Description of a kind"},
        M{"example", "ai_example", 1, 1, "string", "Example of a kind"},
        M{"regex.generate", "ai_regex_generate", 1, 1, "string", "Regex from an English description"},
        M{"regex.can_generate", "ai_regex_can_generate", 1, 1, "bool", "The description is understood"},
        M{"regex.explain", "ai_regex_explain", 1, 1, "string", "Plain English description of a regex"},
        M{"regex.optimize", "ai_regex_optimize", 1, 1, "string", "Shorter equivalent regex"},
        M{"regex.test", "ai_regex_test", 2, 2, "bool", "Whole text matches the regex"},
        M{"regex.valid", "ai_regex_valid", 1, 1, "bool", "The regex compiles"},
        M{"regex.examples", "ai_regex_examples", 1, 2, "array<string>", "Strings the regex accepts"},
        M{"regex.of", "ai_regex_of", 1, 1, "string", "Canonical regex of a kind"},
        M{"extract", "ai_extract", 2, 2, "array<string>", "Every match of a kind inside text"},
        M{"count", "ai_count", 2, 2, "int", "Number of matches of a kind"},
        M{"validate", "ai_validate", 2, 2, "bool", "Whole text is of that kind"},
        M{"redact", "ai_redact", 2, 2, "string", "Replace matches of a kind with [KIND]"},
        M{"redact_all", "ai_redact_all", 1, 1, "string", "Redact emails, urls, ids, IPs, cards, phones"},
    }});

    mods.push_back({"regex", {"builtin/regex"}, "re", "Regular expressions (ECMAScript) and expect patterns", {
        M{"match", "regex_match", 2, 2, "bool", "Whole text matches the pattern"},
        M{"test", "regex_match", 2, 2, "bool", "Whole text matches the pattern"},
        M{"expect", "regex_expect", 2, 2, "bool", "Whole text matches an expect pattern (x digit, X letter)"},
        M{"explain", "regex_explain", 1, 1, "string", "Regex an expect pattern expands to"},
        M{"infer", "regex_infer", 1, 1, "string", "Compact regex for text shaped like the sample"},
        M{"find", "regex_find", 2, 2, "string", "First match (or first group) anywhere in text"},
        M{"find_all", "regex_find_all", 2, 2, "array<string>", "Every match"},
        M{"replace", "regex_replace", 3, 3, "string", "Replace every match"},
        M{"split", "regex_split", 2, 2, "array<string>", "Split text on the pattern"},
        M{"capture", "regex_capture", 2, 2, "bool", "Search and remember capture groups"},
        M{"group", "regex_group", 1, 1, "string", "Capture group from the last capture"},
        M{"compile", "regex_compile", 1, 1, "string", "Compile a pattern, returns an id"},
        M{"free", "regex_free", 1, 1, "void", "Free a compiled pattern"},
        M{"regex_match", "regex_match", 2, 2, "bool", "Whole text matches", true},
        M{"regex_find", "regex_find", 2, 2, "string", "First match", true},
        M{"regex_replace", "regex_replace", 3, 3, "string", "Replace every match", true},
    }});

    mods.push_back({"perf", {"builtin/performance", "builtin/perf"}, "perf", "Profiling and memory statistics", {
        M{"profile.begin", "perf.profile.begin", 1, 1, "void", "Start timing a named section"},
        M{"profile.end", "perf.profile.end", 1, 1, "void", "Stop timing a named section"},
        M{"profile.duration", "perf.profile.duration", 1, 1, "string", "Total milliseconds of a section"},
        M{"profile.calls", "perf.profile.calls", 1, 1, "string", "How often a section ran"},
        M{"profile.report", "perf.profile.report", 0, 0, "string", "Report of every section"},
        M{"mem.usage", "perf.mem.usage", 0, 0, "string", "Current process memory in bytes"},
        M{"mem.peak", "perf.mem.peak", 0, 0, "string", "Peak process memory in bytes"},
        M{"gc.collect", "perf.gc.collect", 0, 0, "void", "Run a collection"},
        M{"gc.threshold", "perf.gc.threshold", 1, 1, "void", "Set the collection threshold"},
        M{"gc.pause", "perf.gc.pause", 0, 0, "void", "Pause automatic collection"},
        M{"gc.resume", "perf.gc.resume", 0, 0, "void", "Resume automatic collection"},
    }, {"perf."}});

    mods.push_back({"crypto", {"builtin/crypto", "std/crypto"}, "crypto", "Hashes, random bytes, AES-256-GCM", {
        M{"hash", "hash_fnv1a", 1, 1, "string", "FNV-1a hash (fast, not cryptographic)"},
        M{"hash_fnv1a", "hash_fnv1a", 1, 1, "string", "FNV-1a hash", true},
        M{"sha256", "hash_sha256", 1, 1, "string", "SHA-256 as hex"},
        M{"hash_sha256", "hash_sha256", 1, 1, "string", "SHA-256 as hex", true},
        M{"random_bytes", "random_bytes", 1, 1, "string", "Cryptographically random bytes as hex"},
        M{"aes_encrypt", "aes_encrypt", 2, 2, "string", "AES-256-GCM encrypt (key, plaintext)"},
        M{"aes_decrypt", "aes_decrypt", 2, 2, "string", "AES-256-GCM decrypt (key, ciphertext)"},
    }});

    mods.push_back({"pipe", {"std/pipe", "builtin/pipe"}, "pipe", "Channels between tasks", {
        M{"new", "chan_new", 0, 1, "string", "New channel, optional capacity"},
        M{"send", "chan_send", 2, 2, "void", "Send, blocks when full"},
        M{"receive", "chan_recv", 1, 1, "string", "Receive, blocks when empty"},
        M{"recv", "chan_recv", 1, 1, "string", "Receive, blocks when empty", true},
        M{"try_send", "chan_try_send", 2, 2, "bool", "Send without blocking"},
        M{"try_receive", "chan_try_recv", 1, 1, "string", "Receive without blocking"},
        M{"try_recv", "chan_try_recv", 1, 1, "string", "Receive without blocking", true},
        M{"close", "chan_close", 1, 1, "void", "Close the channel"},
        M{"len", "chan_len", 1, 1, "int", "Items waiting"},
        M{"size", "chan_len", 1, 1, "int", "Items waiting", true},
    }});

    mods.push_back({"net", {"builtin/network", "builtin/net", "std/net"}, "net", "HTTP client and server, UDP", {
        M{"get", "http_get", 1, 1, "string", "GET, returns the body"},
        M{"get_auth", "http_get_auth", 2, 2, "string", "GET with an Authorization header line"},
        M{"post", "http_post", 3, 3, "string", "POST (url, body, content type)"},
        M{"post_auth", "http_post_auth", 4, 4, "string", "POST with an Authorization header line"},
        M{"put", "http_put", 3, 3, "string", "PUT (url, body, content type)"},
        M{"put_auth", "http_put_auth", 4, 4, "string", "PUT with an Authorization header line"},
        M{"patch", "http_patch", 3, 4, "string", "PATCH (url, body, content type)"},
        M{"patch_auth", "http_patch_auth", 4, 4, "string", "PATCH with an Authorization header line"},
        M{"delete", "http_delete", 1, 1, "string", "DELETE"},
        M{"delete_auth", "http_delete_auth", 2, 2, "string", "DELETE with an Authorization header line"},
        M{"head", "http_head", 1, 1, "string", "HEAD, returns headers"},
        M{"status", "http_status", 1, 1, "string", "Status code of a GET"},
        M{"download", "http_download", 2, 2, "bool", "Download a URL to a file"},
        M{"encode", "url_encode", 1, 1, "string", "Percent-encode text"},
        M{"url_encode", "url_encode", 1, 1, "string", "Percent-encode text", true},
        M{"json_encode", "json_encode", 1, 1, "string", "Quote text as a JSON string"},
        M{"json_decode", "json_decode", 1, 1, "string", "Unquote a JSON string"},
        M{"get_resp", "http_get_resp", 1, 1, "string", "GET, returns a response handle"},
        M{"create_server", "http_create_server", 1, 3, "string", "HTTP server on a port"},
        M{"create_server_tls", "http_create_server_tls", 3, 3, "string", "HTTPS server (port, cert, key)"},
        M{"udp_bind", "udp_bind", 2, 2, "string", "UDP socket bound to host and port"},
        M{"udp", "udp_bind", 2, 2, "string", "UDP socket bound to host and port", true},
        M{"hls_download_best", "hls_download_best", 2, 2, "bool", "Download the best HLS variant", true},
        M{"ip.flush", "network.ip.flush", 0, 0, "string", "Flush the DNS cache", true},
    }, {"http_", "network."}});

    mods.push_back({"binary", {"builtin/binary"}, "bin", "Byte buffers", {
        M{"new", "bin_new", 0, 0, "string", "Empty byte buffer"},
        M{"from_hex", "bin_from_hex", 1, 1, "string", "Buffer from hex text"},
        M{"to_hex", "bin_hex", 1, 1, "string", "Buffer as hex text"},
        M{"len", "bin_len", 1, 1, "int", "Length in bytes"},
        M{"push_u8", "bin_push_u8", 2, 2, "void", "Append one byte"},
        M{"get_u8", "bin_get_u8", 2, 2, "int", "Byte at an index"},
    }, {"bin_"}});

    mods.push_back({"threads", {"builtin/threads"}, "threads", "OS threads (experimental)", {
        M{"spawn", "thread_run", 1, 2, "string", "Run an action on a new thread"},
        M{"spawn_detached", "thread_run", 1, 2, "string", "Run an action without joining"},
        M{"sleep", "thread_sleep", 1, 1, "void", "Sleep the current thread (ms)"},
        M{"result", "thread_result", 1, 1, "any", "Return value of a finished thread"},
        M{"join", "thread_join", 1, 1, "bool", "Wait for a thread"},
        M{"join_timeout", "thread_join_timeout", 2, 2, "bool", "Wait with a timeout"},
        M{"kill", "thread_purge", 0, 0, "void", "Stop finished threads"},
        M{"active", "thread_count", 0, 0, "string", "Number of live threads"},
        M{"wait_all", "thread_wait_all", 0, 0, "void", "Wait for every thread"},
        M{"done", "thread_done", 1, 1, "bool", "Thread finished"},
        M{"list", "thread_list", 0, 0, "string", "Thread ids"},
        M{"yield", "thread_yield", 0, 0, "void", "Yield the current thread"},
        M{"gc", "thread_gc", 0, 0, "void", "Collect finished threads"},
        M{"gc_all", "thread_gc_all", 0, 0, "void", "Collect every finished thread"},
        M{"remove", "thread_remove", 1, 2, "string", "Forget a thread"},
        M{"state", "thread_state", 1, 1, "string", "State of a thread"},
    }, {"thread_"}, true});

    mods.push_back({"monitor", {"builtin/monitor"}, "monitor", "File change monitoring (experimental)", {
        M{"add", "monitor_add", 1, 2, "string", "Watch a path"},
        M{"remove", "monitor_remove", 1, 1, "void", "Stop watching"},
        M{"list", "monitor_list", 0, 0, "string", "Watched paths"},
        M{"info", "monitor_info", 1, 1, "string", "State of a watch"},
        M{"last_change", "monitor_last_change", 1, 1, "string", "Last change of a watch"},
        M{"set_interval", "monitor_set_interval", 2, 2, "void", "Polling interval (ms)"},
    }, {"monitor_"}, true});

    mods.push_back({"math", {"builtin/math"}, "math", "Numeric functions", {
        M{"add", "add", 2, 2, "int", "a + b, error on overflow"},
        M{"sub", "sub", 2, 2, "int", "a - b, error on overflow"},
        M{"mul", "mul", 2, 2, "int", "a * b, error on overflow"},
        M{"div", "div", 2, 2, "int", "Integer division, error on zero"},
        M{"mod", "mod", 2, 2, "int", "Remainder, error on zero"},
        M{"min", "min", 2, 2, "any", "Smaller of two numbers"},
        M{"max", "max", 2, 2, "any", "Larger of two numbers"},
        M{"abs", "abs", 1, 1, "any", "Absolute value"},
        M{"clamp", "clamp", 3, 3, "any", "Value limited to [lo, hi]"},
        M{"sign", "sign", 1, 1, "int", "-1, 0 or 1"},
        M{"sin", "sin", 1, 1, "double", "Sine (radians)"},
        M{"cos", "cos", 1, 1, "double", "Cosine (radians)"},
        M{"tan", "tan", 1, 1, "double", "Tangent (radians)"},
        M{"asin", "asin", 1, 1, "double", "Arc sine"},
        M{"acos", "acos", 1, 1, "double", "Arc cosine"},
        M{"atan", "atan", 1, 1, "double", "Arc tangent"},
        M{"atan2", "atan2", 2, 2, "double", "Angle of (y, x)"},
        M{"sqrt", "sqrt", 1, 1, "double", "Square root"},
        M{"cbrt", "cbrt", 1, 1, "double", "Cube root"},
        M{"pow", "pow", 2, 2, "double", "a to the power b"},
        M{"exp", "exp", 1, 1, "double", "e to the power x"},
        M{"log", "log", 1, 1, "double", "Natural logarithm"},
        M{"log2", "log2", 1, 1, "double", "Base 2 logarithm"},
        M{"log10", "log10", 1, 1, "double", "Base 10 logarithm"},
        M{"floor", "floor", 1, 1, "int", "Largest integer not above x"},
        M{"ceil", "ceil", 1, 1, "int", "Smallest integer not below x"},
        M{"round", "round", 1, 1, "int", "Nearest integer, halves away from zero"},
        M{"trunc", "trunc", 1, 1, "int", "Integer part"},
        M{"hypot", "hypot", 2, 2, "double", "Length of (x, y)"},
        M{"lerp", "lerp", 3, 3, "double", "a + (b - a) * t"},
        M{"gcd", "gcd", 2, 2, "int", "Greatest common divisor"},
        M{"lcm", "lcm", 2, 2, "int", "Least common multiple"},
        M{"is_prime", "is_prime", 1, 1, "bool", "Primality test"},
        M{"factorial", "factorial", 1, 1, "int", "n!, error above 20"},
        M{"pi", "pi", 0, 0, "double", "3.14159..."},
        M{"e", "e", 0, 0, "double", "2.71828..."},
        M{"radians", "radians", 1, 1, "double", "Degrees to radians"},
        M{"degrees", "degrees", 1, 1, "double", "Radians to degrees"},
        M{"collatz_len", "collatz_len", 1, 1, "int", "Steps of the Collatz sequence from n"},
        M{"collatz_sweep", "collatz_sweep", 1, 1, "int", "Run collatz_len for 1..limit"},
        M{"collatz_best_n", "collatz_best_n", 0, 0, "int", "Longest start from the last sweep"},
        M{"collatz_best_steps", "collatz_best_steps", 0, 0, "int", "Its length"},
        M{"collatz_total_steps", "collatz_total_steps", 0, 0, "int", "Sum of steps of the last sweep"},
        M{"collatz_avg_steps", "collatz_avg_steps", 0, 0, "int", "Average steps of the last sweep"},
    }, {"collatz_"}});

    mods.push_back({"ws", {"builtin/websocket", "builtin/ws"}, "ws", "WebSocket client and server", {
        M{"connect", "ws_connect", 1, 1, "string", "Connect, returns a ws handle"},
        M{"send", "ws_send", 2, 2, "bool", "Send text on a handle id"},
        M{"recv", "ws_recv", 1, 1, "string", "Receive (blocking)"},
        M{"recv_timeout", "ws_recv_timeout", 2, 2, "string", "Receive, \"\" after the timeout"},
        M{"close", "ws_close", 1, 1, "void", "Close the connection"},
        M{"state", "ws_state", 1, 1, "string", "open, closing or closed"},
        M{"broadcast", "ws_broadcast", 1, 2, "bool", "Send to every client of a server"},
        M{"send_binary", "ws_send_binary", 2, 2, "bool", "Send a binary frame"},
    }, {"ws_"}});

    mods.push_back({"tcp", {"builtin/tcp", "builtin/rawtcp"}, "tcp", "Raw TCP client", {
        M{"connect", "tcp_connect", 2, 2, "string", "Connect to host and port, returns a tcp handle"},
    }, {"tcp_"}});

    mods.push_back({"data", {"builtin/data"}, "data", "Key value stores saved to disk", {
        M{"new", "data_new", 0, 0, "string", "New store"},
        M{"set", "data_set", 3, 3, "void", "Set a key"},
        M{"get", "data_get", 2, 2, "string", "Get a key"},
        M{"has", "data_has", 2, 2, "bool", "Key exists"},
        M{"keys", "data_keys", 1, 1, "string", "Keys, comma separated"},
        M{"save", "data_save", 2, 2, "void", "Write a store to a file"},
        M{"load", "data_load", 1, 1, "string", "Read a store from a file"},
    }, {"data_"}});

    mods.push_back({"perm", {"builtin/perm"}, "perm", "Runtime permissions", {
        M{"grant", "perm_grant", 1, 1, "void", "Grant a permission"},
        M{"revoke", "perm_revoke", 1, 1, "void", "Revoke a permission"},
        M{"has", "perm_has", 1, 1, "bool", "Permission granted"},
        M{"list", "perm_list", 0, 0, "string", "Granted permissions"},
    }, {"perm_"}});

    const std::vector<Member> systemMembers = {
        M{"execute", "system.execute", 1, -1, "int", "Run a program with arguments (no shell), capture output"},
        M{"execute_in", "system.execute_in", 2, -1, "int", "Same as execute, in a working directory"},
        M{"cmd", "system.cmd", 1, 2, "string", "Run a command line through the shell, return its output"},
        M{"shell", "system.cmd", 1, 2, "string", "Run a command line through the shell, return its output"},
        M{"output", "system.output", 0, 0, "string", "Output of the last execute or cmd"},
        M{"last_exit", "system.last_exit", 0, 0, "int", "Exit code of the last execute or cmd"},
        M{"exit_code", "system.last_exit", 0, 0, "int", "Exit code of the last execute or cmd", true},
        M{"start", "proc_start", 1, 2, "string", "Start a process in the background, returns a proc handle"},
        M{"kill", "proc_kill", 1, 1, "bool", "Kill a proc handle"},
        M{"wait", "proc_wait", 1, 2, "string", "Wait for a proc handle, \"\" on timeout"},
        M{"alive", "proc_alive", 1, 1, "bool", "Process still running"},
        M{"os", "system.os", 0, 0, "string", "Operating system: windows, linux or macos"},
        M{"ip.flush", "system.ip.flush", 0, 0, "string", "Flush the DNS cache"},
    };
    mods.push_back({"system", {"builtin/system", "std/system"}, "sys", "Run programs and commands", systemMembers, {"system."}});
    std::vector<Member> procMembers = systemMembers;
    procMembers.push_back(M{"spawn", "proc_start", 1, 2, "string", "Start a process in the background", true});
    mods.push_back({"process", {"builtin/process", "builtin/proc", "std/process"}, "proc",
                    "Run programs and control background processes", procMembers, {"system.", "proc_"}});

    mods.push_back({"json", {"builtin/json", "std/json"}, "json", "JSON parse and stringify", {
        M{"parse", "json_parse", 1, 1, "any", "Parse JSON into maps, arrays and values"},
        M{"stringify", "json_stringify", 1, 2, "string", "Value as JSON, optional indent"},
        M{"encode", "json_encode", 1, 1, "string", "Quote text as a JSON string"},
        M{"decode", "json_decode", 1, 1, "string", "Unquote a JSON string"},
        M{"valid", "json_valid", 1, 1, "bool", "Text is valid JSON"},
    }, {"json_"}});

    return mods;
}

std::vector<Member> build_core() {
    return {
        M{"print", "print", 1, -1, "void", "Write a line to stdout"},
        M{"now_ms", "now_ms", 0, 0, "int", "Milliseconds since the Unix epoch"},
        M{"now_iso", "now_iso", 0, 0, "string", "Current time as ISO 8601"},
        M{"env", "env", 1, 1, "string", "Environment variable, \"\" when unset"},
        M{"dotenv_load", "dotenv_load", 0, 1, "int", "Load a .env file into the environment"},
        M{"rand_int", "rand_int", 0, 2, "int", "Random integer, optional range"},
        M{"uuid", "uuid", 0, 0, "string", "Random UUID"},
        M{"args_count", "args_count", 0, 0, "int", "Number of script arguments"},
        M{"args_get", "args_get", 1, 1, "string", "Script argument by index"},
        M{"read_line", "read_line", 0, 0, "string", "Line from stdin"},
        M{"input", "input", 0, 1, "string", "Prompt and read a line"},
        M{"stderr_print", "stderr_print", 1, 1, "void", "Write a line to stderr"},
        M{"exec", "exec", 1, 1, "int", "Run a shell command, return its exit code"},
        M{"spawn", "spawn", 1, 1, "int", "Start a shell command in the background, returns its process id"},
        M{"exit", "exit", 1, 1, "void", "End the program with an exit code"},
        M{"fail", "fail", 1, 1, "never", "Stop with an error message"},
        M{"int", "int", 1, 1, "int", "Convert to int"},
        M{"float", "float", 1, 1, "double", "Convert to float"},
        M{"string", "string", 1, 1, "string", "Convert to string"},
        M{"bool", "bool", 1, 1, "bool", "Convert to bool"},
        M{"to_json", "to_json", 1, 1, "string", "Value as JSON"},
        M{"from_json", "from_json", 1, 1, "any", "JSON object as a map or array as a list; nested values stay JSON text"},
        M{"json.encode", "json.encode", 1, 1, "string", "Value as JSON", true},
        M{"json.decode", "json.decode", 1, 1, "string", "JSON object as a map handle", true},
        M{"url_encode", "url_encode", 1, 1, "string", "Percent-encode text"},
        M{"json_encode", "json_encode", 1, 1, "string", "Quote text as a JSON string", true},
        M{"json_decode", "json_decode", 1, 1, "string", "Unquote a JSON string", true},
        M{"http_get_resp", "http_get_resp", 1, 1, "string", "GET, response handle", true},
        M{"tcp_connect", "tcp_connect", 2, 2, "string", "Raw TCP connect", true},
        M{"tcp.connect", "tcp_connect", 2, 2, "string", "Raw TCP connect", true},
        M{"load_elan", "load_elan", 1, 1, "string", "Load an .elan file into the running program"},
        M{"load_elan_dir", "load_elan_dir", 1, 1, "any", "Load every .elan file in a directory"},
        M{"call_action", "call_action", 1, -1, "any", "Call an action by name"},
        M{"string.len", "string.len", 1, 1, "int", "Length in bytes"},
        M{"string.strip", "string.strip", 1, 1, "string", "Trim whitespace"},
        M{"string.lstrip", "string.lstrip", 1, 1, "string", "Trim leading whitespace"},
        M{"string.rstrip", "string.rstrip", 1, 1, "string", "Trim trailing whitespace"},
        M{"string.lower", "string.lower", 1, 1, "string", "Lowercase copy"},
        M{"string.upper", "string.upper", 1, 1, "string", "Uppercase copy"},
        M{"string.find", "string.find", 2, 2, "int", "First index of a substring, -1 if absent"},
        M{"string.substr", "string.substr", 2, 3, "string", "Substring (start, length)"},
        M{"string.starts_with", "string.starts_with", 2, 2, "bool", "Prefix test"},
        M{"string.ends_with", "string.ends_with", 2, 2, "bool", "Suffix test"},
        M{"string.contains", "string.contains", 2, 2, "bool", "Substring test"},
        M{"string.replace", "string.replace", 3, 3, "string", "Replace every occurrence"},
        M{"string.split", "string.split", 2, 2, "array<string>", "Split on a separator"},
        M{"string.repeat", "string.repeat", 2, 2, "string", "Text repeated n times"},
        M{"string.reverse", "string.reverse", 1, 1, "string", "Reversed copy"},
        M{"is_int", "is_int", 1, 1, "bool", "Text parses as an int"},
        M{"is_float", "is_float", 1, 1, "bool", "Text parses as a float"},
        M{"char_is_digit", "char_is_digit", 1, 1, "bool", "Character is 0-9"},
        M{"char_is_alpha", "char_is_alpha", 1, 1, "bool", "Character is a letter"},
        M{"char_is_space", "char_is_space", 1, 1, "bool", "Character is whitespace"},
        M{"map", "map", 2, 2, "string", "Apply a function to every element", true},
        M{"filter", "filter", 2, 2, "string", "Elements a predicate accepts", true},
        M{"reduce", "reduce", 3, 3, "string", "Fold elements into one value", true},
        M{"set_new", "set_new", 0, -1, "set<any>", "New set"},
        M{"set_of", "set_of", 0, -1, "set<any>", "Set from values"},
        M{"set_add", "set_add", 2, 2, "bool", "Add to a set"},
        M{"set_has", "set_has", 2, 2, "bool", "Set membership"},
        M{"set_remove", "set_remove", 2, 2, "bool", "Remove from a set"},
        M{"set_size", "set_size", 1, 1, "int", "Set size"},
        M{"mutex_new", "mutex_new", 0, 0, "string", "New mutex"},
        M{"mutex_lock", "mutex_lock", 1, 1, "void", "Lock a mutex"},
        M{"mutex_unlock", "mutex_unlock", 1, 1, "void", "Unlock a mutex"},
        M{"mutex_try_lock", "mutex_try_lock", 1, 1, "bool", "Lock if free"},
        M{"alloc", "alloc", 0, 1, "pointer", "Allocate raw memory"},
        M{"free", "free", 1, 1, "void", "Free raw memory"},
        M{"realloc", "realloc", 2, 2, "pointer", "Resize raw memory"},
        M{"copy", "copy", 3, 3, "void", "Copy bytes between pointers"},
        M{"move", "move", 3, 3, "void", "Move bytes between pointers"},
        M{"fill", "fill", 3, 3, "void", "Fill memory with a value"},
        M{"zero", "zero", 2, 2, "void", "Zero memory"},
        M{"mem_stats", "mem_stats", 0, 0, "string", "Allocation statistics"},
        M{"mem.stats", "mem_stats", 0, 0, "string", "Allocation statistics", true},
        M{"mem_alive", "mem_alive", 0, 0, "int", "Live allocations"},
        M{"mem.alive", "mem_alive", 0, 0, "int", "Live allocations", true},
        M{"mem_reset_stats", "mem_reset_stats", 0, 0, "void", "Reset allocation statistics"},
        M{"mem.reset_stats", "mem_reset_stats", 0, 0, "void", "Reset allocation statistics", true},
        M{"heap", "heap", 1, 1, "unknown", "Owned heap value"},
        M{"shared", "shared", 1, 1, "unknown", "Reference counted value"},
        M{"weak", "weak", 1, 1, "unknown", "Weak reference to a shared value"},
        M{"buffer", "buffer", 1, 1, "unknown", "Typed buffer"},
        M{"dynamic_cast", "dynamic_cast", 2, 2, "unknown", "Checked cast"},
        M{"reinterpret_cast", "reinterpret_cast", 2, 2, "unknown", "Unchecked cast"},
        M{"bit_cast", "bit_cast", 2, 2, "unknown", "Reinterpret bits"},
        M{"__builtin_sizeof", "__builtin_sizeof", 1, 1, "int", "Size of a type", true},
        M{"__builtin_alignof", "__builtin_alignof", 1, 1, "int", "Alignment of a type", true},
        M{"__builtin_typeof", "__builtin_typeof", 1, 1, "string", "Type name of a value", true},
        M{"__builtin_decltype", "__builtin_decltype", 1, 1, "string", "Declared type", true},
        M{"__builtin_offsetof", "__builtin_offsetof", 2, 2, "int", "Field offset", true},
        M{"__builtin_is_base_of", "__builtin_is_base_of", 2, 2, "bool", "Type relation", true},
        M{"mini_ir_run", "mini_ir_run", 1, 1, "string", "Run mini IR text", true},
        M{"mini_ir.run", "mini_ir_run", 1, 1, "string", "Run mini IR text", true},
        M{"mini_ir_run_file", "mini_ir_run_file", 1, 1, "string", "Run a mini IR file", true},
        M{"mini_ir.run_file", "mini_ir_run_file", 1, 1, "string", "Run a mini IR file", true},
        M{"mini_lex_buffer", "mini_lex_buffer", 1, 1, "unknown", "Lex into a buffer", true},
        M{"language_name", "language_name", 0, 0, "string", "Name of the running language"},
        M{"language_version", "language_version", 0, 0, "string", "Version of the runtime"},
        M{"plugin_core", "plugin_core", 2, 2, "string", "Value from a plugin .core file"},
        M{"plugin_core_files", "plugin_core_files", 1, 1, "string", "A plugin's .core files"},
        M{"plugin_core_keys", "plugin_core_keys", 2, 2, "string", "Keys of a plugin .core file"},
    };
}

using T = TypeMember;

std::vector<TypeSpec> build_types() {
    return {
        {"array", "Array<T>, list literals", {
            T{"len", 0, 0, "int", "Number of elements"},
            T{"length", 0, 0, "int", "Number of elements"},
            T{"size", 0, 0, "int", "Number of elements"},
            T{"capacity", 0, 0, "int", "Reserved slots"},
            T{"find", 1, 1, "int", "Index of a value, -1 if absent"},
            T{"index_of", 1, 1, "int", "Index of a value, -1 if absent"},
            T{"empty", 0, 0, "bool", "No elements"},
            T{"contains", 1, 1, "bool", "Value is present"},
            T{"erase", 1, 1, "bool", "Remove the element at an index"},
            T{"remove", 1, 1, "bool", "Remove the element at an index"},
            T{"remove_at", 1, 1, "bool", "Remove the element at an index"},
            T{"get", 1, 1, "T", "Element at an index"},
            T{"at", 1, 1, "T", "Element at an index"},
            T{"front", 0, 0, "T", "First element"},
            T{"back", 0, 0, "T", "Last element"},
            T{"first", 0, 0, "T", "First element"},
            T{"last", 0, 0, "T", "Last element"},
            T{"pop", 0, 0, "T", "Remove and return the last element"},
            T{"pop_back", 0, 0, "T", "Remove and return the last element"},
            T{"push", 1, 1, "void", "Append an element"},
            T{"append", 1, 1, "void", "Append an element"},
            T{"push_back", 1, 1, "void", "Append an element"},
            T{"emplace_back", 1, 1, "void", "Append an element"},
            T{"emplace", 1, 1, "void", "Append an element"},
            T{"insert", 2, 2, "void", "Insert at an index"},
            T{"set", 2, 2, "void", "Replace the element at an index"},
            T{"clear", 0, 0, "void", "Remove every element"},
            T{"reserve", 1, 1, "void", "Reserve capacity"},
            T{"shrink_to_fit", 0, 0, "void", "Release spare capacity"},
            T{"join", 0, 1, "string", "Elements joined by a separator"},
        }},
        {"map", "Map<K, V>, dict literals", {
            T{"len", 0, 0, "int", "Number of entries"},
            T{"length", 0, 0, "int", "Number of entries"},
            T{"size", 0, 0, "int", "Number of entries"},
            T{"has", 1, 1, "bool", "Key is present"},
            T{"contains", 1, 1, "bool", "Key is present"},
            T{"containsKey", 1, 1, "bool", "Key is present"},
            T{"count", 1, 1, "bool", "Key is present"},
            T{"remove", 1, 1, "bool", "Remove a key"},
            T{"erase", 1, 1, "bool", "Remove a key"},
            T{"empty", 0, 0, "bool", "No entries"},
            T{"get", 1, 1, "V", "Value of a key"},
            T{"at", 1, 1, "V", "Value of a key"},
            T{"getOr", 2, 2, "V", "Value of a key or a default"},
            T{"get_or", 2, 2, "V", "Value of a key or a default"},
            T{"get_or_default", 2, 2, "V", "Value of a key or a default"},
            T{"getOrDefault", 2, 2, "V", "Value of a key or a default"},
            T{"keys", 0, 0, "array<K>", "Keys"},
            T{"values", 0, 0, "array<V>", "Values"},
            T{"clone", 0, 0, "self", "Copy of the map"},
            T{"set", 2, 2, "void", "Set a key"},
            T{"put", 2, 2, "void", "Set a key"},
            T{"insert", 2, 2, "void", "Set a key"},
            T{"clear", 0, 0, "void", "Remove every entry"},
            T{"merge", 1, 1, "void", "Copy entries from another map"},
        }},
        {"string", "string values", {
            T{"len", 0, 0, "int", "Length in bytes"},
            T{"length", 0, 0, "int", "Length in bytes"},
            T{"lower", 0, 0, "string", "Lowercase copy"},
            T{"upper", 0, 0, "string", "Uppercase copy"},
            T{"strip", 0, 0, "string", "Trim whitespace"},
            T{"lstrip", 0, 0, "string", "Trim leading whitespace"},
            T{"rstrip", 0, 0, "string", "Trim trailing whitespace"},
            T{"find", 1, 1, "int", "First index of a substring, -1 if absent"},
            T{"substr", 1, 2, "string", "Substring (start, length)"},
            T{"starts_with", 1, 1, "bool", "Prefix test"},
            T{"ends_with", 1, 1, "bool", "Suffix test"},
            T{"contains", 1, 1, "bool", "Substring test"},
            T{"replace", 2, 2, "string", "Replace every occurrence"},
            T{"split", 1, 1, "array<string>", "Split on a separator"},
            T{"repeat", 1, 1, "string", "Text repeated n times"},
            T{"reverse", 0, 0, "string", "Reversed copy"},
        }},
        {"ws", "WebSocket handle from ws.connect", {
            T{"send", 1, 1, "bool", "Send a text frame"},
            T{"send_binary", 1, 1, "bool", "Send a binary frame"},
            T{"recv", 0, 0, "string", "Receive (blocking)"},
            T{"recv_timeout", 1, 1, "string", "Receive, \"\" after the timeout (ms)"},
            T{"close", 0, 2, "void", "Close, optional code and reason"},
            T{"broadcast", 1, 1, "bool", "Send to every client of a server"},
            T{"state", 0, 0, "string", "open, closing or closed"},
            T{"close_code", 0, 0, "string", "Close code the peer sent"},
            T{"close_reason", 0, 0, "string", "Close reason the peer sent"},
        }},
        {"tcp", "TCP handle from tcp.connect", {
            T{"send", 1, 1, "int", "Send bytes, returns bytes sent"},
            T{"recv", 0, 0, "string", "Receive (blocking)"},
            T{"recv_timeout", 0, 1, "string", "Receive with a timeout (ms)"},
            T{"close", 0, 0, "bool", "Close the socket"},
            T{"state", 0, 0, "string", "Connection state"},
        }},
        {"udp", "UDP handle from net.udp_bind", {
            T{"send", 3, 3, "bool", "Send (host, port, data)"},
            T{"receive", 1, 1, "string", "Receive up to n bytes"},
            T{"port", 0, 0, "string", "Bound port"},
            T{"close", 0, 0, "bool", "Close the socket"},
        }},
        {"resp", "HTTP response handle from net.get_resp", {
            T{"status", 0, 0, "int", "Status code"},
            T{"body", 0, 0, "string", "Response body"},
            T{"header", 1, 1, "string", "Response header"},
            T{"json", 0, 0, "string", "Body as JSON text"},
        }},
        {"req", "Request handle inside an HTTP server route", {
            T{"body", 0, 0, "string", "Request body"},
            T{"query", 1, 1, "string", "Query parameter"},
            T{"header", 1, 1, "string", "Request header"},
            T{"method", 0, 0, "string", "HTTP method"},
            T{"path", 0, 0, "string", "Request path"},
            T{"cookie", 1, 1, "string", "Cookie value"},
            T{"file", 1, 1, "string", "Uploaded file"},
            T{"save_upload", 2, 2, "string", "Save an uploaded file"},
        }},
        {"res", "Response handle inside an HTTP server route", {
            T{"html", 1, 1, "void", "Send HTML"},
            T{"json", 1, 1, "void", "Send JSON"},
            T{"text", 1, 1, "void", "Send plain text"},
            T{"write", 1, 1, "void", "Write to the body"},
            T{"status", 1, 1, "void", "Set the status code"},
            T{"header", 2, 2, "void", "Set a header"},
            T{"cookie", 3, 6, "void", "Set a cookie"},
            T{"end", 0, 0, "void", "Finish the response"},
        }},
        {"sse", "Server-sent events stream", {
            T{"emit", 2, 2, "void", "Send an event"},
            T{"close", 0, 0, "void", "Close the stream"},
        }},
        {"file", "File handle from fs.open", {
            T{"read", 0, 1, "string", "Read, optional byte count"},
            T{"write", 1, 1, "string", "Write text"},
            T{"seek", 1, 2, "bool", "Move the position"},
            T{"tell", 0, 0, "int", "Current position"},
            T{"flush", 0, 0, "bool", "Flush buffered writes"},
            T{"buffer", 0, 1, "bool", "Set the buffer size"},
            T{"close", 0, 0, "bool", "Close the file"},
        }},
        {"chan", "Channel from pipe.new", {
            T{"send", 1, 1, "void", "Send, blocks when full"},
            T{"receive", 0, 0, "string", "Receive, blocks when empty"},
            T{"recv", 0, 0, "string", "Receive, blocks when empty"},
            T{"try_send", 1, 1, "bool", "Send without blocking"},
            T{"try_receive", 0, 0, "string", "Receive without blocking"},
            T{"try_recv", 0, 0, "string", "Receive without blocking"},
            T{"close", 0, 0, "void", "Close the channel"},
            T{"len", 0, 0, "int", "Items waiting"},
            T{"size", 0, 0, "int", "Items waiting"},
        }},
        {"future", "Result of an async call", {
            T{"cancel", 0, 0, "void", "Request cancellation"},
            T{"done", 0, 0, "bool", "Finished"},
            T{"cancelled", 0, 0, "bool", "Was cancelled"},
            T{"result", 0, 0, "any", "Value, waits if needed"},
        }},
        {"proc", "Process handle from sys.start / spawn", {
            T{"wait", 0, 1, "string", "Exit code; with a timeout (ms) \"\" if still running"},
            T{"alive", 0, 0, "bool", "Still running"},
            T{"kill", 0, 0, "bool", "Terminate the process"},
            T{"pid", 0, 0, "int", "Operating system process id"},
            T{"read", 0, 0, "string", "New stdout since the last read"},
            T{"read_err", 0, 0, "string", "New stderr since the last read"},
            T{"output", 0, 0, "string", "All stdout so far"},
            T{"errors", 0, 0, "string", "All stderr so far"},
            T{"write", 1, 1, "bool", "Write to stdin"},
            T{"close_input", 0, 0, "bool", "Close stdin"},
            T{"exit_code", 0, 0, "int", "Exit code, -1 while running"},
        }},
    };
}

void json_escape(std::ostringstream& o, std::string_view s) {
    o << '"';
    for (const char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\t': o << "\\t"; break;
            default: o << c;
        }
    }
    o << '"';
}

void json_member(std::ostringstream& o, const char* name, const char* builtin, int mn, int mx, const char* ret,
                 const char* doc, bool hidden) {
    o << "{\"name\":";
    json_escape(o, name);
    if (builtin) { o << ",\"builtin\":"; json_escape(o, builtin); }
    o << ",\"min\":" << mn << ",\"max\":" << mx << ",\"returns\":";
    json_escape(o, ret);
    o << ",\"doc\":";
    json_escape(o, doc ? doc : "");
    if (hidden) o << ",\"hidden\":true";
    o << "}";
}

}  // namespace

const std::vector<Module>& modules() {
    static const std::vector<Module> m = build_modules();
    return m;
}

const std::vector<Member>& core() {
    static const std::vector<Member> c = build_core();
    return c;
}

const std::vector<TypeSpec>& types() {
    static const std::vector<TypeSpec> t = build_types();
    return t;
}

const std::vector<const char*>& keywords() {
    static const std::vector<const char*> k = {
        "if", "else", "while", "do", "repeat", "for", "in", "switch", "case", "default", "match", "break", "continue",
        "return", "const", "let", "global", "action", "entity", "struct", "enum", "trait", "impl", "type", "hook",
        "run", "public", "private", "export", "extern", "namespace", "async", "await", "try", "catch", "unsafe",
        "parallel", "wait", "all", "fire", "pause", "new", "lambda", "true", "false", "null", "nil", "nullptr",
        "sizeof", "typeof", "decltype", "alignof", "offsetof", "is_base_of", "reinterpret_cast", "bit_cast",
        "dynamic_cast"};
    return k;
}

const std::vector<const char*>& type_words() {
    static const std::vector<const char*> t = {
        "int", "float", "double", "string", "str", "bool", "char", "void", "any", "never", "unit", "auto",
        "pointer", "Array", "Map", "Set", "HashMap", "Option", "Result", "u8", "u16", "u32", "u64", "i8", "i16",
        "i32", "i64", "uint", "unsigned"};
    return t;
}

std::string normalize_path(std::string_view path) {
    std::string out = lower(path);
    for (auto& c : out) if (c == '\\') c = '/';
    return out;
}

const Module* module_for_path(std::string_view importPath) {
    const std::string want = normalize_path(importPath);
    for (const auto& m : modules()) {
        for (const char* p : m.paths) if (want == p) return &m;
    }
    return nullptr;
}

const Module* module_by_id(std::string_view id) {
    for (const auto& m : modules()) if (id == m.id) return &m;
    return nullptr;
}

const Member* find_member(const Module& m, std::string_view name) {
    const std::string want = lower(name);
    for (const auto& mem : m.members) if (lower(mem.name) == want) return &mem;
    return nullptr;
}

const TypeSpec* type_spec(std::string_view kind) {
    for (const auto& t : types()) if (kind == t.kind) return &t;
    return nullptr;
}

const TypeMember* type_member(std::string_view kind, std::string_view name) {
    const TypeSpec* t = type_spec(kind);
    if (!t) return nullptr;
    for (const auto& m : t->members) if (name == m.name) return &m;
    return nullptr;
}

std::string manifest_json() {
    std::ostringstream o;
    o << "{\"version\":1,\"modules\":[";
    bool firstMod = true;
    for (const auto& m : modules()) {
        if (!firstMod) o << ",";
        firstMod = false;
        o << "{\"id\":";
        json_escape(o, m.id);
        o << ",\"paths\":[";
        for (std::size_t i = 0; i < m.paths.size(); ++i) { if (i) o << ","; json_escape(o, m.paths[i]); }
        o << "],\"alias\":";
        json_escape(o, m.defaultAlias);
        o << ",\"summary\":";
        json_escape(o, m.summary);
        if (m.experimental) o << ",\"experimental\":true";
        o << ",\"members\":[";
        for (std::size_t i = 0; i < m.members.size(); ++i) {
            if (i) o << ",";
            const auto& x = m.members[i];
            json_member(o, x.name, x.builtin, x.minArgs, x.maxArgs, x.returns, x.doc, x.hidden);
        }
        o << "]}";
    }
    o << "],\"core\":[";
    for (std::size_t i = 0; i < core().size(); ++i) {
        if (i) o << ",";
        const auto& x = core()[i];
        json_member(o, x.name, x.builtin, x.minArgs, x.maxArgs, x.returns, x.doc, x.hidden);
    }
    o << "],\"types\":[";
    for (std::size_t i = 0; i < types().size(); ++i) {
        if (i) o << ",";
        const auto& t = types()[i];
        o << "{\"kind\":";
        json_escape(o, t.kind);
        o << ",\"summary\":";
        json_escape(o, t.summary);
        o << ",\"members\":[";
        for (std::size_t j = 0; j < t.members.size(); ++j) {
            if (j) o << ",";
            const auto& x = t.members[j];
            json_member(o, x.name, nullptr, x.minArgs, x.maxArgs, x.returns, x.doc, false);
        }
        o << "]}";
    }
    o << "],\"keywords\":[";
    for (std::size_t i = 0; i < keywords().size(); ++i) { if (i) o << ","; json_escape(o, keywords()[i]); }
    o << "],\"typeWords\":[";
    for (std::size_t i = 0; i < type_words().size(); ++i) { if (i) o << ","; json_escape(o, type_words()[i]); }
    o << "]}";
    return o.str();
}

std::string manifest_text() {
    std::ostringstream o;
    const auto sig = [&](const char* name, int mn, int mx, const char* ret) {
        o << name << "(";
        if (mx < 0) o << mn << "+ args";
        else if (mn == mx) o << mn << (mn == 1 ? " arg" : " args");
        else o << mn << ".." << mx << " args";
        o << ") -> " << ret;
    };
    for (const auto& m : modules()) {
        o << "#include <" << m.paths[0] << "> as " << m.defaultAlias << "    // " << m.summary
          << (m.experimental ? " (experimental)" : "") << "\n";
        for (const auto& x : m.members) {
            if (x.hidden) continue;
            o << "    " << m.defaultAlias << ".";
            sig(x.name, x.minArgs, x.maxArgs, x.returns);
            o << "    " << x.doc << "\n";
        }
        o << "\n";
    }
    o << "core (no import)\n";
    for (const auto& x : core()) {
        if (x.hidden) continue;
        o << "    ";
        sig(x.name, x.minArgs, x.maxArgs, x.returns);
        o << "    " << x.doc << "\n";
    }
    for (const auto& t : types()) {
        o << "\n" << t.kind << " methods    // " << t.summary << "\n";
        for (const auto& x : t.members) {
            o << "    .";
            sig(x.name, x.minArgs, x.maxArgs, x.returns);
            o << "    " << x.doc << "\n";
        }
    }
    return o.str();
}

}  // namespace erelang::registry
