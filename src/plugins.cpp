// SPDX-License-Identifier: Apache-2.0
//
// Erelang plugin discovery and manifest parsing.

#include "erelang/plugins.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace erelang {
namespace {
namespace fs = std::filesystem;

[[nodiscard]] fs::path compute_user_plugin_root() {
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        if (*local) {
            return fs::path(local) / "Erelang" / "Plugins";
        }
    }
    if (const char* roaming = std::getenv("APPDATA")) {
        if (*roaming) {
            return fs::path(roaming) / "Erelang" / "Plugins";
        }
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (*xdg) {
            return fs::path(xdg) / "erelang" / "plugins";
        }
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home) {
            return fs::path(home) / ".local" / "share" / "erelang" / "plugins";
        }
    }
#endif
    return {};
}

[[nodiscard]] std::string trim_copy(std::string_view v) {
    const auto first = v.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = v.find_last_not_of(" \t\r\n");
    return std::string{v.substr(first, last - first + 1)};
}

[[nodiscard]] std::string to_lower_copy(std::string_view v) {
    std::string result;
    result.reserve(v.size());
    for (unsigned char ch : v) {
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

[[nodiscard]] std::unordered_map<std::string, std::string> parse_attributes(std::string_view tagSpec) {
    std::unordered_map<std::string, std::string> attrs;
    std::size_t i = 0;
    // Skip tag name
    while (i < tagSpec.size() && !std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
        ++i;
    }
    while (i < tagSpec.size()) {
        while (i < tagSpec.size() && std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
            ++i;
        }
        if (i >= tagSpec.size()) {
            break;
        }
        const std::size_t keyStart = i;
        while (i < tagSpec.size() && tagSpec[i] != '=' && !std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
            ++i;
        }
        if (i <= keyStart) {
            break;
        }
        std::string key = to_lower_copy(tagSpec.substr(keyStart, i - keyStart));
        while (i < tagSpec.size() && std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
            ++i;
        }
        if (i >= tagSpec.size() || tagSpec[i] != '=') {
            continue;
        }
        ++i;
        while (i < tagSpec.size() && std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
            ++i;
        }
        if (i >= tagSpec.size()) {
            break;
        }
        std::string value;
        if (tagSpec[i] == '"' || tagSpec[i] == '\'') {
            char quote = static_cast<char>(tagSpec[i]);
            ++i;
            const std::size_t valueStart = i;
            while (i < tagSpec.size() && tagSpec[i] != quote) {
                ++i;
            }
            value = std::string(tagSpec.substr(valueStart, i - valueStart));
            if (i < tagSpec.size()) {
                ++i;
            }
        } else {
            const std::size_t valueStart = i;
            while (i < tagSpec.size() && !std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
                ++i;
            }
            value = std::string(tagSpec.substr(valueStart, i - valueStart));
        }
        attrs[key] = trim_copy(value);
    }
    return attrs;
}

[[nodiscard]] std::unordered_map<std::string, std::vector<std::string>> parse_hooks_block(std::string_view block, std::ostream* log) {
    std::unordered_map<std::string, std::vector<std::string>> hooks;
    std::size_t pos = 0;
    while (true) {
        const auto open = block.find('<', pos);
        if (open == std::string_view::npos) {
            break;
        }
        if (open + 1 < block.size() && block[open + 1] == '/') {
            pos = open + 2;
            continue;
        }
        const auto close = block.find('>', open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        const std::string tagSpec = trim_copy(block.substr(open + 1, close - open - 1));
        if (tagSpec.empty()) {
            pos = close + 1;
            continue;
        }
        std::string tagName;
        std::size_t i = 0;
        while (i < tagSpec.size() && !std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
            tagName.push_back(tagSpec[i]);
            ++i;
        }
        if (tagName.empty()) {
            pos = close + 1;
            continue;
        }
        const auto attrs = parse_attributes(tagSpec);
        const std::string closingToken = "</" + tagName + ">";
        const auto end = block.find(closingToken, close + 1);
        if (end == std::string_view::npos) {
            if (log) {
                *log << "[plugins] unmatched hook tag <" << tagName << "> in hooks block\n";
            }
            break;
        }
        std::string hookName = to_lower_copy(tagName);
        if (hookName == "hook") {
            for (const char* key : {"name", "event", "stage", "type"}) {
                auto it = attrs.find(key);
                if (it != attrs.end() && !it->second.empty()) {
                    hookName = to_lower_copy(it->second);
                    break;
                }
            }
            if (hookName == "hook") {
                if (log) {
                    *log << "[plugins] <hook> missing name attribute; skipping entry\n";
                }
                pos = end + closingToken.size();
                continue;
            }
        }
        std::string action = trim_copy(block.substr(close + 1, end - (close + 1)));
        for (const char* key : {"action", "run", "call"}) {
            auto it = attrs.find(key);
            if (it != attrs.end() && !it->second.empty()) {
                action = it->second;
                break;
            }
        }
        action = trim_copy(action);
        if (action.empty()) {
            if (log) {
                *log << "[plugins] hook <" << hookName << "> missing target action; skipping entry\n";
            }
            pos = end + closingToken.size();
            continue;
        }
        auto& list = hooks[hookName];
        if (std::find(list.begin(), list.end(), action) == list.end()) {
            list.push_back(action);
        }
        pos = end + closingToken.size();
    }
    return hooks;
}

[[nodiscard]] std::optional<std::string> extract_block(std::string_view text, std::string_view tag) {
    const std::string open = "<" + std::string{tag} + ">";
    const std::string close = "</" + std::string{tag} + ">";
    auto pos = text.find(open);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += open.size();
    std::size_t depth = 1;
    auto cur = pos;
    while (depth > 0) {
        const auto nextOpen = text.find(open, cur);
        const auto nextClose = text.find(close, cur);
        if (nextClose == std::string_view::npos) {
            return std::nullopt;
        }
        if (nextOpen != std::string_view::npos && nextOpen < nextClose) {
            ++depth;
            cur = nextOpen + open.size();
            continue;
        }
        if (--depth == 0) {
            return std::string{text.substr(pos, nextClose - pos)};
        }
        cur = nextClose + close.size();
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> extract_single(std::string_view text, std::string_view tag) {
    const std::string open = "<" + std::string{tag} + ">";
    const std::string close = "</" + std::string{tag} + ">";
    const auto start = text.find(open);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto valueStart = start + open.size();
    const auto end = text.find(close, valueStart);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return trim_copy(text.substr(valueStart, end - valueStart));
}

[[nodiscard]] std::vector<std::string> extract_multi(std::string_view text, std::string_view tag) {
    std::vector<std::string> values;
    const std::string open = "<" + std::string{tag} + ">";
    const std::string close = "</" + std::string{tag} + ">";
    auto searchStart = std::size_t{0};
    while (true) {
        const auto start = text.find(open, searchStart);
        if (start == std::string_view::npos) {
            break;
        }
        const auto valueStart = start + open.size();
        const auto end = text.find(close, valueStart);
        if (end == std::string_view::npos) {
            break;
        }
        values.emplace_back(trim_copy(text.substr(valueStart, end - valueStart)));
        searchStart = end + close.size();
    }
    return values;
}

[[nodiscard]] const std::unordered_set<std::string>& builtin_script_extensions() {
    static const std::unordered_set<std::string> exts = { ".0bs", ".erelang", ".obsecret" };
    return exts;
}

[[nodiscard]] bool is_script_extension(const fs::path& p, const std::unordered_set<std::string>& extras) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (builtin_script_extensions().count(ext) > 0) {
        return true;
    }
    return extras.count(ext) > 0;
}

[[nodiscard]] bool is_core_extension(const fs::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".core";
}

[[nodiscard]] std::unordered_map<std::string, std::string> parse_core_file(const fs::path& path, std::ostream* log) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (log) {
            *log << "[plugins] failed to open .core file: " << path.string() << "\n";
        }
        return values;
    }

    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        auto trimmed = trim_copy(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.rfind("//", 0) == 0 || trimmed[0] == '#') {
            continue;
        }
        const auto sep = trimmed.find('=');
        if (sep == std::string::npos) {
            if (log) {
                *log << "[plugins] ignoring line " << lineNo << " in " << path.string() << ": expected key=value\n";
            }
            continue;
        }
        auto key = trim_copy(trimmed.substr(0, sep));
        auto value = trim_copy(trimmed.substr(sep + 1));
        if (key.empty()) {
            if (log) {
                *log << "[plugins] ignoring empty key at line " << lineNo << " in " << path.string() << "\n";
            }
            continue;
        }
        values[key] = value;
    }
    return values;
}

[[nodiscard]] std::unordered_map<std::string, std::string> parse_keyword_mappings(std::string_view block, std::ostream* log) {
    std::unordered_map<std::string, std::string> mappings;
    std::size_t pos = 0;
    while (true) {
        const auto open = block.find('<', pos);
        if (open == std::string_view::npos) {
            break;
        }
        if (open + 1 < block.size() && block[open + 1] == '/') {
            pos = open + 2;
            continue;
        }
        const auto close = block.find('>', open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        std::string tagSpec = trim_copy(block.substr(open + 1, close - open - 1));
        if (tagSpec.empty()) {
            pos = close + 1;
            continue;
        }
        std::string tagName;
        std::size_t i = 0;
        while (i < tagSpec.size() && !std::isspace(static_cast<unsigned char>(tagSpec[i]))) {
            tagName.push_back(tagSpec[i]);
            ++i;
        }
        if (tagName != "keyword" && tagName != "map" && tagName != "alias") {
            pos = close + 1;
            continue;
        }
        bool selfClosing = false;
        if (!tagSpec.empty() && tagSpec.back() == '/') {
            selfClosing = true;
            tagSpec.pop_back();
            while (!tagSpec.empty() && std::isspace(static_cast<unsigned char>(tagSpec.back()))) {
                tagSpec.pop_back();
            }
        }
        const auto attrs = parse_attributes(tagSpec);
        std::string inner;
        std::size_t end = std::string_view::npos;
        std::string closingToken;
        if (!selfClosing) {
            closingToken = "</" + tagName + ">";
            end = block.find(closingToken, close + 1);
            if (end == std::string_view::npos) {
                if (log) {
                    *log << "[plugins] unmatched <" << tagName << "> in <language><keywords> block\n";
                }
                break;
            }
            inner = trim_copy(block.substr(close + 1, end - (close + 1)));
        }
        std::string alias = inner;
        auto aliasAttr = attrs.find("alias");
        if (aliasAttr != attrs.end() && !aliasAttr->second.empty()) {
            alias = aliasAttr->second;
        } else if (auto fromAttr = attrs.find("from"); fromAttr != attrs.end() && !fromAttr->second.empty()) {
            alias = fromAttr->second;
        }
        std::string canonical;
        if (auto canonicalAttr = attrs.find("canonical"); canonicalAttr != attrs.end() && !canonicalAttr->second.empty()) {
            canonical = canonicalAttr->second;
        } else if (auto toAttr = attrs.find("to"); toAttr != attrs.end() && !toAttr->second.empty()) {
            canonical = toAttr->second;
        } else if (auto targetAttr = attrs.find("target"); targetAttr != attrs.end() && !targetAttr->second.empty()) {
            canonical = targetAttr->second;
        }
        alias = trim_copy(alias);
        canonical = trim_copy(canonical);
        if (alias.empty() || canonical.empty()) {
            if (log) {
                *log << "[plugins] <" << tagName << "> requires alias and canonical values; skipping\n";
            }
            if (selfClosing) {
                pos = close + 1;
            } else {
                pos = end + closingToken.size();
            }
            continue;
        }
        auto aliasKey = to_lower_copy(alias);
        auto canonicalValue = to_lower_copy(canonical);
        mappings[aliasKey] = canonicalValue;
        if (selfClosing) {
            pos = close + 1;
        } else {
            pos = end + closingToken.size();
        }
    }
    return mappings;
}

[[nodiscard]] std::string manifest_label(const fs::path& manifestPath) {
    if (manifestPath.empty()) {
        return "<unknown manifest>";
    }
    return manifestPath.string();
}

} // namespace

std::filesystem::path default_user_plugin_root() {
    return compute_user_plugin_root();
}

std::filesystem::path ensure_user_plugin_root(std::ostream* log) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = compute_user_plugin_root();
    if (root.empty()) {
        if (log) {
            *log << "[plugins] unable to determine user plugin directory (missing LOCALAPPDATA/APPDATA/XDG_DATA_HOME/HOME)\n";
        }
        return {};
    }

    fs::create_directories(root, ec);
    if (ec) {
        if (log) {
            *log << "[plugins] failed to initialize user plugin directory '" << root.string() << "': " << ec.message() << "\n";
        }
        return {};
    }

    return root;
}

std::vector<PluginManifest> discover_plugins(const std::filesystem::path& rootDir, std::ostream* log) {
    namespace fs = std::filesystem;
    std::vector<PluginManifest> manifests;
    std::unordered_map<std::string, std::size_t> manifestIndex;

    const fs::path builtinPlugins = rootDir / "plugins";
    const fs::path userPlugins = ensure_user_plugin_root(log);

    auto scan_root = [&](const fs::path& root, bool createDirectories) {
        if (root.empty()) {
            return;
        }

        std::error_code ec;
        if (createDirectories) {
            fs::create_directories(root, ec);
            if (ec) {
                if (log) {
                    *log << "[plugins] failed to prepare plugin directory '" << root.string() << "': " << ec.message() << "\n";
                }
                return;
            }
        }

        if (!fs::exists(root)) {
            return;
        }

        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) {
                if (log) {
                    *log << "[plugins] directory iteration error in '" << root.string() << "': " << ec.message() << "\n";
                }
                break;
            }
            if (!entry.is_directory()) {
                continue;
            }

            const fs::path manifestPath = entry.path() / "project.elp";
            if (!fs::exists(manifestPath)) {
                if (log) {
                    *log << "[plugins] skipping " << entry.path().string() << ": missing project.elp\n";
                }
                continue;
            }

            std::ifstream in(manifestPath, std::ios::binary);
            if (!in) {
                if (log) {
                    *log << "[plugins] failed to open manifest: " << manifestPath.string() << "\n";
                }
                continue;
            }

            std::ostringstream buffer;
            buffer << in.rdbuf();
            const std::string manifestText = buffer.str();
            auto pluginBlock = extract_block(manifestText, "plugin");
            if (!pluginBlock) {
                if (log) {
                    *log << "[plugins] invalid manifest (missing <plugin>): " << manifest_label(manifestPath) << "\n";
                }
                continue;
            }

            auto metaBlock = extract_block(*pluginBlock, "erelang_manifest");
            if (!metaBlock) {
                if (log) {
                    *log << "[plugins] invalid manifest (missing <erelang_manifest>): " << manifest_label(manifestPath) << "\n";
                }
                continue;
            }

            PluginManifest manifest;
            manifest.baseDirectory = entry.path();
            manifest.manifestPath = manifestPath;
            manifest.id = extract_single(*metaBlock, "id").value_or(std::string{});
            manifest.name = extract_single(*metaBlock, "name").value_or(std::string{});
            manifest.version = extract_single(*metaBlock, "version").value_or(std::string{});
            manifest.author = extract_single(*metaBlock, "author").value_or(std::string{});
            manifest.target = extract_single(*metaBlock, "target").value_or(std::string{});
            manifest.description = extract_single(*metaBlock, "description").value_or(std::string{});

            if (manifest.id.empty() || manifest.name.empty()) {
                if (log) {
                    *log << "[plugins] manifest missing required id/name: " << manifest_label(manifestPath) << "\n";
                }
                continue;
            }

            std::unordered_set<std::string> languageExtensions;
            if (auto languageBlock = extract_block(*pluginBlock, "language")) {
                erodsl::DslSpec spec;
                spec.id = extract_single(*languageBlock, "id").value_or(manifest.id + ".language");
                spec.name = extract_single(*languageBlock, "name").value_or(manifest.name.empty() ? manifest.id : manifest.name);
                spec.version = extract_single(*languageBlock, "version").value_or(manifest.version);

                auto collect_extensions = [&](std::string_view container) {
                    auto directExts = extract_multi(container, "extension");
                    auto extTags = extract_multi(container, "ext");
                    directExts.insert(directExts.end(), extTags.begin(), extTags.end());
                    for (auto& rawExt : directExts) {
                        auto trimmed = trim_copy(rawExt);
                        if (trimmed.empty()) {
                            continue;
                        }
                        auto normalized = erodsl::normalize_extension(trimmed);
                        if (normalized.empty()) {
                            continue;
                        }
                        if (languageExtensions.insert(normalized).second) {
                            spec.extensions.push_back(normalized);
                        }
                    }
                };

                if (auto extBlock = extract_block(*languageBlock, "extensions")) {
                    collect_extensions(*extBlock);
                }
                collect_extensions(*languageBlock);
                if (!spec.extensions.empty()) {
                    std::sort(spec.extensions.begin(), spec.extensions.end());
                }

                std::unordered_map<std::string, std::string> keywordMappings;
                if (auto keywordsBlock = extract_block(*languageBlock, "keywords")) {
                    keywordMappings = parse_keyword_mappings(*keywordsBlock, log);
                } else {
                    keywordMappings = parse_keyword_mappings(*languageBlock, log);
                }
                if (!keywordMappings.empty()) {
                    spec.keywordAliases = std::move(keywordMappings);
                }

                if (!spec.extensions.empty()) {
                    manifest.dslSpec = std::move(spec);
                }
            }

            if (auto depsBlock = extract_block(*pluginBlock, "dependencies")) {
                manifest.dependencies = extract_multi(*depsBlock, "require");
            }

            if (auto contentBlock = extract_block(*pluginBlock, "content")) {
                auto includes = extract_multi(*contentBlock, "include");
                for (const auto& rel : includes) {
                    if (rel.empty()) {
                        continue;
                    }

                    fs::path resolved = manifest.baseDirectory / fs::path(rel).lexically_normal();
                    if (!fs::exists(resolved)) {
                        if (log) {
                            *log << "[plugins] missing include file '" << rel << "' for " << manifest_label(manifestPath) << "\n";
                        }
                        continue;
                    }

                    std::error_code pathEc;
                    if (is_script_extension(resolved, languageExtensions)) {
                        manifest.scriptFiles.push_back(fs::weakly_canonical(resolved, pathEc));
                        if (pathEc) {
                            pathEc.clear();
                            manifest.scriptFiles.back() = resolved;
                        }
                    } else if (is_core_extension(resolved)) {
                        manifest.coreFiles.push_back(fs::weakly_canonical(resolved, pathEc));
                        if (pathEc) {
                            pathEc.clear();
                            manifest.coreFiles.back() = resolved;
                        }
                        auto stem = resolved.stem().string();
                        if (stem.empty()) {
                            stem = resolved.filename().string();
                        }
                        auto values = parse_core_file(resolved, log);
                        if (!values.empty()) {
                            manifest.coreProperties[stem] = std::move(values);
                        } else {
                            manifest.coreProperties.emplace(stem, std::unordered_map<std::string, std::string>{});
                        }
                    } else {
                        manifest.assetFiles.push_back(fs::weakly_canonical(resolved, pathEc));
                        if (pathEc) {
                            pathEc.clear();
                            manifest.assetFiles.back() = resolved;
                        }
                    }
                }
            }

            if (auto hooksBlock = extract_block(*pluginBlock, "hooks")) {
                manifest.hookBindings = parse_hooks_block(*hooksBlock, log);

                auto register_hook = [&](const std::string& hookName, const std::string& action) {
                    if (hookName.empty() || action.empty()) {
                        return;
                    }
                    auto key = to_lower_copy(hookName);
                    auto& list = manifest.hookBindings[key];
                    if (std::find(list.begin(), list.end(), action) == list.end()) {
                        list.push_back(action);
                    }
                };

                manifest.onLoadAction = extract_single(*hooksBlock, "onLoad").value_or(std::string{});
                manifest.onUnloadAction = extract_single(*hooksBlock, "onUnload").value_or(std::string{});
                manifest.dataHookAction = extract_single(*hooksBlock, "dataHook").value_or(std::string{});

                register_hook("onload", manifest.onLoadAction);
                register_hook("onunload", manifest.onUnloadAction);
                register_hook("datahook", manifest.dataHookAction);
            }

            const auto existing = manifestIndex.find(manifest.id);
            std::string previousLocation;
            if (existing != manifestIndex.end()) {
                previousLocation = manifests[existing->second].baseDirectory.string();
            }

            if (log) {
                *log << "[plugins] loaded " << manifest.id << " v" << manifest.version;
                if (!manifest.name.empty()) {
                    *log << " (" << manifest.name << ")";
                }
                *log << " from " << manifest.baseDirectory.string();
                if (!previousLocation.empty()) {
                    *log << " (overriding " << previousLocation << ")";
                }
                *log << "\n";
            }

            if (existing != manifestIndex.end()) {
                manifests[existing->second] = std::move(manifest);
            } else {
                manifestIndex[manifest.id] = manifests.size();
                manifests.push_back(std::move(manifest));
            }
        }
    };

    scan_root(builtinPlugins, true);
    scan_root(userPlugins, false);

    return manifests;
}

} // namespace erelang
