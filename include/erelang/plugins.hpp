// SPDX-License-Identifier: Apache-2.0
//
// Erelang plugin manifest utilities.
// Provides discovery and parsed metadata for .elp (Erelang Language Plugin)
// packages placed inside the runtime "plugins" directory.

#pragma once

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "erelang/erodsl/spec.hpp"

namespace erelang {

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string target;
    std::string description;
    std::vector<std::string> dependencies;
    std::vector<std::filesystem::path> scriptFiles;
    std::vector<std::filesystem::path> assetFiles;
    std::vector<std::filesystem::path> coreFiles;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> coreProperties;
    std::unordered_map<std::string, std::vector<std::string>> hookBindings;
    std::string onLoadAction;
    std::string onUnloadAction;
    std::string dataHookAction;
    std::filesystem::path baseDirectory;
    std::filesystem::path manifestPath;
    std::optional<erodsl::DslSpec> dslSpec;
};

// Returns the user-specific plugin root (e.g. %LOCALAPPDATA%\Erelang\Plugins on Windows).
// May be empty when no suitable location exists.
std::filesystem::path default_user_plugin_root();

// Ensures the user plugin root exists (creating directories when possible).
// Returns the ready-to-use path or an empty path on failure.
std::filesystem::path ensure_user_plugin_root(std::ostream* log = nullptr);

// Discover plugins within the executable-relative and user plugin roots.
// Returns manifests for plugins with valid project.elp files.
// When log is provided, warnings and informational messages are emitted there.
std::vector<PluginManifest> discover_plugins(const std::filesystem::path& rootDir, std::ostream* log = nullptr);

} // namespace erelang
