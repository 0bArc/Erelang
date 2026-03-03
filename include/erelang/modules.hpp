#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace erelang {

// Legacy placeholder (kept for compatibility)
struct ModuleInfo {
	std::string name;
	std::string path;
};

[[nodiscard]] std::vector<ModuleInfo> resolve_imports(const std::vector<std::string>& imports);

// Module packaging API
struct ModuleFile {
	const char* name;        // logical filename (e.g., "std/math.0bs")
	const char* contents;    // UTF-8 contents
};

struct ModuleDef {
	const char* name;                // module name (e.g., "std.math")
	const ModuleFile* files;         // array of files
	size_t file_count;               // count of files
};

// Registry of embedded and dynamically loaded modules
void register_embedded_module(const ModuleDef& def);

[[nodiscard]] std::vector<ModuleDef> get_registered_modules();

// Load dynamic modules (.odll) from a directory (Windows). Non-Windows: no-op.
void load_dynamic_modules_in_dir(const std::filesystem::path& dir);

}
