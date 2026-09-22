// SPDX-License-Identifier: Apache-2.0
//
// Runtime core interface for Erelang / erelang.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <thread>
#include <memory>
#include <mutex>

#include "runtime_internals.hpp"
#include "erelang/erodsl/spec.hpp"
#include "erelang/value.hpp"

namespace erelang {
struct Program;
struct Block;
struct Action;
struct Hook;
struct Entity;
struct Expr;
struct Chunk;
using ExprPtr = std::shared_ptr<Expr>;
} // namespace erelang

#include "erelang/parser.hpp"

namespace erelang {

class Runtime {
public:
    void initialize_environment(const Program& program);
    Runtime();

    int run(const Program& program) const;

    struct PluginRecord {
        std::string id;
        std::string slug;
        std::string name;
        std::string version;
        std::string author;
        std::string target;
        std::string description;
        std::vector<std::string> dependencies;
        std::filesystem::path baseDirectory;
        std::filesystem::path manifestPath;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> coreProperties;
        std::unordered_map<std::string, std::vector<std::string>> hookBindings;
        std::optional<erodsl::DslSpec> dslSpec;
        std::string onLoad;
        std::string onUnload;
        std::string dataHook;
    };

    void register_plugins(std::vector<PluginRecord> plugins);
    const std::vector<PluginRecord>& plugin_records() const { return pluginRecords_; }

    int run_with_imports(const std::vector<Program>& modules, const Program& mainProgram) const;
    int run_single_action(const Program& program, std::string_view actionName) const;

    static void set_cli_args(const std::vector<std::string>& args);

    using WorkerParkFn = void (*)();
    static void set_worker_park_hook(WorkerParkFn fn);
    static void park_worker_threads();

    const Program* currentProgram() const { return currentProgram_; }

    std::string load_elan_file(const std::filesystem::path& path) const;
    std::string load_elan_directory(const std::filesystem::path& dir) const;
    std::string call_action_by_name(std::string_view actionName, const std::vector<std::string>& args) const;
    std::string call_action_by_name(std::string_view actionName, const std::vector<std::string>& args,
                                    const std::unordered_map<std::string, std::string>& inject) const;

    // Env is public to builtins that seed aliases into maps.
    struct Object {
        std::string typeName;
        std::unordered_map<std::string, std::string> fields;
        void* native = nullptr;
    };
    using ObjPtr = std::shared_ptr<Object>;

    struct Env {
        ValueMap vars;
        std::unordered_map<std::string, ObjPtr> objects;
        std::vector<Value> slots;
        std::unordered_map<std::string, int> slotIndex;
        std::vector<std::string> slotNames;
        bool useSlots{false};
    };

private:
    struct ExecContext {
        std::vector<std::thread> threads;
        bool returned = false;
        bool breakSignal = false;
        bool continueSignal = false;
        Value returnValue;
    };

    mutable const Program* currentProgram_ = nullptr;
    mutable std::filesystem::path scriptDirectory_;
    mutable ValueMap globalVars_;
    mutable std::unordered_set<std::string> globalNames_;
    mutable std::unordered_map<std::string, ExprPtr> interpolationExprCache_;
    mutable std::mutex interpolationExprCacheMutex_;
    std::vector<PluginRecord> pluginRecords_;

    mutable std::vector<Action> dynamicActions_;
    mutable std::mutex dynamicActionsMutex_;

    void exec_block(const Block& b, const Program& program, ExecContext& ctx, Env& env) const;
    void exec_stmt(const Statement& s, const Program& program, ExecContext& ctx, Env& env) const;
    const Action* find_action(const Program& program, std::string_view name) const;
    const Hook* find_hook(const Program& program, std::string_view name) const;
    const Entity* find_entity(const Program& program, std::string_view name) const;
    const Action* find_entity_method(const Entity& e, std::string_view name) const;

    Value eval_value(const Expr& e, const Env& env) const;
    std::string eval_string(const Expr& e, const Env& env) const;

    Value env_get(const Env& env, const std::string& name) const;
    void env_set(Env& env, const std::string& name, Value value) const;
    void prepare_action_slots(Env& env, const Action& action) const;

    friend Value run_chunk(const Chunk& chunk, Runtime& rt, Env& env);

    std::optional<ExprPtr> parse_interpolation_expr(std::string_view exprText) const;
    std::optional<std::string> eval_interpolation_expr(std::string_view exprText, const Env& env) const;
    std::string eval_builtin_call(std::string_view name, const std::vector<ExprPtr>& args, const Env& env, bool allowCollectionHelpers = false) const;

    void seed_plugin_aliases(const Program& program, Env& targetEnv) const;
    void dispatch_plugin_hooks(const Program& program, std::string_view hookName, bool reverseOrder) const;

    static std::vector<std::string> s_cliArgs;
    static WorkerParkFn s_workerParkFn;
};

} // namespace erelang
