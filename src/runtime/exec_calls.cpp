// Statement execution: action calls, method calls and expression statements.
// Dispatch lives in Runtime::exec_stmt (actions.cpp).
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

void Runtime::exec_expr(const ExprStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    if (!st.expr) return;
    auto lvalue_name = [](const ExprPtr& e) -> std::optional<std::string> {
        if (e && std::holds_alternative<ExprIdent>(e->node)) return std::get<ExprIdent>(e->node).name;
        return std::nullopt;
    };
    if (std::holds_alternative<PostfixExpr>(st.expr->node)) {
        const auto& pe = std::get<PostfixExpr>(st.expr->node);
        if (auto name = lvalue_name(pe.operand)) {
            const int64_t cur = value_as_int(env_get(env, *name));
            env_set(env, *name, Value::from_int(cur + (pe.isInc ? 1 : -1)));
        } else {
            (void)eval_value(*st.expr, env);
        }
        return;
    }
    if (std::holds_alternative<PrefixExpr>(st.expr->node)) {
        const auto& pe = std::get<PrefixExpr>(st.expr->node);
        if (auto name = lvalue_name(pe.operand)) {
            const int64_t cur = value_as_int(env_get(env, *name));
            env_set(env, *name, Value::from_int(cur + (pe.isInc ? 1 : -1)));
        } else {
            (void)eval_value(*st.expr, env);
        }
        return;
    }
    if (std::holds_alternative<CompoundAssignExpr>(st.expr->node)) {
        const auto& ca = std::get<CompoundAssignExpr>(st.expr->node);
        if (auto name = lvalue_name(ca.left)) {
            const Expr binExpr{ BinaryExpr{ ca.op, ca.left, ca.right } };
            env_set(env, *name, eval_value(binExpr, env));
        } else {
            (void)eval_value(*st.expr, env);
        }
        return;
    }
    (void)eval_value(*st.expr, env);
    return;
}

void Runtime::exec_method_call(const MethodCallStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& mc = node;
    std::string methodName = mc.method;
    {
        Value recv = env_get(env, mc.objectName);
        const bool isList = value_is_handle(recv, HandleKind::List);
        if (isList || value_is_handle(recv, HandleKind::Dict)) {
            if (methodName == "forEach") {
                // forEach(actionName): runs the action with `item` (list) or `key`/`value` (dict).
                if (mc.args.empty()) return;
                const std::string actionName = eval_string(*mc.args[0], env);
                const int id = static_cast<int>(value_handle_id(recv));
                if (isList) {
                    const std::vector<Value> items = g_lists[id];
                    for (const auto& item : items) {
                        if (const Action* a = find_action(program, actionName)) {
                            Env callee; callee.vars["item"] = item; exec_block(a->body, program, ctx, callee);
                        }
                    }
                } else {
                    const auto entries = g_dicts[id];
                    for (const auto& kv : entries) {
                        if (const Action* a = find_action(program, actionName)) {
                            Env callee;
                            callee.vars["key"] = kv.first;
                            callee.vars["value"] = kv.second;
                            exec_block(a->body, program, ctx, callee);
                        }
                    }
                }
                return;
            }
            std::vector<Value> cargs;
            cargs.reserve(mc.args.size());
            for (const auto& a : mc.args) cargs.push_back(eval_arg(*a, env));
            if (auto result = collection_method(recv, methodName, cargs)) {
                if (result->kind != ValueKind::Null || !collection_method_is_void(methodName)) {
                    env.vars["_"] = std::move(*result);
                }
                return;
            }
        }
        if (value_is_handle(recv, HandleKind::Buffer)) {
            std::vector<Value> bargs;
            bargs.reserve(mc.args.size());
            for (const auto& a : mc.args) bargs.push_back(eval_value(*a, env));
            env.vars["_"] = mem_buffer_method(recv, methodName, bargs);
            return;
        }
        if (value_is_handle(recv, HandleKind::Weak) && methodName == "get") {
            env.vars["_"] = mem_weak_get(recv);
            return;
        }
        {
            std::vector<Value> margs;
            margs.reserve(mc.args.size());
            for (const auto& a : mc.args) margs.push_back(eval_value(*a, env));
            if (auto handled = dispatch_value_method(recv, methodName, margs)) {
                env.vars["_"] = *handled;
                return;
            }
        }
    }
    if (const Value* moduleBuiltin = env_find(env, mc.objectName + "." + mc.method)) {
        if (moduleBuiltin->kind == ValueKind::String &&
            moduleBuiltin->string_ref().rfind(kBuiltinAliasPrefix.data(), 0) == 0) {
            env.vars["_"] = eval_builtin_call(mc.objectName + "." + mc.method, mc.args, env);
            return;
        }
    }
    if (auto builtinTarget = resolve_builtin_module_method(program, mc.objectName, mc.method); builtinTarget.has_value()) {
        env.vars["_"] = eval_builtin_call(*builtinTarget, mc.args, env);
        return;
    }
    for (const auto& importDecl : program.imports) {
        if (!importDecl.alias || *importDecl.alias != mc.objectName || importDecl.path.empty()) {
            continue;
        }
        std::string normalizedPath = importDecl.path;
        for (auto& ch : normalizedPath) if (ch == '\\') ch = '/';
        std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        const bool isBuiltinAlias = (normalizedPath == "builtin/fs" || normalizedPath == "builtin/erefs" ||
                                     normalizedPath == "std/fs" ||
                                     normalizedPath == "builtin/path" || normalizedPath == "builtin/erepath" ||
                                     normalizedPath == "std/path" ||
                                     normalizedPath == "std/pipe" || normalizedPath == "builtin/pipe");
        if (isBuiltinAlias) {
            continue;
        }
        if (const Action* aliased = find_action(program, methodName)) {
            if (program.strict && aliased->visibility != Visibility::Public) {
                throw std::runtime_error("Action not public: " + aliased->name);
            }
            Env calleeEnv;
            for (size_t i = 0; i < aliased->params.size() && i < mc.args.size(); ++i) {
                calleeEnv.vars[aliased->params[i].name] = eval_string(*mc.args[i], env);
            }
            prepare_action_slots(calleeEnv, *aliased);
            ExecContext calleeCtx;
            exec_block(aliased->body, program, calleeCtx, calleeEnv);
            for (auto& th : calleeCtx.threads) if (th.joinable()) th.join();
            return;
        }
        break;
    }

    if (const Value* bound = env_find(env, mc.objectName)) {
        const std::string handle = to_display_string(*bound);
        // Set handle dispatch (handle prefix "set:")
        {
        if (handle.rfind("set:", 0) == 0 && methodName == "add") {
            int id = to_int(handle.substr(4));
            if (!mc.args.empty()) {
                std::string v = eval_string(*mc.args[0], env);
                env.vars["_"] = g_sets[id].insert(v).second ? "true" : "false";
            }
            return;
        }
        if (handle.rfind("set:", 0) == 0 && methodName == "has") {
            int id = to_int(handle.substr(4));
            std::string v = mc.args.empty() ? std::string() : eval_string(*mc.args[0], env);
            env.vars["_"] = g_sets[id].count(v) ? "true" : "false";
            return;
        }
        if (handle.rfind("set:", 0) == 0 && methodName == "remove") {
            int id = to_int(handle.substr(4));
            std::string v = mc.args.empty() ? std::string() : eval_string(*mc.args[0], env);
            env.vars["_"] = g_sets[id].erase(v) ? "true" : "false";
            return;
        }
        if (handle.rfind("set:", 0) == 0 && methodName == "size") {
            int id = to_int(handle.substr(4));
            env.vars["_"] = std::to_string(static_cast<int>(g_sets[id].size()));
            return;
        }
        if (handle.rfind("set:", 0) == 0 && methodName == "values") {
            int id = to_int(handle.substr(4));
            int lid = g_nextListId++;
            g_lists[lid] = {};
            for (const auto& v : g_sets[id]) g_lists[lid].push_back(value_from_legacy_string(v));
            env.vars["_"] = std::string("list:") + std::to_string(lid);
            return;
        }
        if (handle.rfind("set:", 0) == 0 && methodName == "union") {
            int id = to_int(handle.substr(4));
            if (!mc.args.empty()) {
                std::string otherHandle = eval_string(*mc.args[0], env);
                if (otherHandle.rfind("set:", 0) == 0) {
                    int srcId = to_int(otherHandle.substr(4));
                    int resultId = g_nextSetId++;
                    g_sets[resultId] = g_sets[id];
                    for (const auto& v : g_sets[srcId]) g_sets[resultId].insert(v);
                    env.vars["_"] = std::string("set:") + std::to_string(resultId);
                }
            }
            return;
        }
        if (handle.rfind("set:", 0) == 0 && methodName == "intersect") {
            int id = to_int(handle.substr(4));
            if (!mc.args.empty()) {
                std::string otherHandle = eval_string(*mc.args[0], env);
                if (otherHandle.rfind("set:", 0) == 0) {
                    int srcId = to_int(otherHandle.substr(4));
                    int resultId = g_nextSetId++;
                    for (const auto& v : g_sets[id]) {
                        if (g_sets[srcId].count(v)) g_sets[resultId].insert(v);
                    }
                    env.vars["_"] = std::string("set:") + std::to_string(resultId);
                }
            }
            return;
        }
        if (handle.rfind("set:", 0) == 0 && methodName == "diff") {
            int id = to_int(handle.substr(4));
            if (!mc.args.empty()) {
                std::string otherHandle = eval_string(*mc.args[0], env);
                if (otherHandle.rfind("set:", 0) == 0) {
                    int srcId = to_int(otherHandle.substr(4));
                    int resultId = g_nextSetId++;
                    for (const auto& v : g_sets[id]) {
                        if (!g_sets[srcId].count(v)) g_sets[resultId].insert(v);
                    }
                    env.vars["_"] = std::string("set:") + std::to_string(resultId);
                }
            }
            return;
        }
        }
        // Queue handle dispatch (handle prefix "queue:")
        {
        if (handle.rfind("queue:", 0) == 0 && methodName == "push") {
            int id = to_int(handle.substr(6));
            if (!mc.args.empty()) {
                g_queues[id].push_back(eval_string(*mc.args[0], env));
            }
            return;
        }
        if (handle.rfind("queue:", 0) == 0 && methodName == "pop") {
            int id = to_int(handle.substr(6));
            auto& q = g_queues[id];
            if (!q.empty()) {
                env.vars["_"] = q.front();
                q.pop_front();
            } else {
                env.vars["_"] = std::string();
            }
            return;
        }
        if (handle.rfind("queue:", 0) == 0 && methodName == "peek") {
            int id = to_int(handle.substr(6));
            auto& q = g_queues[id];
            env.vars["_"] = q.empty() ? std::string() : q.front();
            return;
        }
        if (handle.rfind("queue:", 0) == 0 && (methodName == "len" || methodName == "size")) {
            int id = to_int(handle.substr(6));
            env.vars["_"] = std::to_string(static_cast<int>(g_queues[id].size()));
            return;
        }
        if (handle.rfind("queue:", 0) == 0 && methodName == "clear") {
            int id = to_int(handle.substr(6));
            g_queues[id].clear();
            return;
        }
        }
        // StrBuf handle dispatch (handle prefix "strbuf:")
        {
        if (handle.rfind("strbuf:", 0) == 0 && methodName == "append") {
            int id = to_int(handle.substr(7));
            if (!mc.args.empty()) {
                g_strBuffers[id] += eval_string(*mc.args[0], env);
            }
            return;
        }
        if (handle.rfind("strbuf:", 0) == 0 && methodName == "clear") {
            int id = to_int(handle.substr(7));
            g_strBuffers[id].clear();
            return;
        }
        if (handle.rfind("strbuf:", 0) == 0 && (methodName == "len" || methodName == "size")) {
            int id = to_int(handle.substr(7));
            env.vars["_"] = std::to_string(static_cast<int>(g_strBuffers[id].size()));
            return;
        }
        if (handle.rfind("strbuf:", 0) == 0 && methodName == "to_string") {
            int id = to_int(handle.substr(7));
            env.vars["_"] = g_strBuffers[id];
            return;
        }
        if (handle.rfind("strbuf:", 0) == 0 && methodName == "free") {
            int id = to_int(handle.substr(7));
            g_strBuffers.erase(id);
            return;
        }
        if (handle.rfind("strbuf:", 0) == 0 && methodName == "reserve") {
            int id = to_int(handle.substr(7));
            if (!mc.args.empty()) {
                size_t cap = static_cast<size_t>(to_int(eval_string(*mc.args[0], env)));
                g_strBuffers[id].reserve(cap);
            }
            return;
        }
        }
        // Ptr handle dispatch (handle prefix "ptr:")
        {
        if (handle.rfind("ptr:", 0) == 0 && methodName == "get") {
            int id = to_int(handle.substr(4));
            auto it = g_ptrs.find(id);
            env.vars["_"] = (it != g_ptrs.end()) ? it->second : std::string();
            return;
        }
        if (handle.rfind("ptr:", 0) == 0 && methodName == "set") {
            int id = to_int(handle.substr(4));
            if (!mc.args.empty()) {
                g_ptrs[id] = eval_string(*mc.args[0], env);
            }
            return;
        }
        if (handle.rfind("ptr:", 0) == 0 && methodName == "valid") {
            int id = to_int(handle.substr(4));
            env.vars["_"] = g_ptrs.count(id) ? "true" : "false";
            return;
        }
        if (handle.rfind("ptr:", 0) == 0 && methodName == "free") {
            int id = to_int(handle.substr(4));
            g_ptrs.erase(id);
            return;
        }
        }
        // File handle dispatch (handle prefix "file:")
        {
        if (handle.rfind("file:", 0) == 0) {
            std::string builtinName;
            if (methodName == "read") builtinName = "file_read";
            else if (methodName == "write") builtinName = "file_write";
            else if (methodName == "seek") builtinName = "file_seek";
            else if (methodName == "tell") builtinName = "file_tell";
            else if (methodName == "flush") builtinName = "file_flush";
            else if (methodName == "close") builtinName = "file_close";
            else if (methodName == "buffer") builtinName = "file_buffer";
            if (!builtinName.empty()) {
                std::vector<ExprPtr> callArgs;
                callArgs.push_back(std::make_shared<Expr>(Expr{ExprString{handle, true}}));
                for (const auto& a : mc.args) callArgs.push_back(a);
                env.vars["_"] = eval_builtin_call(builtinName, callArgs, env, true);
                return;
            }
        }
        }
        // Module handles (ws:, tcp:, proc:, ...): whichever module registered the prefix owns them.
        if (const auto colon = handle.find(':'); colon != std::string::npos && colon > 0 && colon + 1 < handle.size() &&
            handle.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
            const std::string_view prefix = std::string_view(handle).substr(0, colon);
            if (modules::has_handle_prefix(prefix)) {
                const int id = to_int(handle.substr(colon + 1));
                std::vector<std::string> args;
                args.reserve(mc.args.size());
                for (const auto& a : mc.args) args.push_back(eval_string(*a, env));
                std::string result;
                if (modules::dispatch_handle(const_cast<Runtime*>(this), prefix, id, methodName, args, result)) {
                    env.vars["_"] = result;
                    return;
                }
            }
        }
        // Closure dispatch (handle prefix "func:")
        if (handle.rfind("func:", 0) == 0) {
            const std::string funcIdStr = handle.substr(5);
            int funcId = 0;
            try { funcId = std::stoi(funcIdStr); }
            catch (...) { throw std::runtime_error("Invalid func handle: " + handle); }
            auto it = g_closures.find(funcId);
            if (it == g_closures.end() || !it->second) {
                // Allow call on null - silently return empty string
                env.vars["_"] = "";
                return;
            }
            ClosureData* cd = it->second;
            Env callEnv;
            // Seed captured values
            for (const auto& cv : cd->captured) {
                if (cv.cell) {
                    callEnv.cells[cv.name] = cv.cell;
                    callEnv.vars[cv.name] = *cv.cell;
                }
            }
            // Bind arguments
            for (size_t i = 0; i < cd->body.params.size() && i < mc.args.size(); ++i) {
                callEnv.vars[cd->body.params[i].name] = eval_string(*mc.args[i], env);
            }
            // Execute body
            ExecContext child;
            exec_block(cd->body.body, program, child, callEnv);
            env.vars["_"] = child.returnValue;
            return;
        }
        if (handle.rfind("struct:", 0) == 0) {
            const std::string structName = handle.substr(7);
            const StructDecl* sd = find_struct_decl(program, structName);
            const Action* method = sd ? find_struct_method(*sd, methodName) : nullptr;
            if (!method) {
                throw std::runtime_error("Unknown struct method: " + structName + "." + methodName);
            }
            if (program.strict && method->visibility != Visibility::Public && mc.objectName != "self") {
                throw std::runtime_error("Method not visible: " + methodName);
            }
            bool isHidden = false;
            for (const auto& at : method->attributes) if (at.name == "hidden") { isHidden = true; break; }
            if (isHidden && mc.objectName != "self") {
                throw std::runtime_error("Method hidden: " + methodName);
            }

            Env callEnv;
            for (size_t i = 0; i < method->params.size() && i < mc.args.size(); ++i) {
                callEnv.vars[method->params[i].name] = eval_string(*mc.args[i], env);
            }

            callEnv.vars["self"] = handle;
            for (const auto& f : sd->fields) {
                const std::string objectField = mc.objectName + "." + f.name;
                auto fit = env.vars.find(objectField);
                const std::string value = (fit != env.vars.end()) ? fit->second : std::string{};
                callEnv.vars[f.name] = value;
                callEnv.vars["self." + f.name] = value;
            }

            ExecContext child;
            exec_block(method->body, program, child, callEnv);
            for (auto& th : child.threads) if (th.joinable()) th.join();

            for (const auto& f : sd->fields) {
                const std::string selfKey = "self." + f.name;
                auto selfIt = callEnv.vars.find(selfKey);
                auto fieldIt = callEnv.vars.find(f.name);
                if (selfIt != callEnv.vars.end()) {
                    env_set(env, mc.objectName + "." + f.name, selfIt->second);
                } else if (fieldIt != callEnv.vars.end()) {
                    env_set(env, mc.objectName + "." + f.name, fieldIt->second);
                }
            }
            return;
        }
    }
    auto it = env.objects.find(mc.objectName);
    if (it == env.objects.end()) throw std::runtime_error("Unknown object: " + mc.objectName);
    ObjPtr obj = it->second;
// fallback to scripted entity methods
    const Entity* ent = find_entity(program, obj->typeName);
    if (!ent) throw std::runtime_error("Entity type not found: " + obj->typeName);
    const Action* meth = find_entity_method(*ent, mc.method);
    if (!meth) throw std::runtime_error("Unknown method: " + mc.method);
    // Enforce visibility in strict mode for scripted entity methods
    if (program.strict && meth->visibility != Visibility::Public && mc.objectName != "self") {
        throw std::runtime_error("Method not visible: " + mc.method);
    }
    // Hidden enforcement: if method has @hidden, allow only when caller is self
    bool isHidden = false;
    for (const auto& at : meth->attributes) if (at.name == "hidden") { isHidden = true; break; }
    if (isHidden && mc.objectName != "self") {
        throw std::runtime_error("Method hidden: " + mc.method);
    }
    Env callEnv;
    // bind positional args into method params
    for (size_t i=0; i<meth->params.size() && i<mc.args.size(); ++i) {
        callEnv.vars[meth->params[i].name] = eval_string(*mc.args[i], env);
    }
    // expose 'self' with fields accessible as variables
    callEnv.objects["self"] = obj;
    for (const auto& kv : obj->fields) callEnv.vars[kv.first] = kv.second;
    ExecContext child;
    exec_block(meth->body, program, child, callEnv);
    for (auto& th : child.threads) if (th.joinable()) th.join();
    // propagate any changed fields back to object
    for (auto& f : obj->fields) {
        auto vit = callEnv.vars.find(f.first);
        if (vit != callEnv.vars.end()) f.second = vit->second;
    }
    return;
}

void Runtime::exec_action_call(const ActionCallStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& call = node;
    if (call.name == "alloc" || call.name == "free" || call.name == "realloc" ||
        call.name == "copy" || call.name == "move" || call.name == "fill" ||
        call.name == "zero" || call.name == "heap" || call.name == "shared" ||
        call.name == "weak" || call.name == "buffer") {
        FunctionCallExpr fc;
        fc.name = call.name;
        fc.args = call.args;
        fc.typeArgs = call.typeArgs;
        (void)eval_value(Expr{std::move(fc)}, env);
        return;
    }
    if (const Action* a = find_action(program, call.name)) {
        if (program.strict && a->visibility != Visibility::Public) {
            throw std::runtime_error("Action not public: " + a->name);
        }
        if (a->isAsync) {
            (void)invoke_async_action(*a, call.args, env, async_in_async_action());
            return;
        }
        Env calleeEnv;
        // Bind positional args into parameter names
        for (size_t i=0; i<a->params.size() && i<call.args.size(); ++i) {
            const std::string paramName = a->params[i].name;
            if (std::holds_alternative<ExprIdent>(call.args[i]->node)) {
                const auto& id = std::get<ExprIdent>(call.args[i]->node).name;
                const Value* bound = env_find(env, id);
                const bool carriesFields = value_may_carry_fields(bound);
                if (bound && value_is_struct_marker(*bound))
                    calleeEnv.vars[paramName] = *bound;
                else
                    calleeEnv.vars[paramName] = eval_value(*call.args[i], env);
                if (!env.objects.empty()) {
                    auto oit = env.objects.find(id);
                    if (oit != env.objects.end()) {
                        calleeEnv.objects[paramName] = oit->second;
                    }
                }
                if (carriesFields) {
                    const std::string prefix = id + ".";
                    for (const auto& kv : env.vars) {
                        if (kv.first.rfind(prefix, 0) == 0) {
                            calleeEnv.vars[paramName + kv.first.substr(id.size())] = kv.second;
                        }
                    }
                }
            } else {
                calleeEnv.vars[paramName] = eval_arg(*call.args[i], env);
            }
        }
        prepare_action_slots(calleeEnv, *a);
        const bool prevAsync = async_in_async_action();
        async_set_in_async_action(false);
        ExecContext calleeCtx;
        exec_block(a->body, program, calleeCtx, calleeEnv);
        for (auto& th : calleeCtx.threads) {
            if (th.joinable()) {
                th.join();
            }
        }
        async_set_in_async_action(prevAsync);
        return;
    }
    else {
        // Check if name resolves to a func:N handle in env
        const Value* bound = env_find(env, call.name);
        const std::string handle = bound ? to_display_string(*bound) : std::string();
        if (handle.rfind("func:", 0) == 0) {
            const std::string funcIdStr = handle.substr(5);
            int funcId = 0;
            try { funcId = std::stoi(funcIdStr); }
            catch (...) { throw std::runtime_error("Invalid func handle: " + handle); }
            auto cit = g_closures.find(funcId);
            if (cit != g_closures.end() && cit->second) {
                ClosureData* cd = cit->second;
                Env callEnv;
                for (const auto& cv : cd->captured) {
                    if (cv.cell) {
                        callEnv.cells[cv.name] = cv.cell;
                        callEnv.vars[cv.name] = *cv.cell;
                    }
                }
                for (size_t i = 0; i < cd->body.params.size() && i < call.args.size(); ++i) {
                    callEnv.vars[cd->body.params[i].name] = eval_string(*call.args[i], env);
                }
                prepare_action_slots(callEnv, cd->body);
                ExecContext child;
                exec_block(cd->body.body, program, child, callEnv);
                env.vars["_"] = child.returnValue;
                return;
            }
        }
        for (const auto& ex : program.externs) {
            if (ex.name == call.name) {
                throw std::runtime_error("Extern action not bound at runtime: " + call.name);
            }
        }
        // Fallback: treat as built-in call with side effects
        (void)eval_builtin_call(call.name, call.args, env);
    }
    return;
}

} // namespace erelang
