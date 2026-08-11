// SPDX-License-Identifier: Apache-2.0
//
// Runtime core interface for Erelang / erelang.
// Provides program execution, single-action dispatch, and limited
// accessors used by builtins (e.g. thread subsystem) to reference
// the currently executing Program. Threaded builtins assume the
// Runtime instance outlives any spawned worker threads.
//
// NOTE: Heavy language pipeline headers intentionally avoided here
// via forward declarations to reduce rebuild fan-out.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <thread>
#include <memory> // for std::shared_ptr forward declaration usage
#include <mutex>

#include "runtime_internals.hpp" // expose container handles to certain builtins
#include "erelang/erodsl/spec.hpp"

// Forward declarations (instead of including parser.hpp) to keep header light.
namespace erelang {
struct Program;
struct Block;
struct Action;
struct Hook;
struct Entity;
struct Expr;
using ExprPtr = std::shared_ptr<Expr>; // required by eval_builtin_call signature
} // namespace erelang // close forward-declaration namespace to avoid leaking over includes

// parser.hpp needed for downstream translation units; kept for users that relied
// on the old transitive include. (Consider removing after a transition period.)
#include "erelang/parser.hpp"

namespace erelang { // reopen for Runtime definition

class Runtime {
public:
    // Initialize builtin module aliases in globalVars_
    void initialize_environment(const Program& program);
    Runtime();

    // Execute an entire Program (parses already complete). Returns 0 on success.
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

    // Execute mainProgram with pre-linked module Programs. Module actions,
    // hooks, entities, structs, enums, globals, and type aliases are merged
    // into the main program before execution (main program wins on name clash).
    int run_with_imports(const std::vector<Program>& modules, const Program& mainProgram) const;

    // Execute a single action by name (no full run pipeline). Returns 0 on success, non-zero on failure.
    int run_single_action(const Program& program, std::string_view actionName) const;

    // Set CLI arguments (captured once at process start).
    static void set_cli_args(const std::vector<std::string>& args);

    // Optional cleanup hook installed by the (experimental) thread builtin
    // module. Invoked at the end of run() so spawned worker threads are
    // joined/parked before the Program and Runtime could be destroyed.
    // Worker lambdas capture raw pointers, so they must never outlive run().
    using WorkerParkFn = void (*)();
    static void set_worker_park_hook(WorkerParkFn fn);
    // Invoke the registered park hook (no-op if none registered). Safe to call
    // at any point; the thread module parks/joins all outstanding workers.
    static void park_worker_threads();

    // Access currently executing Program (for builtins launching threads). Valid only during callbacks/runs.
    const Program* currentProgram() const { return currentProgram_; }

    // Dynamically load a .elan file into the runtime action table (JS-style require).
    // Actions are prefixed with "__cmd_<stem>_" to avoid collisions across files.
    // Returns the module stem on success, empty string on failure.
    std::string load_elan_file(const std::filesystem::path& path) const;

    // Recursively load all .elan files under dir. Returns list handle (list:N) of stems.
    std::string load_elan_directory(const std::filesystem::path& dir) const;

    // Invoke a loaded/program action by name with string args; returns its return value.
    std::string call_action_by_name(std::string_view actionName, const std::vector<std::string>& args) const;

private:
    struct ExecContext {
        std::vector<std::thread> threads;   // child threads spawned by actions
        bool returned = false;              // early return flag
        bool breakSignal = false; // break signal
        bool continueSignal = false; // continue signal
        std::string returnValue;            // captured return value
    };

    struct Object {
        std::string typeName;
        std::unordered_map<std::string, std::string> fields;
        void* native = nullptr; // external/native integration hook
    };
    using ObjPtr = std::shared_ptr<Object>;

    struct Env {
        std::unordered_map<std::string, std::string> vars;    // variable storage
        std::unordered_map<std::string, ObjPtr> objects;      // object instances
    };

    // Track currently running Program for callbacks (thread-safe under assumption single active run() at a time).
    mutable const Program* currentProgram_ = nullptr; // lifetime: referenced Program must outlive thread workers.
    mutable std::filesystem::path scriptDirectory_; // entry script parent dir for relative fs paths
    mutable std::unordered_map<std::string, std::string> globalVars_; // global variable backing store
    mutable std::unordered_set<std::string> globalNames_;             // quick membership for globals
    mutable std::unordered_map<std::string, ExprPtr> interpolationExprCache_;
    mutable std::mutex interpolationExprCacheMutex_;
    std::vector<PluginRecord> pluginRecords_;

    // Actions loaded at runtime via load_elan / load_elan_dir (mutable during const run()).
    mutable std::vector<Action> dynamicActions_;
    mutable std::mutex dynamicActionsMutex_;

    // Internal execution helpers
    void exec_block(const Block& b, const Program& program, ExecContext& ctx, Env& env) const;
    void exec_stmt(const Statement& s, const Program& program, ExecContext& ctx, Env& env) const;
    const Action* find_action(const Program& program, std::string_view name) const;
    const Hook* find_hook(const Program& program, std::string_view name) const;
    const Entity* find_entity(const Program& program, std::string_view name) const;
    const Action* find_entity_method(const Entity& e, std::string_view name) const;
    std::string eval_string(const Expr& e, const Env& env) const;
    std::optional<ExprPtr> parse_interpolation_expr(std::string_view exprText) const;
    std::optional<std::string> eval_interpolation_expr(std::string_view exprText, const Env& env) const;
    std::string eval_builtin_call(std::string_view name, const std::vector<ExprPtr>& args, const Env& env, bool allowCollectionHelpers = false) const;

    // Shared plugin seeding/dispatch helpers (used by both run() and run_single_action())
    void seed_plugin_aliases(const Program& program, Env& targetEnv) const;
    void dispatch_plugin_hooks(const Program& program, std::string_view hookName, bool reverseOrder) const;

    static std::vector<std::string> s_cliArgs;
    static WorkerParkFn s_workerParkFn;
};

} // namespace erelang
