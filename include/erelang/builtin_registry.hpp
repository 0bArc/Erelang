// Single source of truth for builtin modules, their script names, arity and return types.
//
// Everything that needs to know "what does `#include <builtin/x> as a` give a script" reads this
// table: alias resolution (imports.cpp), the typechecker, `erelang --dump-builtins` and through that
// the VS Code extension and the docs. Add a builtin here once and every consumer sees it.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace erelang::registry {

// One callable a module exposes. `name` is what a script writes after the alias (`regex.generate`
// in `ai.regex.generate(...)`); `builtin` is the runtime dispatch name. Return types use the
// typechecker spelling (int, string, bool, void, any, array<string>, ...).
struct Member {
    const char* name;
    const char* builtin;
    int minArgs;
    int maxArgs;  // -1 = variadic
    const char* returns;
    const char* doc;
    bool hidden = false;  // legacy spelling: resolvable, but not offered by completion or docs
};

struct Module {
    const char* id;                         // "regex"
    std::vector<const char*> paths;         // "builtin/regex", ...
    const char* defaultAlias;               // alias used in docs and snippets
    const char* summary;
    std::vector<Member> members;
    std::vector<const char*> passthrough;   // `alias.<prefix>anything` resolves to that name verbatim
    bool experimental = false;
};

// Methods on a value of a given kind (array, map, string) or handle prefix (ws, tcp, proc, ...).
// Return type placeholders: T (element), K (key), V (value), self (the receiver type).
struct TypeMember {
    const char* name;
    int minArgs;
    int maxArgs;
    const char* returns;
    const char* doc;
};

struct TypeSpec {
    const char* kind;     // "array", "map", "string", or a handle prefix like "ws"
    const char* summary;
    std::vector<TypeMember> members;
};

const std::vector<Module>& modules();
const std::vector<Member>& core();        // always available, no import
const std::vector<TypeSpec>& types();
const std::vector<const char*>& keywords();
const std::vector<const char*>& type_words();

// Path lookup: "builtin/net" or "std\\net" (any case) -> module, or nullptr.
const Module* module_for_path(std::string_view importPath);
const Module* module_by_id(std::string_view id);
// Member of a module by script name (case-insensitive), or nullptr.
const Member* find_member(const Module& m, std::string_view name);
const TypeSpec* type_spec(std::string_view kind);
const TypeMember* type_member(std::string_view kind, std::string_view name);

std::string normalize_path(std::string_view path);

// JSON manifest of everything above (for `erelang --dump-builtins --json`).
std::string manifest_json();
// Human readable listing (for `erelang --dump-builtins`).
std::string manifest_text();

}  // namespace erelang::registry
