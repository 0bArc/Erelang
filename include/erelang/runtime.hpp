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

    // Execute mainProgram with pre-linked module Programs (future: link semantics TBD).
    int run_with_imports(const std::vector<Program>& modules, const Program& mainProgram) const { return run(mainProgram); }

    // Execute a single action by name (no full run pipeline). Returns 0 on success, non-zero on failure.
    int run_single_action(const Program& program, std::string_view actionName) const;

    // GUI event hook (platform-specific). No-op on non-GUI builds.
    void handle_gui_click(int id, void* nativeWinPtr) const;

    // Set CLI arguments (captured once at process start).
    static void set_cli_args(const std::vector<std::string>& args);

    // Access currently executing Program (for builtins launching threads). Valid only during callbacks/runs.
    const Program* currentProgram() const { return currentProgram_; }

private:
    struct ExecContext {
        std::vector<std::thread> threads;   // child threads spawned by actions
        bool returned = false;              // early return flag
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
    mutable std::unordered_map<std::string, std::string> globalVars_; // global variable backing store
    mutable std::unordered_set<std::string> globalNames_;             // quick membership for globals
    mutable std::unordered_map<std::string, ExprPtr> interpolationExprCache_;
    mutable std::mutex interpolationExprCacheMutex_;
    std::vector<PluginRecord> pluginRecords_;

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
    std::string eval_builtin_call(std::string_view name, const std::vector<ExprPtr>& args, const Env& env) const;

    static std::vector<std::string> s_cliArgs;
};

} // namespace erelang
