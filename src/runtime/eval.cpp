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

thread_local int tls_asyncDepth = 0;
thread_local bool tls_in_async_action = false;

} // namespace

void async_root_enter() {
    ++tls_asyncDepth;
    tls_in_async_action = true;
}
void async_root_leave() {
    if (tls_asyncDepth > 0) --tls_asyncDepth;
    if (tls_asyncDepth == 0) tls_in_async_action = false;
}
bool async_in_async_action() { return tls_in_async_action; }
void async_set_in_async_action(bool value) { tls_in_async_action = value; }

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
                if (node.pattern) {
                    std::function<void(const PatternPtr&)> walk = [&](const PatternPtr& p) {
                        if (!p) return;
                        std::visit([&](const auto& n) {
                            using N = std::decay_t<decltype(n)>;
                            if constexpr (std::is_same_v<N, PatBinding>) {
                                add(n.name);
                            } else if constexpr (std::is_same_v<N, PatCtor>) {
                                for (const auto& a : n.args) walk(a);
                            } else if constexpr (std::is_same_v<N, PatStruct>) {
                                for (const auto& f : n.fields) walk(f.pattern);
                            } else if constexpr (std::is_same_v<N, PatArray>) {
                                for (const auto& e : n.elements) walk(e);
                            } else if constexpr (std::is_same_v<N, PatTuple>) {
                                for (const auto& e : n.elements) walk(e);
                            }
                        }, p->node);
                    };
                    walk(node.pattern);
                } else {
                    add(node.name);
                }
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
                                } else if constexpr (std::is_same_v<N, PatStruct>) {
                                    for (const auto& f : node.fields) walk(f.pattern);
                                } else if constexpr (std::is_same_v<N, PatArray>) {
                                    for (const auto& e : node.elements) walk(e);
                                } else if constexpr (std::is_same_v<N, PatTuple>) {
                                    for (const auto& e : node.elements) walk(e);
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

const char* operator_method_name(BinOp op) {
    switch (op) {
        case BinOp::Add: return "add";
        case BinOp::Sub: return "sub";
        case BinOp::Mul: return "mul";
        case BinOp::Div: return "div";
        case BinOp::EQ:
        case BinOp::NE: return "eq";
        default: return nullptr;
    }
}

Value lookup_prefixed_field(const Runtime::Env& env,
                            const std::unordered_map<std::string, Value>& fallback,
                            const std::string& prefix,
                            const std::string& field) {
    auto from_env = [&](const std::string& key) -> std::optional<Value> {
        if (env.useSlots) {
            auto slot = env.slotIndex.find(key);
            if (slot != env.slotIndex.end() && slot->second >= 0 &&
                static_cast<size_t>(slot->second) < env.slots.size()) {
                const Value& slotted = env.slots[static_cast<size_t>(slot->second)];
                if (slotted.kind != ValueKind::Null) return slotted;
            }
        }
        auto it = env.vars.find(key);
        if (it != env.vars.end()) return it->second;
        return std::nullopt;
    };
    if (!prefix.empty()) {
        if (auto v = from_env(prefix + "." + field)) return *v;
    }
    auto it = fallback.find(field);
    if (it != fallback.end()) return it->second;
    auto it2 = fallback.find("self." + field);
    if (it2 != fallback.end()) return it2->second;
    return Value::null_value();
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
    if (auto cit = env.cells.find(name); cit != env.cells.end() && cit->second) {
        return *cit->second;
    }
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
    if (auto cit = env.cells.find(name); cit != env.cells.end() && cit->second) {
        *cit->second = value;
        env.vars[name] = value;
        if (env.useSlots) {
            auto slot = env.slotIndex.find(name);
            if (slot != env.slotIndex.end() && slot->second >= 0) {
                const size_t index = static_cast<size_t>(slot->second);
                if (index >= env.slots.size()) env.slots.resize(index + 1);
                env.slots[index] = value;
            }
        }
        if (globalNames_.count(name)) globalVars_[name] = std::move(value);
        return;
    }
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
            if (value_is_handle(value, HandleKind::Own) || value_is_handle(value, HandleKind::Shared) ||
                value_is_handle(value, HandleKind::Ptr) || parse_pointer_handle(text).has_value()) {
                Value got = mem_deref(value);
                if (got.kind == ValueKind::String && got.string_ref().rfind("ref:", 0) == 0) {
                    return dereference_name(got.string_ref());
                }
                return got;
            }
            throw std::runtime_error("dereference of non-pointer");
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
    if (std::holds_alternative<RangeExpr>(e.node)) {
        const auto& range = std::get<RangeExpr>(e.node);
        const int64_t start = value_as_int(eval_value(*range.start, env));
        const int64_t end = value_as_int(eval_value(*range.end, env));
        const int id = g_nextListId++;
        auto& list = g_lists[id];
        if (range.exclusive) {
            for (int64_t i = start; i < end; ++i) list.push_back(std::to_string(i));
        } else {
            for (int64_t i = start; i <= end; ++i) list.push_back(std::to_string(i));
        }
        return make_handle_value(HandleKind::List, static_cast<uint32_t>(id));
    }
    if (std::holds_alternative<BinaryExpr>(e.node)) {
        const auto& binary = std::get<BinaryExpr>(e.node);
        if (!binary.left || !binary.right) return Value::null_value();
        std::string leftName;
        if (std::holds_alternative<ExprIdent>(binary.left->node))
            leftName = std::get<ExprIdent>(binary.left->node).name;
        std::string rightName;
        if (std::holds_alternative<ExprIdent>(binary.right->node))
            rightName = std::get<ExprIdent>(binary.right->node).name;
        Value left = eval_value(*binary.left, env);
        auto leftFields = lastReturnFields_;
        Value right = eval_value(*binary.right, env);
        auto rightFields = lastReturnFields_;
        if ((binary.op == BinOp::Add || binary.op == BinOp::Sub) && mem_is_ptr(left)) {
            const int64_t delta = value_as_int(right);
            return mem_ptr_add(left, binary.op == BinOp::Add ? delta : -delta);
        }
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
        if (const char* methodName = operator_method_name(binary.op)) {
            const StructDecl* decl = nullptr;
            const Action* method = nullptr;
            const std::string leftText = to_display_string(left);
            if (leftText.rfind("struct:", 0) == 0 && currentProgram_) {
                decl = find_struct_decl(*currentProgram_, leftText.substr(7));
                method = decl ? find_struct_method(*decl, methodName) : nullptr;
            }
            if (!method && currentProgram_ && !leftName.empty()) {
                for (const auto& sd : currentProgram_->structs) {
                    const Action* candidate = find_struct_method(sd, methodName);
                    if (!candidate) continue;
                    bool looksLike = false;
                    for (const auto& field : sd.fields) {
                        Value fv = env_get(env, leftName + "." + field.name);
                        if (fv.kind != ValueKind::Null ||
                            (env.useSlots && env.slotIndex.count(leftName + "." + field.name)) ||
                            env.vars.count(leftName + "." + field.name)) {
                            looksLike = true;
                            break;
                        }
                    }
                    if (!looksLike && sd.fields.empty()) looksLike = true;
                    if (!looksLike) continue;
                    decl = &sd;
                    method = candidate;
                    break;
                }
            }
            if (method && decl) {
                Env callEnv;
                for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                callEnv.vars["self"] = leftText.rfind("struct:", 0) == 0
                    ? left
                    : Value::from_string(std::string("struct:") + decl->name);
                for (const auto& field : decl->fields) {
                    Value value = lookup_prefixed_field(env, leftFields, leftName, field.name);
                    if (value.kind == ValueKind::Null && !leftName.empty())
                        value = env_get(env, leftName + "." + field.name);
                    callEnv.vars[field.name] = value;
                    callEnv.vars["self." + field.name] = value;
                }
                if (!method->params.empty()) {
                    const std::string& param = method->params[0].name;
                    callEnv.vars[param] = right;
                    const std::string rightText = to_display_string(right);
                    if (rightText.rfind("struct:", 0) == 0 || !rightName.empty()) {
                        for (const auto& field : decl->fields) {
                            Value value = lookup_prefixed_field(env, rightFields, rightName, field.name);
                            if (value.kind == ValueKind::Null && !rightName.empty())
                                value = env_get(env, rightName + "." + field.name);
                            callEnv.vars[param + "." + field.name] = value;
                        }
                    }
                }
                prepare_action_slots(callEnv, *method);
                ExecContext child;
                exec_block(method->body, *currentProgram_, child, callEnv);
                for (auto& thread : child.threads) if (thread.joinable()) thread.join();
                lastReturnFields_ = std::move(child.returnFields);
                Value result = child.returned ? child.returnValue : Value::null_value();
                if (binary.op == BinOp::NE) {
                    return Value::from_bool(!is_truthy(result));
                }
                return result;
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
        std::string objectName;
        if (std::holds_alternative<ExprIdent>(index.object->node))
            objectName = std::get<ExprIdent>(index.object->node).name;
        Value objectValue = eval_value(*index.object, env);
        auto objectFields = lastReturnFields_;
        const std::string container = to_display_string(objectValue);
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
        if (value_is_handle(objectValue, HandleKind::Buffer)) {
            return mem_buffer_index_get(objectValue, value_as_int(indexValue));
        }
        if (container.rfind("dict:", 0) == 0) {
            const int id = static_cast<int>(to_int(container.substr(5)));
            auto dict = g_dicts.find(id);
            if (dict != g_dicts.end()) {
                auto item = dict->second.find(to_display_string(indexValue));
                if (item != dict->second.end()) return value_from_legacy_string(item->second);
            }
        }
        if (container.rfind("struct:", 0) == 0 && currentProgram_) {
            const StructDecl* decl = find_struct_decl(*currentProgram_, container.substr(7));
            const Action* method = decl ? find_struct_method(*decl, "get") : nullptr;
            if (method) {
                Env callEnv;
                for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                callEnv.vars["self"] = objectValue;
                for (const auto& field : decl->fields) {
                    Value value = lookup_prefixed_field(env, objectFields, objectName, field.name);
                    callEnv.vars[field.name] = value;
                    callEnv.vars["self." + field.name] = value;
                }
                if (!method->params.empty()) {
                    callEnv.vars[method->params[0].name] = indexValue;
                }
                prepare_action_slots(callEnv, *method);
                ExecContext child;
                exec_block(method->body, *currentProgram_, child, callEnv);
                for (auto& thread : child.threads) if (thread.joinable()) thread.join();
                lastReturnFields_ = std::move(child.returnFields);
                return child.returned ? child.returnValue : Value::null_value();
            }
        }
        return Value::null_value();
    }
    if (std::holds_alternative<FunctionCallExpr>(e.node)) {
        const auto& call = std::get<FunctionCallExpr>(e.node);
        if (call.name == "alloc") {
            if (call.typeArgs.empty()) throw std::runtime_error("alloc<T>() requires a type argument");
            std::size_t count = 1;
            if (!call.args.empty()) count = static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(eval_value(*call.args[0], env))));
            return mem_alloc(call.typeArgs[0], count);
        }
        if (call.name == "heap") {
            if (call.typeArgs.empty()) throw std::runtime_error("heap<T>(value) requires a type argument");
            Value payload = call.args.empty() ? mem_zero_value(call.typeArgs[0]) : eval_value(*call.args[0], env);
            return mem_make_own(call.typeArgs[0], std::move(payload));
        }
        if (call.name == "shared") {
            if (call.typeArgs.empty()) throw std::runtime_error("shared<T>(value) requires a type argument");
            Value payload = call.args.empty() ? mem_zero_value(call.typeArgs[0]) : eval_value(*call.args[0], env);
            return mem_make_shared(call.typeArgs[0], std::move(payload));
        }
        if (call.name == "weak") {
            if (call.args.empty()) throw std::runtime_error("weak(shared) requires an argument");
            return mem_make_weak(eval_value(*call.args[0], env));
        }
        if (call.name == "buffer") {
            if (call.typeArgs.empty()) throw std::runtime_error("buffer<T>(n) requires a type argument");
            std::size_t count = 0;
            if (!call.args.empty()) count = static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(eval_value(*call.args[0], env))));
            return mem_make_buffer(call.typeArgs[0], count);
        }
        if (call.name == "free") {
            if (call.args.empty()) throw std::runtime_error("free(ptr) requires a pointer");
            mem_free_ptr(eval_value(*call.args[0], env));
            return Value::null_value();
        }
        if (call.name == "realloc") {
            if (call.args.size() < 2) throw std::runtime_error("realloc(ptr, count)");
            return mem_realloc(eval_value(*call.args[0], env),
                              static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(eval_value(*call.args[1], env)))));
        }
        if (call.name == "copy") {
            if (call.args.size() < 3) throw std::runtime_error("copy(dst, src, count)");
            mem_copy(eval_value(*call.args[0], env), eval_value(*call.args[1], env),
                     static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(eval_value(*call.args[2], env)))));
            return Value::null_value();
        }
        if (call.name == "move" && call.args.size() >= 3) {
            mem_move(eval_value(*call.args[0], env), eval_value(*call.args[1], env),
                     static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(eval_value(*call.args[2], env)))));
            return Value::null_value();
        }
        if (call.name == "fill") {
            if (call.args.size() < 3) throw std::runtime_error("fill(ptr, value, count)");
            mem_fill(eval_value(*call.args[0], env), eval_value(*call.args[1], env),
                     static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(eval_value(*call.args[2], env)))));
            return Value::null_value();
        }
        if (call.name == "zero") {
            if (call.args.size() < 2) throw std::runtime_error("zero(ptr, count)");
            mem_zero(eval_value(*call.args[0], env),
                     static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(eval_value(*call.args[1], env)))));
            return Value::null_value();
        }
        // Chained / dotted enum methods: .method / obj.method
        {
            std::string methodName;
            Value recv;
            size_t argOff = 0;
            bool methodCandidate = false;
            if (!call.name.empty() && call.name[0] == '.' && !call.args.empty()) {
                methodName = call.name.substr(1);
                recv = eval_value(*call.args[0], env);
                argOff = 1;
                methodCandidate = true;
            } else {
                const auto dot = call.name.rfind('.');
                if (dot != std::string::npos) {
                    methodName = call.name.substr(dot + 1);
                    const std::string objectName = call.name.substr(0, dot);
                    if (objectName.find('<') == std::string::npos) {
                        recv = env_get(env, objectName);
                        argOff = 0;
                        methodCandidate = true;
                    }
                }
            }
            if (methodCandidate && currentProgram_) {
                if (value_is_handle(recv, HandleKind::Weak) && methodName == "get") {
                    return mem_weak_get(recv);
                }
                if (value_is_handle(recv, HandleKind::Buffer)) {
                    std::vector<Value> bargs;
                    for (size_t i = argOff; i < call.args.size(); ++i) {
                        bargs.push_back(eval_value(*call.args[i], env));
                    }
                    return mem_buffer_method(recv, methodName, bargs);
                }
                {
                    std::vector<Value> margs;
                    for (size_t i = argOff; i < call.args.size(); ++i) {
                        margs.push_back(eval_value(*call.args[i], env));
                    }
                    if (auto handled = dispatch_value_method(recv, methodName, margs)) {
                        return *handled;
                    }
                }
                const std::string text = to_display_string(recv);
                std::string tag;
                std::vector<std::string> payloads;
                if (!decode_enum_variant(text, tag, payloads)) {
                    tag = text;
                    payloads.clear();
                }
                for (const auto& en : currentProgram_->enums) {
                    bool hasVariant = false;
                    for (const auto& v : en.variants) {
                        if (v.name == tag) { hasVariant = true; break; }
                    }
                    if (!hasVariant) continue;
                    const Action* method = find_enum_method(en, methodName);
                    if (!method) continue;
                    return invoke_enum_method(*method, recv, call.args, argOff, env);
                }
            }
        }
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
                if (action->isAsync) {
                    return invoke_async_action(*action, call.args, env, tls_in_async_action);
                }
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
                const bool prevAsync = tls_in_async_action;
                tls_in_async_action = false;
                ExecContext child;
                exec_block(action->body, *currentProgram_, child, callEnv);
                for (auto& thread : child.threads) if (thread.joinable()) thread.join();
                tls_in_async_action = prevAsync;
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
                for (const auto& captured : data->captured) {
                    if (captured.cell) {
                        callEnv.cells[captured.name] = captured.cell;
                        callEnv.vars[captured.name] = *captured.cell;
                    }
                }
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
    if (std::holds_alternative<TupleLiteralExpr>(e.node)) {
        const auto& literal = std::get<TupleLiteralExpr>(e.node);
        const int id = g_nextTupleId++;
        g_tuples[id] = {};
        for (const auto& el : literal.elements) {
            g_tuples[id].push_back(to_display_string(eval_value(*el, env)));
        }
        return Value::from_string(std::string("tuple:") + std::to_string(id));
    }
    if (std::holds_alternative<TryExpr>(e.node)) {
        const auto& te = std::get<TryExpr>(e.node);
        Value inner = eval_value(*te.inner, env);
        const std::string text = to_display_string(inner);
        std::string tag;
        std::vector<std::string> payloads;
        if (!decode_enum_variant(text, tag, payloads)) {
            tag = text;
            payloads.clear();
        }
        if (tag == "Ok" || tag == "Some") {
            if (payloads.empty()) return Value::from_string("");
            return value_from_legacy_string(payloads[0]);
        }
        if (tag == "Error") {
            throw TryPropagateException{inner};
        }
        if (tag == "None") {
            throw TryPropagateException{Value::from_string("None")};
        }
        throw std::runtime_error("`?` requires Result or Option value, got `" + tag + "`");
    }
    if (std::holds_alternative<AwaitExpr>(e.node)) {
        const auto& ae = std::get<AwaitExpr>(e.node);
        return await_future_value(eval_value(*ae.inner, env));
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
            auto cit = env.cells.find(name);
            if (cit != env.cells.end() && cit->second) {
                captured.cell = cit->second;
            } else {
                auto cell = std::make_shared<Value>(env_get(env, name));
                // Bind shared cell into the enclosing env so later writes from the
                // closure update the outer local (and vice versa).
                const_cast<Env&>(env).cells[name] = cell;
                if (env.useSlots) {
                    auto slot = env.slotIndex.find(name);
                    if (slot != env.slotIndex.end() && slot->second >= 0) {
                        const size_t index = static_cast<size_t>(slot->second);
                        if (index >= env.slots.size()) const_cast<Env&>(env).slots.resize(index + 1);
                        const_cast<Env&>(env).slots[index] = *cell;
                    }
                }
                const_cast<Env&>(env).vars[name] = *cell;
                captured.cell = cell;
            }
            captured.type = "any";
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

Value Runtime::invoke_enum_method(const Action& method, const Value& selfValue,
                                  const std::vector<ExprPtr>& args, size_t argOffset,
                                  const Env& env) const {
    Env callEnv;
    for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
    callEnv.vars["self"] = selfValue;
    for (size_t i = 0; i < method.params.size() && argOffset + i < args.size(); ++i) {
        callEnv.vars[method.params[i].name] = eval_value(*args[argOffset + i], env);
    }
    prepare_action_slots(callEnv, method);
    ExecContext child;
    exec_block(method.body, *currentProgram_, child, callEnv);
    for (auto& thread : child.threads) if (thread.joinable()) thread.join();
    lastReturnFields_ = std::move(child.returnFields);
    return child.returned ? child.returnValue : Value::null_value();
}

std::optional<Value> Runtime::dispatch_value_method(const Value& recv, std::string_view method, const std::vector<Value>& args) const {
    const std::string methodName(method);
    const std::string handle = to_display_string(recv);

    if (handle.rfind("chan:", 0) == 0) {
        auto makeArg = [](const Value& v) {
            return std::make_shared<Expr>(Expr{ ExprString{ to_display_string(v) } });
        };
        std::vector<ExprPtr> callArgs;
        callArgs.push_back(makeArg(recv));
        for (const auto& a : args) callArgs.push_back(makeArg(a));
        Env empty;
        if (methodName == "send") {
            if (args.size() != 1) throw std::runtime_error("pipe.send expects 1 argument");
            return value_from_legacy_string(eval_builtin_call("chan_send", callArgs, empty, true));
        }
        if (methodName == "receive" || methodName == "recv") {
            if (!args.empty()) throw std::runtime_error("pipe.receive expects 0 arguments");
            return value_from_legacy_string(eval_builtin_call("chan_recv", callArgs, empty, true));
        }
        if (methodName == "try_send") {
            if (args.size() != 1) throw std::runtime_error("pipe.try_send expects 1 argument");
            return value_from_legacy_string(eval_builtin_call("chan_try_send", callArgs, empty, true));
        }
        if (methodName == "try_receive" || methodName == "try_recv") {
            if (!args.empty()) throw std::runtime_error("pipe.try_receive expects 0 arguments");
            return value_from_legacy_string(eval_builtin_call("chan_try_recv", callArgs, empty, true));
        }
        if (methodName == "close") {
            if (!args.empty()) throw std::runtime_error("pipe.close expects 0 arguments");
            return value_from_legacy_string(eval_builtin_call("chan_close", callArgs, empty, true));
        }
        if (methodName == "len" || methodName == "size") {
            if (!args.empty()) throw std::runtime_error("pipe.len expects 0 arguments");
            return value_from_legacy_string(eval_builtin_call("chan_len", callArgs, empty, true));
        }
    }

    if (handle.rfind("future:", 0) == 0) {
        const int id = static_cast<int>(to_int(handle.substr(7)));
        auto it = g_futures.find(id);
        if (it == g_futures.end() || !it->second) {
            throw std::runtime_error("unknown future handle");
        }
        auto fut = it->second;
        if (methodName == "cancel") {
            if (!args.empty()) throw std::runtime_error("future.cancel expects 0 arguments");
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(fut->mu);
                if (!fut->done) {
                    fut->cancelled = true;
                    fut->failed = true;
                    fut->error = "future cancelled";
                    ok = true;
                }
            }
            if (ok) {
                fut->cv.notify_all();
                notify_channels_for_cancel();
            }
            return Value::from_bool(ok);
        }
        if (methodName == "done") {
            if (!args.empty()) throw std::runtime_error("future.done expects 0 arguments");
            std::lock_guard<std::mutex> lock(fut->mu);
            return Value::from_bool(fut->done);
        }
        if (methodName == "cancelled") {
            if (!args.empty()) throw std::runtime_error("future.cancelled expects 0 arguments");
            std::lock_guard<std::mutex> lock(fut->mu);
            return Value::from_bool(fut->cancelled);
        }
        if (methodName == "result") {
            if (!args.empty()) throw std::runtime_error("future.result expects 0 arguments");
            std::unique_lock<std::mutex> lock(fut->mu);
            fut->cv.wait(lock, [&] { return fut->done || fut->cancelled; });
            if (fut->cancelled && !fut->done) throw std::runtime_error("future cancelled");
            if (fut->failed) throw std::runtime_error(fut->error.empty() ? "async action failed" : fut->error);
            return fut->result;
        }
    }

    if (handle.rfind("file:", 0) == 0 || value_is_handle(recv, HandleKind::File)) {
        auto makeArg = [](const Value& v) {
            return std::make_shared<Expr>(Expr{ ExprString{ to_display_string(v) } });
        };
        std::vector<ExprPtr> callArgs;
        callArgs.push_back(makeArg(recv));
        for (const auto& a : args) callArgs.push_back(makeArg(a));
        Env empty;
        if (methodName == "read") {
            return value_from_legacy_string(eval_builtin_call("file_read", callArgs, empty, true));
        }
        if (methodName == "write") {
            if (args.size() != 1) throw std::runtime_error("file.write expects 1 argument");
            return value_from_legacy_string(eval_builtin_call("file_write", callArgs, empty, true));
        }
        if (methodName == "seek") {
            return value_from_legacy_string(eval_builtin_call("file_seek", callArgs, empty, true));
        }
        if (methodName == "tell") {
            if (!args.empty()) throw std::runtime_error("file.tell expects 0 arguments");
            return value_from_legacy_string(eval_builtin_call("file_tell", callArgs, empty, true));
        }
        if (methodName == "flush") {
            if (!args.empty()) throw std::runtime_error("file.flush expects 0 arguments");
            return value_from_legacy_string(eval_builtin_call("file_flush", callArgs, empty, true));
        }
        if (methodName == "close") {
            if (!args.empty()) throw std::runtime_error("file.close expects 0 arguments");
            return value_from_legacy_string(eval_builtin_call("file_close", callArgs, empty, true));
        }
    }

    if (recv.kind == ValueKind::String) {
        static const std::unordered_set<std::string> kStringMethods = {
            "lower", "upper", "strip", "lstrip", "rstrip", "find", "substr",
            "starts_with", "ends_with", "replace", "split", "len", "length"
        };
        if (kStringMethods.count(methodName)) {
            auto makeArg = [](const Value& v) {
                return std::make_shared<Expr>(Expr{ ExprString{ to_display_string(v) } });
            };
            std::vector<ExprPtr> callArgs;
            callArgs.push_back(makeArg(recv));
            for (const auto& a : args) callArgs.push_back(makeArg(a));
            Env empty;
            std::string builtin = "string." + (methodName == "length" ? "len" : methodName);
            return value_from_legacy_string(eval_builtin_call(builtin, callArgs, empty, true));
        }
    }

    return std::nullopt;
}

Value Runtime::await_future_value(const Value& value) const {
    const std::string text = to_display_string(value);
    if (text.rfind("future:", 0) != 0) return value;
    const int id = static_cast<int>(to_int(text.substr(7)));
    auto it = g_futures.find(id);
    if (it == g_futures.end() || !it->second) {
        throw std::runtime_error("await on unknown future");
    }
    auto fut = it->second;
    async_pool_release_slot();
    async_pool_help_while_waiting(fut);
    async_pool_acquire_slot();
    std::unique_lock<std::mutex> lock(fut->mu);
    fut->cv.wait(lock, [&] { return fut->done || fut->cancelled; });
    if (fut->cancelled && !fut->done) {
        throw std::runtime_error("future cancelled");
    }
    if (fut->failed) {
        throw std::runtime_error(fut->error.empty() ? "async action failed" : fut->error);
    }
    if (!fut->returnFields.empty()) {
        lastReturnFields_ = fut->returnFields;
    }
    return fut->result;
}

Value Runtime::invoke_async_action(const Action& action, const std::vector<ExprPtr>& args,
                                   const Env& env, bool returnFuture) const {
    Action actionCopy = action;
    std::vector<Value> boundArgs;
    boundArgs.reserve(args.size());
    for (const auto& arg : args) boundArgs.push_back(eval_value(*arg, env));

    auto run_body = [this, actionCopy, boundArgs]() -> std::pair<Value, std::unordered_map<std::string, Value>> {
        Env callEnv;
        for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
        for (size_t i = 0; i < actionCopy.params.size() && i < boundArgs.size(); ++i) {
            callEnv.vars[actionCopy.params[i].name] = boundArgs[i];
        }
        prepare_action_slots(callEnv, actionCopy);
        const bool prevAsync = tls_in_async_action;
        tls_in_async_action = true;
        ++tls_asyncDepth;
        ExecContext child;
        exec_block(actionCopy.body, *currentProgram_, child, callEnv);
        for (auto& thread : child.threads) if (thread.joinable()) thread.join();
        --tls_asyncDepth;
        tls_in_async_action = prevAsync;
        return {child.returned ? child.returnValue : Value::null_value(), std::move(child.returnFields)};
    };

    if (!returnFuture) {
        auto [result, fields] = run_body();
        lastReturnFields_ = std::move(fields);
        return result;
    }

    const int id = g_nextFutureId++;
    auto fut = std::make_shared<FutureState>();
    g_futures[id] = fut;
    async_pool_submit([run_body, fut]() mutable {
        tls_current_future = fut;
        struct ClearTls {
            ~ClearTls() { tls_current_future.reset(); }
        } clearTls;
        try {
            {
                std::lock_guard<std::mutex> lock(fut->mu);
                if (fut->cancelled) {
                    fut->failed = true;
                    fut->error = "future cancelled";
                    fut->done = true;
                    fut->cv.notify_all();
                    return;
                }
            }
            auto [result, fields] = run_body();
            std::lock_guard<std::mutex> lock(fut->mu);
            if (fut->cancelled) {
                fut->failed = true;
                fut->error = "future cancelled";
                fut->done = true;
            } else {
                fut->result = std::move(result);
                fut->returnFields = std::move(fields);
                fut->failed = false;
                fut->done = true;
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(fut->mu);
            fut->result = Value::null_value();
            fut->failed = true;
            fut->error = fut->cancelled ? "future cancelled" : ex.what();
            fut->done = true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(fut->mu);
            fut->result = Value::null_value();
            fut->failed = true;
            fut->error = fut->cancelled ? "future cancelled" : "async action failed";
            fut->done = true;
        }
        fut->cv.notify_all();
    });
    return Value::from_string(std::string("future:") + std::to_string(id));
}

} // namespace erelang
