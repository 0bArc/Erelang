// Statement execution: declarations and assignments (let, set, index and pointer stores).
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

void Runtime::exec_let(const LetStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
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
                    std::vector<Value>* list = nullptr;
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
                        bind_destructure(node.elements[i], to_display_string((*list)[i]));
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

            // Evaluating a struct variable by name bakes it into a dict handle, which
            // would destroy the source; copy from its flat fields instead.
            bool structSource = false;
            if (std::holds_alternative<ExprIdent>(st.value->node)) {
                auto src = env.vars.find(std::get<ExprIdent>(st.value->node).name);
                structSource = src != env.vars.end() && src->second == (std::string("struct:") + sd->name);
            }
            const std::string initValue = structSource ? std::string() : eval_string(*st.value, env);
            if (initValue.rfind("dict:", 0) == 0) {
                const int id = to_int(initValue.substr(5));
                auto dit = g_dicts.find(id);
                if (dit != g_dicts.end()) {
                    for (const auto& f : sd->fields) {
                        auto fit = dit->second.find(f.name);
                        if (fit != dit->second.end()) {
                            env_set(env, st.name + "." + f.name, fit->second);
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
        if (st.sizeExpr) apply_declared_size(st, value, env);
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

void Runtime::exec_pointer_set(const PointerSetStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
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

void Runtime::exec_index_set(const IndexSetStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    const Value object = eval_value(*st.object, env);
    const Value indexValue = eval_value(*st.index, env);
    Value value = eval_value(*st.value, env);
    if (value_is_handle(object, HandleKind::Dict)) {
        auto& dict = g_dicts[static_cast<int>(value_handle_id(object))];
        if (indexValue.kind == ValueKind::String) dict[indexValue.string_ref()] = std::move(value);
        else dict[to_display_string(indexValue)] = std::move(value);
        return;
    }
    const int64_t index = value_as_int(indexValue);
    if (value_is_handle(object, HandleKind::Buffer)) {
        mem_buffer_index_set(object, index, value);
        return;
    }
    if (value_is_handle(object, HandleKind::List)) {
        auto& list = g_lists[static_cast<int>(value_handle_id(object))];
        if (index < 0 || static_cast<std::size_t>(index) >= list.size()) {
            throw std::runtime_error("list index " + std::to_string(index) + " out of bounds (length " +
                                     std::to_string(list.size()) + ")");
        }
        list[static_cast<std::size_t>(index)] = std::move(value);
        return;
    }
    throw std::runtime_error("index assignment requires buffer, list or map");
}

void Runtime::exec_set(const SetStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    if (st.isMember) {
        if (resolve_builtin_module_method(program, st.objectName, st.varOrField).has_value()) {
            throw std::runtime_error("Cannot assign to builtin module alias: " + st.objectName + "." + st.varOrField);
        }
        auto it = env.objects.find(st.objectName);
        if (it == env.objects.end()) {
            const Value* bound = env_find(env, st.objectName);
            const std::string boundText = bound ? to_display_string(*bound) : std::string();
            if (boundText.rfind("dict:", 0) == 0) {
                int id = to_int(boundText.substr(5));
                std::string val = eval_string(*st.value, env);
                g_dicts[id][st.varOrField] = val;
                return;
            }
            if (boundText.rfind("struct:", 0) == 0) {
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

} // namespace erelang
