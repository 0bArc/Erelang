// SPDX-License-Identifier: Apache-2.0
// Split from runtime.cpp

#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/runtime_builtins.hpp"
#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace erelang {

bool Runtime::try_get_int_var(const Env& env, const std::string& name, int64_t& out) const {
    if (auto it = env.intVars.find(name); it != env.intVars.end()) {
        out = it->second;
        return true;
    }
    if (auto it = env.vars.find(name); it != env.vars.end()) {
        if (!is_int_string(it->second)) return false;
        out = to_int(it->second);
        return true;
    }
    if (auto it = globalIntVars_.find(name); it != globalIntVars_.end()) {
        out = it->second;
        return true;
    }
    if (auto it = globalVars_.find(name); it != globalVars_.end()) {
        if (!is_int_string(it->second)) return false;
        out = to_int(it->second);
        return true;
    }
    return false;
}

void Runtime::set_var_int(Env& env, const std::string& name, int64_t value) const {
    env.intVars[name] = value;
    env.vars.erase(name);
    if (globalNames_.count(name)) {
        globalIntVars_[name] = value;
        globalVars_.erase(name);
    }
}

void Runtime::set_var_str(Env& env, const std::string& name, const std::string& value) const {
    env.vars[name] = value;
    env.intVars.erase(name);
    if (globalNames_.count(name)) {
        globalVars_[name] = value;
        globalIntVars_.erase(name);
    }
}

bool Runtime::try_eval_int(const Expr& e, const Env& env, int64_t& out) const {
    if (std::holds_alternative<ExprNumber>(e.node)) {
        out = std::get<ExprNumber>(e.node).v;
        return true;
    }
    if (std::holds_alternative<ExprBool>(e.node)) {
        out = std::get<ExprBool>(e.node).v ? 1 : 0;
        return true;
    }
    if (std::holds_alternative<ExprIdent>(e.node)) {
        return try_get_int_var(env, std::get<ExprIdent>(e.node).name, out);
    }
    if (std::holds_alternative<UnaryExpr>(e.node)) {
        const auto& u = std::get<UnaryExpr>(e.node);
        int64_t v = 0;
        if (!u.expr || !try_eval_int(*u.expr, env, v)) return false;
        if (u.op == UnOp::Neg) { out = -v; return true; }
        if (u.op == UnOp::Not) { out = (v == 0) ? 1 : 0; return true; }
        return false;
    }
    if (std::holds_alternative<BinaryExpr>(e.node)) {
        const auto& b = std::get<BinaryExpr>(e.node);
        int64_t li = 0, ri = 0;
        if (!b.left || !b.right) return false;
        if (!try_eval_int(*b.left, env, li) || !try_eval_int(*b.right, env, ri)) return false;
        switch (b.op) {
            case BinOp::Add: out = li + ri; return true;
            case BinOp::Sub: out = li - ri; return true;
            case BinOp::Mul: out = li * ri; return true;
            case BinOp::Div: if (ri == 0) return false; out = li / ri; return true;
            case BinOp::Mod: if (ri == 0) return false; out = li % ri; return true;
            case BinOp::LT: out = (li < ri) ? 1 : 0; return true;
            case BinOp::LE: out = (li <= ri) ? 1 : 0; return true;
            case BinOp::GT: out = (li > ri) ? 1 : 0; return true;
            case BinOp::GE: out = (li >= ri) ? 1 : 0; return true;
            case BinOp::EQ: out = (li == ri) ? 1 : 0; return true;
            case BinOp::NE: out = (li != ri) ? 1 : 0; return true;
            default: return false;
        }
    }
    if (std::holds_alternative<PostfixExpr>(e.node)) {
        // Value of postfix is pre-increment value; mutation handled by ExprStmt.
        const auto& pe = std::get<PostfixExpr>(e.node);
        if (!pe.operand || !std::holds_alternative<ExprIdent>(pe.operand->node)) return false;
        return try_get_int_var(env, std::get<ExprIdent>(pe.operand->node).name, out);
    }
    if (std::holds_alternative<PrefixExpr>(e.node)) {
        const auto& pe = std::get<PrefixExpr>(e.node);
        if (!pe.operand || !std::holds_alternative<ExprIdent>(pe.operand->node)) return false;
        int64_t cur = 0;
        if (!try_get_int_var(env, std::get<ExprIdent>(pe.operand->node).name, cur)) return false;
        out = cur + (pe.isInc ? 1 : -1);
        return true;
    }
    return false;
}

std::optional<ExprPtr> Runtime::parse_interpolation_expr(std::string_view exprText) const {
    const std::string key = trim_copy(exprText);
    if (key.empty()) {
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> lock(interpolationExprCacheMutex_);
        auto it = interpolationExprCache_.find(key);
        if (it != interpolationExprCache_.end()) {
            return it->second;
        }
    }

    try {
        std::string script;
        script.reserve(key.size() + 64);
        script += "@erelang\n";
        script += "public action __fmt {\n";
        script += "  return ";
        script += key;
        script += ";\n}";

        LexerOptions lxopts;
        lxopts.enableDurations = true;
        lxopts.enableUnits = true;
        lxopts.enablePolyIdentifiers = true;
        lxopts.emitDocComments = false;
        lxopts.emitComments = false;
        Lexer lexer(script, lxopts);
        Parser parser(lexer.lex());
        Program program = parser.parse();
        for (const auto& action : program.actions) {
            if (action.name != "__fmt") {
                continue;
            }
            for (const auto& stmt : action.body.stmts) {
                if (!std::holds_alternative<ReturnStmt>(stmt)) {
                    continue;
                }
                const auto& ret = std::get<ReturnStmt>(stmt);
                if (!ret.value.has_value() || !(*ret.value)) {
                    return std::nullopt;
                }
                ExprPtr parsed = *ret.value;
                {
                    std::lock_guard<std::mutex> lock(interpolationExprCacheMutex_);
                    interpolationExprCache_[key] = parsed;
                }
                return parsed;
            }
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<std::string> Runtime::eval_interpolation_expr(std::string_view exprText, const Env& env) const {
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

std::string Runtime::eval_string(const Expr& e, const Env& env) const {
    if (std::holds_alternative<ExprString>(e.node)) {
        const auto& n = std::get<ExprString>(e.node);
        std::string out; out.reserve(n.v.size());
        const std::string& s = n.v;
        for (size_t i=0;i<s.size();){
            if (s[i]=='{') {
                size_t j = s.find('}', i+1);
                if (j!=std::string::npos) {
                    std::string key = s.substr(i+1, j-(i+1));
                    std::string trimmedKey = trim_copy(key);
                    int64_t iv = 0;
                    if (try_get_int_var(env, trimmedKey, iv)) {
                        out += std::to_string(iv);
                    } else {
                    auto it = env.vars.find(trimmedKey);
                    if (it != env.vars.end()) {
                        out += it->second;
                    } else {
                        auto git = globalVars_.find(trimmedKey);
                        if (git != globalVars_.end()) {
                            out += git->second;
                        } else if (auto exprValue = eval_interpolation_expr(trimmedKey, env); exprValue.has_value()) {
                            out += *exprValue;
                        } else {
                            out += '{';
                            out += key;
                            out += '}';
                        }
                    }
                    }
                    i = j+1; continue;
                }
            }
            out.push_back(s[i++]);
        }
        return out;
    }
    if (std::holds_alternative<ExprNull>(e.node)) {
        return "nullptr";
    }
    if (std::holds_alternative<ExprNumber>(e.node)) {
        const auto& n = std::get<ExprNumber>(e.node);
        // Preserve the literal spelling only when it round-trips as a plain
        // decimal int/float (e.g. "42", "2.5"). Non-decimal literals like
        // hex ("0x1e") must be emitted as their numeric value.
        if (!n.raw.empty() && (is_int_string(n.raw) || is_float_string(n.raw))) return n.raw;
        return std::to_string(n.v);
    }
    if (std::holds_alternative<ExprBool>(e.node)) return std::get<ExprBool>(e.node).v ? "true" : "false";
    if (std::holds_alternative<ExprIdent>(e.node)) {
        const auto& n = std::get<ExprIdent>(e.node);
        int64_t iv = 0;
        if (try_get_int_var(env, n.name, iv)) {
            return std::to_string(iv);
        }
        auto it = env.vars.find(n.name);
        if (it!=env.vars.end()) {
            if (it->second.rfind("struct:", 0) == 0) {
                int id = g_nextDictId++;
                auto& dict = g_dicts[id];
                const std::string prefix = n.name + ".";
                for (const auto& kv : env.vars) {
                    if (kv.first.rfind(prefix, 0) == 0) {
                        dict[kv.first.substr(prefix.size())] = kv.second;
                    }
                }
                return std::string("dict:") + std::to_string(id);
            }
            return it->second;
        }
        auto git = globalVars_.find(n.name);
        if (git != globalVars_.end()) {
            if (git->second.rfind("struct:", 0) == 0) {
                int id = g_nextDictId++;
                auto& dict = g_dicts[id];
                const std::string prefix = n.name + ".";
                for (const auto& kv : globalVars_) {
                    if (kv.first.rfind(prefix, 0) == 0) {
                        dict[kv.first.substr(prefix.size())] = kv.second;
                    }
                }
                return std::string("dict:") + std::to_string(id);
            }
            return git->second;
        }
        if (env.objects.find(n.name) != env.objects.end()) return n.name;
        std::string lowered = n.name;
        for (auto& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lowered == "array") {
            int id = g_nextListId++;
            g_lists[id] = {};
            return std::string("list:") + std::to_string(id);
        }
        if (lowered == "map" || lowered == "dictionary" || lowered == "dict" || lowered == "hashmap") {
            int id = g_nextDictId++;
            g_dicts[id] = {};
            return std::string("dict:") + std::to_string(id);
        }
        return n.name;
    }
    if (std::holds_alternative<UnaryExpr>(e.node)) {
        const auto& u = std::get<UnaryExpr>(e.node);
        std::string v = eval_string(*u.expr, env);
        switch (u.op) {
            case UnOp::Neg: {
                if (is_float_string(v)) {
                    const double dv = to_double(v);
                    return std::to_string(-dv);
                }
                return std::to_string(-to_int(v));
            }
            case UnOp::Not: return is_truthy(v) ? "false" : "true";
            case UnOp::Deref: {
                if (v.rfind("ref:", 0) == 0) {
                    const std::string varName = v.substr(4);
                    if (auto it = env.vars.find(varName); it != env.vars.end()) return it->second;
                    if (auto git = globalVars_.find(varName); git != globalVars_.end()) return git->second;
                    return {};
                }
                if (auto idOpt = parse_pointer_handle(v); idOpt.has_value()) {
                    const int id = *idOpt;
                    auto it = g_ptrs.find(id);
                    if (it != g_ptrs.end()) {
                        if (it->second.rfind("ref:", 0) == 0) {
                            const std::string varName = it->second.substr(4);
                            if (auto vit = env.vars.find(varName); vit != env.vars.end()) return vit->second;
                            if (auto git = globalVars_.find(varName); git != globalVars_.end()) return git->second;
                            return {};
                        }
                        return it->second;
                    }
                    return {};
                }
                return {};
            }
            case UnOp::AddressOf: {
                if (std::holds_alternative<ExprIdent>(u.expr->node)) {
                    const auto& id = std::get<ExprIdent>(u.expr->node).name;
                    const int ptrId = g_nextPtrId++;
                    g_ptrs[ptrId] = std::string("ref:") + id;
                    return format_pointer_handle(ptrId);
                }
                const int id = g_nextPtrId++;
                g_ptrs[id] = v;
                return format_pointer_handle(id);
            }
        }
    }
    if (std::holds_alternative<BinaryExpr>(e.node)) {
        const auto& b = std::get<BinaryExpr>(e.node);
        // Fast path: pure int arithmetic / compare without string round-trips.
        {
            int64_t iv = 0;
            if (try_eval_int(e, env, iv)) {
                switch (b.op) {
                    case BinOp::LT: case BinOp::LE: case BinOp::GT: case BinOp::GE:
                    case BinOp::EQ: case BinOp::NE:
                        return iv ? "true" : "false";
                    default:
                        return std::to_string(iv);
                }
            }
        }
        std::string ls = eval_string(*b.left, env);
        std::string rs = eval_string(*b.right, env);
        int64_t li = to_int(ls), ri = to_int(rs);
        auto is_explicit_string_expr = [](const ExprPtr& expr) -> bool {
            if (!expr) return false;
            if (std::holds_alternative<ExprString>(expr->node)) return true;
            if (std::holds_alternative<FunctionCallExpr>(expr->node)) {
                const auto& fc = std::get<FunctionCallExpr>(expr->node);
                if (fc.name == "tostr" || fc.name == "toString") return true;
                if (fc.name.rfind("string.", 0) == 0) return true;
            }
            return false;
        };
        // Lightweight unit arithmetic: pattern <int><unit>, same unit on both sides
        auto parseUnit = [](const std::string& s) -> std::optional<std::pair<long long,std::string>> {
            if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0]))) return std::nullopt;
            size_t i = 0; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (i == 0 || i >= s.size()) return std::nullopt; // must have unit suffix
            // Unit must start with a letter
            if (!std::isalpha(static_cast<unsigned char>(s[i]))) return std::nullopt;
            std::string unit = s.substr(i);
            // Basic validation: disallow whitespace
            for (char ch : unit) { if (std::isspace(static_cast<unsigned char>(ch))) return std::nullopt; }
            long long value = 0; try { value = std::stoll(s.substr(0,i)); } catch (...) { return std::nullopt; }
            return std::make_pair(value, unit);
        };
        auto lu = parseUnit(ls);
        auto ru = parseUnit(rs);
        auto unitAddSub = [&](BinOp op)->std::string {
            if (lu && ru && lu->second == ru->second) {
                if (op == BinOp::Add) return std::to_string(lu->first + ru->first) + lu->second;
                if (op == BinOp::Sub) return std::to_string(lu->first - ru->first) + lu->second;
            }
            return std::string();
        };
        auto is_char_like = [](const std::string& s) -> bool {
            return s.size() == 1 && std::isalpha(static_cast<unsigned char>(s[0])) != 0;
        };
        auto require_int_operands = [&](const char* opName) {
            if (!is_int_string(ls) || !is_int_string(rs)) {
                throw std::runtime_error(std::string("Illegal operation: ") + opName + " requires int operands");
            }
        };
        switch (b.op) {
            case BinOp::Add: {
                if (auto r = unitAddSub(BinOp::Add); !r.empty()) return r;
                const bool leftIsInt = is_int_string(ls);
                const bool rightIsInt = is_int_string(rs);
                if ((is_char_like(ls) && rightIsInt) || (leftIsInt && is_char_like(rs))) {
                    throw std::runtime_error("Illegal operation: char + int");
                }
                if (leftIsInt && rightIsInt) {
                    if (is_explicit_string_expr(b.left) || is_explicit_string_expr(b.right)) return ls + rs;
                    return std::to_string(li + ri);
                }
                if (!leftIsInt && !rightIsInt) return ls + rs;
                if (!leftIsInt && rightIsInt) return ls + rs;
                if (leftIsInt && !rightIsInt) return ls + rs;
            }
            case BinOp::Sub: {
                if (auto r = unitAddSub(BinOp::Sub); !r.empty()) return r;
                require_int_operands("-");
                return std::to_string(li - ri);
            }
            case BinOp::Mul:
                require_int_operands("*");
                return std::to_string(li * ri);
            case BinOp::Div:
                require_int_operands("/");
                if (ri == 0) throw std::runtime_error("Division by zero");
                return std::to_string(li / ri);
            case BinOp::Mod:
                require_int_operands("%");
                if (ri == 0) throw std::runtime_error("Modulo by zero");
                return std::to_string(li % ri);
            case BinOp::Pow: {
                require_int_operands("^");
                if (ri < 0) return "0";
                if (ri > 62) throw std::runtime_error("Exponent too large for integer power");
                int64_t value = 1;
                for (int64_t i = 0; i < ri; ++i) value *= li;
                return std::to_string(value);
            }
            case BinOp::EQ: return (ls == rs) ? "true" : "false";
            case BinOp::NE: return (ls != rs) ? "true" : "false";
            case BinOp::LT: return (is_int_string(ls) && is_int_string(rs) ? (li < ri) : (ls < rs)) ? "true" : "false";
            case BinOp::LE: return (is_int_string(ls) && is_int_string(rs) ? (li <= ri) : (ls <= rs)) ? "true" : "false";
            case BinOp::GT: return (is_int_string(ls) && is_int_string(rs) ? (li > ri) : (ls > rs)) ? "true" : "false";
            case BinOp::GE: return (is_int_string(ls) && is_int_string(rs) ? (li >= ri) : (ls >= rs)) ? "true" : "false";
            case BinOp::And: return (is_truthy(ls) && is_truthy(rs)) ? "true" : "false";
            case BinOp::Or: return (is_truthy(ls) || is_truthy(rs)) ? "true" : "false";
            case BinOp::Coalesce: {
                const bool leftNullish = ls.empty() || ls == "nullptr";
                return (!leftNullish ? ls : rs);
            }
        }
    }
    if (std::holds_alternative<TernaryExpr>(e.node)) {
        const auto& t = std::get<TernaryExpr>(e.node);
        const std::string cond = eval_string(*t.cond, env);
        const bool truthy = is_truthy(cond);
        return truthy ? eval_string(*t.thenExpr, env) : eval_string(*t.elseExpr, env);
    }
    if (std::holds_alternative<MemberExpr>(e.node)) {
        const auto& m = std::get<MemberExpr>(e.node);
        auto oit = env.objects.find(m.objectName);
        if (oit != env.objects.end()) {
            auto fit = oit->second->fields.find(m.field);
            if (fit != oit->second->fields.end()) return fit->second;
        }
        auto sv = env.vars.find(m.objectName);
        if (sv != env.vars.end() && sv->second.rfind("dict:", 0) == 0) {
            int id = to_int(sv->second.substr(5));
            auto dit = g_dicts[id].find(m.field);
            if (dit != g_dicts[id].end()) return dit->second;
        }
        if (sv != env.vars.end() && sv->second.rfind("struct:", 0) == 0) {
            auto fit = env.vars.find(m.objectName + "." + m.field);
            if (fit != env.vars.end()) return fit->second;
        }
        auto it = env.vars.find(m.objectName + "." + m.field);
        if (it != env.vars.end()) return it->second;
        return {};
    }
    if (std::holds_alternative<IndexExpr>(e.node)) {
        const auto& ix = std::get<IndexExpr>(e.node);
        const std::string container = eval_string(*ix.object, env);
        const std::string indexValue = eval_string(*ix.index, env);
        if (container.rfind("list:", 0) == 0) {
            const int id = static_cast<int>(to_int(container.substr(5)));
            const int idx = static_cast<int>(to_int(indexValue));
            auto it = g_lists.find(id);
            if (it != g_lists.end()) {
                const auto& vec = it->second;
                if (idx >= 0 && idx < static_cast<int>(vec.size())) return vec[idx];
            }
            return "0";
        }
        if (container.rfind("dict:", 0) == 0) {
            const int id = static_cast<int>(to_int(container.substr(5)));
            auto dit = g_dicts.find(id);
            if (dit != g_dicts.end()) {
                auto fit = dit->second.find(indexValue);
                if (fit != dit->second.end()) return fit->second;
            }
            return {};
        }
        return {};
    }
    if (std::holds_alternative<FunctionCallExpr>(e.node)) {
        const auto& fc = std::get<FunctionCallExpr>(e.node);
        if (currentProgram_) {
            const auto dot = fc.name.rfind('.');
            if (dot != std::string::npos && dot > 0 && dot + 1 < fc.name.size()) {
                const std::string objectName = fc.name.substr(0, dot);
                const std::string methodName = fc.name.substr(dot + 1);
                auto invoke_entity_method = [&](ObjPtr obj) -> std::optional<std::string> {
                    const Entity* ent = find_entity(*currentProgram_, obj->typeName);
                    if (!ent) return std::nullopt;
                    const Action* meth = find_entity_method(*ent, methodName);
                    if (!meth) return std::nullopt;
                    Env callEnv;
                    for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                    for (size_t i = 0; i < meth->params.size() && i < fc.args.size(); ++i) {
                        callEnv.vars[meth->params[i].name] = eval_string(*fc.args[i], env);
                    }
                    callEnv.objects["self"] = obj;
                    for (const auto& kv : obj->fields) callEnv.vars[kv.first] = kv.second;
                    ExecContext child;
                    exec_block(meth->body, *currentProgram_, child, callEnv);
                    for (auto& th : child.threads) if (th.joinable()) th.join();
                    for (auto& f : obj->fields) {
                        auto vit = callEnv.vars.find(f.first);
                        if (vit != callEnv.vars.end()) f.second = vit->second;
                    }
                    if (child.returned) return child.returnValue;
                    return std::string{};
                };
                if (auto oit = env.objects.find(objectName); oit != env.objects.end()) {
                    if (auto ret = invoke_entity_method(oit->second)) return *ret;
                }
                if (objectName == "self") {
                    if (auto oit = env.objects.find("self"); oit != env.objects.end()) {
                        if (auto ret = invoke_entity_method(oit->second)) return *ret;
                    }
                }
                auto varIt = env.vars.find(objectName);
                if (varIt != env.vars.end() && varIt->second.rfind("struct:", 0) == 0) {
                    const std::string structName = varIt->second.substr(7);
                    const StructDecl* sd = find_struct_decl(*currentProgram_, structName);
                    const Action* method = sd ? find_struct_method(*sd, methodName) : nullptr;
                    if (method) {
                        Env callEnv;
                        for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                        for (size_t i = 0; i < method->params.size() && i < fc.args.size(); ++i) {
                            callEnv.vars[method->params[i].name] = eval_string(*fc.args[i], env);
                        }
                        callEnv.vars["self"] = varIt->second;
                        for (const auto& f : sd->fields) {
                            const std::string objectField = objectName + "." + f.name;
                            auto fit = env.vars.find(objectField);
                            const std::string value = (fit != env.vars.end()) ? fit->second : std::string{};
                            callEnv.vars[f.name] = value;
                            callEnv.vars["self." + f.name] = value;
                        }
                        ExecContext child;
                        exec_block(method->body, *currentProgram_, child, callEnv);
                        for (auto& th : child.threads) if (th.joinable()) th.join();
                        if (child.returned) return child.returnValue;
                        return {};
                    }
                }
            }
            if (const StructDecl* sd = find_struct_decl(*currentProgram_, fc.name)) {
                (void)sd;
                (void)fc;
                return std::string("struct:") + sd->name;
            }
            if (const Action* action = find_action(*currentProgram_, fc.name)) {
                if (currentProgram_->strict && action->visibility != Visibility::Public) {
                    throw std::runtime_error("Action not public: " + action->name);
                }
                Env calleeEnv;
                for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
                for (size_t i = 0; i < action->params.size() && i < fc.args.size(); ++i) {
                    const std::string paramName = action->params[i].name;
                    calleeEnv.vars[paramName] = eval_string(*fc.args[i], env);
                    if (std::holds_alternative<ExprIdent>(fc.args[i]->node)) {
                        const auto& id = std::get<ExprIdent>(fc.args[i]->node).name;
                        auto oit = env.objects.find(id);
                        if (oit != env.objects.end()) {
                            calleeEnv.objects[paramName] = oit->second;
                        }
                    }
                }
                ExecContext calleeCtx;
                exec_block(action->body, *currentProgram_, calleeCtx, calleeEnv);
                for (auto& th : calleeCtx.threads) {
                    if (th.joinable()) th.join();
                }
                if (!calleeCtx.returned) return {};
                return calleeCtx.returnValue;
            }
        }
        // Check if fc.name is a variable holding a func:N handle
        auto funcVarIt = env.vars.find(fc.name);
        if (funcVarIt != env.vars.end() && funcVarIt->second.rfind("func:", 0) == 0) {
            const std::string& handle = funcVarIt->second;
            const std::string funcIdStr = handle.substr(5);
            int funcId = 0;
            try { funcId = std::stoi(funcIdStr); }
            catch (...) { return {}; }
            auto cit = g_closures.find(funcId);
            if (cit != g_closures.end() && cit->second && currentProgram_) {
                ClosureData* cd = cit->second;
                Env callEnv;
                for (const auto& cv : cd->captured) callEnv.vars[cv.name] = cv.value;
                for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                for (size_t i = 0; i < cd->body.params.size() && i < fc.args.size(); ++i) {
                    callEnv.vars[cd->body.params[i].name] = eval_string(*fc.args[i], env);
                }
                ExecContext child;
                exec_block(cd->body.body, *currentProgram_, child, callEnv);
                if (!child.returned) return {};
                return child.returnValue;
            }
            return {};
        }
        return eval_builtin_call(fc.name, fc.args, env, true);
    }
    if (std::holds_alternative<ListLiteralExpr>(e.node)) {
        const auto& lit = std::get<ListLiteralExpr>(e.node);
        return eval_builtin_call("list_new", lit.elements, env, true);
    }
    if (std::holds_alternative<DictLiteralExpr>(e.node)) {
        const auto& lit = std::get<DictLiteralExpr>(e.node);
        return eval_builtin_call("dict_new", lit.entries, env, true);
    }
    if (std::holds_alternative<LambdaExpr>(e.node)) {
        const auto& lam = std::get<LambdaExpr>(e.node);
        // Create a ClosureData from the LambdaExpr
        int closureId = g_nextClosureId++;
        ClosureData* cd = new ClosureData();
        cd->refCount = 1;
        cd->sourcePath = "<lambda>";
        cd->sourceLine = 0;
        // Build the Action from the lambda
        cd->body.name = "lambda_" + std::to_string(closureId);
        cd->body.params = lam.params;
        cd->body.returnType = lam.returnType;
        cd->body.visibility = Visibility::Public;
        cd->body.exported = false;
        cd->body.isAsync = false;
        if (lam.isArrow && lam.body) {
            // Arrow form: wrap single expression in return
            cd->body.body.stmts.push_back(ReturnStmt{lam.body});
        } else {
            cd->body.body = lam.blockBody;
        }
        // Capture free variables from current env
        for (const auto& cv : lam.capturedVars) {
            ClosedVar capped;
            capped.name = cv;
            // Look up in environment
            auto it = env.vars.find(cv);
            if (it != env.vars.end()) {
                capped.value = it->second;
                capped.type = "string"; // default
            } else {
                capped.value = ""; // not found at runtime
                capped.type = "unknown";
            }
            cd->captured.push_back(capped);
        }
        g_closures[closureId] = cd;
        return "func:" + std::to_string(closureId);
    }
    if (std::holds_alternative<NewExpr>(e.node)) {
        const auto& ne = std::get<NewExpr>(e.node);
        return std::string{"<new:"} + ne.typeName + ">";
    }
    if (std::holds_alternative<PostfixExpr>(e.node)) {
        // Value context: evaluate operand (mutation handled at statement level).
        const auto& pe = std::get<PostfixExpr>(e.node);
        return eval_string(*pe.operand, env);
    }
    if (std::holds_alternative<PrefixExpr>(e.node)) {
        const auto& pe = std::get<PrefixExpr>(e.node);
        const std::string cur = eval_string(*pe.operand, env);
        if (is_int_string(cur)) return std::to_string(to_int(cur) + (pe.isInc ? 1 : -1));
        return cur;
    }
    if (std::holds_alternative<CompoundAssignExpr>(e.node)) {
        const auto& ca = std::get<CompoundAssignExpr>(e.node);
        auto bin = Expr{ BinaryExpr{ ca.op, ca.left, ca.right } };
        return eval_string(bin, env);
    }
    return {};
}

} // namespace erelang
