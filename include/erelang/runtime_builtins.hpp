#pragma once

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

} // namespace erelang
