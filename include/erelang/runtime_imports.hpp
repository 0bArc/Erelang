#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace erelang {

struct Program;

inline constexpr std::string_view kBuiltinAliasPrefix = "__builtin__:";

void bind_builtin_module_aliases(const Program& program, std::unordered_map<std::string, std::string>& vars);

[[nodiscard]] std::optional<std::string> resolve_builtin_module_method(
    const Program& program,
    std::string_view alias,
    std::string_view method);

[[nodiscard]] bool program_imports_module(const Program* program, std::string_view modulePath);

} // namespace erelang
