#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace erelang {

class Runtime;
struct Program;

[[nodiscard]] std::optional<std::string> dispatch_imported_builtin_modules(
    Runtime* runtime,
    const Program* program,
    const std::string& name,
    const std::vector<std::string>& argv);

// Builtin module dispatch declarations
std::string __erelang_builtin_math_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_network_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_system_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_crypto_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_data_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_regex_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_perm_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_binary_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_websocket_dispatch(const std::string& name, const std::vector<std::string>& argv);

// WS handle method dispatch for ws: prefix handles (called from actions.cpp)
std::string __erelang_ws_handle_method(int id, const std::string& method, const std::vector<std::string>& args);

// HTTP server handle method dispatch for http:/req:/res:/sse: prefix handles
std::string __erelang_http_handle_method(int id, const std::string& method, const std::vector<std::string>& args);
std::string __erelang_req_handle_method(int id, const std::string& method, const std::vector<std::string>& args);
std::string __erelang_res_handle_method(int id, const std::string& method, const std::vector<std::string>& args);
std::string __erelang_sse_handle_method(int id, const std::string& method, const std::vector<std::string>& args);

// HTTP response handle (resp: prefix) — stores status + body + headers
std::string __erelang_resp_handle_method(int id, const std::string& method, const std::vector<std::string>& args);

// Raw TCP socket handle (tcp: prefix)
std::string __erelang_tcp_handle_method(int id, const std::string& method, const std::vector<std::string>& args);

} // namespace erelang
