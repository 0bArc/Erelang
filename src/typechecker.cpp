#include "erelang/typechecker.hpp"
#include <algorithm>
#include <cctype>
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

} // namespace

// ================= ExprChecker =================
TypeInfo ExprChecker::check(const ExprPtr& e, CheckContext& ctx) {
    if (!e) return {"void"};
    auto itC = cache_.find(e.get()); if (itC!=cache_.end()) return itC->second;
    TypeInfo inferred{"unknown"};
    std::visit([&](auto&& node){
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ExprNumber>) inferred = {node.isFloatLiteral ? "double" : "int"};
        else if constexpr (std::is_same_v<T, ExprBool>) inferred = {"bool"};
        else if constexpr (std::is_same_v<T, ExprString>) inferred = {"string"};
        else if constexpr (std::is_same_v<T, ExprNull>) inferred = {"pointer"};
        else if constexpr (std::is_same_v<T, ExprIdent>) {
            if (ctx.scopes) {
                if (auto* v = ctx.scopes->lookup(node.name)) { v->used = true; inferred = v->type; }
                else {
                    DiagBuilder(result_, Severity::Error, "Use before declaration: " + node.name, "TC010", ctx.actionName()).emit();
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
            else inferred = check(node.expr, ctx);
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
        } else if constexpr (std::is_same_v<T, FunctionCallExpr>) {
            if (node.name == "list_new") {
                if (node.args.empty()) {
                    inferred = {"array<any>"};
                } else {
                    std::string mergedElementType;
                    for (const auto& arg : node.args) {
                        mergedElementType = merge_inferred_type(mergedElementType, check(arg, ctx).name);
                    }
                    if (mergedElementType.empty() || mergedElementType == "unknown") mergedElementType = "any";
                    inferred = {"array<" + mergedElementType + ">"};
                }
                return;
            }

            if (node.name == "dict_new" || node.name == "hashmap_new") {
                if (node.args.empty()) {
                    inferred = {"map<any,any>"};
                } else {
                    std::string mergedKeyType;
                    std::string mergedValueType;
                    for (size_t index = 0; index + 1 < node.args.size(); index += 2) {
                        mergedKeyType = merge_inferred_type(mergedKeyType, check(node.args[index], ctx).name);
                        mergedValueType = merge_inferred_type(mergedValueType, check(node.args[index + 1], ctx).name);
                    }
                    if ((node.args.size() % 2) != 0) {
                        check(node.args.back(), ctx);
                    }
                    if (mergedKeyType.empty() || mergedKeyType == "unknown") mergedKeyType = "any";
                    if (mergedValueType.empty() || mergedValueType == "unknown") mergedValueType = "any";
                    inferred = {"map<" + mergedKeyType + "," + mergedValueType + ">"};
                }
                return;
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
                inferred = { aIt->second->returnType.empty()?"void":aIt->second->returnType };
            } else {
                auto bIt = tc_.builtins_.find(node.name);
                inferred = bIt==tc_.builtins_.end()?TypeInfo{"unknown"}:TypeInfo{bIt->second.returnType};
            }
        }
    }, e->node);
    cache_[e.get()] = inferred;
    return inferred;
}

TypeInfo ExprChecker::require_bool(const ExprPtr& e, CheckContext& ctx, const std::string& code, const std::string& msg) {
    auto t = check(e, ctx);
    if (!TypeChecker::is_bool(t)) {
        DiagBuilder(result_, Severity::Error, msg, code, ctx.actionName()).emit();
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
                if (it->second->params.size() != stmt.args.size()) DiagBuilder(result_, Severity::Error, "Param count mismatch calling action " + stmt.name, "TC020", ctx.actionName()).emit();
            } else if (tc_.externActions_.count(stmt.name)) {
                // extern action declared: allow unresolved runtime binding
            } else {
                static const std::unordered_set<std::string> deprecatedBuiltins = {
                    "list_new", "list_push", "dict_new", "dict_set"
                };
                if (deprecatedBuiltins.count(stmt.name) > 0) {
                    DiagBuilder(result_, Severity::Warning,
                        "Deprecated builtin: " + stmt.name + " (prefer array/map literals and method-style usage)",
                        "TC140", ctx.actionName()).emit();
                }
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
                    auto bIt = tc_.builtins_.find(stmt.name);
                    if (bIt == tc_.builtins_.end()) {
                        DiagBuilder(result_, Severity::Error, "Unknown action: " + stmt.name, "TC001", ctx.actionName()).emit();
                    } else {
                        auto& bi = bIt->second;
                        if ((int)stmt.args.size() < bi.minParams || (bi.maxParams>=0 && (int)stmt.args.size() > bi.maxParams)) DiagBuilder(result_, Severity::Error, "Param count mismatch calling builtin " + stmt.name, "TC021", ctx.actionName()).emit();
                    }
                }
            }
            for (auto& a : stmt.args) expr_.check(a, ctx);
        } else if constexpr (std::is_same_v<T, MethodCallStmt>) {
            for (auto& a : stmt.args) expr_.check(a, ctx);
            std::string methodName = stmt.method;
            if (methodName == "put") methodName = "set";
            if (methodName == "contains") methodName = "has";
            if (methodName == "containsKey") methodName = "has";
            if (methodName == "getOrDefault") methodName = "getOr";

            if (auto* owner = scopes.lookup(stmt.objectName)) {
                if (owner->type.name.rfind("entity:", 0) == 0) {
                    const std::string entityName = owner->type.name.substr(7);
                    auto mit = tc_.methods_.find(entityName);
                    if (mit != tc_.methods_.end()) {
                        auto hit = mit->second.find(methodName);
                        if (hit == mit->second.end()) {
                            DiagBuilder(result_, Severity::Error, "Unknown method: " + stmt.objectName + "." + methodName, "TC022", ctx.actionName()).emit();
                        } else {
                            tc_.methodUsage_[entityName][methodName].referenced = true;
                            if (hit->second->params.size() != stmt.args.size()) {
                                DiagBuilder(result_, Severity::Error, "Param count mismatch calling method " + stmt.objectName + "." + methodName, "TC023", ctx.actionName()).emit();
                            }
                        }
                    }
                } else if (owner->type.name.rfind("struct:", 0) == 0 && ctx.program) {
                    const std::string structName = owner->type.name.substr(7);
                    const StructDecl* sd = nullptr;
                    for (const auto& s : ctx.program->structs) {
                        if (s.name == structName) { sd = &s; break; }
                    }
                    if (sd) {
                        const Action* method = nullptr;
                        for (const auto& m : sd->methods) {
                            if (m.name == methodName) { method = &m; break; }
                        }
                        if (!method) {
                            DiagBuilder(result_, Severity::Error, "Unknown struct method: " + structName + "." + methodName, "TC024", ctx.actionName()).emit();
                        } else if (method->params.size() != stmt.args.size()) {
                            DiagBuilder(result_, Severity::Error, "Param count mismatch calling struct method " + structName + "." + methodName, "TC025", ctx.actionName()).emit();
                        }
                    }
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
                    bool matchedStruct = false;
                    for (const auto& sd : ctx.program->structs) {
                        std::string structName = sd.name;
                        std::transform(structName.begin(), structName.end(), structName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                        structName.erase(std::remove_if(structName.begin(), structName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), structName.end());
                        if (structName == declNormalized) {
                            expected = "struct:" + sd.name;
                            matchedStruct = true;
                            break;
                        }
                    }
                    if (!matchedStruct) {
                        for (const auto& en : ctx.program->enums) {
                            std::string enumName = en.name;
                            std::transform(enumName.begin(), enumName.end(), enumName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                            enumName.erase(std::remove_if(enumName.begin(), enumName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), enumName.end());
                            if (enumName == declNormalized) {
                                expected = "enum:" + en.name;
                                matchedStruct = true;
                                break;
                            }
                        }
                    }
                    if (!matchedStruct) {
                        known = false;
                        DiagBuilder(result_, Severity::Error, "Unknown declared type: " + stmt.declaredType, "TC041", ctx.actionName()).emit();
                    }
                } else {
                    known = false;
                    DiagBuilder(result_, Severity::Error, "Unknown declared type: " + stmt.declaredType, "TC041", ctx.actionName()).emit();
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
                if (known && !is_compatible(expected, vi.type.name)) {
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
                if (t.name != "unknown" && t.name != retType) DiagBuilder(result_, Severity::Error, "Return type mismatch: expected " + retType + " got " + t.name, "TC040", ctx.actionName()).emit();
            }
            flow = ReturnFlow::AlwaysReturn;
        } else if constexpr (std::is_same_v<T, SetStmt>) {
            if (!stmt.isMember) {
                auto* v = scopes.lookup(stmt.varOrField);
                if (!v) DiagBuilder(result_, Severity::Error, "Assign to undeclared variable: " + stmt.varOrField, "TC050", ctx.actionName()).emit();
                else {
                    if (v->isConst) DiagBuilder(result_, Severity::Error, "Cannot assign to const variable: " + stmt.varOrField, "TC051", ctx.actionName()).emit();
                    auto valT = expr_.check(stmt.value, ctx);
                    if (v->type.name == "unknown") v->type = valT; else if (valT.name != "unknown" && v->type.name != valT.name) DiagBuilder(result_, Severity::Error, "Assignment type mismatch on " + stmt.varOrField, "TC052", ctx.actionName()).emit();
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
            VarInfo vi; vi.assigned=true; scopes.declare(stmt.var, vi);
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
            expr_.check(stmt.selector, ctx);
            for (auto& c : stmt.cases) { auto guardC = scopes.push(); check_block(*c.body, ctx, scopes, retType); }
            if (stmt.defaultBlk) { auto guardD = scopes.push(); check_block(*stmt.defaultBlk, ctx, scopes, retType); }
        } else if constexpr (std::is_same_v<T, ParallelStmt>) {
            auto guardP = scopes.push(); check_block(stmt.body, ctx, scopes, retType);
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

    ExprChecker expr(*this, out); StmtChecker stmt(*this, expr, out);
    for (auto& a : program.actions) {
        CheckContext ctx; ctx.program=&program; ctx.currentAction=&a;
        ScopeManager scopes; // base scope
        ctx.scopes = &scopes;
        // seed params
        for (auto& p : a.params) { VarInfo vi; vi.type={p.type.empty()?"unknown":p.type}; vi.assigned=true; scopes.declare(p.name, vi); }
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
    TCResult r; pass_collect(program); pass_check_program(program, r); finalize_unused(program, r); return r;
}

void TypeChecker::init_builtins() {
    if (!builtins_.empty()) return;
    auto add=[&](std::string n,int minP,int maxP,std::string rt){ builtins_[std::move(n)] = BuiltinInfo{minP,maxP,rt}; };
    // Core/time/env
    add("now_ms",0,0,"int"); add("now_iso",0,0,"string");
    add("env",1,1,"string"); add("username",0,0,"string"); add("machine_guid",0,0,"string"); add("computer_name",0,0,"string"); add("volume_serial",0,0,"string"); add("hwid",0,0,"string"); add("rand_int",0,2,"int"); add("uuid",0,0,"string");
    add("args_count",0,0,"int"); add("args_get",1,1,"string");
    add("os.args",0,0,"array<any>"); add("os.args_count",0,0,"int"); add("os.args_get",1,1,"string");
    add("exec",1,1,"int"); add("run_file",1,1,"void"); add("run_bat",1,1,"void"); add("read_line",0,0,"string"); add("input",0,1,"string"); add("prompt",1,1,"string");
    add("os.exec",1,1,"int"); add("spawn",1,1,"int"); add("os.spawn",1,1,"int"); add("exit",1,1,"void"); add("stdin_read",0,0,"string");
    add("stderr_print",1,1,"void");
    add("option_none",0,0,"unknown"); add("option_some",1,1,"unknown"); add("option_is_some",1,1,"bool"); add("option_unwrap_or",2,2,"unknown");
    add("option.none",0,0,"unknown"); add("option.some",1,1,"unknown"); add("option.is_some",1,1,"bool"); add("option.unwrap_or",2,2,"unknown");
    add("result_ok",1,1,"unknown"); add("result_err",1,1,"unknown"); add("result_is_ok",1,1,"bool"); add("result_unwrap_or",2,2,"unknown");
    add("result.ok",1,1,"unknown"); add("result.err",1,1,"unknown"); add("result.is_ok",1,1,"bool"); add("result.unwrap_or",2,2,"unknown");
    add("toint",1,1,"int"); add("toInt",1,1,"int"); add("tostr",1,1,"string"); add("toString",1,1,"string"); add("tofloat",1,1,"double"); add("tobool",1,1,"bool");
    add("dynamic_cast",2,2,"unknown");
    add("reinterpret_cast",2,2,"pointer");
    add("bit_cast",2,2,"unknown");
    add("bitcast",2,2,"unknown");
    add("__builtin_sizeof",1,1,"int");
    add("__builtin_alignof",1,1,"int");
    add("__builtin_typeof",1,1,"string");
    add("__builtin_decltype",1,1,"string");
    add("__builtin_offsetof",2,2,"int");
    add("__builtin_is_base_of",2,2,"bool");
    add("ptr_new",1,1,"pointer");
    add("ptr_get",1,1,"string");
    add("ptr_set",2,2,"void");
    add("ptr_free",1,1,"void");
    add("ptr_valid",1,1,"bool");
    add("malloc",1,1,"pointer");
    add("free",1,1,"void");
    add("make_unique",1,1,"pointer");
    add("make_shared",1,1,"pointer");
    add("unique_reset",1,1,"void");
    add("shared_reset",1,1,"void");
    add("to_json",1,1,"string"); add("from_json",1,1,"map<any,any>");
    add("string.lstrip",1,1,"string"); add("string.rstrip",1,1,"string"); add("string.strip",1,1,"string"); add("string.lower",1,1,"string"); add("string.upper",1,1,"string");
    add("string.starts_with",2,2,"bool"); add("string.ends_with",2,2,"bool"); add("string.find",2,2,"int"); add("string.substr",2,3,"string"); add("string.len",1,1,"int");
    // Filesystem
    add("read_text",1,1,"string"); add("write_text",2,2,"void"); add("append_text",2,2,"void"); add("file_exists",1,1,"bool"); add("mkdirs",1,1,"void"); add("copy_file",2,2,"bool"); add("move_file",2,2,"bool"); add("delete_file",1,1,"bool");
    add("file_size",1,1,"int");
    add("list_files",1,1,"unknown"); add("cwd",0,0,"string"); add("chdir",1,1,"bool");
    add("path_join",1,-1,"string"); add("path_dirname",1,1,"string"); add("path_basename",1,1,"string"); add("path_ext",1,1,"string");
    add("file_mtime",1,1,"int");
    add("file_open",2,2,"string"); add("file_close",1,1,"bool"); add("file_read",1,2,"string");
    add("file_write",2,2,"int"); add("file_seek",2,3,"bool"); add("file_tell",1,1,"int"); add("file_flush",1,1,"bool");
    add("fopen",2,2,"string"); add("fclose",1,1,"bool"); add("fread",1,2,"string");
    add("fwrite",2,2,"int"); add("fseek",2,3,"bool"); add("ftell",1,1,"int"); add("fflush",1,1,"bool");
    add("strbuf_new",0,1,"string"); add("strbuf_append",2,2,"void"); add("strbuf_clear",1,1,"void");
    add("strbuf_len",1,1,"int"); add("strbuf_to_string",1,1,"string"); add("strbuf_free",1,1,"void"); add("strbuf_reserve",2,2,"void");
    add("string_buffer_new",0,1,"string"); add("string_buffer_append",2,2,"void"); add("string_buffer_clear",1,1,"void");
    add("string_buffer_len",1,1,"int"); add("string_buffer_to_string",1,1,"string"); add("string_buffer_free",1,1,"void"); add("string_buffer_reserve",2,2,"void");
    add("color.red",1,1,"string"); add("color.green",1,1,"string"); add("color.yellow",1,1,"string"); add("color.blue",1,1,"string");
    add("color.magenta",1,1,"string"); add("color.cyan",1,1,"string"); add("color.bold",1,1,"string"); add("color.reset",0,0,"string");
    // Collections
    add("list_new",0,-1,"array<any>"); add("list_push",2,2,"void"); add("list_get",2,2,"unknown"); add("list_len",1,1,"int"); add("list_join",2,2,"string"); add("list_clear",1,1,"void"); add("list_remove_at",2,2,"void");
    add("set_new",0,-1,"unknown"); add("set_add",2,2,"bool"); add("set_has",2,2,"bool"); add("set_remove",2,2,"bool"); add("set_size",1,1,"int"); add("set_values",1,1,"array<any>");
    add("set_union",2,2,"unknown"); add("set_intersect",2,2,"unknown"); add("set_diff",2,2,"unknown");
    add("queue_new",0,-1,"unknown"); add("queue_push",2,2,"void"); add("queue_pop",1,1,"unknown"); add("queue_peek",1,1,"unknown"); add("queue_len",1,1,"int"); add("queue_clear",1,1,"void");
    add("dict_new",0,-1,"map<any,any>"); add("dict_set",3,-1,"void"); add("dict_get",2,-1,"unknown"); add("dict_has",2,-1,"bool"); add("dict_keys",1,1,"array<any>"); add("dict_values",1,1,"array<any>");
    add("dict_get_or",3,-1,"unknown"); add("dict_remove",2,-1,"bool"); add("dict_clear",1,1,"void"); add("dict_size",1,1,"int");
    add("dict_merge",2,2,"void"); add("dict_clone",1,1,"map<any,any>"); add("dict_items",1,1,"array<any>"); add("dict_entries",1,1,"array<any>");
    add("dict_set_path",3,3,"void"); add("dict_get_path",2,3,"unknown"); add("dict_has_path",2,2,"bool"); add("dict_remove_path",2,2,"bool");
    add("hashmap_new",0,-1,"map<any,any>"); add("hashmap_set",3,-1,"void"); add("hashmap_put",3,-1,"void"); add("hashmap_get",2,-1,"unknown");
    add("hashmap_has",2,-1,"bool"); add("hashmap_contains",2,-1,"bool"); add("hashmap_get_or",3,-1,"unknown"); add("hashmap_get_or_default",3,-1,"unknown");
    add("hashmap_remove",2,-1,"bool"); add("hashmap_clear",1,1,"void"); add("hashmap_size",1,1,"int"); add("hashmap_keys",1,1,"array<any>");
    add("hashmap_values",1,1,"array<any>"); add("hashmap_merge",2,2,"void");
    add("table_new",0,0,"unknown"); add("table_put",4,4,"void"); add("table_get",3,4,"unknown"); add("table_has",3,3,"bool");
    add("table_remove",3,3,"bool"); add("table_rows",1,1,"unknown"); add("table_columns",1,1,"unknown"); add("table_row_keys",2,2,"unknown");
    add("table_clear_row",2,2,"void"); add("table_count_row",2,2,"int");
    // Network
    add("http_get",1,1,"string"); add("http_download",2,2,"bool"); add("hls_download_best",2,2,"bool"); add("url_encode",1,1,"string");
    add("network.ip.flush",0,0,"string"); add("network.ip.release",0,1,"string"); add("network.ip.renew",0,1,"string"); add("network.ip.registerdns",0,0,"string");
    add("network.debug.enable",0,1,"string"); add("network.debug.disable",0,0,"string");
    add("network.debug.status",0,0,"string"); add("network.debug.last",0,0,"string");
    add("network.debug.clear",0,0,"string"); add("network.debug.log_tail",0,1,"string");
    add("char_is_digit",1,1,"bool"); add("char_is_space",1,1,"bool"); add("char_is_alpha",1,1,"bool"); add("char_is_ident_start",1,1,"bool"); add("char_is_ident_part",1,1,"bool");
    // Language info
    add("language_name",0,0,"string"); add("language_version",0,0,"string"); add("language_about",0,0,"string"); add("language_limitations",0,0,"string");
    // GUI / windowing (Windows only semantics, but we still typecheck symbol existence)
    add("win_window_create",3,3,"string"); // title,w,h -> "win:<id>"
    add("win_button_create",7,7,"void");   // handle,id,text,x,y,w,h
    add("win_checkbox_create",7,7,"void");
    add("win_radiobutton_create",7,8,"void"); // optional groupStart
    add("win_slider_create",9,9,"void");
    add("win_label_create",6,6,"void");
    add("win_textbox_create",6,6,"void");
    add("win_set_title",2,2,"void");
    add("win_move",3,3,"void");
    add("win_resize",3,3,"void");
    add("win_set_text",3,3,"void");
    add("win_get_text",2,2,"string");
    add("win_get_check",2,2,"bool");
    add("win_set_check",3,3,"void");
    add("win_get_slider",2,2,"int");
    add("win_set_slider",3,3,"void");
    add("win_on",3,3,"void");
    add("win_show",1,1,"void");
    add("win_close",1,1,"void");
    add("win_set_scale",2,2,"void");
    add("win_auto_scale",1,1,"void");
    // win_message_box(handle,title,message[,iconKind])
    add("win_message_box",3,4,"void");
    add("win_loop",1,1,"void");
        add("ui_window_create",3,8,"string");
        add("ui_label",2,4,"void");
        add("ui_button",3,6,"void");
        add("ui_checkbox",3,7,"void");
        add("ui_radio",3,8,"void");
        add("ui_slider",5,8,"void");
        add("ui_textbox",2,6,"void");
        add("ui_same_line",1,1,"void");
        add("ui_newline",1,1,"void");
        add("ui_spacer",1,3,"void");
        add("ui_separator",1,3,"void");
        add("ui_load",1,4,"string");

    // Data store builtins
    add("data_new",0,0,"string");          // returns handle data:<id>
    add("data_set",3,3,"void");            // handle,key,value
    add("data_get",2,2,"string");          // handle,key -> value or empty
    add("data_has",2,2,"bool");            // handle,key -> bool
    add("data_keys",1,1,"string");         // comma separated keys (temporary design)
    add("data_save",2,2,"void");           // handle,path
    add("data_load",1,1,"string");         // path -> new handle

    // Math / numeric helpers
    add("add",2,2,"int"); add("sub",2,2,"int"); add("mul",2,2,"int"); add("div",2,2,"int"); add("mod",2,2,"int");
    add("min",2,2,"int"); add("max",2,2,"int"); add("abs",1,1,"int");
    add("sin",1,1,"int"); add("cos",1,1,"int"); add("tan",1,1,"int"); // currently return numeric string; treat as int
    add("sqrt",1,1,"int"); add("pow",2,2,"int");
    add("collatz_len",1,1,"int"); add("collatz_sweep",1,1,"int"); add("collatz_best_n",0,0,"int"); add("collatz_best_steps",0,0,"int"); add("collatz_total_steps",0,0,"int"); add("collatz_avg_steps",0,0,"int");

    // Crypto
    add("hash_fnv1a",1,1,"string"); add("random_bytes",1,1,"string");

    // Regex
    add("regex_match",2,2,"bool"); add("regex_find",2,2,"string"); add("regex_replace",3,3,"string");

    // Binary buffers
    add("bin_new",0,0,"string"); add("bin_push_u8",2,2,"void"); add("bin_len",1,1,"int"); add("bin_hex",1,1,"string"); add("bin_from_hex",1,1,"string"); add("bin_get_u8",2,2,"int");

    // Permissions
    add("perm_grant",1,1,"void"); add("perm_revoke",1,1,"void"); add("perm_has",1,1,"bool"); add("perm_list",0,0,"string");

    // Threads
    add("thread_run",1,2,"string"); add("thread_join",1,1,"bool"); add("thread_join_timeout",2,2,"bool"); add("thread_done",1,1,"bool");
    add("thread_list",0,0,"string"); add("thread_wait_all",0,0,"void"); add("thread_count",0,0,"int"); add("thread_yield",0,0,"void");
    add("thread_gc",0,0,"void"); add("thread_gc_all",0,0,"void"); add("thread_purge",0,0,"void"); add("thread_remove",1,2,"string"); add("thread_state",1,1,"string");

    // Monitor
    add("monitor_add",1,2,"string"); add("monitor_remove",1,1,"void"); add("monitor_list",0,0,"string"); add("monitor_info",1,1,"string");
    add("monitor_last_change",1,1,"string"); add("monitor_set_interval",2,2,"void");
        add("plugin_core",2,2,"string"); add("plugin_core_files",1,1,"string"); add("plugin_core_keys",2,2,"string");
}

} // namespace erelang
