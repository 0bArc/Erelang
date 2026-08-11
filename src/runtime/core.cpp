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

int Runtime::run_with_imports(const std::vector<Program>& modules, const Program& mainProgram) const {
    // Merge module declarations into a combined program so actions/hooks from
    // imported modules are visible during execution. The main program wins on
    // name clashes, matching "local shadows imported" semantics.
    Program combined = mainProgram;
    for (const auto& mod : modules) {
        for (const auto& a : mod.actions) {
            if (!std::any_of(combined.actions.begin(), combined.actions.end(),
                             [&](const Action& existing) { return existing.name == a.name; })) {
                combined.actions.push_back(a);
            }
        }
        for (const auto& h : mod.hooks) {
            if (!std::any_of(combined.hooks.begin(), combined.hooks.end(),
                             [&](const Hook& existing) { return existing.name == h.name; })) {
                combined.hooks.push_back(h);
            }
        }
        for (const auto& e : mod.entities) {
            if (!std::any_of(combined.entities.begin(), combined.entities.end(),
                             [&](const Entity& existing) { return existing.name == e.name; })) {
                combined.entities.push_back(e);
            }
        }
        for (const auto& s : mod.structs) {
            if (!std::any_of(combined.structs.begin(), combined.structs.end(),
                             [&](const StructDecl& existing) { return existing.name == s.name; })) {
                combined.structs.push_back(s);
            }
        }
        for (const auto& en : mod.enums) {
            if (!std::any_of(combined.enums.begin(), combined.enums.end(),
                             [&](const EnumDecl& existing) { return existing.name == en.name; })) {
                combined.enums.push_back(en);
            }
        }
        for (const auto& t : mod.typeAliases) {
            if (!std::any_of(combined.typeAliases.begin(), combined.typeAliases.end(),
                             [&](const TypeAliasDecl& existing) { return existing.name == t.name; })) {
                combined.typeAliases.push_back(t);
            }
        }
        for (const auto& g : mod.globals) {
            if (!std::any_of(combined.globals.begin(), combined.globals.end(),
                             [&](const GlobalDecl& existing) { return existing.name == g.name; })) {
                combined.globals.push_back(g);
            }
        }
        for (const auto& d : mod.directives) {
            if (!std::any_of(combined.directives.begin(), combined.directives.end(),
                             [&](const Attribute& existing) { return existing.name == d.name; })) {
                combined.directives.push_back(d);
            }
        }
    }
    return run(combined);
}

void Runtime::initialize_environment(const Program& program) {
    // Historical debug stub; environment seeding now happens inside run() and
    // run_single_action() where a real Env exists. Kept as a no-op to preserve
    // the public ABI.
    (void)program;
}

std::vector<std::string> Runtime::s_cliArgs;
Runtime::WorkerParkFn Runtime::s_workerParkFn = nullptr;
void Runtime::set_cli_args(const std::vector<std::string>& args) { s_cliArgs = args; }
void Runtime::set_worker_park_hook(Runtime::WorkerParkFn fn) { s_workerParkFn = fn; }
void Runtime::park_worker_threads() { if (s_workerParkFn) s_workerParkFn(); }

int Runtime::run(const Program& program) const {
    // Reset per-run global container state so a second run() in the same
    // process starts with fresh list/dict/ptr/file handle IDs.
    reset_global_container_state();
    currentProgram_ = &program;
    scriptDirectory_ = infer_entry_script_directory(program);
    // Park thread-builtin workers on every exit path (including exceptions) so
    // they can never outlive this Program/Runtime.
    struct ParkGuard { ~ParkGuard() { Runtime::park_worker_threads(); } } parkGuard;
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
    for (const auto& en : program.enums) {
        for (const auto& member : en.members) {
            const std::string scopedColon = en.name + "::" + member;
            const std::string scopedDot = en.name + "." + member;
            rootEnv.vars[scopedColon] = member;
            rootEnv.vars[scopedDot] = member;
            globalVars_[scopedColon] = member;
            globalVars_[scopedDot] = member;
        }
    }
    // Seed file-level directives into env (simple string values); apply special ones
    for (const auto& d : program.directives) {
        if (d.value) rootEnv.vars[d.name] = *d.value; else rootEnv.vars[d.name] = "true";
    }
    seed_plugin_aliases(program, rootEnv);
    bind_builtin_module_aliases(program, globalVars_);
    bind_builtin_module_aliases(program, rootEnv.vars);
    dispatch_plugin_hooks(program, "datahook", false);
    dispatch_plugin_hooks(program, "onload", false);
    dispatch_plugin_hooks(program, "onstart", false);

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

    dispatch_plugin_hooks(program, "onend", true);
    dispatch_plugin_hooks(program, "onexit", true);
    dispatch_plugin_hooks(program, "onunload", true);

    currentProgram_ = nullptr;
    return 0;
}

void Runtime::seed_plugin_aliases(const Program& program, Env& targetEnv) const {
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
}

void Runtime::dispatch_plugin_hooks(const Program& program, std::string_view hookName, bool reverseOrder) const {
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
        seed_plugin_aliases(program, envCopy);
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
}

int Runtime::run_single_action(const Program& program, std::string_view actionName) const {
    currentProgram_ = &program;
    scriptDirectory_ = infer_entry_script_directory(program);
    const Action* a = find_action(program, actionName);
    if (!a) { currentProgram_ = nullptr; return 1; }
    if (program.strict && a->visibility != Visibility::Public) { currentProgram_ = nullptr; return 2; }
    ExecContext ctx; Env env;
    seed_plugin_aliases(program, env);
    bind_builtin_module_aliases(program, env.vars);
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
        for (const auto& en : program.enums) {
            for (const auto& member : en.members) {
                const std::string scopedColon = en.name + "::" + member;
                const std::string scopedDot = en.name + "." + member;
                globalVars_[scopedColon] = member;
                globalVars_[scopedDot] = member;
            }
        }
        dispatch_plugin_hooks(program, "datahook", false);
        dispatch_plugin_hooks(program, "onload", false);
        dispatch_plugin_hooks(program, "onstart", false);
        // Run onStart hook once if present
        if (const Hook* s = find_hook(program, "onStart")) {
            ExecContext sc; Env se; for (const auto& kv : globalVars_) se.vars[kv.first] = kv.second; exec_block(s->body, program, sc, se); for (auto& th : sc.threads) if (th.joinable()) th.join();
            // propagate any mutated globals back
            for (auto& kv : se.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
        }
    }
    bind_builtin_module_aliases(program, globalVars_);
    for (const auto& kv : globalVars_) env.vars[kv.first] = kv.second;
    seed_plugin_aliases(program, env);
    bind_builtin_module_aliases(program, env.vars);
    exec_block(a->body, program, ctx, env);
    for (auto& th : ctx.threads) if (th.joinable()) th.join();
    // Persist mutated globals
    for (auto& kv : env.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
    currentProgram_ = nullptr;
    return 0;
}

std::string Runtime::load_elan_file(const std::filesystem::path& path) const {
    std::error_code ec;
    auto resolved = path;
    if (!resolved.is_absolute() && !scriptDirectory_.empty()) {
        resolved = scriptDirectory_ / path;
    }
    resolved = std::filesystem::weakly_canonical(resolved, ec);
    if (ec || !std::filesystem::is_regular_file(resolved, ec)) {
        fprintf(stderr, "[load_elan] not a file: %s\n", path.string().c_str());
        return {};
    }
    if (resolved.extension() != ".elan" && resolved.extension() != ".ere") {
        fprintf(stderr, "[load_elan] not an elan file: %s\n", resolved.string().c_str());
        return {};
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
        fprintf(stderr, "[load_elan] cannot open: %s\n", resolved.string().c_str());
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    LexerOptions lxopts;
    lxopts.enableDurations = true;
    lxopts.enableUnits = true;
    lxopts.enablePolyIdentifiers = true;
    lxopts.emitDocComments = true;
    lxopts.emitComments = false;
    Lexer lexer(source, lxopts);
    auto tokens = lexer.lex();
    Parser parser(std::move(tokens), resolved.string());
    Program mod;
    try {
        mod = parser.parse();
    } catch (const std::exception& ex) {
        fprintf(stderr, "[load_elan] parse error in %s: %s\n", resolved.string().c_str(), ex.what());
        return {};
    }

    const std::string stem = resolved.stem().string();
    const std::string prefix = std::string("__cmd_") + stem + "_";

    // Merge globals from the module into the live runtime globals.
    Env seedEnv;
    for (const auto& kv : globalVars_) seedEnv.vars[kv.first] = kv.second;
    if (currentProgram_) {
        bind_builtin_module_aliases(*currentProgram_, seedEnv.vars);
        bind_builtin_module_aliases(mod, seedEnv.vars);
    } else {
        bind_builtin_module_aliases(mod, seedEnv.vars);
    }

    for (const auto& g : mod.globals) {
        globalNames_.insert(g.name);
        if (g.value) {
            try { globalVars_[g.name] = eval_string(*g.value, seedEnv); }
            catch (...) { globalVars_[g.name] = ""; }
        } else if (!globalVars_.count(g.name)) {
            globalVars_[g.name] = "";
        }
    }

    {
        std::lock_guard<std::mutex> lock(dynamicActionsMutex_);
        // Drop previously loaded actions for this stem (reload support)
        dynamicActions_.erase(
            std::remove_if(dynamicActions_.begin(), dynamicActions_.end(),
                [&](const Action& a) {
                    return a.name.rfind(prefix, 0) == 0;
                }),
            dynamicActions_.end());

        for (Action a : mod.actions) {
            // Keep already-prefixed names; otherwise namespace by file stem
            if (a.name.rfind("__cmd_", 0) != 0) {
                a.name = prefix + a.name;
            }
            dynamicActions_.push_back(std::move(a));
        }
    }

    return stem;
}

std::string Runtime::load_elan_directory(const std::filesystem::path& dir) const {
    std::error_code ec;
    auto resolved = dir;
    if (!resolved.is_absolute() && !scriptDirectory_.empty()) {
        resolved = scriptDirectory_ / dir;
    }
    if (!std::filesystem::is_directory(resolved, ec)) {
        fprintf(stderr, "[load_elan_dir] not a directory: %s\n", dir.string().c_str());
        const int emptyId = g_nextListId++;
        g_lists[emptyId] = {};
        return std::string("list:") + std::to_string(emptyId);
    }

    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(resolved, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension().string();
        if (ext == ".elan" || ext == ".ere") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());

    const int id = g_nextListId++;
    g_lists[id] = {};
    for (const auto& file : files) {
        std::string stem = load_elan_file(file);
        if (!stem.empty()) {
            g_lists[id].push_back(stem);
        }
    }
    return std::string("list:") + std::to_string(id);
}

std::string Runtime::call_action_by_name(std::string_view actionName, const std::vector<std::string>& args) const {
    if (!currentProgram_) return {};
    const Action* a = find_action(*currentProgram_, actionName);
    if (!a) {
        fprintf(stderr, "[call_action] unknown action: %s\n", std::string(actionName).c_str());
        return {};
    }
    Env calleeEnv;
    for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
    for (size_t i = 0; i < a->params.size() && i < args.size(); ++i) {
        calleeEnv.vars[a->params[i].name] = args[i];
    }
    ExecContext calleeCtx;
    exec_block(a->body, *currentProgram_, calleeCtx, calleeEnv);
    for (auto& th : calleeCtx.threads) {
        if (th.joinable()) th.join();
    }
    for (auto& kv : calleeEnv.vars) {
        if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
    }
    if (!calleeCtx.returned) return {};
    return calleeCtx.returnValue;
}


} // namespace erelang
