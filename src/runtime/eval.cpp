// SPDX-License-Identifier: Apache-2.0
// Split from runtime.cpp

#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/runtime_builtins.hpp"
#include "erelang/bytecode.hpp"
#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"

#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace erelang {
namespace {

void collect_action_slot_names(const Block& block,
                               std::vector<std::string>& names,
                               std::unordered_set<std::string>& seen) {
    auto add = [&](const std::string& name) {
        if (name.empty()) return;
        if (!seen.insert(name).second) return;
        names.push_back(name);
    };

    for (const auto& stmt : block.stmts) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, LetStmt>) {
                add(node.name);
            } else if constexpr (std::is_same_v<T, SetStmt>) {
                if (!node.isMember) add(node.varOrField);
            } else if constexpr (std::is_same_v<T, InputStmt>) {
                add(node.name);
            } else if constexpr (std::is_same_v<T, ForInStmt>) {
                add(node.var);
                if (node.valueVar) add(*node.valueVar);
                if (node.body) collect_action_slot_names(*node.body, names, seen);
            } else if constexpr (std::is_same_v<T, ForStmt>) {
                if (node.init) collect_action_slot_names(*node.init, names, seen);
                if (node.step) collect_action_slot_names(*node.step, names, seen);
                if (node.body) collect_action_slot_names(*node.body, names, seen);
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                if (node.thenBlk) collect_action_slot_names(*node.thenBlk, names, seen);
                if (node.elseBlk) collect_action_slot_names(*node.elseBlk, names, seen);
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                if (node.body) collect_action_slot_names(*node.body, names, seen);
            } else if constexpr (std::is_same_v<T, DoWhileStmt>) {
                if (node.body) collect_action_slot_names(*node.body, names, seen);
            } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                if (node.body) collect_action_slot_names(*node.body, names, seen);
            } else if constexpr (std::is_same_v<T, SwitchStmt>) {
                for (const auto& c : node.cases) {
                    if (c.body) collect_action_slot_names(*c.body, names, seen);
                }
                if (node.defaultBlk) collect_action_slot_names(*node.defaultBlk, names, seen);
            } else if constexpr (std::is_same_v<T, MatchStmt>) {
                for (const auto& c : node.cases) {
                    if (c.pattern) {
                        std::function<void(const PatternPtr&)> walk = [&](const PatternPtr& p) {
                            if (!p) return;
                            std::visit([&](const auto& node) {
                                using N = std::decay_t<decltype(node)>;
                                if constexpr (std::is_same_v<N, PatBinding>) {
                                    add(node.name);
                                } else if constexpr (std::is_same_v<N, PatCtor>) {
                                    for (const auto& a : node.args) walk(a);
                                }
                            }, p->node);
                        };
                        walk(c.pattern);
                    }
                    if (c.body) collect_action_slot_names(*c.body, names, seen);
                }
            } else if constexpr (std::is_same_v<T, TryCatchStmt>) {
                add(node.catchVar);
                if (node.tryBlk) collect_action_slot_names(*node.tryBlk, names, seen);
                if (node.catchBlk) collect_action_slot_names(*node.catchBlk, names, seen);
            } else if constexpr (std::is_same_v<T, UnsafeStmt>) {
                if (node.body) collect_action_slot_names(*node.body, names, seen);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ParallelStmt>>) {
                if (node) collect_action_slot_names(node->body, names, seen);
            }
        }, stmt);
    }
}

ValueBinOp to_value_bin_op(BinOp op) {
    switch (op) {
        case BinOp::Add: return ValueBinOp::Add;
        case BinOp::Sub: return ValueBinOp::Sub;
        case BinOp::Mul: return ValueBinOp::Mul;
        case BinOp::Div: return ValueBinOp::Div;
        case BinOp::Mod: return ValueBinOp::Mod;
        case BinOp::Pow: return ValueBinOp::Pow;
        case BinOp::EQ: return ValueBinOp::Eq;
        case BinOp::NE: return ValueBinOp::Ne;
        case BinOp::LT: return ValueBinOp::Lt;
        case BinOp::LE: return ValueBinOp::Le;
        case BinOp::GT: return ValueBinOp::Gt;
        case BinOp::GE: return ValueBinOp::Ge;
        case BinOp::And: return ValueBinOp::And;
        case BinOp::Or: return ValueBinOp::Or;
        case BinOp::Coalesce: return ValueBinOp::Coalesce;
        case BinOp::BitAnd: return ValueBinOp::BitAnd;
        case BinOp::BitXor: return ValueBinOp::BitXor;
        case BinOp::BitOr: return ValueBinOp::BitOr;
        case BinOp::Shl: return ValueBinOp::Shl;
        case BinOp::Shr: return ValueBinOp::Shr;
        case BinOp::StrictEQ: return ValueBinOp::StrictEq;
        case BinOp::StrictNE: return ValueBinOp::StrictNe;
    }
    return ValueBinOp::Add;
}

std::optional<std::pair<int64_t, std::string>> parse_unit_value(const Value& value) {
    if (value.kind != ValueKind::String) return std::nullopt;
    const std::string& text = value.string_ref();
    if (text.empty()) return std::nullopt;

    size_t i = 0;
    if (text[i] == '-' || text[i] == '+') ++i;
    const size_t digitsBegin = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    if (i == digitsBegin || i >= text.size() ||
        !std::isalpha(static_cast<unsigned char>(text[i]))) {
        return std::nullopt;
    }
    for (size_t j = i; j < text.size(); ++j) {
        if (std::isspace(static_cast<unsigned char>(text[j]))) return std::nullopt;
    }
    try {
        return std::make_pair(std::stoll(text.substr(0, i)), text.substr(i));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

Value Runtime::env_get(const Env& env, const std::string& name) const {
    if (env.useSlots) {
        auto slot = env.slotIndex.find(name);
        if (slot != env.slotIndex.end() && slot->second >= 0 &&
            static_cast<size_t>(slot->second) < env.slots.size()) {
            const Value& slotted = env.slots[static_cast<size_t>(slot->second)];
            if (slotted.kind != ValueKind::Null) return slotted;
            if (auto it = env.vars.find(name); it != env.vars.end()) return it->second;
            return slotted;
        }
    }
    if (auto it = env.vars.find(name); it != env.vars.end()) return it->second;
    if (auto it = globalVars_.find(name); it != globalVars_.end()) return it->second;
    return Value::null_value();
}

void Runtime::env_set(Env& env, const std::string& name, Value value) const {
    if (env.useSlots) {
        auto slot = env.slotIndex.find(name);
        if (slot != env.slotIndex.end() && slot->second >= 0) {
            const size_t index = static_cast<size_t>(slot->second);
            if (index >= env.slots.size()) env.slots.resize(index + 1);
            env.slots[index] = value;
            env.vars[name] = value;
            if (globalNames_.count(name)) globalVars_[name] = std::move(value);
            return;
        }
    }
    env.vars[name] = value;
    if (globalNames_.count(name)) globalVars_[name] = std::move(value);
}

void Runtime::prepare_action_slots(Env& env, const Action& action) const {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    names.reserve(action.params.size() + 8);

    for (const auto& param : action.params) {
        if (param.name.empty()) continue;
        if (seen.insert(param.name).second) names.push_back(param.name);
    }
    collect_action_slot_names(action.body, names, seen);

    if (names.empty()) {
        env.useSlots = false;
        env.slotIndex.clear();
        env.slotNames.clear();
        env.slots.clear();
        return;
    }

    env.slotIndex.clear();
    env.slotIndex.reserve(names.size());
    env.slotNames = names;
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        env.slotIndex.emplace(names[static_cast<size_t>(i)], i);
    }

    env.slots.assign(names.size(), Value::null_value());
    for (const auto& name : names) {
        const int idx = env.slotIndex[name];
        if (auto it = env.vars.find(name); it != env.vars.end()) {
            env.slots[static_cast<size_t>(idx)] = it->second;
        } else if (auto git = globalVars_.find(name); git != globalVars_.end()) {
            env.slots[static_cast<size_t>(idx)] = git->second;
        }
    }
    env.useSlots = true;
}

std::optional<ExprPtr> Runtime::parse_interpolation_expr(std::string_view exprText) const {
    const std::string key = trim_copy(exprText);
    if (key.empty()) return std::nullopt;

    {
        std::lock_guard<std::mutex> lock(interpolationExprCacheMutex_);
        auto it = interpolationExprCache_.find(key);
        if (it != interpolationExprCache_.end()) return it->second;
    }

    try {
        std::string script;
        script.reserve(key.size() + 64);
        script += "@erelang\npublic action __fmt {\n  return ";
        script += key;
        script += ";\n}";

        LexerOptions options;
        options.enableDurations = true;
        options.enableUnits = true;
        options.enablePolyIdentifiers = true;
        options.emitDocComments = false;
        options.emitComments = false;
        Lexer lexer(script, options);
        Parser parser(lexer.lex());
        Program program = parser.parse();
        for (const auto& action : program.actions) {
            if (action.name != "__fmt") continue;
            for (const auto& stmt : action.body.stmts) {
                if (!std::holds_alternative<ReturnStmt>(stmt)) continue;
                const auto& ret = std::get<ReturnStmt>(stmt);
                if (!ret.value.has_value() || !(*ret.value)) return std::nullopt;
                ExprPtr parsed = *ret.value;
                std::lock_guard<std::mutex> lock(interpolationExprCacheMutex_);
                interpolationExprCache_[key] = parsed;
                return parsed;
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> Runtime::eval_interpolation_expr(
    std::string_view exprText, const Env& env) const {
    auto parsed = parse_interpolation_expr(exprText);
    if (!parsed.has_value() || !(*parsed)) {
        std::string raw = trim_copy(exprText);
        if (raw.size() > 2 && raw.back() == ')' && raw[raw.size() - 2] == '(') {
            const std::string fn = raw.substr(0, raw.size() - 2);
            if (is_identifier_text(fn)) {
                try {
                    return eval_builtin_call(fn, {}, env);
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
        return std::nullopt;
    }
    try {
        return eval_string(*(*parsed), env);
    } catch (...) {
        return std::nullopt;
    }
}

Value Runtime::eval_value(const Expr& e, const Env& env) const {
    {
        const bool candidate =
            std::holds_alternative<BinaryExpr>(e.node) ||
            std::holds_alternative<UnaryExpr>(e.node) ||
            std::holds_alternative<ExprNumber>(e.node) ||
            std::holds_alternative<ExprBool>(e.node) ||
            std::holds_alternative<ExprString>(e.node) ||
            std::holds_alternative<ExprNull>(e.node);
        if (candidate) {
            Chunk chunk;
            if (try_compile_expr(e, chunk, &env)) {
                return run_chunk(chunk, const_cast<Runtime&>(*this), const_cast<Env&>(env));
            }
        }
    }

    if (std::holds_alternative<ExprString>(e.node)) {
        return Value::from_string(std::get<ExprString>(e.node).v);
    }
    if (std::holds_alternative<ExprNull>(e.node)) return Value::null_value();
    if (std::holds_alternative<ExprNumber>(e.node)) {
        const auto& number = std::get<ExprNumber>(e.node);
        if (number.isFloatLiteral) {
            try {
                return Value::from_float(std::stod(number.raw));
            } catch (...) {
                return Value::from_float(static_cast<double>(number.v));
            }
        }
        return Value::from_int(number.v);
    }
    if (std::holds_alternative<ExprBool>(e.node)) {
        return Value::from_bool(std::get<ExprBool>(e.node).v);
    }
    if (std::holds_alternative<ExprIdent>(e.node)) {
        const std::string& name = std::get<ExprIdent>(e.node).name;
        Value value = env_get(env, name);
        const bool found =
            (env.useSlots && env.slotIndex.count(name)) ||
            env.vars.count(name) || globalVars_.count(name);
        if (found) {
            if (value.rfind("struct:", 0) == 0) {
                const int id = g_nextDictId++;
                auto& dict = g_dicts[id];
                const std::string prefix = name + ".";
                for (const auto& kv : env.vars) {
                    if (kv.first.rfind(prefix, 0) == 0)
                        dict[kv.first.substr(prefix.size())] = to_display_string(kv.second);
                }
                for (const auto& kv : globalVars_) {
                    if (kv.first.rfind(prefix, 0) == 0)
                        dict[kv.first.substr(prefix.size())] = to_display_string(kv.second);
                }
                return make_handle_value(HandleKind::Dict, static_cast<uint32_t>(id));
            }
            return value;
        }
        if (env.objects.find(name) != env.objects.end()) return Value::from_string(name);

        std::string lowered = name;
        for (char& ch : lowered)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lowered == "array") {
            const int id = g_nextListId++;
            g_lists[id] = {};
            return make_handle_value(HandleKind::List, static_cast<uint32_t>(id));
        }
        if (lowered == "map" || lowered == "dictionary" ||
            lowered == "dict" || lowered == "hashmap") {
            const int id = g_nextDictId++;
            g_dicts[id] = {};
            return make_handle_value(HandleKind::Dict, static_cast<uint32_t>(id));
        }
        return Value::from_string(name);
    }
    if (std::holds_alternative<UnaryExpr>(e.node)) {
        const auto& unary = std::get<UnaryExpr>(e.node);
        if (!unary.expr) return Value::null_value();
        if (unary.op == UnOp::Neg)
            return apply_unary(ValueUnOp::Neg, eval_value(*unary.expr, env));
        if (unary.op == UnOp::Not)
            return apply_unary(ValueUnOp::Not, eval_value(*unary.expr, env));
        if (unary.op == UnOp::BitNot)
            return apply_unary(ValueUnOp::BitNot, eval_value(*unary.expr, env));

        Value value = eval_value(*unary.expr, env);
        const std::string text = to_display_string(value);
        if (unary.op == UnOp::Deref) {
            auto dereference_name = [&](const std::string& ref) -> Value {
                if (ref.rfind("ref:", 0) == 0) return env_get(env, ref.substr(4));
                return Value::null_value();
            };
            if (text.rfind("ref:", 0) == 0) return dereference_name(text);
            if (auto id = parse_pointer_handle(text); id.has_value()) {
                auto it = g_ptrs.find(*id);
                if (it == g_ptrs.end()) return Value::null_value();
                if (it->second.rfind("ref:", 0) == 0) return dereference_name(it->second);
                return value_from_legacy_string(it->second);
            }
            return Value::null_value();
        }
        if (std::holds_alternative<ExprIdent>(unary.expr->node)) {
            const std::string& name = std::get<ExprIdent>(unary.expr->node).name;
            const int id = g_nextPtrId++;
            g_ptrs[id] = "ref:" + name;
            return make_handle_value(HandleKind::Ptr, static_cast<uint32_t>(id));
        }
        const int id = g_nextPtrId++;
        g_ptrs[id] = text;
        return make_handle_value(HandleKind::Ptr, static_cast<uint32_t>(id));
    }
    if (std::holds_alternative<BinaryExpr>(e.node)) {
        const auto& binary = std::get<BinaryExpr>(e.node);
        if (!binary.left || !binary.right) return Value::null_value();
        Value left = eval_value(*binary.left, env);
        Value right = eval_value(*binary.right, env);
        if (binary.op == BinOp::Add || binary.op == BinOp::Sub) {
            auto leftUnit = parse_unit_value(left);
            auto rightUnit = parse_unit_value(right);
            if (leftUnit && rightUnit && leftUnit->second == rightUnit->second) {
                const int64_t result = binary.op == BinOp::Add
                    ? leftUnit->first + rightUnit->first
                    : leftUnit->first - rightUnit->first;
                return Value::from_string(std::to_string(result) + leftUnit->second);
            }
        }
        return apply_binary(to_value_bin_op(binary.op), left, right);
    }
    if (std::holds_alternative<TernaryExpr>(e.node)) {
        const auto& ternary = std::get<TernaryExpr>(e.node);
        if (!ternary.cond) return Value::null_value();
        return is_truthy(eval_value(*ternary.cond, env))
            ? eval_value(*ternary.thenExpr, env)
            : eval_value(*ternary.elseExpr, env);
    }
    if (std::holds_alternative<MemberExpr>(e.node)) {
        const auto& member = std::get<MemberExpr>(e.node);
        if (currentProgram_) {
            TypeRef applied;
            try { applied = parse_type_ref_string(member.objectName); } catch (...) { applied = make_type_ref(member.objectName); }
            for (const auto& en : currentProgram_->enums) {
                if (en.name != applied.name && member.objectName != en.name) continue;
                for (const auto& variant : en.variants) {
                    if (variant.name == member.field && variant.payloads.empty()) {
                        return Value::from_string(variant.name);
                    }
                }
            }
        }
        if (auto object = env.objects.find(member.objectName); object != env.objects.end()) {
            auto field = object->second->fields.find(member.field);
            if (field != object->second->fields.end())
                return value_from_legacy_string(field->second);
        }
        Value objectValue = env_get(env, member.objectName);
        const std::string objectText = to_display_string(objectValue);
        if (objectText.rfind("dict:", 0) == 0) {
            const int id = static_cast<int>(to_int(objectText.substr(5)));
            auto dict = g_dicts.find(id);
            if (dict != g_dicts.end()) {
                auto field = dict->second.find(member.field);
                if (field != dict->second.end()) return value_from_legacy_string(field->second);
            }
        }
        return env_get(env, member.objectName + "." + member.field);
    }
    if (std::holds_alternative<IndexExpr>(e.node)) {
        const auto& index = std::get<IndexExpr>(e.node);
        const std::string container = to_display_string(eval_value(*index.object, env));
        const Value indexValue = eval_value(*index.index, env);
        if (container.rfind("list:", 0) == 0) {
            const int id = static_cast<int>(to_int(container.substr(5)));
            const int position = static_cast<int>(value_as_int(indexValue));
            auto list = g_lists.find(id);
            if (list != g_lists.end() && position >= 0 &&
                position < static_cast<int>(list->second.size())) {
                return value_from_legacy_string(list->second[static_cast<size_t>(position)]);
            }
            return Value::from_int(0);
        }
        if (container.rfind("dict:", 0) == 0) {
            const int id = static_cast<int>(to_int(container.substr(5)));
            auto dict = g_dicts.find(id);
            if (dict != g_dicts.end()) {
                auto item = dict->second.find(to_display_string(indexValue));
                if (item != dict->second.end()) return value_from_legacy_string(item->second);
            }
        }
        return Value::null_value();
    }
    if (std::holds_alternative<FunctionCallExpr>(e.node)) {
        const auto& call = std::get<FunctionCallExpr>(e.node);
        if (currentProgram_) {
            // Enum variant construction: Option<int>.Some(42)
            {
                const auto dot = call.name.rfind('.');
                if (dot != std::string::npos && dot > 0) {
                    const std::string typePart = call.name.substr(0, dot);
                    const std::string variantName = call.name.substr(dot + 1);
                    TypeRef applied;
                    try { applied = parse_type_ref_string(typePart); } catch (...) { applied = make_type_ref(typePart); }
                    for (const auto& en : currentProgram_->enums) {
                        if (en.name != applied.name && typePart != en.name) continue;
                        for (const auto& variant : en.variants) {
                            if (variant.name != variantName) continue;
                            if (variant.payloads.empty() && call.args.empty()) {
                                return Value::from_string(variantName);
                            }
                            std::vector<std::string> payloads;
                            payloads.reserve(call.args.size());
                            for (const auto& arg : call.args) {
                                payloads.push_back(to_display_string(eval_value(*arg, env)));
                            }
                            return Value::from_string(encode_enum_variant(variantName, payloads));
                        }
                    }
                }
            }
            const auto dot = call.name.rfind('.');
            if (dot != std::string::npos && dot > 0 && dot + 1 < call.name.size()) {
                const std::string objectName = call.name.substr(0, dot);
                const std::string methodName = call.name.substr(dot + 1);
                auto invoke_entity_method = [&](ObjPtr object) -> std::optional<Value> {
                    const Entity* entity = find_entity(*currentProgram_, object->typeName);
                    if (!entity) return std::nullopt;
                    const Action* method = find_entity_method(*entity, methodName);
                    if (!method) return std::nullopt;
                    Env callEnv;
                    for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                    for (size_t i = 0; i < method->params.size() && i < call.args.size(); ++i)
                        callEnv.vars[method->params[i].name] = eval_value(*call.args[i], env);
                    callEnv.objects["self"] = object;
                    for (const auto& kv : object->fields) callEnv.vars[kv.first] = kv.second;
                    ExecContext child;
                    exec_block(method->body, *currentProgram_, child, callEnv);
                    for (auto& thread : child.threads) if (thread.joinable()) thread.join();
                    for (auto& field : object->fields) {
                        auto value = callEnv.vars.find(field.first);
                        if (value != callEnv.vars.end())
                            field.second = to_display_string(value->second);
                    }
                    return child.returned ? child.returnValue : Value::null_value();
                };
                if (auto object = env.objects.find(objectName); object != env.objects.end()) {
                    if (auto result = invoke_entity_method(object->second)) return *result;
                }
                if (objectName == "self") {
                    if (auto object = env.objects.find("self"); object != env.objects.end()) {
                        if (auto result = invoke_entity_method(object->second)) return *result;
                    }
                }
                Value objectValue = env_get(env, objectName);
                if (objectValue.rfind("struct:", 0) == 0) {
                    const std::string structName = objectValue.substr(7);
                    const StructDecl* decl = find_struct_decl(*currentProgram_, structName);
                    const Action* method = decl ? find_struct_method(*decl, methodName) : nullptr;
                    if (method) {
                        Env callEnv;
                        for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                        for (size_t i = 0; i < method->params.size() && i < call.args.size(); ++i) {
                            const std::string& parameter = method->params[i].name;
                            if (std::holds_alternative<ExprIdent>(call.args[i]->node)) {
                                const std::string argName = std::get<ExprIdent>(call.args[i]->node).name;
                                Value argValue = env_get(env, argName);
                                if (argValue.rfind("struct:", 0) == 0)
                                    callEnv.vars[parameter] = argValue;
                                else
                                    callEnv.vars[parameter] = eval_value(*call.args[i], env);
                                for (const auto& field : decl->fields) {
                                    callEnv.vars[parameter + "." + field.name] =
                                        env_get(env, argName + "." + field.name);
                                }
                            } else {
                                callEnv.vars[parameter] = eval_value(*call.args[i], env);
                            }
                        }
                        callEnv.vars["self"] = objectValue;
                        for (const auto& field : decl->fields) {
                            Value value = env_get(env, objectName + "." + field.name);
                            callEnv.vars[field.name] = value;
                            callEnv.vars["self." + field.name] = value;
                        }
                        ExecContext child;
                        exec_block(method->body, *currentProgram_, child, callEnv);
                        for (auto& thread : child.threads) if (thread.joinable()) thread.join();
                        lastReturnFields_ = std::move(child.returnFields);
                        return child.returned ? child.returnValue : Value::null_value();
                    }
                }
            }
            if (const StructDecl* decl = find_struct_decl(*currentProgram_, call.name)) {
                return Value::from_string("struct:" + decl->name);
            }
            if (const Action* action = find_action(*currentProgram_, call.name)) {
                if (currentProgram_->strict && action->visibility != Visibility::Public)
                    throw std::runtime_error("Action not public: " + action->name);
                Env callEnv;
                for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                for (size_t i = 0; i < action->params.size() && i < call.args.size(); ++i) {
                    const std::string& parameter = action->params[i].name;
                    if (std::holds_alternative<ExprIdent>(call.args[i]->node)) {
                        const std::string& argument =
                            std::get<ExprIdent>(call.args[i]->node).name;
                        Value argValue = env_get(env, argument);
                        if (argValue.rfind("struct:", 0) == 0)
                            callEnv.vars[parameter] = argValue;
                        else
                            callEnv.vars[parameter] = eval_value(*call.args[i], env);
                        auto object = env.objects.find(argument);
                        if (object != env.objects.end()) callEnv.objects[parameter] = object->second;
                        for (const auto& kv : env.vars) {
                            const std::string prefix = argument + ".";
                            if (kv.first.rfind(prefix, 0) == 0) {
                                callEnv.vars[parameter + kv.first.substr(argument.size())] = kv.second;
                            }
                        }
                    } else {
                        callEnv.vars[parameter] = eval_value(*call.args[i], env);
                    }
                }
                prepare_action_slots(callEnv, *action);
                ExecContext child;
                exec_block(action->body, *currentProgram_, child, callEnv);
                for (auto& thread : child.threads) if (thread.joinable()) thread.join();
                lastReturnFields_ = std::move(child.returnFields);
                return child.returned ? child.returnValue : Value::null_value();
            }
        }

        Value function = env_get(env, call.name);
        const std::string handle = to_display_string(function);
        if (handle.rfind("func:", 0) == 0) {
            int id = 0;
            try {
                id = std::stoi(handle.substr(5));
            } catch (...) {
                return Value::null_value();
            }
            auto closure = g_closures.find(id);
            if (closure != g_closures.end() && closure->second && currentProgram_) {
                ClosureData* data = closure->second;
                Env callEnv;
                for (const auto& captured : data->captured)
                    callEnv.vars[captured.name] = captured.value;
                for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                for (size_t i = 0; i < data->body.params.size() && i < call.args.size(); ++i)
                    callEnv.vars[data->body.params[i].name] = eval_value(*call.args[i], env);
                prepare_action_slots(callEnv, data->body);
                ExecContext child;
                exec_block(data->body.body, *currentProgram_, child, callEnv);
                return child.returned ? child.returnValue : Value::null_value();
            }
            return Value::null_value();
        }
        return value_from_legacy_string(eval_builtin_call(call.name, call.args, env, true));
    }
    if (std::holds_alternative<ListLiteralExpr>(e.node)) {
        const auto& literal = std::get<ListLiteralExpr>(e.node);
        return value_from_legacy_string(eval_builtin_call("list_new", literal.elements, env, true));
    }
    if (std::holds_alternative<DictLiteralExpr>(e.node)) {
        const auto& literal = std::get<DictLiteralExpr>(e.node);
        return value_from_legacy_string(eval_builtin_call("dict_new", literal.entries, env, true));
    }
    if (std::holds_alternative<LambdaExpr>(e.node)) {
        const auto& lambda = std::get<LambdaExpr>(e.node);
        const int closureId = g_nextClosureId++;
        ClosureData* data = new ClosureData();
        data->refCount = 1;
        data->sourcePath = "<lambda>";
        data->sourceLine = 0;
        data->body.name = "lambda_" + std::to_string(closureId);
        data->body.params = lambda.params;
        data->body.returnType = lambda.returnType;
        data->body.visibility = Visibility::Public;
        data->body.exported = false;
        data->body.isAsync = false;
        if (lambda.isArrow && lambda.body)
            data->body.body.stmts.push_back(ReturnStmt{lambda.body});
        else
            data->body.body = lambda.blockBody;
        for (const std::string& name : lambda.capturedVars) {
            ClosedVar captured;
            captured.name = name;
            const bool found =
                (env.useSlots && env.slotIndex.count(name)) ||
                env.vars.count(name) || globalVars_.count(name);
            captured.value = found ? to_display_string(env_get(env, name)) : std::string{};
            captured.type = found ? "string" : "unknown";
            data->captured.push_back(std::move(captured));
        }
        g_closures[closureId] = data;
        return make_handle_value(HandleKind::Func, static_cast<uint32_t>(closureId));
    }
    if (std::holds_alternative<NewExpr>(e.node)) {
        return Value::from_string("<new:" + std::get<NewExpr>(e.node).typeName + ">");
    }
    if (std::holds_alternative<PostfixExpr>(e.node)) {
        const auto& postfix = std::get<PostfixExpr>(e.node);
        return postfix.operand ? eval_value(*postfix.operand, env) : Value::null_value();
    }
    if (std::holds_alternative<PrefixExpr>(e.node)) {
        const auto& prefix = std::get<PrefixExpr>(e.node);
        if (!prefix.operand) return Value::null_value();
        Value current = eval_value(*prefix.operand, env);
        if (current.kind == ValueKind::Int)
            return Value::from_int(current.i + (prefix.isInc ? 1 : -1));
        if (current.kind == ValueKind::Float)
            return Value::from_float(current.f + (prefix.isInc ? 1.0 : -1.0));
        return current;
    }
    if (std::holds_alternative<CompoundAssignExpr>(e.node)) {
        const auto& assign = std::get<CompoundAssignExpr>(e.node);
        if (!assign.left || !assign.right) return Value::null_value();
        return apply_binary(
            to_value_bin_op(assign.op),
            eval_value(*assign.left, env),
            eval_value(*assign.right, env));
    }
    return Value::null_value();
}

std::string Runtime::eval_string(const Expr& e, const Env& env) const {
    if (!std::holds_alternative<ExprString>(e.node))
        return to_display_string(eval_value(e, env));

    const std::string& input = std::get<ExprString>(e.node).v;
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '{') {
            const size_t close = input.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string raw = input.substr(i + 1, close - i - 1);
                const std::string key = trim_copy(raw);
                const bool found =
                    (env.useSlots && env.slotIndex.count(key)) ||
                    env.vars.count(key) || globalVars_.count(key);
                if (found) {
                    out += to_display_string(env_get(env, key));
                } else if (auto value = eval_interpolation_expr(key, env); value.has_value()) {
                    out += *value;
                } else {
                    out += '{';
                    out += raw;
                    out += '}';
                }
                i = close + 1;
                continue;
            }
        }
        out.push_back(input[i++]);
    }
    return out;
}

} // namespace erelang
