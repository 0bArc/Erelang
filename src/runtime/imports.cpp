// Builtin module imports: which modules a program includes, and what `alias.method` means.
// The data lives in the builtin registry (src/runtime/builtin_registry.cpp); this file only reads it.
#include "erelang/runtime_imports.hpp"
#include "erelang/builtin_registry.hpp"
#include "erelang/value.hpp"

#include "erelang/parser.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace erelang {

namespace {

std::string lowercase_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// Modules imported by a program, as registry pointers. Cached per thread because the runtime asks
// on every builtin call; the cache key includes the import count so load_elan additions are seen.
struct ImportCache {
    const Program* program = nullptr;
    std::size_t importCount = static_cast<std::size_t>(-1);
    std::vector<const registry::Module*> modules;
};

const std::vector<const registry::Module*>& imported_modules(const Program& program) {
    thread_local ImportCache cache;
    if (cache.program != &program || cache.importCount != program.imports.size()) {
        cache.program = &program;
        cache.importCount = program.imports.size();
        cache.modules.clear();
        for (const auto& importDecl : program.imports) {
            if (importDecl.path.empty()) continue;
            if (const auto* m = registry::module_for_path(importDecl.path)) cache.modules.push_back(m);
        }
    }
    return cache.modules;
}

}  // namespace

bool program_imports_module(const Program* program, std::string_view modulePath) {
    if (!program) {
        return false;
    }
    const registry::Module* want = registry::module_for_path(modulePath);
    if (want) {
        for (const auto* m : imported_modules(*program)) {
            if (m == want) return true;
        }
        return false;
    }
    // Not a builtin module (script or package include): compare paths.
    const std::string normalized = registry::normalize_path(modulePath);
    for (const auto& importDecl : program->imports) {
        if (!importDecl.path.empty() && registry::normalize_path(importDecl.path) == normalized) return true;
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
        const registry::Module* module = registry::module_for_path(importDecl.path);
        if (!module) {
            continue;
        }
        if (const registry::Member* member = registry::find_member(*module, methodName)) {
            return std::string(member->builtin);
        }
        for (const char* prefix : module->passthrough) {
            if (methodName.rfind(prefix, 0) == 0) return methodName;
        }
    }

    return std::nullopt;
}

void bind_builtin_module_aliases(const Program& program, ValueMap& vars) {
    for (const auto& importDecl : program.imports) {
        if (!importDecl.alias || importDecl.path.empty()) {
            continue;
        }
        const registry::Module* module = registry::module_for_path(importDecl.path);
        if (!module) {
            continue;
        }
        const std::string& alias = *importDecl.alias;
        for (const auto& member : module->members) {
            vars[alias + "." + member.name] = std::string(kBuiltinAliasPrefix) + member.builtin;
        }
    }
}

}  // namespace erelang
