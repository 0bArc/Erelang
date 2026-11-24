// SPDX-License-Identifier: Apache-2.0
// Split from runtime.cpp

#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/runtime_builtins.hpp"
#include "erelang/module_registry.hpp"
#include "erelang/runtime_imports.hpp"
#include "erelang/bytecode.hpp"
#include "erelang/parser.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace erelang {

void Runtime::index_actions(const Program& program) const {
    actionIndex_.clear();
    actionSuffixIndex_.clear();
    actionIndex_.reserve(program.actions.size());
    for (const auto& a : program.actions) {
        actionIndex_.emplace(a.name, &a);
        const auto sep = a.name.rfind("::");
        if (sep == std::string::npos || sep + 2 >= a.name.size()) continue;
        const std::string_view last = std::string_view(a.name).substr(sep + 2);
        auto [it, inserted] = actionSuffixIndex_.emplace(last, &a);
        if (!inserted) it->second = nullptr; // ambiguous unqualified name
    }
    actionIndexProgram_ = &program;
    actionIndexCount_ = program.actions.size();
}

const Action* Runtime::find_action(const Program& program, std::string_view name) const {
    if (&program == actionIndexProgram_ && program.actions.size() == actionIndexCount_) {
        if (auto it = actionIndex_.find(name); it != actionIndex_.end()) return it->second;
        const bool hasDynamic = hasDynamicActions_.load(std::memory_order_acquire);
        if (hasDynamic) {
            std::lock_guard<std::mutex> lock(dynamicActionsMutex_);
            for (const auto& a : dynamicActions_) if (a.name == name) return &a;
        }
        if (name.find("::") != std::string_view::npos) return nullptr;
        if (auto it = actionSuffixIndex_.find(name); it != actionSuffixIndex_.end()) return it->second;
        if (!hasDynamic) return nullptr;
        const Action* found = nullptr;
        std::string suffix = std::string("::") + std::string(name);
        std::lock_guard<std::mutex> lock(dynamicActionsMutex_);
        for (const auto& a : dynamicActions_) {
            if (a.name.size() > suffix.size() && a.name.rfind(suffix) == a.name.size() - suffix.size()) {
                if (found) return nullptr;
                found = &a;
            }
        }
        return found;
    }
    for (const auto& a : program.actions) if (a.name == name) return &a;
    {
        std::lock_guard<std::mutex> lock(dynamicActionsMutex_);
        for (const auto& a : dynamicActions_) if (a.name == name) return &a;
    }
    if (name.find("::") == std::string_view::npos) {
        const Action* found = nullptr;
        std::string suffix = std::string("::") + std::string(name);
        for (const auto& a : program.actions) {
            if (a.name.size() > suffix.size() && a.name.rfind(suffix) == a.name.size() - suffix.size()) {
                if (found) return nullptr;
                found = &a;
            }
        }
        if (!found) {
            std::lock_guard<std::mutex> lock(dynamicActionsMutex_);
            for (const auto& a : dynamicActions_) {
                if (a.name.size() > suffix.size() && a.name.rfind(suffix) == a.name.size() - suffix.size()) {
                    if (found) return nullptr;
                    found = &a;
                }
            }
        }
        if (found) return found;
    }
    return nullptr;
}

const Hook* Runtime::find_hook(const Program& program, std::string_view name) const {
    for (const auto& h : program.hooks) if (h.name == name) return &h;
    return nullptr;
}

const Entity* Runtime::find_entity(const Program& program, std::string_view name) const {
    std::string_view bare = name;
    if (bare.rfind("entity:", 0) == 0) bare = bare.substr(7);
    const auto lt = bare.find('<');
    if (lt != std::string_view::npos) bare = bare.substr(0, lt);
    for (const auto& e : program.entities) if (e.name == bare) return &e;
    return nullptr;
}

const Action* Runtime::find_entity_method(const Entity& e, std::string_view name) const {
    for (const auto& a : e.methods) if (a.name == name) return &a;
    return nullptr;
}

// Index of alternative T in variant V, for switching on Statement::index().
template <typename T, typename V> struct VariantIndex;
template <typename T, typename... Ts> struct VariantIndex<T, std::variant<Ts...>> {
    static constexpr std::size_t value = [] {
        constexpr bool same[] = {std::is_same_v<T, Ts>...};
        for (std::size_t i = 0; i < sizeof...(Ts); ++i) if (same[i]) return i;
        return sizeof...(Ts);
    }();
};
template <typename T> constexpr std::size_t kStmt = VariantIndex<T, Statement>::value;

void Runtime::exec_stmt(const Statement& s, const Program& program, ExecContext& ctx, Env& env) const {
    // A jump table over the statement kind; each kind has its own handler.
    switch (s.index()) {
        case kStmt<ImportStmt>: return exec_import(*std::get_if<ImportStmt>(&s), program, ctx, env);
        case kStmt<PrintStmt>: return exec_print(*std::get_if<PrintStmt>(&s), program, ctx, env);
        case kStmt<SleepStmt>: return exec_sleep(*std::get_if<SleepStmt>(&s), program, ctx, env);
        case kStmt<std::shared_ptr<ParallelStmt>>: return exec_parallel(*std::get_if<std::shared_ptr<ParallelStmt>>(&s), program, ctx, env);
        case kStmt<WaitAllStmt>: return exec_wait_all(*std::get_if<WaitAllStmt>(&s), program, ctx, env);
        case kStmt<PauseStmt>: return exec_pause(*std::get_if<PauseStmt>(&s), program, ctx, env);
        case kStmt<InputStmt>: return exec_input(*std::get_if<InputStmt>(&s), program, ctx, env);
        case kStmt<LetStmt>: return exec_let(*std::get_if<LetStmt>(&s), program, ctx, env);
        case kStmt<ReturnStmt>: return exec_return(*std::get_if<ReturnStmt>(&s), program, ctx, env);
        case kStmt<FireStmt>: return exec_fire(*std::get_if<FireStmt>(&s), program, ctx, env);
        case kStmt<IfStmt>: return exec_if(*std::get_if<IfStmt>(&s), program, ctx, env);
        case kStmt<BreakStmt>: return exec_break(*std::get_if<BreakStmt>(&s), program, ctx, env);
        case kStmt<ContinueStmt>: return exec_continue(*std::get_if<ContinueStmt>(&s), program, ctx, env);
        case kStmt<WhileStmt>: return exec_while(*std::get_if<WhileStmt>(&s), program, ctx, env);
        case kStmt<RepeatStmt>: return exec_repeat(*std::get_if<RepeatStmt>(&s), program, ctx, env);
        case kStmt<ForStmt>: return exec_for(*std::get_if<ForStmt>(&s), program, ctx, env);
        case kStmt<DoWhileStmt>: return exec_do_while(*std::get_if<DoWhileStmt>(&s), program, ctx, env);
        case kStmt<ForInStmt>: return exec_for_in(*std::get_if<ForInStmt>(&s), program, ctx, env);
        case kStmt<TryCatchStmt>: return exec_try_catch(*std::get_if<TryCatchStmt>(&s), program, ctx, env);
        case kStmt<UnsafeStmt>: return exec_unsafe(*std::get_if<UnsafeStmt>(&s), program, ctx, env);
        case kStmt<PointerSetStmt>: return exec_pointer_set(*std::get_if<PointerSetStmt>(&s), program, ctx, env);
        case kStmt<IndexSetStmt>: return exec_index_set(*std::get_if<IndexSetStmt>(&s), program, ctx, env);
        case kStmt<SwitchStmt>: return exec_switch(*std::get_if<SwitchStmt>(&s), program, ctx, env);
        case kStmt<MatchStmt>: return exec_match(*std::get_if<MatchStmt>(&s), program, ctx, env);
        case kStmt<ExprStmt>: return exec_expr(*std::get_if<ExprStmt>(&s), program, ctx, env);
        case kStmt<SetStmt>: return exec_set(*std::get_if<SetStmt>(&s), program, ctx, env);
        case kStmt<MethodCallStmt>: return exec_method_call(*std::get_if<MethodCallStmt>(&s), program, ctx, env);
        case kStmt<ActionCallStmt>: return exec_action_call(*std::get_if<ActionCallStmt>(&s), program, ctx, env);
        default: return;
    }
}

// `Array<T> a {N} = init`: a list of exactly N elements (initializer, then zero values of T),
// marked fixed so push/pop/insert/erase/clear fail. Map/Set: N reserves capacity.
void Runtime::apply_declared_size(const LetStmt& st, Value& value, const Env& env) const {
    const int64_t n = value_as_int(eval_value(*st.sizeExpr, env));
    if (n < 0) {
        throw std::runtime_error("collection size for '" + st.name + "' must not be negative (got " +
                                 std::to_string(n) + ")");
    }
    const auto size = static_cast<std::size_t>(n);
    if (value_is_handle(value, HandleKind::List)) {
        int id = static_cast<int>(value_handle_id(value));
        if (!std::holds_alternative<ListLiteralExpr>(st.value->node)) {
            // Initialized from another list: copy so the source keeps its own length.
            const int copyId = g_nextListId++;
            g_lists[copyId] = g_lists[id];
            id = copyId;
            value = make_handle_value(HandleKind::List, static_cast<uint32_t>(id));
        }
        auto& items = g_lists[id];
        if (items.size() > size) {
            throw std::runtime_error("Array '" + st.name + "' has fixed size " + std::to_string(size) + " but " +
                                     std::to_string(items.size()) + " elements were given");
        }
        std::string elemType;
        if (!parse_runtime_array_type(normalize_runtime_type_name(st.declaredType), elemType)) elemType = "int";
        items.reserve(size);
        items.resize(size, mem_zero_value(elemType));
        g_fixedLists.insert(id);
        return;
    }
    if (value_is_handle(value, HandleKind::Dict)) {
        g_dicts[static_cast<int>(value_handle_id(value))].reserve(size);
        return;
    }
    if (value_is_handle(value, HandleKind::Set)) {
        g_sets[static_cast<int>(value_handle_id(value))].reserve(size);
        return;
    }
    throw std::runtime_error("size `{N}` on '" + st.name + "' requires an Array, Map or Set value");
}

// Releases owners/weak refs and closes files bound to names this block introduced
// (keys inserted into env.vars since `mark`). Names that existed before the block stay.
// Surviving new names stay logged so an enclosing block still treats them as its own.
void Runtime::release_block_scope(Env& env, std::size_t mark) const {
    auto& log = env.vars.insertLog;
    std::size_t keep = mark;
    for (std::size_t i = mark; i < log.size(); ++i) {
        auto it = env.vars.data.find(log[i]);
        if (it == env.vars.data.end()) continue;
        const Value& v = it->second;
        bool released = false;
        if (v.kind == ValueKind::Handle || v.kind == ValueKind::String) {
            if (mem_is_owner(v) || value_is_handle(v, HandleKind::Weak)) {
                mem_release(v);
                env.vars.data.erase(it);
                released = true;
            } else if (value_is_handle(v, HandleKind::File) ||
                       (v.kind == ValueKind::String && v.string_ref().rfind("file:", 0) == 0)) {
                std::vector<ExprPtr> callArgs{
                    std::make_shared<Expr>(Expr{ ExprString{ to_display_string(v), true } }) };
                (void)eval_builtin_call("file_close", callArgs, env, true);
                env.vars.data.erase(it);
                released = true;
            }
        }
        if (!released) {
            if (keep != i) log[keep] = std::move(log[i]);
            ++keep;
        }
    }
    log.resize(keep);
}

void Runtime::exec_block(const Block& b, const Program& program, ExecContext& ctx, Env& env) const {
    const bool dbg = debug_enabled();
    const std::size_t mark = env.vars.insertLog.size();
    for (size_t i = 0; i < b.stmts.size(); ++i) {
        if (dbg) {
            const int line = (i < b.lines.size()) ? b.lines[i] : 0;
            debug_hook(line, env);
        }
        try {
            exec_stmt(b.stmts[i], program, ctx, env);
        } catch (const TryPropagateException& prop) {
            ctx.returned = true;
            ctx.returnValue = prop.value;
            break;
        } catch (...) {
            release_block_scope(env, mark);
            throw;
        }
        if (ctx.returned || ctx.breakSignal || ctx.continueSignal) {
            break;
        }
    }
    release_block_scope(env, mark);
}

} // namespace erelang
