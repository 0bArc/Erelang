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

} // namespace erelang
