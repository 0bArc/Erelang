// SPDX-License-Identifier: Apache-2.0
//
// Erelang Creation Kit: interactive assistant for plugin scaffolding and syntax/runtime customization.

#include "erelang/creation_kit.hpp"

#include "erelang/plugins.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace erelang {
namespace {

struct KitIo {
    std::istream& in;
    std::ostream& out;
    std::ostream* log;
};

[[nodiscard]] std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string{value.substr(first, last - first + 1)};
}

[[nodiscard]] std::string to_lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

[[nodiscard]] std::string slugify(std::string_view value) {
    std::string slug;
    slug.reserve(value.size());
    bool lastDash = false;
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
            lastDash = false;
        } else if (ch == '-' || ch == '_' || std::isspace(ch)) {
            if (!lastDash && !slug.empty()) {
                slug.push_back('-');
                lastDash = true;
            }
        }
    }
    if (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "plugin";
    }
    return slug;
}

[[nodiscard]] std::string default_author() {
    if (const char* user = std::getenv("USERNAME")) {
        if (*user) {
            return std::string{user};
        }
    }
    if (const char* user = std::getenv("USER")) {
        if (*user) {
            return std::string{user};
        }
    }
    return "Unknown";
}

std::string prompt_line(KitIo& io, std::string_view prompt, std::string_view defaultValue = {}) {
    io.out << prompt;
    if (!defaultValue.empty()) {
        io.out << " [" << defaultValue << "]";
    }
    io.out << ": ";
    io.out.flush();
    std::string line;
    if (!std::getline(io.in, line)) {
        return std::string{defaultValue};
    }
    line = trim(line);
    if (line.empty()) {
        return std::string{defaultValue};
    }
    return line;
}

bool prompt_confirm(KitIo& io, std::string_view prompt, bool defaultYes = false) {
    const std::string label = defaultYes ? "Y/n" : "y/N";
    while (true) {
        std::string answer = prompt_line(io, prompt, label);
        std::string lowered = to_lower(answer);
        if (answer == label || lowered == to_lower(label) || lowered.empty()) {
            return defaultYes;
        }
        if (lowered == "y" || lowered == "yes") {
            return true;
        }
        if (lowered == "n" || lowered == "no") {
            return false;
        }
        io.out << "Please answer yes or no.\n";
    }
}

[[nodiscard]] std::vector<std::string> split_items(std::string_view input) {
    std::vector<std::string> items;
    std::string current;
    for (char ch : input) {
        if (ch == ',') {
            auto trimmed = trim(current);
            if (!trimmed.empty()) {
                items.push_back(std::move(trimmed));
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    auto trimmed = trim(current);
    if (!trimmed.empty()) {
        items.push_back(std::move(trimmed));
    }
    return items;
}

struct SyntaxProfile {
    std::vector<std::string> keywords;
    std::vector<std::string> directives;
    std::vector<std::string> types;
    std::vector<std::string> operators;
    std::vector<std::string> visibilityModifiers;
    std::vector<std::string> stringQuotes;
    std::vector<std::string> requiredHooks;
    std::string requiredEntry;
    std::string commentToken;
    std::string attributePrefix;
    std::map<std::string, std::string> brackets;
};

[[nodiscard]] SyntaxProfile default_profile() {
    SyntaxProfile profile;
    profile.keywords = {"action", "entity", "hook", "run", "print", "sleep", "parallel", "wait", "all", "let", "const", "return", "if", "else", "switch", "case", "default", "pause", "input", "public", "private", "export", "import"};
    profile.directives = {"erelang", "strict", "debug", "deprecated", "profile", "describe", "parallel", "pure", "final", "override", "hidden", "event"};
    profile.types = {"void", "int", "str", "bool", "list", "array", "dict", "map"};
    profile.operators = {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "=", "!"};
    profile.visibilityModifiers = {"public", "private"};
    profile.stringQuotes = {"\"", "'"};
    profile.requiredHooks = {"onStart", "onEnd"};
    profile.requiredEntry = "main";
    profile.commentToken = "//";
    profile.attributePrefix = "@";
    profile.brackets = {{"lbrace", "{"}, {"rbrace", "}"}, {"lparen", "("}, {"rparen", ")"}};
    return profile;
}

[[nodiscard]] std::vector<std::string> parse_string_array(std::string_view text, std::string_view key) {
    std::vector<std::string> values;
    const std::string marker = "\"" + std::string{key} + "\"";
    auto pos = text.find(marker);
    if (pos == std::string_view::npos) {
        return values;
    }
    pos = text.find('[', pos);
    if (pos == std::string_view::npos) {
        return values;
    }
    auto end = text.find(']', pos);
    if (end == std::string_view::npos) {
        return values;
    }
    bool inString = false;
    bool escape = false;
    std::string current;
    for (std::size_t i = pos + 1; i < end; ++i) {
        const char ch = text[i];
        if (!inString) {
            if (ch == '"') {
                inString = true;
                current.clear();
            }
            continue;
        }
        if (escape) {
            current.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            inString = false;
            values.push_back(current);
            continue;
        }
        current.push_back(ch);
    }
    return values;
}

[[nodiscard]] std::optional<std::string> parse_string_value(std::string_view text, std::string_view key) {
    const std::string marker = "\"" + std::string{key} + "\"";
    auto pos = text.find(marker);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = text.find(':', pos);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = text.find('"', pos);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    bool escape = false;
    std::string value;
    for (std::size_t i = pos + 1; i < text.size(); ++i) {
        const char ch = text[i];
        if (escape) {
            value.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

[[nodiscard]] std::map<std::string, std::string> parse_string_map(std::string_view text, std::string_view key) {
    std::map<std::string, std::string> values;
    const std::string marker = "\"" + std::string{key} + "\"";
    auto pos = text.find(marker);
    if (pos == std::string_view::npos) {
        return values;
    }
    pos = text.find('{', pos);
    if (pos == std::string_view::npos) {
        return values;
    }
    auto end = text.find('}', pos);
    if (end == std::string_view::npos) {
        return values;
    }
    pos += 1;
    while (pos < end) {
        auto keyStart = text.find('"', pos);
        if (keyStart == std::string_view::npos || keyStart >= end) {
            break;
        }
        auto keyEnd = text.find('"', keyStart + 1);
        if (keyEnd == std::string_view::npos || keyEnd >= end) {
            break;
        }
        std::string keyToken(text.substr(keyStart + 1, keyEnd - keyStart - 1));
        auto valueStart = text.find('"', keyEnd + 1);
        if (valueStart == std::string_view::npos || valueStart >= end) {
            break;
        }
        auto valueEnd = text.find('"', valueStart + 1);
        if (valueEnd == std::string_view::npos || valueEnd >= end) {
            break;
        }
        std::string valueToken(text.substr(valueStart + 1, valueEnd - valueStart - 1));
        values.emplace(std::move(keyToken), std::move(valueToken));
        pos = valueEnd + 1;
    }
    return values;
}

[[nodiscard]] SyntaxProfile load_baseline(const KitIo& io, const std::filesystem::path& runtimeRoot) {
    SyntaxProfile profile = default_profile();
    const auto syntaxPath = runtimeRoot / "syntax.json";
    std::ifstream in(syntaxPath, std::ios::binary);
    if (!in) {
        if (io.log) {
            *io.log << "[kit] using embedded syntax defaults; failed to read " << syntaxPath.string() << "\n";
        }
        return profile;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    auto keywords = parse_string_array(text, "keywords");
    if (!keywords.empty()) {
        profile.keywords = std::move(keywords);
    }
    auto directives = parse_string_array(text, "directives");
    if (!directives.empty()) {
        profile.directives = std::move(directives);
    }
    auto types = parse_string_array(text, "types");
    if (!types.empty()) {
        profile.types = std::move(types);
    }
    auto operators = parse_string_array(text, "operators");
    if (!operators.empty()) {
        profile.operators = std::move(operators);
    }
    auto modifiers = parse_string_array(text, "visibilityModifiers");
    if (!modifiers.empty()) {
        profile.visibilityModifiers = std::move(modifiers);
    }
    auto quotes = parse_string_array(text, "stringQuotes");
    if (!quotes.empty()) {
        profile.stringQuotes = std::move(quotes);
    }
    auto hooks = parse_string_array(text, "requiredHooks");
    if (!hooks.empty()) {
        profile.requiredHooks = std::move(hooks);
    }
    if (auto entry = parse_string_value(text, "requiredEntry")) {
        profile.requiredEntry = *entry;
    }
    if (auto comment = parse_string_value(text, "comment")) {
        profile.commentToken = *comment;
    }
    if (auto attr = parse_string_value(text, "attributePrefix")) {
        profile.attributePrefix = *attr;
    }
    auto brackets = parse_string_map(text, "brackets");
    if (!brackets.empty()) {
        profile.brackets = std::move(brackets);
    }
    return profile;
}

void apply_additions(std::vector<std::string>& values, const std::vector<std::string>& additions) {
    for (const auto& add : additions) {
        if (add.empty()) {
            continue;
        }
        if (std::find(values.begin(), values.end(), add) == values.end()) {
            values.push_back(add);
        }
    }
}

void apply_removals(std::vector<std::string>& values, const std::vector<std::string>& removals) {
    if (removals.empty()) {
        return;
    }
    std::set<std::string> toRemove(removals.begin(), removals.end());
    values.erase(std::remove_if(values.begin(), values.end(), [&](const std::string& item) { return toRemove.count(item) > 0; }), values.end());
}

[[nodiscard]] std::string escape_json(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 4);
    for (char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string join_with(const std::vector<std::string>& items, std::string_view delimiter) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            oss << delimiter;
        }
        oss << items[i];
    }
    return oss.str();
}

bool write_text_file(const std::filesystem::path& path, std::string_view content, KitIo& io) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (io.log) {
            *io.log << "[kit] failed to create directories for " << path.string() << ": " << ec.message() << "\n";
        }
        io.out << "Failed to create directory for " << path.string() << "\n";
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (io.log) {
            *io.log << "[kit] failed to open " << path.string() << " for writing\n";
        }
        io.out << "Failed to write " << path.string() << "\n";
        return false;
    }
    out << content;
    return true;
}

struct PluginResult {
    std::string slug;
    std::filesystem::path directory;
};

std::optional<PluginResult> run_plugin_wizard(KitIo& io, const CreationKitOptions& options) {
    namespace fs = std::filesystem;
    io.out << "\n=== Plugin Template Builder ===\n";
    const std::string name = prompt_line(io, "Plugin display name");
    if (name.empty()) {
        io.out << "Cancelled plugin creation.\n";
        return std::nullopt;
    }
    const std::string defaultId = to_lower(trim(name));
    std::string pluginId = prompt_line(io, "Plugin id", defaultId);
    pluginId = to_lower(trim(pluginId));
    if (pluginId.empty()) {
        pluginId = slugify(name);
    }
    const std::string defaultSlug = slugify(pluginId);
    std::string slug = prompt_line(io, "Folder name", defaultSlug);
    slug = slugify(slug);
    std::string version = prompt_line(io, "Version", "0.1.0");
    if (version.empty()) {
        version = "0.1.0";
    }
    std::string author = prompt_line(io, "Author", default_author());
    std::string description = prompt_line(io, "Short description", "Custom Erelang plugin");

    if (options.userPluginRoot.empty()) {
        io.out << "User plugin directory not available.\n";
        return std::nullopt;
    }
    fs::path targetDir = options.userPluginRoot / slug;
    if (fs::exists(targetDir)) {
        if (!prompt_confirm(io, "Directory already exists. Overwrite?", false)) {
            io.out << "Aborted to avoid overwriting existing plugin.\n";
            return std::nullopt;
        }
    }
    std::error_code ec;
    fs::create_directories(targetDir, ec);
    if (ec) {
        io.out << "Failed to create plugin directory: " << ec.message() << "\n";
        if (io.log) {
            *io.log << "[kit] mkdir failed for " << targetDir.string() << ": " << ec.message() << "\n";
        }
        return std::nullopt;
    }

    std::ostringstream manifest;
    manifest << "<plugin>\n";
    manifest << "  <erelang_manifest>\n";
    manifest << "    id " << pluginId << "\n";
    manifest << "    name " << name << "\n";
    manifest << "    version " << version << "\n";
    manifest << "    author " << author << "\n";
    manifest << "    target runtime\n";
    if (!description.empty()) {
        manifest << "    description " << description << "\n";
    }
    manifest << "  </erelang_manifest>\n";
    manifest << "  <content>\n";
    manifest << "    include init.0bs\n";
    manifest << "  </content>\n";
    manifest << "  <hooks>\n";
    manifest << "    onLoad init::on_load\n";
    manifest << "    onUnload init::on_unload\n";
    manifest << "  </hooks>\n";
    manifest << "</plugin>\n";

    if (!write_text_file(targetDir / "project.elp", manifest.str(), io)) {
        return std::nullopt;
    }

    std::ostringstream script;
    script << "# Erelang plugin bootstrap generated by the Creation Kit\n";
    script << "action on_load {\n";
    script << "    print(\"" << name << " v" << version << " loaded\")\n";
    script << "}\n\n";
    script << "action on_unload {\n";
    script << "    print(\"" << name << " unloading\")\n";
    script << "}\n";

    write_text_file(targetDir / "init.0bs", script.str(), io);

    std::ostringstream readme;
    readme << "# " << name << "\n\n";
    readme << description << "\n";
    readme << "\n## Structure\n\n";
    readme << "- `project.elp` — plugin manifest\n";
    readme << "- `init.0bs` — lifecycle hooks\n";

    write_text_file(targetDir / "README.md", readme.str(), io);

    io.out << "Created plugin at " << targetDir.string() << "\n";
    io.out << "Manifest: " << (targetDir / "project.elp").string() << "\n";
    io.out << "Script: " << (targetDir / "init.0bs").string() << "\n";

    return PluginResult{slug, targetDir};
}

void edit_list_category(KitIo& io, std::string_view label, std::vector<std::string>& values) {
    if (!values.empty()) {
        io.out << label << " (current): " << join_with(values, ", ") << "\n";
    } else {
        io.out << label << " (currently empty)\n";
    }
    std::string additions = prompt_line(io, "Add (comma-separated)");
    if (!additions.empty()) {
        apply_additions(values, split_items(additions));
    }
    std::string removals = prompt_line(io, "Remove (comma-separated)");
    if (!removals.empty()) {
        apply_removals(values, split_items(removals));
    }
}

void run_syntax_wizard(KitIo& io, const CreationKitOptions& options, const std::optional<PluginResult>& lastPlugin) {
    namespace fs = std::filesystem;
    io.out << "\n=== Syntax Profile Builder ===\n";
    auto profile = load_baseline(io, options.runtimeRoot);

    std::vector<std::string> pluginCandidates;
    if (!options.userPluginRoot.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(options.userPluginRoot, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_directory()) {
                pluginCandidates.push_back(entry.path().filename().string());
            }
        }
    }
    if (!pluginCandidates.empty()) {
        std::sort(pluginCandidates.begin(), pluginCandidates.end());
        io.out << "Available plugins: " << join_with(pluginCandidates, ", ") << "\n";
    }
    const std::string defaultSlug = lastPlugin ? lastPlugin->slug : std::string{};
    std::string pluginSlug = prompt_line(io, "Attach to plugin folder (leave empty for global)", defaultSlug);
    std::filesystem::path targetDir = options.userPluginRoot;
    if (!pluginSlug.empty()) {
        pluginSlug = slugify(pluginSlug);
        targetDir /= pluginSlug;
        std::error_code ec;
        fs::create_directories(targetDir, ec);
        if (ec) {
            io.out << "Failed to prepare plugin folder: " << ec.message() << "\n";
            if (io.log) {
                *io.log << "[kit] failed to prepare syntax folder " << targetDir.string() << ": " << ec.message() << "\n";
            }
            return;
        }
    } else {
        std::error_code ec;
        fs::create_directories(targetDir, ec);
    }

    edit_list_category(io, "Keywords", profile.keywords);
    edit_list_category(io, "Directives", profile.directives);
    edit_list_category(io, "Types", profile.types);
    edit_list_category(io, "Operators", profile.operators);
    edit_list_category(io, "Visibility modifiers", profile.visibilityModifiers);
    edit_list_category(io, "String quotes", profile.stringQuotes);
    edit_list_category(io, "Required hooks", profile.requiredHooks);

    std::string comment = prompt_line(io, "Line comment token", profile.commentToken);
    if (!comment.empty()) {
        profile.commentToken = comment;
    }
    std::string attrPrefix = prompt_line(io, "Attribute prefix", profile.attributePrefix);
    if (!attrPrefix.empty()) {
        profile.attributePrefix = attrPrefix;
    }
    std::string entry = prompt_line(io, "Required entry action", profile.requiredEntry);
    if (!entry.empty()) {
        profile.requiredEntry = entry;
    }

    for (auto& [name, token] : profile.brackets) {
        std::string updated = prompt_line(io, std::string{"Bracket token for "}.append(name), token);
        if (!updated.empty()) {
            token = updated;
        }
    }

    const fs::path syntaxPath = targetDir / "syntax.override.json";
    std::ostringstream json;
    json << "{\n";
    json << "  \"keywords\": [";
    for (std::size_t i = 0; i < profile.keywords.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << escape_json(profile.keywords[i]) << "\"";
    }
    json << "],\n";

    json << "  \"directives\": [";
    for (std::size_t i = 0; i < profile.directives.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << escape_json(profile.directives[i]) << "\"";
    }
    json << "],\n";

    json << "  \"types\": [";
    for (std::size_t i = 0; i < profile.types.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << escape_json(profile.types[i]) << "\"";
    }
    json << "],\n";

    json << "  \"operators\": [";
    for (std::size_t i = 0; i < profile.operators.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << escape_json(profile.operators[i]) << "\"";
    }
    json << "],\n";

    json << "  \"visibilityModifiers\": [";
    for (std::size_t i = 0; i < profile.visibilityModifiers.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << escape_json(profile.visibilityModifiers[i]) << "\"";
    }
    json << "],\n";

    json << "  \"stringQuotes\": [";
    for (std::size_t i = 0; i < profile.stringQuotes.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << escape_json(profile.stringQuotes[i]) << "\"";
    }
    json << "],\n";

    json << "  \"brackets\": {";
    std::size_t index = 0;
    for (const auto& [name, token] : profile.brackets) {
        if (index != 0) json << ", ";
        json << "\"" << escape_json(name) << "\": \"" << escape_json(token) << "\"";
        ++index;
    }
    json << "},\n";

    json << "  \"comment\": \"" << escape_json(profile.commentToken) << "\",\n";
    json << "  \"attributePrefix\": \"" << escape_json(profile.attributePrefix) << "\",\n";

    json << "  \"requiredHooks\": [";
    for (std::size_t i = 0; i < profile.requiredHooks.size(); ++i) {
        if (i != 0) json << ", ";
        json << "\"" << escape_json(profile.requiredHooks[i]) << "\"";
    }
    json << "],\n";

    json << "  \"requiredEntry\": \"" << escape_json(profile.requiredEntry) << "\"\n";
    json << "}\n";

    if (write_text_file(syntaxPath, json.str(), io)) {
        io.out << "Saved syntax profile to " << syntaxPath.string() << "\n";
        io.out << "Copy this file into your plugin package or update the VS Code extension if syntax tokens changed.\n";
    }
}

struct PolicyConfig {
    bool defaultDeny = false;
    std::vector<std::string> allowList;
    std::vector<std::string> denyList;
    int maxThreads = 32;
    int maxListItems = 300000;
};

[[nodiscard]] PolicyConfig load_policy(const KitIo& io, const std::filesystem::path& runtimeRoot) {
    PolicyConfig config;
    const auto path = runtimeRoot / "policy.cfg";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (io.log) {
            *io.log << "[kit] using default policy values; failed to read " << path.string() << "\n";
        }
        return config;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));
        if (key == "default_deny") {
            config.defaultDeny = (to_lower(value) == "true");
        } else if (key == "allow") {
            config.allowList = split_items(value);
        } else if (key == "deny") {
            config.denyList = split_items(value);
        } else if (key == "max_threads") {
            try {
                config.maxThreads = std::stoi(value);
            } catch (...) {
                if (io.log) {
                    *io.log << "[kit] ignoring invalid max_threads value: " << value << "\n";
                }
            }
        } else if (key == "max_list_items") {
            try {
                config.maxListItems = std::stoi(value);
            } catch (...) {
                if (io.log) {
                    *io.log << "[kit] ignoring invalid max_list_items value: " << value << "\n";
                }
            }
        }
    }
    return config;
}

int prompt_int(KitIo& io, std::string_view prompt, int current) {
    while (true) {
        std::string value = prompt_line(io, prompt, std::to_string(current));
        if (value.empty()) {
            return current;
        }
        try {
            return std::stoi(value);
        } catch (...) {
            io.out << "Enter a valid integer.\n";
        }
    }
}

void run_policy_wizard(KitIo& io, const CreationKitOptions& options) {
    io.out << "\n=== Runtime Policy Editor ===\n";
    auto config = load_policy(io, options.runtimeRoot);

    config.defaultDeny = prompt_confirm(io, "Deny all built-ins by default?", config.defaultDeny);
    std::string allow = prompt_line(io, "Allow list (comma-separated)", join_with(config.allowList, ","));
    config.allowList = split_items(allow);
    std::string deny = prompt_line(io, "Deny list (comma-separated)", join_with(config.denyList, ","));
    config.denyList = split_items(deny);
    config.maxThreads = prompt_int(io, "Max threads", config.maxThreads);
    config.maxListItems = prompt_int(io, "Max list items", config.maxListItems);

    const auto policyPath = options.userPluginRoot / "policy.override.cfg";
    std::ostringstream content;
    content << "# Generated by the Erelang Creation Kit\n";
    content << "default_deny=" << (config.defaultDeny ? "true" : "false") << "\n";
    if (!config.allowList.empty()) {
        content << "allow=" << join_with(config.allowList, ",") << "\n";
    }
    if (!config.denyList.empty()) {
        content << "deny=" << join_with(config.denyList, ",") << "\n";
    }
    content << "max_threads=" << config.maxThreads << "\n";
    content << "max_list_items=" << config.maxListItems << "\n";

    if (write_text_file(policyPath, content.str(), io)) {
        io.out << "Saved policy override to " << policyPath.string() << "\n";
        io.out << "Copy or reference this file when launching erelang to apply the policy.\n";
    }
}

} // namespace

int run_creation_kit(const CreationKitOptions& options) {
    namespace fs = std::filesystem;
    std::istream& input = options.input ? *options.input : std::cin;
    std::ostream& output = options.output ? *options.output : std::cout;
    std::ostream* log = options.log ? options.log : &std::cerr;
    KitIo io{input, output, log};

    if (options.userPluginRoot.empty()) {
        io.out << "User plugin root is not available.\n";
        if (io.log) {
            *io.log << "[kit] missing user plugin root\n";
        }
        return 1;
    }
    std::error_code ec;
    fs::create_directories(options.userPluginRoot, ec);
    if (ec) {
        io.out << "Failed to access " << options.userPluginRoot.string() << ": " << ec.message() << "\n";
        if (io.log) {
            *io.log << "[kit] create_directories failed for user root: " << ec.message() << "\n";
        }
        return 1;
    }

    io.out << "Erelang Creation Kit\n";
    io.out << "Runtime root: " << options.runtimeRoot.string() << "\n";
    io.out << "User plugins: " << options.userPluginRoot.string() << "\n";

    std::optional<PluginResult> lastPlugin;
    while (true) {
        io.out << "\nChoose an action:\n";
        io.out << "  1) Create a plugin template\n";
        io.out << "  2) Build or edit a syntax profile\n";
        io.out << "  3) Customize runtime policy\n";
        io.out << "  4) Exit\n";
        io.out << "Selection: ";
        io.out.flush();
        std::string line;
        if (!std::getline(io.in, line)) {
            io.out << "\n";
            break;
        }
        const std::string choice = trim(line);
        if (choice == "1") {
            lastPlugin = run_plugin_wizard(io, options);
        } else if (choice == "2") {
            run_syntax_wizard(io, options, lastPlugin);
        } else if (choice == "3") {
            run_policy_wizard(io, options);
        } else if (choice == "4" || choice == "q" || choice == "quit" || choice.empty()) {
            break;
        } else {
            io.out << "Unknown option: " << choice << "\n";
        }
    }

    io.out << "Creation Kit session ended.\n";
    return 0;
}

} // namespace erelang
