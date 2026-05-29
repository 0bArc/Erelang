// SPDX-License-Identifier: Apache-2.0
// Split from runtime.cpp

#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/runtime_builtins.hpp"
#include "erelang/runtime_imports.hpp"

#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"
#include "erelang/typechecker.hpp"
#include "erelang/optimizer.hpp"
#include "erelang/symboltable.hpp"
#include "erelang/modules.hpp"
#include "erelang/version.hpp"
#include "erelang/policy.hpp"
#include "erelang/features/serialization.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace erelang {
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string lowercase_ascii_copy(std::string_view value) {
    std::string result{value};
    for (char& ch : result) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return result;
}

[[nodiscard]] const std::vector<std::string>& hook_actions_for(const Runtime::PluginRecord& plugin, std::string_view hookName) {
    static const std::vector<std::string> kEmpty;
    const std::string key = lowercase_ascii_copy(hookName);
    auto it = plugin.hookBindings.find(key);
    if (it != plugin.hookBindings.end()) {
        return it->second;
    }
    return kEmpty;
}

void join_threads(std::vector<std::thread>& threads) {
    for (auto& th : threads) {
        if (th.joinable()) {
            th.join();
        }
    }
}

std::string join_preserving_order(const std::vector<std::string>& values, char separator = ',') {
    if (values.empty()) {
        return {};
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            oss << separator;
        }
        oss << values[i];
    }
    return oss.str();
}

} // namespace
Runtime::Runtime() {}

void Runtime::register_plugins(std::vector<PluginRecord> plugins) {
    pluginRecords_ = std::move(plugins);
}

void Runtime::initialize_environment(const Program& program) {
    Env rootEnv; // Declare rootEnv
    std::cerr << "[debug] Initializing environment and binding aliases...\n";
    bind_builtin_module_aliases(program, rootEnv.vars);
    std::cerr << "[debug] Aliases after binding: \n";
    for (const auto& [key, value] : rootEnv.vars) {
        std::cerr << key << " -> " << value << "\n";
    }
}

std::vector<std::string> Runtime::s_cliArgs;
void Runtime::set_cli_args(const std::vector<std::string>& args) { s_cliArgs = args; }

int Runtime::run(const Program& program) const {
    currentProgram_ = &program;
    std::string entry = program.runTarget.value_or("main");
    const Action* a = find_action(program, entry);
    if (!a) throw std::runtime_error("Action not found: " + entry);
    if (program.strict && a->visibility != Visibility::Public) {
        throw std::runtime_error("Entry action not public: " + a->name);
    }
    ExecContext ctx;
    Env rootEnv;
    // Seed program-level globals into shared store and initial root env
    globalNames_.clear();
    for (const auto& g : program.globals) globalNames_.insert(g.name);
    for (const auto& g : program.globals) {
        if (g.value) globalVars_[g.name] = eval_string(*g.value, rootEnv);
        rootEnv.vars[g.name] = globalVars_[g.name];
    }
    // Seed file-level directives into env (simple string values); apply special ones
    for (const auto& d : program.directives) {
        if (d.value) rootEnv.vars[d.name] = *d.value; else rootEnv.vars[d.name] = "true";
    }
    auto seedPluginAliases = [&](Env& targetEnv) {
        if (program.pluginAliases.empty() || pluginRecords_.empty()) return;
        auto dedupe_sort = [](std::vector<std::string>& items) {
            std::sort(items.begin(), items.end());
            items.erase(std::unique(items.begin(), items.end()), items.end());
        };
        for (const auto& alias : program.pluginAliases) {
            std::vector<std::string> slugs;
            slugs.reserve(pluginRecords_.size());
            std::vector<std::string> allCoreFiles;
            std::unordered_map<std::string, std::vector<std::string>> coreKeysByFile;
            for (const auto& plugin : pluginRecords_) {
                slugs.push_back(plugin.slug);
                std::string key = alias + ":" + plugin.slug;
                auto obj = std::make_shared<Object>();
                obj->typeName = "Plugin";
                obj->fields["id"] = plugin.id;
                obj->fields["slug"] = plugin.slug;
                obj->fields["name"] = plugin.name;
                obj->fields["version"] = plugin.version;
                obj->fields["author"] = plugin.author;
                obj->fields["target"] = plugin.target;
                obj->fields["description"] = plugin.description;
                obj->fields["dependencies"] = join_strings(plugin.dependencies);
                obj->fields["base_directory"] = plugin.baseDirectory.string();
                obj->fields["manifest_path"] = plugin.manifestPath.string();
                obj->fields["on_load"] = plugin.onLoad;
                obj->fields["on_unload"] = plugin.onUnload;
                obj->fields["data_hook"] = plugin.dataHook;
                if (!plugin.hookBindings.empty()) {
                    std::vector<std::string> hookNames;
                    hookNames.reserve(plugin.hookBindings.size());
                    for (const auto& [hookName, actions] : plugin.hookBindings) {
                        hookNames.push_back(hookName);
                        obj->fields["hook." + hookName] = join_preserving_order(actions);
                        obj->fields["hook." + hookName + ".count"] = std::to_string(actions.size());
                    }
                    std::sort(hookNames.begin(), hookNames.end());
                    obj->fields["hooks"] = join_preserving_order(hookNames);
                    obj->fields["hooks.count"] = std::to_string(hookNames.size());
                }
                if (plugin.dslSpec) {
                    const auto& lang = *plugin.dslSpec;
                    obj->fields["language.id"] = lang.id;
                    obj->fields["language.name"] = lang.name;
                    obj->fields["language.version"] = lang.version;
                    if (!lang.extensions.empty()) {
                        obj->fields["language.extensions"] = join_preserving_order(lang.extensions);
                        obj->fields["language.extensions.count"] = std::to_string(lang.extensions.size());
                    }
                    if (!lang.keywordAliases.empty()) {
                        std::vector<std::string> aliasKeys;
                        aliasKeys.reserve(lang.keywordAliases.size());
                        for (const auto& [alias, canonical] : lang.keywordAliases) {
                            aliasKeys.push_back(alias);
                            obj->fields["language.alias." + alias] = canonical;
                        }
                        std::sort(aliasKeys.begin(), aliasKeys.end());
                        obj->fields["language.aliases"] = join_preserving_order(aliasKeys);
                        obj->fields["language.aliases.count"] = std::to_string(aliasKeys.size());
                    }
                }
                for (const auto& [fileName, entries] : plugin.coreProperties) {
                    allCoreFiles.push_back(fileName);
                    auto& bucket = coreKeysByFile[fileName];
                    bucket.reserve(bucket.size() + entries.size());
                    for (const auto& [entryKey, entryValue] : entries) {
                        bucket.push_back(entryKey);
                        obj->fields["core:" + fileName + ":" + entryKey] = entryValue;
                    }
                }
                targetEnv.objects[key] = std::move(obj);
            }
            targetEnv.vars[alias + ".slugs"] = join_strings(slugs);
            targetEnv.vars[alias + ".count"] = std::to_string(slugs.size());
            if (!allCoreFiles.empty()) {
                dedupe_sort(allCoreFiles);
                targetEnv.vars[alias + ".core.files"] = join_strings(allCoreFiles);
                for (auto& kv : coreKeysByFile) {
                    dedupe_sort(kv.second);
                    targetEnv.vars[alias + ".core." + kv.first + ".keys"] = join_strings(kv.second);
                }
            }
        }
    };
    seedPluginAliases(rootEnv);
    bind_builtin_module_aliases(program, rootEnv.vars);
    auto dispatchPluginHooks = [&](std::string_view hookName, bool reverseOrder) {
        if (pluginRecords_.empty()) {
            return;
        }
        const std::string canonical = lowercase_ascii_copy(hookName);
        auto invoke = [&](const PluginRecord& plugin) {
            const auto& actions = hook_actions_for(plugin, canonical);
            if (actions.empty()) {
                return;
            }
            Env envCopy;
            for (const auto& kv : globalVars_) {
                envCopy.vars[kv.first] = kv.second;
            }
            seedPluginAliases(envCopy);
            bind_builtin_module_aliases(program, envCopy.vars);
            for (const auto& actionName : actions) {
                if (actionName.empty()) {
                    continue;
                }
                if (const Action* action = find_action(program, actionName)) {
                    ExecContext hookCtx;
                    exec_block(action->body, program, hookCtx, envCopy);
                    join_threads(hookCtx.threads);
                } else {
                    std::cerr << "[plugins] hook '" << canonical << "' action '" << actionName << "' not found for plugin '" << plugin.id << "'\n";
                }
            }
            for (auto& kv : envCopy.vars) {
                if (globalNames_.count(kv.first)) {
                    globalVars_[kv.first] = kv.second;
                }
            }
        };
        if (reverseOrder) {
            for (auto it = pluginRecords_.rbegin(); it != pluginRecords_.rend(); ++it) {
                invoke(*it);
            }
        } else {
            for (const auto& plugin : pluginRecords_) {
                invoke(plugin);
            }
        }
    };

    dispatchPluginHooks("datahook", false);
    dispatchPluginHooks("onload", false);
    dispatchPluginHooks("onstart", false);

    if (const Hook* s = find_hook(program, "onStart")) {
        exec_block(s->body, program, ctx, rootEnv);
    }

    exec_block(a->body, program, ctx, rootEnv);
    join_threads(ctx.threads);

    // Support both onEnd (preferred) and onExit (alias). Use the same rootEnv so variables persist.
    if (const Hook* e = find_hook(program, "onEnd"); e || (e = find_hook(program, "onExit"))) {
        ExecContext after;
        exec_block(e->body, program, after, rootEnv);
        join_threads(after.threads);
    }

    dispatchPluginHooks("onend", true);
    dispatchPluginHooks("onexit", true);
    dispatchPluginHooks("onunload", true);

    currentProgram_ = nullptr;
    return 0;
}

int Runtime::run_single_action(const Program& program, std::string_view actionName) const {
    currentProgram_ = &program;
    const Action* a = find_action(program, actionName);
    if (!a) { currentProgram_ = nullptr; return 1; }
    if (program.strict && a->visibility != Visibility::Public) { currentProgram_ = nullptr; return 2; }
    ExecContext ctx; Env env;
    auto seedPluginAliases = [&](Env& targetEnv) {
        if (program.pluginAliases.empty() || pluginRecords_.empty()) return;
        auto dedupe_sort = [](std::vector<std::string>& items) {
            std::sort(items.begin(), items.end());
            items.erase(std::unique(items.begin(), items.end()), items.end());
        };
        for (const auto& alias : program.pluginAliases) {
            std::vector<std::string> slugs;
            slugs.reserve(pluginRecords_.size());
            std::vector<std::string> allCoreFiles;
            std::unordered_map<std::string, std::vector<std::string>> coreKeysByFile;
            for (const auto& plugin : pluginRecords_) {
                slugs.push_back(plugin.slug);
                std::string key = alias + ":" + plugin.slug;
                auto obj = std::make_shared<Object>();
                obj->typeName = "Plugin";
                obj->fields["id"] = plugin.id;
                obj->fields["slug"] = plugin.slug;
                obj->fields["name"] = plugin.name;
                obj->fields["version"] = plugin.version;
                obj->fields["author"] = plugin.author;
                obj->fields["target"] = plugin.target;
                obj->fields["description"] = plugin.description;
                obj->fields["dependencies"] = join_strings(plugin.dependencies);
                obj->fields["base_directory"] = plugin.baseDirectory.string();
                obj->fields["manifest_path"] = plugin.manifestPath.string();
                obj->fields["on_load"] = plugin.onLoad;
                obj->fields["on_unload"] = plugin.onUnload;
                obj->fields["data_hook"] = plugin.dataHook;
                if (!plugin.hookBindings.empty()) {
                    std::vector<std::string> hookNames;
                    hookNames.reserve(plugin.hookBindings.size());
                    for (const auto& [hookName, actions] : plugin.hookBindings) {
                        hookNames.push_back(hookName);
                        obj->fields["hook." + hookName] = join_preserving_order(actions);
                        obj->fields["hook." + hookName + ".count"] = std::to_string(actions.size());
                    }
                    std::sort(hookNames.begin(), hookNames.end());
                    obj->fields["hooks"] = join_preserving_order(hookNames);
                    obj->fields["hooks.count"] = std::to_string(hookNames.size());
                }
                if (plugin.dslSpec) {
                    const auto& lang = *plugin.dslSpec;
                    obj->fields["language.id"] = lang.id;
                    obj->fields["language.name"] = lang.name;
                    obj->fields["language.version"] = lang.version;
                    if (!lang.extensions.empty()) {
                        obj->fields["language.extensions"] = join_preserving_order(lang.extensions);
                        obj->fields["language.extensions.count"] = std::to_string(lang.extensions.size());
                    }
                    if (!lang.keywordAliases.empty()) {
                        std::vector<std::string> aliasKeys;
                        aliasKeys.reserve(lang.keywordAliases.size());
                        for (const auto& [alias, canonical] : lang.keywordAliases) {
                            aliasKeys.push_back(alias);
                            obj->fields["language.alias." + alias] = canonical;
                        }
                        std::sort(aliasKeys.begin(), aliasKeys.end());
                        obj->fields["language.aliases"] = join_preserving_order(aliasKeys);
                        obj->fields["language.aliases.count"] = std::to_string(aliasKeys.size());
                    }
                }
                for (const auto& [fileName, entries] : plugin.coreProperties) {
                    allCoreFiles.push_back(fileName);
                    auto& bucket = coreKeysByFile[fileName];
                    bucket.reserve(bucket.size() + entries.size());
                    for (const auto& [entryKey, entryValue] : entries) {
                        bucket.push_back(entryKey);
                        obj->fields["core:" + fileName + ":" + entryKey] = entryValue;
                    }
                }
                targetEnv.objects[key] = std::move(obj);
            }
            targetEnv.vars[alias + ".slugs"] = join_strings(slugs);
            targetEnv.vars[alias + ".count"] = std::to_string(slugs.size());
            if (!allCoreFiles.empty()) {
                dedupe_sort(allCoreFiles);
                targetEnv.vars[alias + ".core.files"] = join_strings(allCoreFiles);
                for (auto& kv : coreKeysByFile) {
                    dedupe_sort(kv.second);
                    targetEnv.vars[alias + ".core." + kv.first + ".keys"] = join_strings(kv.second);
                }
            }
        }
    };
    seedPluginAliases(env);
    bind_builtin_module_aliases(program, env.vars);
    auto dispatchPluginHooks = [&](std::string_view hookName, bool reverseOrder) {
        if (pluginRecords_.empty()) {
            return;
        }
        const std::string canonical = lowercase_ascii_copy(hookName);
        auto invoke = [&](const PluginRecord& plugin) {
            const auto& actions = hook_actions_for(plugin, canonical);
            if (actions.empty()) {
                return;
            }
            Env envCopy;
            for (const auto& kv : globalVars_) {
                envCopy.vars[kv.first] = kv.second;
            }
            seedPluginAliases(envCopy);
            bind_builtin_module_aliases(program, envCopy.vars);
            for (const auto& actionName : actions) {
                if (actionName.empty()) {
                    continue;
                }
                if (const Action* action = find_action(program, actionName)) {
                    ExecContext hookCtx;
                    exec_block(action->body, program, hookCtx, envCopy);
                    join_threads(hookCtx.threads);
                } else {
                    std::cerr << "[plugins] hook '" << canonical << "' action '" << actionName << "' not found for plugin '" << plugin.id << "'\n";
                }
            }
            for (auto& kv : envCopy.vars) {
                if (globalNames_.count(kv.first)) {
                    globalVars_[kv.first] = kv.second;
                }
            }
        };
        if (reverseOrder) {
            for (auto it = pluginRecords_.rbegin(); it != pluginRecords_.rend(); ++it) {
                invoke(*it);
            }
        } else {
            for (const auto& plugin : pluginRecords_) {
                invoke(plugin);
            }
        }
    };
    // Lazy one-time global initializer evaluation (if we haven't populated names or vars yet)
    if (globalNames_.empty()) {
        for (const auto& g : program.globals) globalNames_.insert(g.name);
        for (const auto& g : program.globals) {
            if (g.value) {
                try { globalVars_[g.name] = eval_string(*g.value, env); }
                catch (...) { globalVars_[g.name] = ""; }
            } else {
                globalVars_[g.name] = "";
            }
        }
        dispatchPluginHooks("datahook", false);
        dispatchPluginHooks("onload", false);
        dispatchPluginHooks("onstart", false);
        // Run onStart hook once if present
        if (const Hook* s = find_hook(program, "onStart")) {
            ExecContext sc; Env se; for (const auto& kv : globalVars_) se.vars[kv.first] = kv.second; exec_block(s->body, program, sc, se); for (auto& th : sc.threads) if (th.joinable()) th.join();
            // propagate any mutated globals back
            for (auto& kv : se.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
        }
    }
    for (const auto& kv : globalVars_) env.vars[kv.first] = kv.second;
    seedPluginAliases(env);
    bind_builtin_module_aliases(program, env.vars);
    exec_block(a->body, program, ctx, env);
    for (auto& th : ctx.threads) if (th.joinable()) th.join();
    // Persist mutated globals
    for (auto& kv : env.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
    currentProgram_ = nullptr;
    return 0;
}


} // namespace erelang
