// SPDX-License-Identifier: Apache-2.0
// Split from runtime.cpp

#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/runtime_builtins.hpp"
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

const Action* Runtime::find_action(const Program& program, std::string_view name) const {
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

void Runtime::exec_stmt(const Statement& s, const Program& program, ExecContext& ctx, Env& env) const {
    if (std::holds_alternative<ImportStmt>(s)) {
        (void)program;
        (void)ctx;
        (void)env;
        return;
    }
    if (std::holds_alternative<PrintStmt>(s)) {
        const auto& st = std::get<PrintStmt>(s);
        std::cout << eval_string(*st.value, env) << std::endl;
        return;
    }
    if (std::holds_alternative<SleepStmt>(s)) {
        const auto& st = std::get<SleepStmt>(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(st.ms));
        return;
    }
    if (std::holds_alternative<std::shared_ptr<ParallelStmt>>(s)) {
        const auto& p = std::get<std::shared_ptr<ParallelStmt>>(s);
        ctx.threads.emplace_back([this, &program, p, env]() mutable {
            ExecContext child;
            Env childEnv = env;
            exec_block(p->body, program, child, childEnv);
            for (auto& th : child.threads) if (th.joinable()) th.join();
        });
        return;
    }
    if (std::holds_alternative<WaitAllStmt>(s)) {
        for (auto& th : ctx.threads) if (th.joinable()) th.join();
        ctx.threads.clear();
        return;
    }
    if (std::holds_alternative<PauseStmt>(s)) {
        std::string dummy; std::getline(std::cin, dummy);
        return;
    }
    if (std::holds_alternative<InputStmt>(s)) {
        const auto& is = std::get<InputStmt>(s);
        std::string line; std::getline(std::cin, line);
        env_set(env, is.name, value_from_legacy_string(std::move(line)));
        return;
    }
    if (std::holds_alternative<LetStmt>(s)) {
        const auto& st = std::get<LetStmt>(s);
        if (st.pattern) {
            std::function<void(const PatternPtr&, const std::string&)> bind_destructure;
            bind_destructure = [&](const PatternPtr& pat, const std::string& value) {
                if (!pat) return;
                std::visit([&](const auto& node) {
                    using N = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<N, PatWildcard>) {
                        return;
                    } else if constexpr (std::is_same_v<N, PatBinding>) {
                        env_set(env, node.name, value_from_legacy_string(value));
                    } else if constexpr (std::is_same_v<N, PatStruct>) {
                        std::string sourceName;
                        if (std::holds_alternative<ExprIdent>(st.value->node)) {
                            sourceName = std::get<ExprIdent>(st.value->node).name;
                        } else if (env.objects.count(value)) {
                            sourceName = value;
                        }
                        for (const auto& f : node.fields) {
                            std::string fieldVal;
                            if (!lastReturnFields_.empty()) {
                                auto lit = lastReturnFields_.find(f.field);
                                if (lit != lastReturnFields_.end()) {
                                    fieldVal = to_display_string(lit->second);
                                }
                            }
                            if (fieldVal.empty() && !sourceName.empty()) {
                                auto fit = env.vars.find(sourceName + "." + f.field);
                                if (fit != env.vars.end()) {
                                    fieldVal = to_display_string(fit->second);
                                } else {
                                    auto oit = env.objects.find(sourceName);
                                    if (oit != env.objects.end() && oit->second) {
                                        auto ff = oit->second->fields.find(f.field);
                                        if (ff != oit->second->fields.end()) fieldVal = ff->second;
                                    }
                                }
                            }
                            if (fieldVal.empty() && value.rfind("dict:", 0) == 0) {
                                const int id = to_int(value.substr(5));
                                auto dit = g_dicts.find(id);
                                if (dit != g_dicts.end()) {
                                    auto fit = dit->second.find(f.field);
                                    if (fit != dit->second.end()) fieldVal = fit->second;
                                }
                            }
                            bind_destructure(f.pattern, fieldVal);
                        }
                        lastReturnFields_.clear();
                    } else if constexpr (std::is_same_v<N, PatArray>) {
                        std::vector<std::string>* list = nullptr;
                        int listId = -1;
                        if (value.rfind("list:", 0) == 0) {
                            listId = to_int(value.substr(5));
                            auto lit = g_lists.find(listId);
                            if (lit != g_lists.end()) list = &lit->second;
                        }
                        if (!list) {
                            throw std::runtime_error("Array destructuring requires a list value");
                        }
                        if (list->size() < node.elements.size()) {
                            throw std::runtime_error("Array destructuring: too few elements");
                        }
                        for (size_t i = 0; i < node.elements.size(); ++i) {
                            bind_destructure(node.elements[i], (*list)[i]);
                        }
                    } else if constexpr (std::is_same_v<N, PatTuple>) {
                        std::vector<std::string>* tup = nullptr;
                        if (value.rfind("tuple:", 0) == 0) {
                            const int id = to_int(value.substr(6));
                            auto tit = g_tuples.find(id);
                            if (tit != g_tuples.end()) tup = &tit->second;
                        }
                        if (!tup) {
                            throw std::runtime_error("Tuple destructuring requires a tuple value");
                        }
                        if (tup->size() != node.elements.size()) {
                            throw std::runtime_error("tuple destructuring expects " + std::to_string(node.elements.size()) +
                                " elements, got " + std::to_string(tup->size()));
                        }
                        for (size_t i = 0; i < node.elements.size(); ++i) {
                            bind_destructure(node.elements[i], (*tup)[i]);
                        }
                    } else if constexpr (std::is_same_v<N, PatCtor>) {
                        std::string tag;
                        std::vector<std::string> payloads;
                        if (!decode_enum_variant(value, tag, payloads)) {
                            tag = value;
                            payloads.clear();
                        }
                        if (tag != node.name) {
                            throw std::runtime_error("Enum let destructuring failed: expected variant `" + node.name + "`, got `" + tag + "`");
                        }
                        if (payloads.size() != node.args.size()) {
                            throw std::runtime_error("Enum let destructuring payload count mismatch for `" + node.name + "`");
                        }
                        for (size_t i = 0; i < node.args.size(); ++i) {
                            bind_destructure(node.args[i], payloads[i]);
                        }
                    }
                }, pat->node);
            };
            const std::string value = eval_string(*st.value, env);
            bind_destructure(st.pattern, value);
            return;
        }
        if (!st.declaredType.empty()) {
            if (const StructDecl* sd = find_struct_decl(program, st.declaredType)) {
                env_set(env, st.name, Value::from_string(std::string("struct:") + sd->name));
                for (const auto& f : sd->fields) {
                    env_set(env, st.name + "." + f.name, Value::from_string(""));
                }

                const std::string initValue = eval_string(*st.value, env);
                if (initValue.rfind("dict:", 0) == 0) {
                    const int id = to_int(initValue.substr(5));
                    auto dit = g_dicts.find(id);
                    if (dit != g_dicts.end()) {
                        for (const auto& f : sd->fields) {
                            auto fit = dit->second.find(f.name);
                            if (fit != dit->second.end()) {
                                env_set(env, st.name + "." + f.name, value_from_legacy_string(fit->second));
                            }
                        }
                    }
                } else if (std::holds_alternative<ExprIdent>(st.value->node)) {
                    const auto& sourceName = std::get<ExprIdent>(st.value->node).name;
                    auto sit = env.vars.find(sourceName);
                    if (sit != env.vars.end() && sit->second == (std::string("struct:") + sd->name)) {
                        for (const auto& f : sd->fields) {
                            auto srcField = env.vars.find(sourceName + "." + f.name);
                            if (srcField != env.vars.end()) {
                                env_set(env, st.name + "." + f.name, srcField->second);
                            }
                        }
                    }
                }
                if (!lastReturnFields_.empty()) {
                    for (const auto& kv : lastReturnFields_)
                        env_set(env, st.name + "." + kv.first, kv.second);
                    lastReturnFields_.clear();
                }
                return;
            }
            if (std::holds_alternative<ExprNull>(st.value->node)) {
                if (const Entity* ent = find_entity(program, st.declaredType)) {
                    if (program.strict && ent->visibility != Visibility::Public) {
                        throw std::runtime_error("Entity not public: " + ent->name);
                    }
                    auto obj = std::make_shared<Object>();
                    obj->typeName = ent->name;
                    for (const auto& f : ent->fields) {
                        if (f.defaultValue) {
                            obj->fields[f.name] = eval_string(*f.defaultValue, env);
                        } else {
                            obj->fields[f.name] = {};
                        }
                    }
                    env.objects[st.name] = obj;
                    env_set(env, st.name, Value::from_string(st.name));
                    return;
                }
            }
        }
        if (std::holds_alternative<FunctionCallExpr>(st.value->node)) {
            const auto& fc = std::get<FunctionCallExpr>(st.value->node);
            if (const StructDecl* sd = find_struct_decl(program, fc.name)) {
                env_set(env, st.name, Value::from_string(std::string("struct:") + sd->name));
                for (const auto& f : sd->fields) {
                    env_set(env, st.name + "." + f.name, Value::from_string("0"));
                }
                return;
            }
        }
        if (std::holds_alternative<NewExpr>(st.value->node)) {
            const auto& ne = std::get<NewExpr>(st.value->node);
            const Entity* ent = find_entity(program, ne.typeName);
            if (!ent) throw std::runtime_error("Unknown entity: " + ne.typeName);
            if (program.strict && ent->visibility != Visibility::Public) {
                throw std::runtime_error("Entity not public: " + ne.typeName);
            }
            auto obj = std::make_shared<Object>();
            obj->typeName = ne.typeName;
            // initialize fields (default value if provided)
            for (const auto& f : ent->fields) {
                if (f.defaultValue) {
                    obj->fields[f.name] = eval_string(*f.defaultValue, env);
                } else {
                    obj->fields[f.name] = {};
                }
            }
            // bind and run optional init(name, ...) only if 'new' provided arguments
            if (const Action* init = find_entity_method(*ent, "init")) {
                Env selfEnv;
                for (size_t i=0; i<init->params.size() && i<ne.args.size(); ++i) {
                    selfEnv.vars[init->params[i].name] = eval_string(*ne.args[i], env);
                }
                selfEnv.objects["self"] = obj;
                for (const auto& kv : obj->fields) selfEnv.vars[kv.first] = kv.second;
                ExecContext child;
                exec_block(init->body, program, child, selfEnv);
                for (auto& th : child.threads) if (th.joinable()) th.join();
                for (auto& f : obj->fields) {
                    auto vit = selfEnv.vars.find(f.first);
                    if (vit != selfEnv.vars.end()) f.second = vit->second;
                }
            }
            env.objects[st.name] = obj;
            env_set(env, st.name, Value::from_string(st.name));
        } else {
            Value value;
            bool movedOwn = false;
            if (std::holds_alternative<ExprIdent>(st.value->node)) {
                const std::string& srcName = std::get<ExprIdent>(st.value->node).name;
                if (mem_try_move_own_ident(env.vars, srcName, value)) {
                    movedOwn = true;
                }
            }
            if (!movedOwn) {
                value = eval_value(*st.value, env);
                if (value_is_handle(value, HandleKind::Own) &&
                    std::holds_alternative<ExprIdent>(st.value->node)) {
                    const std::string& srcName = std::get<ExprIdent>(st.value->node).name;
                    env_set(env, srcName, Value::null_value());
                } else if (value_is_handle(value, HandleKind::Shared) &&
                           std::holds_alternative<ExprIdent>(st.value->node)) {
                    mem_retain(value);
                }
            }
            if (!st.declaredType.empty()) {
                auto infer_runtime_value_type = [&](const Value& vv) {
                    if (vv.kind == ValueKind::Handle) {
                        switch (vv.h.kind) {
                            case HandleKind::List: return std::string("array<any>");
                            case HandleKind::Dict: return std::string("map<string,any>");
                            case HandleKind::Set: return std::string("set<any>");
                            case HandleKind::Ptr: return std::string("pointer");
                            case HandleKind::Own: return std::string("heap");
                            case HandleKind::Shared: return std::string("shared");
                            case HandleKind::Weak: return std::string("weak");
                            case HandleKind::Buffer: return std::string("buffer");
                            default: break;
                        }
                    }
                    const std::string text = to_display_string(vv);
                    if (text.rfind("list:", 0) == 0) return std::string("array<any>");
                    if (text.rfind("dict:", 0) == 0) return std::string("map<string,any>");
                    if (text.rfind("set:", 0) == 0) return std::string("set<any>");
                    if (text.rfind("struct:", 0) == 0) return normalize_runtime_type_name(text);
                    if (vv.kind == ValueKind::Bool || text == "true" || text == "false") return std::string("bool");
                    if (vv.kind == ValueKind::Int || is_int_string(text)) return std::string("int");
                    if (vv.kind == ValueKind::Float || is_float_string(text)) return std::string("double");
                    if (auto it = env.objects.find(text); it != env.objects.end() && it->second) {
                        return normalize_runtime_type_name(it->second->typeName);
                    }
                    return std::string("string");
                };
                const std::string actualType = infer_runtime_value_type(value);
                const std::string declaredNorm = normalize_runtime_type_name(st.declaredType);
                if (declaredNorm == "string" || declaredNorm == "str") {
                } else if ((!declaredNorm.empty() && declaredNorm.front() == '*') ||
                           declaredNorm.rfind("heap<", 0) == 0 ||
                           declaredNorm.rfind("own<", 0) == 0 ||
                           declaredNorm.rfind("shared<", 0) == 0 || declaredNorm.rfind("weak<", 0) == 0 ||
                           declaredNorm.rfind("buffer<", 0) == 0 || declaredNorm == "pointer") {
                    // Memory handles: skip strict string-equality check; TC already validated.
                } else if (!runtime_declared_type_matches(st.declaredType, actualType)) {
                    throw std::runtime_error(
                        "Type mismatch in declaration '" + st.name + "': declared " + st.declaredType + " but got " + actualType
                    );
                }
            }
            {
                Value prev = env_get(env, st.name);
                if (mem_is_owner(prev) || value_is_handle(prev, HandleKind::Weak)) {
                    mem_release(prev);
                }
            }
            env_set(env, st.name, value);
            if (value.rfind("struct:", 0) == 0 && !lastReturnFields_.empty()) {
                const int id = g_nextDictId++;
                auto& dict = g_dicts[id];
                for (const auto& kv : lastReturnFields_) {
                    dict[kv.first] = to_display_string(kv.second);
                    env_set(env, st.name + "." + kv.first, kv.second);
                }
                lastReturnFields_.clear();
                env_set(env, st.name, make_handle_value(HandleKind::Dict, static_cast<uint32_t>(id)));
            }
            if (std::holds_alternative<FunctionCallExpr>(st.value->node)) {
                const auto& fc = std::get<FunctionCallExpr>(st.value->node);
                if (fc.name == "dynamic_cast") {
                    const std::string v = to_display_string(value);
                    auto source = env.objects.find(v);
                    if (source != env.objects.end()) {
                        env.objects[st.name] = source->second;
                        env_set(env, st.name, Value::from_string(st.name));
                    }
                }
            }
        }
        return;
    }
    if (std::holds_alternative<ReturnStmt>(s)) {
        const auto& rs = std::get<ReturnStmt>(s);
        Value rv = Value::from_string("");
        ctx.returnFields.clear();
        if (rs.value && *rs.value) {
            if (std::holds_alternative<ExprIdent>((*rs.value)->node)) {
                const std::string& name = std::get<ExprIdent>((*rs.value)->node).name;
                if (mem_try_move_own_ident(env.vars, name, rv)) {
                    // moved
                } else {
                    Value bound = env_get(env, name);
                    if (bound.rfind("struct:", 0) == 0) {
                        rv = bound;
                        const std::string prefix = name + ".";
                        for (const auto& kv : env.vars) {
                            if (kv.first.rfind(prefix, 0) == 0)
                                ctx.returnFields[kv.first.substr(prefix.size())] = kv.second;
                        }
                    } else {
                        rv = eval_value(**rs.value, env);
                        if (value_is_handle(rv, HandleKind::Shared)) mem_retain(rv);
                        if (value_is_handle(rv, HandleKind::Buffer)) {
                            // returning buffer transfers ownership: clear local without destroy
                            env_set(env, name, Value::null_value());
                        }
                    }
                }
            } else {
                rv = eval_value(**rs.value, env);
                if (rv.rfind("struct:", 0) == 0 && !lastReturnFields_.empty())
                    ctx.returnFields = lastReturnFields_;
            }
        }
        ctx.returned = true;
        ctx.returnValue = std::move(rv);
        return;
    }
    if (std::holds_alternative<FireStmt>(s)) {
        const auto& st = std::get<FireStmt>(s);
        if (const Hook* h = find_hook(program, st.name)) exec_block(h->body, program, ctx, env);
        return;
    }
    if (std::holds_alternative<IfStmt>(s)) {
        const auto& st = std::get<IfStmt>(s);
        if (is_truthy(eval_value(*st.cond, env))) exec_block(*st.thenBlk, program, ctx, env);
        else if (st.elseBlk) exec_block(*st.elseBlk, program, ctx, env);
        return;
    }

    if (std::holds_alternative<BreakStmt>(s)) {

    ctx.breakSignal = true;

    return;
    }
    if (std::holds_alternative<ContinueStmt>(s)) {
        ctx.continueSignal = true;
        return;
    }

    if (std::holds_alternative<WhileStmt>(s)) {
        const auto& st = std::get<WhileStmt>(s);
        while (is_truthy(eval_value(*st.cond, env))) {
            exec_block(*st.body, program, ctx, env);
            if (ctx.breakSignal) {
                ctx.breakSignal = false;
                break;
            }
            if (ctx.continueSignal) {
                ctx.continueSignal = false;
                continue;
            }
            if (ctx.returned) {
                break;
            }
        }
        return;
    }
    if (std::holds_alternative<RepeatStmt>(s)) {
        const auto& st = std::get<RepeatStmt>(s);
        const int64_t count = to_int(eval_string(*st.count, env));
        for (int64_t i = 0; i < count; ++i) {
            exec_block(*st.body, program, ctx, env);
            if (ctx.continueSignal) {
                ctx.continueSignal = false;
                continue;
            }
            if (ctx.breakSignal) {
                ctx.breakSignal = false;
                break;
            }
            if (ctx.returned) {
                break;
            }
        }
        return;
    }
    if (std::holds_alternative<ForStmt>(s)) {
        const auto& st = std::get<ForStmt>(s);
        if (st.init) exec_block(*st.init, program, ctx, env);
        {
            Chunk loopChunk;
            if (try_compile_for_loop(st, loopChunk, &env)) {
                (void)run_chunk(loopChunk, const_cast<Runtime&>(*this), env);
                return;
            }
        }
        while (true) {
            if (st.cond && !is_truthy(eval_value(**st.cond, env))) break;
            exec_block(*st.body, program, ctx, env);
            if (ctx.breakSignal) {
                ctx.breakSignal = false;
                break;
            }
            if (ctx.continueSignal) {
                ctx.continueSignal = false;
            }
            if (ctx.returned) {
                break;
            }
            if (st.step) exec_block(*st.step, program, ctx, env);
            if (ctx.returned) {
                break;
            }
        }
        return;
    }
    if (std::holds_alternative<DoWhileStmt>(s)) {
        const auto& st = std::get<DoWhileStmt>(s);
        while (true) {
            exec_block(*st.body, program, ctx, env);
            if (ctx.breakSignal) {
                ctx.breakSignal = false;
                break;
            }
            if (ctx.continueSignal) {
                ctx.continueSignal = false;
            }
            if (ctx.returned) {
                break;
            }
            if (!is_truthy(eval_value(*st.cond, env))) {
                break;
            }
        }
        return;
    }
    if (std::holds_alternative<ForInStmt>(s)) {
        const auto& st = std::get<ForInStmt>(s);
        std::string iter = eval_string(*st.iterable, env);
        if (iter.rfind("list:", 0) == 0) {
            if (st.valueVar) {
                throw std::runtime_error("List iteration supports only one variable: for (item : list)");
            }
            int id = to_int(iter.substr(5));
            auto it = g_lists.find(id);
            if (it != g_lists.end()) {
                for (const auto& item : it->second) {
                    env_set(env, st.var, value_from_legacy_string(item));
                    exec_block(*st.body, program, ctx, env);
                    if (ctx.breakSignal) {
                        ctx.breakSignal = false;
                        break;
                    }
                    if (ctx.continueSignal) {
                        ctx.continueSignal = false;
                        continue;
                    }
                    if (ctx.returned) {
                        break;
                    }
                }
            }
        } else if (iter.rfind("dict:", 0) == 0) {
            int id = to_int(iter.substr(5));
            auto it = g_dicts.find(id);
            if (it != g_dicts.end()) {
                for (const auto& kv : it->second) {
                    if (st.valueVar) {
                        env_set(env, st.var, value_from_legacy_string(kv.first));
                        env_set(env, *st.valueVar, value_from_legacy_string(kv.second));
                    } else {
                        env_set(env, st.var, value_from_legacy_string(kv.first));
                    }
                    exec_block(*st.body, program, ctx, env);
                    if (ctx.breakSignal) {
                        ctx.breakSignal = false;
                        break;
                    }
                    if (ctx.continueSignal) {
                        ctx.continueSignal = false;
                        continue;
                    }
                    if (ctx.returned) {
                        break;
                    }
                }
            }
        }
        return;
    }
    if (std::holds_alternative<TryCatchStmt>(s)) {
        const auto& st = std::get<TryCatchStmt>(s);
        try {
            exec_block(*st.tryBlk, program, ctx, env);
        } catch (const std::exception& ex) {
            env_set(env, st.catchVar, Value::from_string(ex.what()));
            exec_block(*st.catchBlk, program, ctx, env);
        }
        return;
    }
    if (std::holds_alternative<UnsafeStmt>(s)) {
        const auto& st = std::get<UnsafeStmt>(s);
        exec_block(*st.body, program, ctx, env);
        return;
    }
    if (std::holds_alternative<PointerSetStmt>(s)) {
        const auto& st = std::get<PointerSetStmt>(s);
        const Value target = eval_value(*st.pointer, env);
        const Value newValue = eval_value(*st.value, env);
        const std::string targetText = to_display_string(target);
        if (targetText.rfind("ref:", 0) == 0) {
            const std::string varName = targetText.substr(4);
            env_set(env, varName, newValue);
            return;
        }
        if (auto idOpt = parse_pointer_handle(targetText); idOpt.has_value()) {
            const int id = *idOpt;
            auto it = g_ptrs.find(id);
            if (it != g_ptrs.end() && it->second.rfind("ref:", 0) == 0) {
                const std::string varName = it->second.substr(4);
                env_set(env, varName, newValue);
                return;
            }
        }
        mem_ptr_set(target, newValue);
        return;
    }
    if (std::holds_alternative<IndexSetStmt>(s)) {
        const auto& st = std::get<IndexSetStmt>(s);
        const Value object = eval_value(*st.object, env);
        const int64_t index = value_as_int(eval_value(*st.index, env));
        const Value value = eval_value(*st.value, env);
        if (value_is_handle(object, HandleKind::Buffer)) {
            mem_buffer_index_set(object, index, value);
            return;
        }
        if (to_display_string(object).rfind("list:", 0) == 0) {
            const int id = to_int(to_display_string(object).substr(5));
            auto& list = g_lists[id];
            if (index < 0 || static_cast<std::size_t>(index) >= list.size()) {
                throw std::runtime_error("list index out of bounds");
            }
            list[static_cast<std::size_t>(index)] = to_display_string(value);
            return;
        }
        throw std::runtime_error("index assignment requires buffer or list");
    }
    if (std::holds_alternative<SwitchStmt>(s)) {
        const auto& sw = std::get<SwitchStmt>(s);
        std::string sel = eval_string(*sw.selector, env);
        bool matched = false;
        for (const auto& c : sw.cases) {
            if (c.value == sel) { exec_block(*c.body, program, ctx, env); matched = true; break; }
        }
        if (!matched && sw.defaultBlk) exec_block(*sw.defaultBlk, program, ctx, env);
        if (ctx.breakSignal) {
            // Consume break inside switch so it does not leak to outer loops.
            ctx.breakSignal = false;
        }
        return;
    }
    if (std::holds_alternative<MatchStmt>(s)) {
        const auto& ms = std::get<MatchStmt>(s);
        const std::string sel = to_display_string(eval_value(*ms.selector, env));

        std::string selTag;
        std::vector<std::string> selPayloads;
        const bool selIsEnum = decode_enum_variant(sel, selTag, selPayloads);
        if (!selIsEnum) {
            selTag = sel;
            selPayloads.clear();
        }

        auto bind_value = [&](Env& caseEnv, const std::string& name, const std::string& val) {
            caseEnv.vars[name] = value_from_legacy_string(val);
            if (val.rfind("dict:", 0) == 0) {
                const int id = to_int(val.substr(5));
                auto dit = g_dicts.find(id);
                if (dit != g_dicts.end()) {
                    for (const auto& kv : dit->second) {
                        caseEnv.vars[name + "." + kv.first] = value_from_legacy_string(kv.second);
                    }
                }
            }
        };

        std::function<bool(const PatternPtr&)> pattern_binds;
        pattern_binds = [&](const PatternPtr& pat) -> bool {
            if (!pat) return false;
            return std::visit([&](const auto& node) -> bool {
                using N = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<N, PatBinding>) {
                    return true;
                } else if constexpr (std::is_same_v<N, PatCtor>) {
                    for (const auto& arg : node.args) {
                        if (pattern_binds(arg)) return true;
                    }
                    return false;
                } else if constexpr (std::is_same_v<N, PatOr>) {
                    for (const auto& alt : node.alts) {
                        if (pattern_binds(alt)) return true;
                    }
                    return false;
                } else {
                    return false;
                }
            }, pat->node);
        };

        auto top_level_ctor_rejects = [&](const PatternPtr& pat) -> bool {
            if (!pat || !std::holds_alternative<PatCtor>(pat->node)) return false;
            const auto& ctor = std::get<PatCtor>(pat->node);
            if (selIsEnum) {
                return selTag != ctor.name || ctor.args.size() != selPayloads.size();
            }
            return !(sel == ctor.name && ctor.args.empty());
        };

        std::function<bool(const PatternPtr&, const std::string&, Env&)> try_match;
        try_match = [&](const PatternPtr& pat, const std::string& value, Env& caseEnv) -> bool {
            if (!pat) return false;
            return std::visit([&](const auto& node) -> bool {
                using N = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<N, PatWildcard>) {
                    return true;
                } else if constexpr (std::is_same_v<N, PatBinding>) {
                    bind_value(caseEnv, node.name, value);
                    return true;
                } else if constexpr (std::is_same_v<N, PatCtor>) {
                    std::string tag;
                    std::vector<std::string> payloads;
                    if (&value == &sel && selIsEnum) {
                        tag = selTag;
                        payloads = selPayloads;
                    } else if (&value == &sel && !selIsEnum) {
                        return value == node.name && node.args.empty();
                    } else if (decode_enum_variant(value, tag, payloads)) {
                        // nested payload
                    } else {
                        return value == node.name && node.args.empty();
                    }
                    if (tag != node.name) return false;
                    if (node.args.size() != payloads.size()) return false;
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        if (!try_match(node.args[i], payloads[i], caseEnv)) return false;
                    }
                    return true;
                } else if constexpr (std::is_same_v<N, PatOr>) {
                    for (const auto& alt : node.alts) {
                        Env altEnv = caseEnv;
                        if (try_match(alt, value, altEnv)) {
                            caseEnv = std::move(altEnv);
                            return true;
                        }
                    }
                    return false;
                } else {
                    return false;
                }
            }, pat->node);
        };

        for (const auto& c : ms.cases) {
            if (top_level_ctor_rejects(c.pattern)) continue;

            const bool needsBind = c.guard || pattern_binds(c.pattern);
            if (!needsBind) {
                if (c.pattern && !try_match(c.pattern, sel, const_cast<Env&>(env))) continue;
                if (c.body) exec_block(*c.body, program, ctx, const_cast<Env&>(env));
                if (ctx.breakSignal) ctx.breakSignal = false;
                return;
            }

            Env caseEnv = env;
            if (!try_match(c.pattern, sel, caseEnv)) continue;
            if (c.guard) {
                if (!is_truthy(to_display_string(eval_value(*c.guard, caseEnv)))) continue;
            }
            if (c.body) exec_block(*c.body, program, ctx, caseEnv);
            for (auto& kv : caseEnv.vars) {
                if (env.vars.count(kv.first) || globalNames_.count(kv.first)) {
                    env_set(env, kv.first, kv.second);
                }
            }
            if (ctx.breakSignal) ctx.breakSignal = false;
            return;
        }
        throw std::runtime_error("non-exhaustive match at runtime");
    }
    if (std::holds_alternative<ExprStmt>(s)) {
        const auto& st = std::get<ExprStmt>(s);
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
                auto binExpr = std::make_shared<Expr>(Expr{ BinaryExpr{ ca.op, std::make_shared<Expr>(Expr{ExprIdent{*name}}), ca.right } });
                env_set(env, *name, eval_value(*binExpr, env));
            } else {
                (void)eval_value(*st.expr, env);
            }
            return;
        }
        (void)eval_value(*st.expr, env);
        return;
    }
    if (std::holds_alternative<SetStmt>(s)) {
        const auto& st = std::get<SetStmt>(s);
        if (st.isMember) {
            if (resolve_builtin_module_method(program, st.objectName, st.varOrField).has_value()) {
                throw std::runtime_error("Cannot assign to builtin module alias: " + st.objectName + "." + st.varOrField);
            }
            auto it = env.objects.find(st.objectName);
            if (it == env.objects.end()) {
                auto structIt = env.vars.find(st.objectName);
                if (structIt != env.vars.end() && structIt->second.rfind("dict:", 0) == 0) {
                    int id = to_int(structIt->second.substr(5));
                    std::string val = eval_string(*st.value, env);
                    g_dicts[id][st.varOrField] = val;
                    return;
                }
                if (structIt != env.vars.end() && structIt->second.rfind("struct:", 0) == 0) {
                    std::string val = eval_string(*st.value, env);
                    env_set(env, st.objectName + "." + st.varOrField, value_from_legacy_string(std::move(val)));
                    return;
                }
                throw std::runtime_error("Unknown object: " + st.objectName);
            }
            std::string val = eval_string(*st.value, env);
            it->second->fields[st.varOrField] = val;
            if (st.objectName == "self") {
                env_set(env, st.varOrField, value_from_legacy_string(std::move(val)));
            }
        } else {
            if (std::holds_alternative<NewExpr>(st.value->node)) {
                const auto& ne = std::get<NewExpr>(st.value->node);
                const Entity* ent = find_entity(program, ne.typeName);
                if (!ent) throw std::runtime_error("Unknown entity: " + ne.typeName);
                if (program.strict && ent->visibility != Visibility::Public)
                    throw std::runtime_error("Entity not public: " + ne.typeName);
                auto obj = std::make_shared<Object>();
                obj->typeName = ne.typeName;
                for (const auto& f : ent->fields) {
                    obj->fields[f.name] = f.defaultValue ? eval_string(*f.defaultValue, env) : std::string{};
                }
                if (const Action* init = find_entity_method(*ent, "init")) {
                    Env selfEnv;
                    for (size_t i = 0; i < init->params.size() && i < ne.args.size(); ++i)
                        selfEnv.vars[init->params[i].name] = eval_string(*ne.args[i], env);
                    selfEnv.objects["self"] = obj;
                    for (const auto& kv : obj->fields) selfEnv.vars[kv.first] = kv.second;
                    ExecContext child;
                    exec_block(init->body, program, child, selfEnv);
                    for (auto& th : child.threads) if (th.joinable()) th.join();
                    for (auto& f : obj->fields) {
                        if (auto vit = selfEnv.vars.find(f.first); vit != selfEnv.vars.end()) f.second = vit->second;
                    }
                }
                env.objects[st.varOrField] = obj;
                env_set(env, st.varOrField, Value::from_string(st.varOrField));
                return;
            }
            {
                Value value;
                bool movedOwn = false;
                if (std::holds_alternative<ExprIdent>(st.value->node)) {
                    const std::string& srcName = std::get<ExprIdent>(st.value->node).name;
                    if (mem_try_move_own_ident(env.vars, srcName, value)) movedOwn = true;
                }
                if (!movedOwn) {
                    value = eval_value(*st.value, env);
                    if (value_is_handle(value, HandleKind::Own) &&
                        std::holds_alternative<ExprIdent>(st.value->node)) {
                        env_set(env, std::get<ExprIdent>(st.value->node).name, Value::null_value());
                    } else if (value_is_handle(value, HandleKind::Shared) &&
                               std::holds_alternative<ExprIdent>(st.value->node)) {
                        mem_retain(value);
                    }
                }
                Value prev = env_get(env, st.varOrField);
                if (mem_is_owner(prev) || value_is_handle(prev, HandleKind::Weak)) mem_release(prev);
                env_set(env, st.varOrField, std::move(value));
            }
            if (!lastReturnFields_.empty()) {
                Value assigned = env_get(env, st.varOrField);
                if (assigned.rfind("struct:", 0) == 0) {
                    for (const auto& kv : lastReturnFields_)
                        env_set(env, st.varOrField + "." + kv.first, kv.second);
                }
                lastReturnFields_.clear();
            }
        }
        return;
    }
    if (std::holds_alternative<MethodCallStmt>(s)) {
        const auto& mc = std::get<MethodCallStmt>(s);
        std::string methodName = mc.method;
        {
            Value recv = env_get(env, mc.objectName);
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
        if (auto moduleBuiltin = env.vars.find(mc.objectName + "." + mc.method); moduleBuiltin != env.vars.end()) {
            if (moduleBuiltin->second.rfind(kBuiltinAliasPrefix.data(), 0) == 0) {
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
                for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
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

        auto vhit = env.vars.find(mc.objectName);
        if (vhit != env.vars.end()) {
            const std::string& handle = vhit->second;
            std::string listMethod = methodName;
            if (listMethod == "append" || listMethod == "push_back" || listMethod == "emplace_back" || listMethod == "emplace") listMethod = "push";
            if (listMethod == "pop_back") listMethod = "pop";
            if (listMethod == "remove_at" || listMethod == "remove") listMethod = "erase";
            if (listMethod == "length") listMethod = "len";
            if (listMethod == "at") listMethod = "get";
            if (listMethod == "first") listMethod = "front";
            if (listMethod == "last") listMethod = "back";

            std::string mapMethod = methodName;
            if (mapMethod == "put" || mapMethod == "insert" || mapMethod == "emplace" || mapMethod == "try_emplace" || mapMethod == "insert_or_assign") mapMethod = "set";
            if (mapMethod == "contains" || mapMethod == "containsKey" || mapMethod == "count") mapMethod = "has";
            if (mapMethod == "getOrDefault" || mapMethod == "get_or" || mapMethod == "get_or_default") mapMethod = "getOr";
            if (mapMethod == "length") mapMethod = "len";
            if (mapMethod == "at") mapMethod = "get";
            if (mapMethod == "erase") mapMethod = "remove";
            if (mapMethod == "set_path") mapMethod = "set";
            if (mapMethod == "get_path") mapMethod = "get";
            if (mapMethod == "has_path") mapMethod = "has";
            if (mapMethod == "remove_path") mapMethod = "remove";

            if (handle.rfind("list:", 0) == 0 && listMethod == "forEach") {
                if (mc.args.empty()) return;
                int id = to_int(handle.substr(5));
                // forEach(actionName)
                std::string actionName = eval_string(*mc.args[0], env);
                for (const auto& item : g_lists[id]) {
                    if (const Action* a = find_action(program, actionName)) {
                        Env callee; callee.vars["item"] = item; exec_block(a->body, program, ctx, callee);
                    }
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "push") {

                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::string v = eval_string(*mc.args[0], env);
                    g_lists[id].push_back(v);
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "insert") {
                int id = to_int(handle.substr(5));
                if (mc.args.size() >= 2) {
                    int idx = to_int(eval_string(*mc.args[0], env));
                    std::string value = eval_string(*mc.args[1], env);
                    auto& vec = g_lists[id];
                    if (idx < 0) idx = 0;
                    if (idx > static_cast<int>(vec.size())) idx = static_cast<int>(vec.size());
                    vec.insert(vec.begin() + idx, value);
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "set") {
                int id = to_int(handle.substr(5));
                if (mc.args.size() >= 2) {
                    int idx = to_int(eval_string(*mc.args[0], env));
                    std::string value = eval_string(*mc.args[1], env);
                    auto& vec = g_lists[id];
                    if (idx >= 0 && idx < static_cast<int>(vec.size())) {
                        vec[idx] = value;
                    }
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "get") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    int idx = to_int(eval_string(*mc.args[0], env));
                    auto& vec = g_lists[id];
                    if (idx >= 0 && idx < (int)vec.size()) {
                        // Write into a special var `_` to return a value (printing via print `_`)
                        env.vars["_"] = vec[idx];
                    } else {
                        env.vars["_"] = std::string();
                    }
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "pop") {
                int id = to_int(handle.substr(5));
                auto& vec = g_lists[id];
                if (!vec.empty()) {
                    env.vars["_"] = vec.back();
                    vec.pop_back();
                } else {
                    env.vars["_"] = std::string();
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "erase") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    int idx = to_int(eval_string(*mc.args[0], env));
                    auto& vec = g_lists[id];
                    if (idx >= 0 && idx < static_cast<int>(vec.size())) {
                        vec.erase(vec.begin() + idx);
                        env.vars["_"] = "true";
                    } else {
                        env.vars["_"] = "false";
                    }
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "clear") {
                int id = to_int(handle.substr(5));
                g_lists[id].clear();
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "len") {
                int id = to_int(handle.substr(5));
                env.vars["_"] = std::to_string((int)g_lists[id].size());
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "capacity") {
                int id = to_int(handle.substr(5));
                env.vars["_"] = std::to_string((int)g_lists[id].size());
                return;
            }
            if (handle.rfind("list:", 0) == 0 && (listMethod == "reserve" || listMethod == "shrink_to_fit")) {
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "empty") {
                int id = to_int(handle.substr(5));
                env.vars["_"] = g_lists[id].empty() ? "true" : "false";
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "front") {
                int id = to_int(handle.substr(5));
                auto& vec = g_lists[id];
                env.vars["_"] = vec.empty() ? std::string() : vec.front();
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "back") {
                int id = to_int(handle.substr(5));
                auto& vec = g_lists[id];
                env.vars["_"] = vec.empty() ? std::string() : vec.back();
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "contains") {
                int id = to_int(handle.substr(5));
                std::string needle = mc.args.empty() ? std::string() : eval_string(*mc.args[0], env);
                auto& vec = g_lists[id];
                const bool found = std::find(vec.begin(), vec.end(), needle) != vec.end();
                env.vars["_"] = found ? "true" : "false";
                return;
            }
            if (handle.rfind("list:", 0) == 0 && (listMethod == "find" || listMethod == "index_of")) {
                int id = to_int(handle.substr(5));
                std::string needle = mc.args.empty() ? std::string() : eval_string(*mc.args[0], env);
                auto& vec = g_lists[id];
                auto hit = std::find(vec.begin(), vec.end(), needle);
                env.vars["_"] = (hit == vec.end()) ? "-1" : std::to_string(static_cast<int>(hit - vec.begin()));
                return;
            }
            if (handle.rfind("list:", 0) == 0 && listMethod == "join") {
                int id = to_int(handle.substr(5));
                std::string sep = mc.args.empty() ? std::string() : eval_string(*mc.args[0], env);
                std::ostringstream out;
                const auto& vec = g_lists[id];
                for (size_t i = 0; i < vec.size(); ++i) {
                    if (i) out << sep;
                    out << vec[i];
                }
                env.vars["_"] = out.str();
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "forEach") {
                if (mc.args.empty()) return;
                int id = to_int(handle.substr(5));
                std::string actionName = eval_string(*mc.args[0], env);
                for (const auto& kv : g_dicts[id]) {
                    if (const Action* a = find_action(program, actionName)) {
                        Env callee; callee.vars["key"] = kv.first; callee.vars["value"] = kv.second; exec_block(a->body, program, ctx, callee);
                    }
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "set") {
                int id = to_int(handle.substr(5));
                if (mc.args.size() >= 2) {
                    std::ostringstream key;
                    for (size_t i = 0; i + 1 < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    std::string v = eval_string(*mc.args[mc.args.size() - 1], env);
                    g_dicts[id][k] = v;
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "get") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::ostringstream key;
                    for (size_t i = 0; i < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    auto it = g_dicts[id].find(k);
                    env.vars["_"] = (it != g_dicts[id].end()) ? it->second : std::string();
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "has") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::ostringstream key;
                    for (size_t i = 0; i < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    env.vars["_"] = (g_dicts[id].count(k) ? "true" : "false");
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "getOr") {
                int id = to_int(handle.substr(5));
                if (mc.args.size() >= 2) {
                    std::ostringstream key;
                    for (size_t i = 0; i + 1 < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    std::string def = eval_string(*mc.args[mc.args.size() - 1], env);
                    auto it = g_dicts[id].find(k);
                    env.vars["_"] = (it != g_dicts[id].end()) ? it->second : def;
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "remove") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::ostringstream key;
                    for (size_t i = 0; i < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    env.vars["_"] = g_dicts[id].erase(k) ? "true" : "false";
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "clear") {
                int id = to_int(handle.substr(5));
                g_dicts[id].clear();
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && (mapMethod == "size" || mapMethod == "len")) {
                int id = to_int(handle.substr(5));
                env.vars["_"] = std::to_string((int)g_dicts[id].size());
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "empty") {
                int id = to_int(handle.substr(5));
                env.vars["_"] = g_dicts[id].empty() ? "true" : "false";
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "keys") {
                int id = to_int(handle.substr(5));
                int lid = g_nextListId++;
                g_lists[lid] = {};
                for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.first);
                env.vars["_"] = std::string("list:") + std::to_string(lid);
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "values") {
                int id = to_int(handle.substr(5));
                int lid = g_nextListId++;
                g_lists[lid] = {};
                for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.second);
                env.vars["_"] = std::string("list:") + std::to_string(lid);
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && (mapMethod == "items" || mapMethod == "entries")) {
                int id = to_int(handle.substr(5));
                int lid = g_nextListId++;
                g_lists[lid] = {};
                for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.first + ":" + kv.second);
                env.vars["_"] = std::string("list:") + std::to_string(lid);
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "merge") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::string otherHandle = eval_string(*mc.args[0], env);
                    if (otherHandle.rfind("dict:", 0) == 0) {
                        int sourceId = to_int(otherHandle.substr(5));
                        for (const auto& kv : g_dicts[sourceId]) {
                            g_dicts[id][kv.first] = kv.second;
                        }
                    }
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mapMethod == "clone") {
                int id = to_int(handle.substr(5));
                int cloneId = g_nextDictId++;
                g_dicts[cloneId] = g_dicts[id];
                env.vars["_"] = std::string("dict:") + std::to_string(cloneId);
                return;
            }
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
                for (const auto& v : g_sets[id]) g_lists[lid].push_back(v);
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
                    callArgs.push_back(std::make_shared<Expr>(Expr{ExprString{handle}}));
                    for (const auto& a : mc.args) callArgs.push_back(a);
                    env.vars["_"] = eval_builtin_call(builtinName, callArgs, env, true);
                    return;
                }
            }
            }
            // WebSocket handle dispatch (handle prefix "ws:")
            {
            std::string wsMethod = methodName;
            if (handle.rfind("ws:", 0) == 0) {
                int id = to_int(handle.substr(3));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result;
                if (__erelang_ws_server_try_method(id, wsMethod, args, result)) {
                    env.vars["_"] = result;
                    return;
                }
                result = __erelang_ws_handle_method(id, wsMethod, args);
                env.vars["_"] = result;
                return;
            }
            }
            // HTTP server handle dispatch (handle prefix "http:")
            {
            if (handle.rfind("http:", 0) == 0) {
                int id = to_int(handle.substr(5));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result = __erelang_http_handle_method(const_cast<Runtime*>(this), id, methodName, args);
                env.vars["_"] = result;
                return;
            }
            }
            // Request handle dispatch (handle prefix "req:")
            {
            if (handle.rfind("req:", 0) == 0) {
                int id = to_int(handle.substr(4));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result = __erelang_req_handle_method(id, methodName, args);
                env.vars["_"] = result;
                return;
            }
            }
            // Response handle dispatch (handle prefix "res:")
            {
            if (handle.rfind("res:", 0) == 0) {
                int id = to_int(handle.substr(4));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result = __erelang_res_handle_method(id, methodName, args);
                env.vars["_"] = result;
                return;
            }
            }
            // SSE handle dispatch (handle prefix "sse:")
            {
            if (handle.rfind("sse:", 0) == 0) {
                int id = to_int(handle.substr(4));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result = __erelang_sse_handle_method(id, methodName, args);
                env.vars["_"] = result;
                return;
            }
            }
            // HTTP Response handle dispatch (handle prefix "resp:")
            {
            if (handle.rfind("resp:", 0) == 0) {
                int id = to_int(handle.substr(5));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result = __erelang_resp_handle_method(id, methodName, args);
                env.vars["_"] = result;
                return;
            }
            }
            // Raw TCP handle dispatch (handle prefix "tcp:")
            {
            if (handle.rfind("tcp:", 0) == 0) {
                int id = to_int(handle.substr(4));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result = __erelang_tcp_handle_method(id, methodName, args);
                env.vars["_"] = result;
                return;
            }
            }
            // UDP handle dispatch (handle prefix "udp:")
            {
            if (handle.rfind("udp:", 0) == 0) {
                int id = to_int(handle.substr(4));
                std::vector<std::string> args;
                for (size_t i = 0; i < mc.args.size(); ++i) {
                    args.push_back(eval_string(*mc.args[i], env));
                }
                std::string result = __erelang_udp_handle_method(id, methodName, args);
                env.vars["_"] = result;
                return;
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
                for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
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
        // Seed with current shared globals
        for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
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
    if (std::holds_alternative<ActionCallStmt>(s)) {
        const auto& call = std::get<ActionCallStmt>(s);
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
            // Seed with current shared globals
            for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
            // Bind positional args into parameter names
            for (size_t i=0; i<a->params.size() && i<call.args.size(); ++i) {
                const std::string paramName = a->params[i].name;
                if (std::holds_alternative<ExprIdent>(call.args[i]->node)) {
                    const auto& id = std::get<ExprIdent>(call.args[i]->node).name;
                    Value argValue = env_get(env, id);
                    if (argValue.rfind("struct:", 0) == 0)
                        calleeEnv.vars[paramName] = argValue;
                    else
                        calleeEnv.vars[paramName] = eval_string(*call.args[i], env);
                    auto oit = env.objects.find(id);
                    if (oit != env.objects.end()) {
                        calleeEnv.objects[paramName] = oit->second;
                    }
                    for (const auto& kv : env.vars) {
                        const std::string prefix = id + ".";
                        if (kv.first.rfind(prefix, 0) == 0) {
                            calleeEnv.vars[paramName + kv.first.substr(id.size())] = kv.second;
                        }
                    }
                } else {
                    calleeEnv.vars[paramName] = eval_string(*call.args[i], env);
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
            auto envIt = env.vars.find(call.name);
            if (envIt != env.vars.end() && envIt->second.rfind("func:", 0) == 0) {
                const std::string& handle = envIt->second;
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
                    for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
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
}

void Runtime::exec_block(const Block& b, const Program& program, ExecContext& ctx, Env& env) const {
    const bool dbg = debug_enabled();
    std::unordered_set<std::string> before;
    before.reserve(env.vars.size());
    for (const auto& kv : env.vars) before.insert(kv.first);
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
            for (auto it = env.vars.begin(); it != env.vars.end(); ) {
                if (before.count(it->first)) {
                    ++it;
                    continue;
                }
                if (mem_is_owner(it->second) || value_is_handle(it->second, HandleKind::Weak)) {
                    mem_release(it->second);
                    it = env.vars.data.erase(it);
                } else if (value_is_handle(it->second, HandleKind::File) ||
                           to_display_string(it->second).rfind("file:", 0) == 0) {
                    auto makeArg = [](const Value& v) {
                        return std::make_shared<Expr>(Expr{ ExprString{ to_display_string(v) } });
                    };
                    std::vector<ExprPtr> callArgs{ makeArg(it->second) };
                    (void)eval_builtin_call("file_close", callArgs, env, true);
                    it = env.vars.data.erase(it);
                } else {
                    ++it;
                }
            }
            throw;
        }
        if (ctx.returned || ctx.breakSignal || ctx.continueSignal) {
            break;
        }
    }
    for (auto it = env.vars.begin(); it != env.vars.end(); ) {
        if (before.count(it->first)) {
            ++it;
            continue;
        }
        if (mem_is_owner(it->second) || value_is_handle(it->second, HandleKind::Weak)) {
            mem_release(it->second);
            it = env.vars.data.erase(it);
        } else if (value_is_handle(it->second, HandleKind::File) ||
                   to_display_string(it->second).rfind("file:", 0) == 0) {
            auto makeArg = [](const Value& v) {
                return std::make_shared<Expr>(Expr{ ExprString{ to_display_string(v) } });
            };
            std::vector<ExprPtr> callArgs{ makeArg(it->second) };
            (void)eval_builtin_call("file_close", callArgs, env, true);
            it = env.vars.data.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace erelang
