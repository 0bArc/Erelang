#include "erelang/typechecker.hpp"
#include "erelang/runtime_imports.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace erelang {

namespace {

std::string merge_inferred_type(const std::string& left, const std::string& right) {
    if (left.empty() || left == "unknown") return right;
    if (right.empty() || right == "unknown") return left;
    if (left == right) return left;
    return "any";
}

bool parse_array_type(const std::string& typeName, std::string& elementType) {
    constexpr const char* prefix = "array<";
    if (typeName.rfind(prefix, 0) != 0 || typeName.back() != '>') return false;
    elementType = typeName.substr(6, typeName.size() - 7);
    return !elementType.empty();
}

bool parse_map_type(const std::string& typeName, std::string& keyType, std::string& valueType) {
    constexpr const char* prefix = "map<";
    if (typeName.rfind(prefix, 0) != 0 || typeName.back() != '>') return false;
    const std::string inner = typeName.substr(4, typeName.size() - 5);
    int depth = 0;
    for (size_t index = 0; index < inner.size(); ++index) {
        const char ch = inner[index];
        if (ch == '<') {
            ++depth;
            continue;
        }
        if (ch == '>') {
            --depth;
            continue;
        }
        if (ch == ',' && depth == 0) {
            keyType = inner.substr(0, index);
            valueType = inner.substr(index + 1);
            return !keyType.empty() && !valueType.empty();
        }
    }
    return false;
}

bool generic_type_compatible(const std::string& expected, const std::string& actual) {
    if (expected == "any" || expected == "unknown") return true;
    if (actual == "any" || actual == "unknown") return true;
    return expected == actual;
}

bool collection_type_compatible(const std::string& expected, const std::string& actual) {
    if (expected == actual) return true;

    std::string expectedArrayElem;
    std::string actualArrayElem;
    if (parse_array_type(expected, expectedArrayElem)) {
        if (actual == "array<any>") return true;
        if (!parse_array_type(actual, actualArrayElem)) return false;
        return generic_type_compatible(expectedArrayElem, actualArrayElem);
    }

    std::string expectedMapKey;
    std::string expectedMapValue;
    std::string actualMapKey;
    std::string actualMapValue;
    if (parse_map_type(expected, expectedMapKey, expectedMapValue)) {
        if (actual == "map<any,any>") return true;
        if (!parse_map_type(actual, actualMapKey, actualMapValue)) return false;
        return generic_type_compatible(expectedMapKey, actualMapKey) && generic_type_compatible(expectedMapValue, actualMapValue);
    }

    return false;
}

std::optional<std::string> resolve_builtin_module_alias_call(const Program* program, const std::string& callName) {
    if (!program) {
        return std::nullopt;
    }
    const auto dotPos = callName.find('.');
    if (dotPos == std::string::npos || dotPos == 0 || dotPos + 1 >= callName.size()) {
        return std::nullopt;
    }
    const std::string alias = callName.substr(0, dotPos);
    const std::string method = callName.substr(dotPos + 1);
    return resolve_builtin_module_method(*program, alias, method);
}

std::optional<std::string> hint_for_unknown_call(const std::string& name) {
    auto starts = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
    if (starts("file_") || name == "fopen" || name == "fread" || name == "fwrite" ||
        name == "fclose" || name == "fseek" || name == "ftell" || name == "fflush") {
        return "Filesystem builtin — add `#include <builtin/fs> as fs`, then `fs." + name + "(...)` or `" + name + "` after include";
    }
    if (starts("regex_")) {
        return "Regex builtin — add `#include <builtin/regex> as regex`";
    }
    if (starts("sha") || starts("hash_") || name == "md5" || starts("aes_")) {
        return "Crypto builtin — add `#include <builtin/crypto> as crypto`";
    }
    if (starts("thread_") || name == "spawn_thread") {
        return "Threads builtin — add `#include <builtin/threads> as threads`";
    }
    if (starts("net_") || name == "http_get" || name == "http_post") {
        return "Network builtin — add `#include <builtin/network> as net`";
    }
    return std::nullopt;
}

std::string format_param_range(int minP, int maxP) {
    if (maxP < 0) return std::to_string(minP) + "+ args";
    if (minP == maxP) return std::to_string(minP) + " arg" + (minP == 1 ? "" : "s");
    return std::to_string(minP) + ".." + std::to_string(maxP) + " args";
}

void emit_unknown_call(TCResult& result, const std::string& name, const std::string& ctx) {
    DiagBuilder b(result, Severity::Error, "Unknown action or builtin: " + name, "TC001", ctx);
    if (auto hint = hint_for_unknown_call(name)) {
        b.hint(*hint);
    } else {
        b.hint("Check spelling, add `#include <...>` for builtins, or declare `extern action " + name + "(...)`");
    }
    b.emit();
}

std::string infer_switch_case_type(const std::string& literal) {
    if (literal.empty()) return "unknown";
    if (literal.front() == '"' && literal.back() == '"') return "string";
    if (literal == "true" || literal == "false") return "bool";
    bool numeric = true;
    bool hasDot = false;
    for (size_t i = 0; i < literal.size(); ++i) {
        const char c = literal[i];
        if (i == 0 && c == '-') continue;
        if (c == '.') { hasDot = hasDot || (i > 0); if (hasDot && literal.find('.', i + 1) != std::string::npos) { numeric = false; break; } continue; }
        if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
    }
    if (numeric) return hasDot ? "double" : "int";
    return "unknown";
}

// Mark variables referenced inside a string literal's `{...}` interpolation
// as used so TC120 does not report false positives. Interpolated expressions
// are evaluated at runtime (Runtime::parse_interpolation_expr); here we
// extract word tokens that resolve to a declared identifier and mark them.
void mark_interpolation_uses(const std::string& text, CheckContext& ctx) {
    if (!ctx.scopes) return;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const std::size_t open = text.find('{', i);
        if (open == std::string::npos) break;
        const std::size_t close = text.find('}', open + 1);
        if (close == std::string::npos) break;
        std::size_t j = open + 1;
        while (j < close) {
            const unsigned char c = static_cast<unsigned char>(text[j]);
            if (std::isalpha(c) || c == '_') {
                const std::size_t start = j;
                while (j < close &&
                       (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) {
                    ++j;
                }
                const std::string word = text.substr(start, j - start);
                if (VarInfo* v = ctx.scopes->lookup(word)) v->used = true;
            } else {
                ++j;
            }
        }
        i = close + 1;
    }
}

} // namespace

std::string format_diagnostic(const Diagnostic& d) {
    std::ostringstream oss;
    const char* tag = d.severity == Severity::Warning ? "[warn] "
                    : (d.severity == Severity::Note ? "[hint] " : "[error] ");
    oss << tag << d.code << ": " << d.message;
    if (!d.context.empty()) oss << " (" << d.context << ")";
    if (d.line >= 0) oss << " at " << d.line << ":" << d.col;
    return oss.str();
}

// ================= ExprChecker =================
TypeInfo ExprChecker::check(const ExprPtr& e, CheckContext& ctx) {
    if (!e) return {"void"};
    auto itC = cache_.find(e.get()); if (itC!=cache_.end()) return itC->second;
    TypeInfo inferred{"unknown"};
    std::visit([&](auto&& node){
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ExprNumber>) inferred = {node.isFloatLiteral ? "double" : "int"};
        else if constexpr (std::is_same_v<T, ExprBool>) inferred = {"bool"};
        else if constexpr (std::is_same_v<T, ExprString>) {
            inferred = {"string"};
            mark_interpolation_uses(node.v, ctx);
        }
        else if constexpr (std::is_same_v<T, ExprNull>) inferred = {"pointer"};
        else if constexpr (std::is_same_v<T, ExprIdent>) {
            if (ctx.scopes) {
                if (auto* v = ctx.scopes->lookup(node.name)) { v->used = true; inferred = v->type; }
                else {
                    bool resolvedEnum = false;
                    if (ctx.program) {
                        for (const auto& en : ctx.program->enums) {
                            for (const auto& member : en.members) {
                                if (node.name == member ||
                                    node.name == en.name + "::" + member ||
                                    node.name == en.name + "." + member) {
                                    inferred = {"enum:" + en.name};
                                    resolvedEnum = true;
                                    break;
                                }
                            }
                            if (resolvedEnum) break;
                        }
                    }
                    if (!resolvedEnum) {
                        DiagBuilder(result_, Severity::Error, "Use before declaration: " + node.name, "TC010", ctx.actionName())
                            .hint("Add `" + node.name + " = ...;` above this line, or import the symbol")
                            .emit();
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            auto lt = check(node.left, ctx);
            auto rt = check(node.right, ctx);
            switch (node.op) {
                case BinOp::Add:
                    if (TypeChecker::is_int(lt) && TypeChecker::is_int(rt)) { inferred = {"int"}; break; }
                    if (TypeChecker::is_string(lt) && TypeChecker::is_string(rt)) { inferred={"string"}; break; }
                    if ((TypeChecker::is_string(lt) && TypeChecker::is_int(rt)) || (TypeChecker::is_int(lt) && TypeChecker::is_string(rt))) {
                        DiagBuilder(result_, Severity::Error, "Illegal '+' operands", "TC011", ctx.actionName()).emit();
                    }
                    inferred={"unknown"};
                    break;
                case BinOp::Sub: case BinOp::Mul: case BinOp::Div: case BinOp::Mod: case BinOp::Pow:
                    inferred = {"int"}; break;
                case BinOp::And: case BinOp::Or: inferred={"bool"}; break;
                default: inferred={"bool"}; break; // comparisons
            }
        } else if constexpr (std::is_same_v<T, TernaryExpr>) {
            auto ct = check(node.cond, ctx);
            if (!TypeChecker::is_bool(ct) && ct.name != "unknown") {
                DiagBuilder(result_, Severity::Error, "Ternary condition not bool", "TC012", ctx.actionName()).emit();
            }
            auto tt = check(node.thenExpr, ctx);
            auto et = check(node.elseExpr, ctx);
            if (tt.name == et.name) inferred = tt;
            else if (tt.name == "unknown") inferred = et;
            else if (et.name == "unknown") inferred = tt;
            else inferred = {"unknown"};
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            if (node.op == UnOp::AddressOf) inferred = {"pointer"};
            else if (node.op == UnOp::Deref) inferred = {"unknown"};
            else if (node.op == UnOp::Not) {
                check(node.expr, ctx);
                inferred = {"bool"};
            } else inferred = check(node.expr, ctx);
        } else if constexpr (std::is_same_v<T, NewExpr>) {
            inferred = TypeInfo{"entity:" + node.typeName};
        } else if constexpr (std::is_same_v<T, MemberExpr>) {
            inferred = {"unknown"};
            if (ctx.scopes) {
                if (auto* owner = ctx.scopes->lookup(node.objectName)) {
                    std::string otype = owner->type.name;
                    const std::string prefix = "struct:";
                    if (otype.rfind(prefix, 0) == 0 && ctx.program) {
                        const std::string structName = otype.substr(prefix.size());
                        for (const auto& sd : ctx.program->structs) {
                            if (sd.name != structName) continue;
                            for (const auto& field : sd.fields) {
                                if (field.name == node.field) {
                                    inferred = { field.type.empty() ? "unknown" : field.type };
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, IndexExpr>) {
            (void)check(node.object, ctx);
            (void)check(node.index, ctx);
            inferred = {"unknown"};
        } else if constexpr (std::is_same_v<T, ListLiteralExpr>) {
            if (node.elements.empty()) {
                DiagBuilder(result_, Severity::Error,
                    "Empty array literal `[]` is not allowed — provide at least one element or use a typed declaration with a default value",
                    "TC055", ctx.actionName())
                    .hint("Example: `Array<string> items = [\"first\"];` — empty collections must be initialized with values")
                    .emit();
                inferred = {"array<any>"};
                return;
            }
            std::string elementType = "unknown";
            for (const auto& elem : node.elements) {
                elementType = merge_inferred_type(elementType, check(elem, ctx).name);
            }
            if (elementType.empty() || elementType == "unknown") elementType = "any";
            inferred = {"array<" + elementType + ">"};
            return;
        } else if constexpr (std::is_same_v<T, DictLiteralExpr>) {
            if (node.entries.empty()) {
                DiagBuilder(result_, Severity::Error,
                    "Empty map literal `{}` is not allowed — provide at least one key-value pair",
                    "TC056", ctx.actionName())
                    .hint("Example: `Map<string,string> config = {\"key\": \"value\"};`")
                    .emit();
                inferred = {"map<string,any>"};
                return;
            }
            std::string valueType = "unknown";
            for (std::size_t i = 0; i < node.entries.size(); ++i) {
                const auto entryType = check(node.entries[i], ctx).name;
                if (i % 2 == 1) {
                    valueType = merge_inferred_type(valueType, entryType);
                }
            }
            if (valueType.empty() || valueType == "unknown") valueType = "any";
            inferred = {"map<string," + valueType + ">"};
            return;
        } else if constexpr (std::is_same_v<T, FunctionCallExpr>) {

            // Walk arguments so identifiers inside them are marked used (TC120)
            // and argument expressions are type-checked. The collection-literal
            // and removed-builtin branches above handle their own args.
            for (const auto& arg : node.args) {
                (void)check(arg, ctx);
            }

            // action or builtin
            auto resolve_action = [&](const std::string& name) -> const Action* {
                auto it = tc_.actions_.find(name);
                if (it != tc_.actions_.end()) return it->second;
                if (name.find("::") != std::string::npos) return nullptr;
                const Action* found = nullptr;
                const std::string suffix = "::" + name;
                for (const auto& kv : tc_.actions_) {
                    const std::string& cand = kv.first;
                    if (cand.size() > suffix.size() && cand.rfind(suffix) == cand.size() - suffix.size()) {
                        if (found) return nullptr;
                        found = kv.second;
                    }
                }
                return found;
            };
            const Action* resolved = resolve_action(node.name);
            auto aIt = resolved ? tc_.actions_.find(resolved->name) : tc_.actions_.end();
            if (aIt != tc_.actions_.end()) {
                tc_.actionUsage_[aIt->second->name].referenced = true;
                if (aIt->second->params.size() != node.args.size()) {
                    DiagBuilder(result_, Severity::Error,
                        "Param count mismatch calling action " + node.name + ": got " + std::to_string(node.args.size()) +
                        ", expected " + std::to_string(aIt->second->params.size()),
                        "TC020", ctx.actionName())
                        .hint("Declare params in `action " + node.name + "(...)` or pass the correct number of arguments")
                        .emit();
                }
                inferred = { aIt->second->returnType.empty()?"void":aIt->second->returnType };
            } else {
                std::string builtinLookup = node.name;
                if (auto mapped = resolve_builtin_module_alias_call(ctx.program, node.name)) {
                    builtinLookup = *mapped;
                }
                auto bIt = tc_.builtins_.find(builtinLookup);
                if (bIt == tc_.builtins_.end()) {
                    // Unknown calls in expression position must surface as TC001
                    // instead of silently inferring "unknown". Dotted/qualified
                    // names resolve at runtime (entity/struct methods), so only
                    // flag plain identifiers.
                    if (node.name.find('.') == std::string::npos &&
                        node.name.find("::") == std::string::npos) {
                        emit_unknown_call(result_, node.name, ctx.actionName());
                    }
                    inferred = {"unknown"};
                } else {
                    auto& bi = bIt->second;
                    if ((int)node.args.size() < bi.minParams || (bi.maxParams >= 0 && (int)node.args.size() > bi.maxParams)) {
                        DiagBuilder(result_, Severity::Error,
                            "Param count mismatch calling builtin " + node.name + ": got " + std::to_string(node.args.size()) +
                            ", expected " + format_param_range(bi.minParams, bi.maxParams),
                            "TC021", ctx.actionName())
                            .hint("Builtin signature: " + node.name + "(" + format_param_range(bi.minParams, bi.maxParams) + ")")
                            .emit();
                    }
                    inferred = { bIt->second.returnType };
                }
            }
        }
    }, e->node);
    cache_[e.get()] = inferred;
    return inferred;
}

TypeInfo ExprChecker::require_bool(const ExprPtr& e, CheckContext& ctx, const std::string& code, const std::string& msg) {
    auto t = check(e, ctx);
    if (!TypeChecker::is_bool_condition_type(t)) {
        DiagBuilder(result_, Severity::Error, msg, code, ctx.actionName()).emit();
    }
    if (e && std::holds_alternative<BinaryExpr>(e->node)) {
        const auto& b = std::get<BinaryExpr>(e->node);
        if (b.op == BinOp::EQ || b.op == BinOp::NE) {
            auto is_string_bool_lit = [](const ExprPtr& side) -> bool {
                if (!side || !std::holds_alternative<ExprString>(side->node)) return false;
                const auto& lit = std::get<ExprString>(side->node).v;
                return lit == "true" || lit == "false";
            };
            if (is_string_bool_lit(b.left) || is_string_bool_lit(b.right)) {
                DiagBuilder(result_, Severity::Warning,
                    "Compare bool to false/true, not string \"true\"/\"false\"",
                    "TC063", ctx.actionName()).emit();
            }
        }
    }
    return t;
}

// ================= StmtChecker =================
ReturnFlow StmtChecker::check_stmt(const Statement& s, CheckContext& ctx, ScopeManager& scopes, const std::string& retType) {
    ReturnFlow flow = ReturnFlow::NoReturn;
    std::visit([&](auto&& stmt){
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, PrintStmt>) {
            expr_.check(stmt.value, ctx);
        } else if constexpr (std::is_same_v<T, ImportStmt>) {
            // Imports are recorded on Program during parse; no per-stmt check needed.
        } else if constexpr (std::is_same_v<T, ActionCallStmt>) {
            auto resolve_action_name = [&](const std::string& name) -> std::string {
                if (tc_.actions_.count(name)) return name;
                if (name.find("::") != std::string::npos) return {};
                std::string found;
                const std::string suffix = "::" + name;
                for (const auto& kv : tc_.actions_) {
                    const std::string& cand = kv.first;
                    if (cand.size() > suffix.size() && cand.rfind(suffix) == cand.size() - suffix.size()) {
                        if (!found.empty()) return {};
                        found = cand;
                    }
                }
                return found;
            };
            const std::string resolvedName = resolve_action_name(stmt.name);
            auto it = resolvedName.empty() ? tc_.actions_.end() : tc_.actions_.find(resolvedName);
            if (it != tc_.actions_.end()) {
                tc_.actionUsage_[it->second->name].referenced = true;
                if (it->second->params.size() != stmt.args.size()) {
                    DiagBuilder(result_, Severity::Error,
                        "Param count mismatch calling action " + stmt.name + ": got " + std::to_string(stmt.args.size()) +
                        ", expected " + std::to_string(it->second->params.size()),
                        "TC020", ctx.actionName())
                        .hint("Declare params in `action " + stmt.name + "(...)` or pass the correct number of arguments")
                        .emit();
                }
            } else if (tc_.externActions_.count(stmt.name)) {
                // extern action declared: allow unresolved runtime binding
            } else {
                bool externResolved = false;
                if (stmt.name.find("::") == std::string::npos) {
                    const std::string suffix = "::" + stmt.name;
                    std::string foundExtern;
                    for (const auto& extName : tc_.externActions_) {
                        if (extName == stmt.name ||
                            (extName.size() > suffix.size() && extName.rfind(suffix) == extName.size() - suffix.size())) {
                            if (!foundExtern.empty()) {
                                foundExtern.clear();
                                break;
                            }
                            foundExtern = extName;
                        }
                    }
                    externResolved = !foundExtern.empty();
                }
                if (!externResolved) {
                    std::string builtinLookup = stmt.name;
                    if (auto mapped = resolve_builtin_module_alias_call(ctx.program, stmt.name)) {
                        builtinLookup = *mapped;
                    }
                    auto bIt = tc_.builtins_.find(builtinLookup);
                    if (bIt == tc_.builtins_.end()) {
                        emit_unknown_call(result_, stmt.name, ctx.actionName());
                    } else {
                        auto& bi = bIt->second;
                        if ((int)stmt.args.size() < bi.minParams || (bi.maxParams>=0 && (int)stmt.args.size() > bi.maxParams)) {
                            DiagBuilder(result_, Severity::Error,
                                "Param count mismatch calling builtin " + stmt.name + ": got " + std::to_string(stmt.args.size()) +
                                ", expected " + format_param_range(bi.minParams, bi.maxParams),
                                "TC021", ctx.actionName())
                                .hint("Builtin signature: " + stmt.name + "(" + format_param_range(bi.minParams, bi.maxParams) + ")")
                                .emit();
                        }
                    }
                }
            }
            for (auto& a : stmt.args) expr_.check(a, ctx);
        } else if constexpr (std::is_same_v<T, MethodCallStmt>) {
            for (auto& a : stmt.args) expr_.check(a, ctx);

            const std::string callName = stmt.objectName + "." + stmt.method;
            if (auto mapped = resolve_builtin_module_alias_call(ctx.program, callName)) {
                auto it = tc_.builtins_.find(*mapped);
                if (it == tc_.builtins_.end()) {
                    emit_unknown_call(result_, callName, ctx.actionName());
                } else {
                    const auto& bi = it->second;
                    if ((int)stmt.args.size() < bi.minParams || (bi.maxParams >= 0 && (int)stmt.args.size() > bi.maxParams)) {
                        DiagBuilder(result_, Severity::Error,
                            "Param count mismatch calling builtin " + callName + ": got " + std::to_string(stmt.args.size()) +
                            ", expected " + format_param_range(bi.minParams, bi.maxParams),
                            "TC021", ctx.actionName())
                            .hint("Call as `" + stmt.objectName + "." + stmt.method + "(...)` with " + format_param_range(bi.minParams, bi.maxParams))
                            .emit();
                    }
                }
                return;
            }

            if (ctx.program) {
                for (const auto& importDecl : ctx.program->imports) {
                    if (!importDecl.alias || *importDecl.alias != stmt.objectName) continue;
                    std::string normalizedPath = importDecl.path;
                    for (auto& ch : normalizedPath) if (ch == '\\') ch = '/';
                    std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                    if (normalizedPath.rfind("builtin/", 0) == 0 || normalizedPath.rfind("builtin:", 0) == 0) break;

                    auto actionIt = tc_.actions_.find(stmt.method);
                    if (actionIt == tc_.actions_.end()) {
                        DiagBuilder(result_, Severity::Error, "Unknown imported action: " + callName, "TC001", ctx.actionName())
                            .hint("Module `" + stmt.objectName + "` has no exported action `" + stmt.method + "` — check import path and `public action` visibility")
                            .emit();
                    } else if (actionIt->second->params.size() != stmt.args.size()) {
                        DiagBuilder(result_, Severity::Error,
                            "Param count mismatch calling action " + callName + ": got " + std::to_string(stmt.args.size()) +
                            ", expected " + std::to_string(actionIt->second->params.size()),
                            "TC020", ctx.actionName()).emit();
                    }
                    return;
                }
            }

            // Allow method syntax in manual mode:
            // defer exact receiver/member validation to runtime for now.
            if (ctx.scopes) {
                if (!ctx.scopes->lookup(stmt.objectName)) {
                    DiagBuilder(result_, Severity::Error,
                        "Use before declaration: " + stmt.objectName,
                        "TC010", ctx.actionName())
                        .hint("Declare `" + stmt.objectName + " = ...;` before use, or check import alias spelling")
                        .emit();
                }
            }
        } else if constexpr (std::is_same_v<T, LetStmt>) {
            if (scopes.lookup(stmt.name)) DiagBuilder(result_, Severity::Error, "Variable redeclaration: " + stmt.name, "TC030", ctx.actionName()).emit();
            VarInfo vi; vi.type = expr_.check(stmt.value, ctx); vi.isConst = stmt.isConst; vi.assigned=true;
            if (!stmt.declaredType.empty()) {
                std::string decl = stmt.declaredType;
                std::transform(decl.begin(), decl.end(), decl.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                decl.erase(std::remove_if(decl.begin(), decl.end(), [](unsigned char c){ return std::isspace(c) != 0; }), decl.end());
                if (ctx.program) {
                    for (const auto& alias : ctx.program->typeAliases) {
                        std::string aliasName = alias.name;
                        std::transform(aliasName.begin(), aliasName.end(), aliasName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                        aliasName.erase(std::remove_if(aliasName.begin(), aliasName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), aliasName.end());
                        if (aliasName == decl) {
                            decl = alias.targetType;
                            std::transform(decl.begin(), decl.end(), decl.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                            decl.erase(std::remove_if(decl.begin(), decl.end(), [](unsigned char c){ return std::isspace(c) != 0; }), decl.end());
                            break;
                        }
                    }
                }
                const std::string declNormalized = decl;
                bool known = true;
                std::string expected = "unknown";
                if (decl == "auto") {
                    expected = vi.type.name;
                } else if (decl == "any") {
                    expected = "unknown";
                } else if (decl == "int") {
                    expected = "int";
                } else if (decl == "u8" || decl == "u16" || decl == "u32" || decl == "u64" ||
                           decl == "i8" || decl == "i16" || decl == "i32" || decl == "i64" ||
                           decl == "uint" || decl == "unsigned" || decl == "unsignedint") {
                    expected = "int";
                } else if (decl == "double" || decl == "float") {
                    expected = "double";
                } else if (decl == "bool") {
                    expected = "bool";
                } else if (decl == "pointer") {
                    expected = "pointer";
                } else if (decl == "string" || decl == "str" || decl == "char") {
                    expected = "string";
                } else if (decl == "array") {
                    expected = "array<any>";
                } else if (decl.rfind("array<", 0) == 0) {
                    expected = decl;
                } else if (decl == "map" || decl == "dictionary") {
                    expected = "map<any,any>";
                } else if (decl.rfind("map<", 0) == 0) {
                    expected = decl;
                } else if (!decl.empty() && (decl.back() == '*' || decl.back() == '&')) {
                    expected = "pointer";
                } else if (ctx.program) {
                    bool matchedType = false;
                    for (const auto& sd : ctx.program->structs) {
                        std::string structName = sd.name;
                        std::transform(structName.begin(), structName.end(), structName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                        structName.erase(std::remove_if(structName.begin(), structName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), structName.end());
                        if (structName == declNormalized) {
                            expected = "struct:" + sd.name;
                            matchedType = true;
                            break;
                        }
                    }
                    if (!matchedType) {
                        for (const auto& en : ctx.program->entities) {
                            std::string entityName = en.name;
                            std::transform(entityName.begin(), entityName.end(), entityName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                            entityName.erase(std::remove_if(entityName.begin(), entityName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), entityName.end());
                            if (entityName == declNormalized) {
                                expected = "entity:" + en.name;
                                matchedType = true;
                                break;
                            }
                        }
                    }
                    if (!matchedType) {
                        for (const auto& en : ctx.program->enums) {
                            std::string enumName = en.name;
                            std::transform(enumName.begin(), enumName.end(), enumName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                            enumName.erase(std::remove_if(enumName.begin(), enumName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), enumName.end());
                            if (enumName == declNormalized) {
                                expected = "enum:" + en.name;
                                matchedType = true;
                                break;
                            }
                        }
                    }
                    if (!matchedType) {
                        known = false;
                        DiagBuilder(result_, Severity::Error, "Unknown declared type: " + stmt.declaredType, "TC041", ctx.actionName())
                            .hint("Known primitives: int, string, bool, double, void, pointer, array<T>, map<K,V>, struct:Name, enum:Name")
                            .emit();
                    }
                } else {
                    known = false;
                    DiagBuilder(result_, Severity::Error, "Unknown declared type: " + stmt.declaredType, "TC041", ctx.actionName())
                        .hint("Known primitives: int, string, bool, double, void, pointer, array<T>, map<K,V>, struct:Name, enum:Name")
                        .emit();
                }
                auto is_compatible = [&](const std::string& exp, const std::string& actual) {
                    if (exp == "unknown" || actual == "unknown") return true;
                    if (exp == actual) return true;
                    if (exp == "pointer" && actual == "pointer") return true;
                    if (collection_type_compatible(exp, actual)) return true;
                    if (exp.rfind("struct:", 0) == 0 && actual.rfind("dict:", 0) == 0) return true;
                    if (exp.rfind("struct:", 0) == 0 && (actual.rfind("map", 0) == 0 || actual == "map<any,any>")) return true;
                    return false;
                };
                const bool defaultNullInit =
                    stmt.value && std::holds_alternative<ExprNull>(stmt.value->node) && vi.type.name == "pointer";
                if (known && !is_compatible(expected, vi.type.name) && !defaultNullInit) {
                    DiagBuilder(result_, Severity::Error,
                        "Type mismatch in declaration: " + stmt.name,
                        "TC042", ctx.actionName()).emit();
                }
                if (known && expected != "unknown") {
                    vi.type = TypeInfo{expected};
                }
            }
            scopes.declare(stmt.name, vi);
        } else if constexpr (std::is_same_v<T, ReturnStmt>) {
            if (retType == "void") {
                if (stmt.value) expr_.check(*stmt.value, ctx);
            } else {
                auto t = stmt.value ? expr_.check(*stmt.value, ctx) : TypeInfo{"void"};
                const bool compatible =
                    t.name == "unknown" || t.name == retType ||
                    (retType == "int" && t.name == "bool");
                if (!compatible) {
                    DiagBuilder(result_, Severity::Error,
                        "Return type mismatch: expected " + retType + ", got " + t.name,
                        "TC040", ctx.actionName())
                        .hint("Change return expression type or declare action return type as `" + t.name + "`")
                        .emit();
                }
            }
            flow = ReturnFlow::AlwaysReturn;
        } else if constexpr (std::is_same_v<T, SetStmt>) {
            if (!stmt.isMember) {
                auto* v = scopes.lookup(stmt.varOrField);
                if (!v) {
                    DiagBuilder(result_, Severity::Error,
                        "Variable '" + stmt.varOrField + "' must be declared with an explicit type before use",
                        "TC053", ctx.actionName())
                        .hint("Use a typed declaration: `int " + stmt.varOrField + " = ...;` or `string " + stmt.varOrField + " = ...;`")
                        .emit();
                    VarInfo vi; vi.type = expr_.check(stmt.value, ctx); vi.isConst = false; vi.assigned = true;
                    scopes.declare(stmt.varOrField, vi);
                } else {
                    if (v->isConst) DiagBuilder(result_, Severity::Error, "Cannot assign to const variable: " + stmt.varOrField, "TC051", ctx.actionName()).emit();
                    auto valT = expr_.check(stmt.value, ctx);
                    auto assign_compatible = [&](const std::string& target, const std::string& source) {
                        if (target == "unknown" || source == "unknown") return true;
                        if (target == source) return true;
                        if (collection_type_compatible(target, source)) return true;
                        if (target.rfind("struct:", 0) == 0 && source.rfind("dict:", 0) == 0) return true;
                        if (target.rfind("struct:", 0) == 0 && (source.rfind("map", 0) == 0 || source == "map<any,any>")) return true;
                        return false;
                    };
                    if (v->type.name == "unknown") v->type = valT;
                    else if (!assign_compatible(v->type.name, valT.name)) {
                        DiagBuilder(result_, Severity::Error,
                            "Assignment type mismatch on " + stmt.varOrField + ": variable is " + v->type.name + ", value is " + valT.name,
                            "TC052", ctx.actionName())
                            .hint("Cast/coerce with toint/tostr/tobool, or redeclare with typed form: `" + valT.name + " " + stmt.varOrField + " = ...;`")
                            .emit();
                    }
                    v->assigned = true;
                }
            } else {
                expr_.check(stmt.value, ctx); // member semantics later
            }
        } else if constexpr (std::is_same_v<T, IfStmt>) {
            expr_.require_bool(stmt.cond, ctx, "TC060", "If condition not bool");
            auto guardThen = scopes.push();
            auto rfThen = check_block(*stmt.thenBlk, ctx, scopes, retType);
            ReturnFlow rfElse = ReturnFlow::NoReturn;
            if (stmt.elseBlk) { auto guardElse = scopes.push(); rfElse = check_block(*stmt.elseBlk, ctx, scopes, retType); }
            if (rfThen == ReturnFlow::AlwaysReturn && rfElse == ReturnFlow::AlwaysReturn) flow = ReturnFlow::AlwaysReturn;
        } else if constexpr (std::is_same_v<T, WhileStmt>) {
            expr_.require_bool(stmt.cond, ctx, "TC061", "While condition not bool");
            auto guard = scopes.push();
            CheckContext inner = ctx; inner.loop = LoopCtx::InLoop; inner.scopes = ctx.scopes;
            check_block(*stmt.body, inner, scopes, retType);
        } else if constexpr (std::is_same_v<T, DoWhileStmt>) {
            auto guard = scopes.push();
            CheckContext inner = ctx; inner.loop = LoopCtx::InLoop; inner.scopes = ctx.scopes;
            check_block(*stmt.body, inner, scopes, retType);
            expr_.require_bool(stmt.cond, ctx, "TC064", "Do-while condition not bool");
        } else if constexpr (std::is_same_v<T, RepeatStmt>) {
            auto countType = expr_.check(stmt.count, ctx);
            if (countType.name != "unknown" && !TypeChecker::is_int(countType)) {
                DiagBuilder(result_, Severity::Error, "Repeat count must be int", "TC063", ctx.actionName()).emit();
            }
            auto guard = scopes.push();
            CheckContext inner = ctx; inner.loop = LoopCtx::InLoop; inner.scopes = ctx.scopes;
            check_block(*stmt.body, inner, scopes, retType);
        } else if constexpr (std::is_same_v<T, ForStmt>) {
            auto guard = scopes.push();
            if (stmt.init) check_block(*stmt.init, ctx, scopes, retType);
            if (stmt.cond) expr_.require_bool(*stmt.cond, ctx, "TC062", "For condition not bool");
            if (stmt.step) check_block(*stmt.step, ctx, scopes, retType);
            CheckContext inner = ctx; inner.loop = LoopCtx::InLoop; inner.scopes = ctx.scopes;
            check_block(*stmt.body, inner, scopes, retType);
        } else if constexpr (std::is_same_v<T, ForInStmt>) {
            auto guard = scopes.push();
            if (stmt.varType.empty()) {
                DiagBuilder(result_, Severity::Error,
                    "Loop variable '" + stmt.var + "' must have an explicit type annotation",
                    "TC054", ctx.actionName())
                    .hint("Add a type before the variable name — e.g. `for (string item : items)` or `for (int n : nums)`")
                    .emit();
            }
            VarInfo vi; vi.type = {stmt.varType.empty() ? "unknown" : stmt.varType}; vi.assigned=true; scopes.declare(stmt.var, vi);
            if (stmt.valueVar) {
                VarInfo vv; vv.assigned=true; scopes.declare(*stmt.valueVar, vv);
            }
            expr_.check(stmt.iterable, ctx); // iterable validation later
            CheckContext inner = ctx; inner.loop = LoopCtx::InLoop; inner.scopes = ctx.scopes;
            check_block(*stmt.body, inner, scopes, retType);
        } else if constexpr (std::is_same_v<T, TryCatchStmt>) {
            {
                auto guardTry = scopes.push();
                check_block(*stmt.tryBlk, ctx, scopes, retType);
            }
            {
                auto guardCatch = scopes.push();
                VarInfo errVar; errVar.type = {"string"}; errVar.assigned = true;
                scopes.declare(stmt.catchVar, errVar);
                check_block(*stmt.catchBlk, ctx, scopes, retType);
            }
        } else if constexpr (std::is_same_v<T, SwitchStmt>) {
            const auto selT = expr_.check(stmt.selector, ctx);
            for (auto& c : stmt.cases) {
                const std::string caseT = infer_switch_case_type(c.value);
                if (selT.name != "unknown" && caseT != "unknown" && selT.name != caseT) {
                    DiagBuilder(result_, Severity::Warning,
                        "Switch case `" + c.value + "` type (" + caseT + ") may not match selector (" + selT.name + ")",
                        "TC064", ctx.actionName())
                        .hint("Use case labels with the same type as `switch` expression, or cast selector")
                        .emit();
                }
                auto guardC = scopes.push();
                check_block(*c.body, ctx, scopes, retType);
            }
            if (stmt.defaultBlk) { auto guardD = scopes.push(); check_block(*stmt.defaultBlk, ctx, scopes, retType); }
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ParallelStmt>>) {
            if (stmt) { auto guardP = scopes.push(); check_block(stmt->body, ctx, scopes, retType); }
        } else if constexpr (std::is_same_v<T, UnsafeStmt>) {
            auto guardU = scopes.push();
            check_block(*stmt.body, ctx, scopes, retType);
        } else if constexpr (std::is_same_v<T, PointerSetStmt>) {
            expr_.check(stmt.pointer, ctx);
            expr_.check(stmt.value, ctx);
        }
    }, s);
    return flow;
}

ReturnFlow StmtChecker::check_block(const Block& b, CheckContext& ctx, ScopeManager& scopes, const std::string& retType) {
    ReturnFlow flow = ReturnFlow::NoReturn;
    for (const auto& s : b.stmts) {
        if (flow == ReturnFlow::AlwaysReturn) {
            DiagBuilder(result_, Severity::Warning, "Unreachable code after return", "TC070", ctx.actionName()).emit();
            break;
        }
        auto f = check_stmt(s, ctx, scopes, retType);
        if (f == ReturnFlow::AlwaysReturn) flow = ReturnFlow::AlwaysReturn; else if (f == ReturnFlow::MaybeReturn && flow == ReturnFlow::NoReturn) flow = ReturnFlow::MaybeReturn;
    }
    return flow;
}

// =============== Collection / Program Passes ===============
void TypeChecker::pass_collect(const Program& program) {
    actions_.clear(); entities_.clear(); methods_.clear(); entityFields_.clear();
    actionUsage_.clear(); entityUsage_.clear(); methodUsage_.clear();
    externActions_.clear();
    init_builtins();
    for (const auto& a : program.actions) { actions_[a.name] = &a; actionUsage_[a.name]; }
    for (const auto& e : program.externs) { externActions_.insert(e.name); }
    for (const auto& e : program.entities) {
        entities_[e.name] = &e; entityUsage_[e.name];
        std::unordered_map<std::string,const Action*> mm; std::unordered_set<std::string> fields;
        for (auto& f : e.fields) fields.insert(f.name); entityFields_[e.name]=fields;
        std::unordered_map<std::string,MethodUsage> mu;
        for (auto& m : e.methods) { mm[m.name] = &m; mu[m.name]; }
        methods_[e.name]=std::move(mm); methodUsage_[e.name]=std::move(mu);
    }
}

void TypeChecker::pass_check_program(const Program& program, TCResult& out) {
    std::unordered_set<std::string> seen;
    for (auto& a : program.actions) if (!seen.insert(a.name).second) DiagBuilder(out, Severity::Error, "Duplicate action: " + a.name, "TC100", a.name).emit();
    std::unordered_set<std::string> seenStruct;
    for (auto& s : program.structs) if (!seenStruct.insert(s.name).second) DiagBuilder(out, Severity::Error, "Duplicate struct: " + s.name, "TC104", s.name).emit();
    for (auto& s : program.structs) {
        std::unordered_set<std::string> structMethodSeen;
        for (auto& m : s.methods) {
            if (!structMethodSeen.insert(m.name).second) {
                DiagBuilder(out, Severity::Error, "Duplicate method " + m.name + " in struct " + s.name, "TC107", s.name).emit();
            }
        }
    }
    std::unordered_set<std::string> seenEnum;
    for (auto& e : program.enums) if (!seenEnum.insert(e.name).second) DiagBuilder(out, Severity::Error, "Duplicate enum: " + e.name, "TC105", e.name).emit();
    std::unordered_set<std::string> seenAlias;
    for (auto& a : program.typeAliases) if (!seenAlias.insert(a.name).second) DiagBuilder(out, Severity::Error, "Duplicate type alias: " + a.name, "TC106", a.name).emit();
    std::unordered_set<std::string> seenE;
    for (auto& e : program.entities) {
        if (!seenE.insert(e.name).second) DiagBuilder(out, Severity::Error, "Duplicate entity: " + e.name, "TC101", e.name).emit();
        std::unordered_set<std::string> fSeen; for (auto& f : e.fields) if (!fSeen.insert(f.name).second) DiagBuilder(out, Severity::Error, "Duplicate field " + f.name + " in entity " + e.name, "TC102", e.name).emit();
        std::unordered_set<std::string> mSeen; for (auto& m : e.methods) if (!mSeen.insert(m.name).second) DiagBuilder(out, Severity::Error, "Duplicate method " + m.name + " in entity " + e.name, "TC103", e.name).emit();
    }
    if (program.runTarget) {
        if (!actions_.count(*program.runTarget)) DiagBuilder(out, Severity::Error, "Run target not found: " + *program.runTarget, "TC110", *program.runTarget).emit();
    } else {
        DiagBuilder(out, Severity::Error, "No run target set (expected action main or run directive)", "TC111", "program").emit();
    }

    // Walk a block recursively to check if any ReturnStmt has a value expression.
    std::function<bool(const Block&)> block_has_value_return = [&](const Block& blk) -> bool {
        for (const auto& s : blk.stmts) {
            if (const auto* r = std::get_if<ReturnStmt>(&s)) {
                if (r->value) return true;
            } else if (const auto* ifst = std::get_if<IfStmt>(&s)) {
                if (ifst->thenBlk && block_has_value_return(*ifst->thenBlk)) return true;
                if (ifst->elseBlk && block_has_value_return(*ifst->elseBlk)) return true;
            } else if (const auto* wh = std::get_if<WhileStmt>(&s)) {
                if (block_has_value_return(*wh->body)) return true;
            } else if (const auto* fi = std::get_if<ForInStmt>(&s)) {
                if (block_has_value_return(*fi->body)) return true;
            } else if (const auto* fo = std::get_if<ForStmt>(&s)) {
                if (block_has_value_return(*fo->body)) return true;
            } else if (const auto* tc = std::get_if<TryCatchStmt>(&s)) {
                if (block_has_value_return(*tc->tryBlk)) return true;
                if (block_has_value_return(*tc->catchBlk)) return true;
            }
        }
        return false;
    };

    ExprChecker expr(*this, out); StmtChecker stmt(*this, expr, out);
    for (auto& a : program.actions) {
        CheckContext ctx; ctx.program=&program; ctx.currentAction=&a;
        ScopeManager scopes; // base scope
        ctx.scopes = &scopes;
        // Warn if the action has return-value statements but no declared return type.
        if (a.returnType.empty() && block_has_value_return(a.body)) {
            DiagBuilder(out, Severity::Error,
                "Action '" + a.name + "' returns a value but has no declared return type",
                "TC058", a.name)
                .hint("Add a return type after the parameter list: `public action " + a.name + "(...): int { ... }`")
                .emit();
        }
        // seed params
        for (auto& p : a.params) {
            if (p.type.empty()) {
                DiagBuilder(out, Severity::Error,
                    "Parameter '" + p.name + "' in action '" + a.name + "' has no type annotation",
                    "TC057", a.name)
                    .hint("Add a type: `" + a.name + "(" + p.name + ": int)` or `" + a.name + "(int " + p.name + ")`")
                    .emit();
            }
            VarInfo vi; vi.type={p.type.empty()?"unknown":p.type}; vi.assigned=true; scopes.declare(p.name, vi);
        }
        // Register globals with explicit type annotations so action bodies can reference them
        for (auto& g : program.globals) {
            if (!g.typeName.empty()) {
                VarInfo gi; gi.type = {g.typeName}; gi.assigned = true; gi.used = true;
                scopes.declare(g.name, gi);
            }
        }
        auto rf = stmt.check_block(a.body, ctx, scopes, a.returnType.empty()?"void":a.returnType);
        for (auto& frame : scopes.all()) for (auto& kv : frame) if (!kv.second.used) DiagBuilder(out, Severity::Warning, "Unused variable: " + kv.first, "TC120", a.name).emit();
        if (!returns_void(a) && rf != ReturnFlow::AlwaysReturn) DiagBuilder(out, Severity::Error, "Missing return in action declared to return " + a.returnType, "TC121", a.name).emit();
    }
}

void TypeChecker::finalize_unused(const Program& program, TCResult& out) {
    for (auto& kv : actionUsage_) if (!kv.second.referenced && kv.first != program.runTarget.value_or("")) DiagBuilder(out, Severity::Warning, "Unused action: " + kv.first, "TC130", kv.first).emit();
    for (auto& kv : methodUsage_) for (auto& m : kv.second) if (!m.second.referenced) DiagBuilder(out, Severity::Warning, "Unused method: " + kv.first + "::" + m.first, "TC131", kv.first).emit();
    for (auto& kv : entityUsage_) if (!kv.second.referenced) DiagBuilder(out, Severity::Warning, "Unused entity: " + kv.first, "TC132", kv.first).emit();
}

TCResult TypeChecker::check(const Program& program) {
    // builtins_ must not leak across check() calls: a previous program's module
    // imports would otherwise grant builtins (e.g. system.cmd) to this program.
    builtins_.clear();
    init_builtins();
    register_imported_module_builtins(program);
    TCResult r;
    pass_collect(program);
    pass_check_program(program, r);
    finalize_unused(program, r);
    return r;
}

void TypeChecker::register_imported_module_builtins(const Program& program) {
    auto add = [&](std::string n, int minP, int maxP, std::string rt) {
        builtins_[std::move(n)] = BuiltinInfo{minP, maxP, std::move(rt)};
    };
    if (program_imports_module(&program, "builtin/network") || program_imports_module(&program, "builtin/net")) {
        add("http_get", 1, 1, "string");
        add("http_download", 2, 2, "bool");
        add("hls_download_best", 2, 2, "bool");
        add("url_encode", 1, 1, "string");
        add("network.ip.flush", 0, 0, "string");
        add("network.ip.release", 0, 1, "string");
        add("network.ip.renew", 0, 1, "string");
        add("network.ip.registerdns", 0, 0, "string");
        add("network.debug.enable", 0, 1, "string");
        add("network.debug.disable", 0, 0, "string");
        add("network.debug.status", 0, 0, "string");
        add("network.debug.last", 0, 0, "string");
        add("network.debug.clear", 0, 0, "string");
        add("network.debug.log_tail", 0, 1, "string");
        add("http_get_auth", 2, 2, "string");
        add("http_post_auth", 4, 4, "string");
    }
    if (program_imports_module(&program, "builtin/websocket") || program_imports_module(&program, "builtin/ws")) {
        add("ws_connect", 1, 1, "int");
        add("ws_send", 2, 2, "bool");
        add("ws_recv", 1, 1, "string");
        add("ws_recv_timeout", 2, 2, "string");
        add("ws_close", 1, 1, "void");
    }
    if (program_imports_module(&program, "builtin/regex")) {
        add("regex_match", 2, 2, "bool");
        add("regex_find", 2, 2, "string");
        add("regex_replace", 3, 3, "string");
    }
    if (program_imports_module(&program, "builtin/crypto")) {
        add("hash_fnv1a", 1, 1, "string");
        add("random_bytes", 1, 1, "string");
    }
    if (program_imports_module(&program, "builtin/binary")) {
        add("bin_new", 0, 0, "string");
        add("bin_push_u8", 2, 2, "void");
        add("bin_len", 1, 1, "int");
        add("bin_hex", 1, 1, "string");
        add("bin_from_hex", 1, 1, "string");
        add("bin_get_u8", 2, 2, "int");
    }
    if (program_imports_module(&program, "builtin/threads")) {
        add("thread_run", 1, 2, "string");
        add("thread_join", 1, 1, "bool");
        add("thread_join_timeout", 2, 2, "bool");
        add("thread_done", 1, 1, "bool");
        add("thread_list", 0, 0, "string");
        add("thread_wait_all", 0, 0, "void");
        add("thread_count", 0, 0, "int");
        add("thread_yield", 0, 0, "void");
        add("thread_gc", 0, 0, "void");
        add("thread_gc_all", 0, 0, "void");
        add("thread_purge", 0, 0, "void");
        add("thread_remove", 1, 2, "string");
        add("thread_state", 1, 1, "string");
    }
    if (program_imports_module(&program, "builtin/monitor")) {
        add("monitor_add", 1, 2, "string");
        add("monitor_remove", 1, 1, "void");
        add("monitor_list", 0, 0, "string");
        add("monitor_info", 1, 1, "string");
        add("monitor_last_change", 1, 1, "string");
        add("monitor_set_interval", 2, 2, "void");
    }
    if (program_imports_module(&program, "builtin/math")) {
        add("add", 2, 2, "int"); add("sub", 2, 2, "int"); add("mul", 2, 2, "int"); add("div", 2, 2, "int"); add("mod", 2, 2, "int");
        add("min", 2, 2, "int"); add("max", 2, 2, "int"); add("abs", 1, 1, "int");
        add("sin", 1, 1, "int"); add("cos", 1, 1, "int"); add("tan", 1, 1, "int");
        add("sqrt", 1, 1, "int"); add("pow", 2, 2, "int");
        add("collatz_len", 1, 1, "int"); add("collatz_sweep", 1, 1, "int");
        add("collatz_best_n", 0, 0, "int"); add("collatz_best_steps", 0, 0, "int");
        add("collatz_total_steps", 0, 0, "int"); add("collatz_avg_steps", 0, 0, "int");
    }
    if (program_imports_module(&program, "builtin/data")) {
        add("data_new", 0, 0, "string");
        add("data_set", 3, 3, "void");
        add("data_get", 2, 2, "string");
        add("data_has", 2, 2, "bool");
        add("data_keys", 1, 1, "string");
        add("data_save", 2, 2, "void");
        add("data_load", 1, 1, "string");
    }
    if (program_imports_module(&program, "builtin/perm")) {
        add("perm_grant", 1, 1, "void"); add("perm_revoke", 1, 1, "void");
        add("perm_has", 1, 1, "bool"); add("perm_list", 0, 0, "string");
    }
    if (program_imports_module(&program, "builtin/system")) {
        add("system.cmd", 1, 2, "string");
        add("system.execute", 1, 3, "int");
        add("system.output", 0, 0, "string");
        add("system.last_exit", 0, 0, "int");
        add("system.ip.flush", 0, 0, "string");
    }
    if (program_imports_module(&program, "builtin/path") || program_imports_module(&program, "builtin/erepath")) {
        add("path_join", 1, -1, "string"); add("path_dirname", 1, 1, "string");
        add("path_basename", 1, 1, "string"); add("path_ext", 1, 1, "string");
        add("file_exists", 1, 1, "bool");
    }
    if (program_imports_module(&program, "builtin/fs") || program_imports_module(&program, "builtin/erefs")) {
        add("read_text", 1, 1, "string"); add("write_text", 2, 2, "void"); add("append_text", 2, 2, "void");
        add("file_exists", 1, 1, "bool"); add("mkdirs", 1, 1, "void"); add("copy_file", 2, 2, "bool");
        add("move_file", 2, 2, "bool"); add("delete_file", 1, 1, "bool"); add("list_files", 1, 1, "unknown");
        add("load_elan", 1, 1, "string"); add("load_elan_dir", 1, 1, "unknown");
        add("call_action", 1, -1, "any");
        add("cwd", 0, 0, "string"); add("chdir", 1, 1, "bool");
        add("path_join", 1, -1, "string"); add("path_dirname", 1, 1, "string");
        add("path_basename", 1, 1, "string"); add("path_ext", 1, 1, "string");
        add("file_mtime", 1, 1, "int"); add("file_size", 1, 1, "int");
        add("file_open", 2, 2, "string"); add("file_close", 1, 1, "bool");
        add("file_read", 1, 2, "string"); add("file_write", 2, 2, "string");
        add("file_seek", 2, 3, "bool"); add("file_tell", 1, 1, "int");
        add("file_flush", 1, 1, "bool");
        add("fopen", 2, 2, "string"); add("fclose", 1, 1, "bool");
        add("fread", 1, 2, "string"); add("fwrite", 2, 2, "string");
        add("fseek", 2, 3, "bool"); add("ftell", 1, 1, "int");
        add("fflush", 1, 1, "bool");
    }
}

void TypeChecker::init_builtins() {
    auto add=[&](std::string n,int minP,int maxP,std::string rt){ builtins_[std::move(n)] = BuiltinInfo{minP,maxP,rt}; };
    add("now_ms",0,0,"int");
    add("env",1,1,"string");
    add("dotenv_load",0,1,"int");
    add("rand_int",0,2,"int");
    add("uuid",0,0,"string");
    add("args_count",0,0,"int");
    add("args_get",1,1,"string");
    add("read_line",0,0,"string");
    add("read_text",1,1,"string");
    add("input",0,1,"string");
    add("stderr_print",1,1,"void");
    add("exec",1,1,"int");
    add("spawn",1,1,"int");
    add("exit",1,1,"void");
    add("toint",1,1,"int");
    add("toInt",1,1,"int");
    add("tostr",1,1,"string");
    add("toString",1,1,"string");
    add("tofloat",1,1,"double");
    add("tobool",1,1,"bool");
    add("__builtin_sizeof",1,1,"int");
    add("__builtin_alignof",1,1,"int");
    add("__builtin_typeof",1,1,"string");
    add("__builtin_decltype",1,1,"string");
    add("__builtin_offsetof",2,2,"int");
    add("__builtin_is_base_of",2,2,"bool");
    add("dynamic_cast",2,2,"unknown");
    add("reinterpret_cast",2,2,"unknown");
    add("bit_cast",2,2,"unknown");
    add("to_json",1,1,"string");
    add("from_json",1,1,"map<any,any>");
    // Dynamic module loading (JS-style require) — always available
    add("load_elan",1,1,"string");
    add("load_elan_dir",1,1,"unknown");
    add("call_action",1,-1,"any");
    add("string.len",1,1,"int");
    add("string.strip",1,1,"string");
    add("string.lstrip",1,1,"string");
    add("string.rstrip",1,1,"string");
    add("string.lower",1,1,"string");
    add("string.upper",1,1,"string");
    add("string.find",2,2,"int");
    add("string.substr",2,3,"string");
    add("string.starts_with",2,2,"bool");
    add("string.ends_with",2,2,"bool");
    add("string.replace",3,3,"string");
    add("string.split",2,2,"unknown");
    add("now_iso",0,0,"string");
    add("is_int",1,1,"bool");
    add("is_float",1,1,"bool");
    add("char_is_digit",1,1,"bool");
    add("char_is_alpha",1,1,"bool");
    add("char_is_space",1,1,"bool");
    add("strbuf_new",0,1,"string");
    add("strbuf_append",2,2,"void");
    add("strbuf_to_string",1,1,"string");
    add("strbuf_len",1,1,"int");
    add("strbuf_clear",1,1,"void");
    add("strbuf_reserve",2,2,"void");
    add("strbuf_free",1,1,"void");
    add("list_new",0,-1,"unknown");
    add("list_push",2,2,"void");
    add("list_get",2,2,"string");
    add("list_len",1,1,"int");
    add("list_join",2,2,"string");
    add("dict_new",0,0,"unknown");
    add("dict_set",3,3,"void");
    add("dict_get",2,2,"string");
    add("dict_has",2,2,"bool");
    add("dict_keys",1,1,"unknown");
    add("dict_size",1,1,"int");
    add("language_name",0,0,"string");
    add("language_version",0,0,"string");
    add("plugin_core",2,2,"string");
    add("plugin_core_files",1,1,"string");
    add("plugin_core_keys",2,2,"string");
}

} // namespace erelang
