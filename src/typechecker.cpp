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
    // Strict: conflicting types produce "unknown" (which triggers an error downstream)
    if (left == "any") return right;
    if (right == "any") return left;
    return "unknown";
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

void check_action_arg_types(TypeChecker& tc, TCResult& result, const Action& action,
                            const std::vector<ExprPtr>& args, ExprChecker& expr, CheckContext& ctx,
                            const std::string& callName) {
    if (action.params.size() != args.size()) return;
    for (size_t i = 0; i < action.params.size(); ++i) {
        if (action.params[i].type.empty()) continue;
        bool known = true;
        TypeInfo expected = tc.resolve_type(action.params[i].type, ctx.program, &known);
        if (!known) continue;
        TypeInfo actual = expr.check(args[i], ctx);
        if (!tc.is_assignable(actual, expected)) {
            DiagBuilder(result, Severity::Error,
                "Argument type mismatch calling " + callName + ": param `" + action.params[i].name +
                "` expects " + expected.name + ", got " + actual.name,
                "TC022", ctx.actionName())
                .hint("Pass a value assignable to `" + expected.name + "`")
                .emit();
        }
    }
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

bool TypeChecker::types_equal(const TypeInfo& a, const TypeInfo& b) const {
    return a.name == b.name;
}

bool TypeChecker::is_assignable(const TypeInfo& from, const TypeInfo& to) const {
    if (to.name == "any" || from.name == "any") return true;
    if (to.name == "unknown" || from.name == "unknown") return false;
    if (types_equal(from, to)) return true;

    auto element_ok = [](const std::string& expected, const std::string& actual) {
        if (expected == "any" || actual == "any") return true;
        if (expected == "unknown" || actual == "unknown") return false;
        return expected == actual;
    };

    std::string toArrayElem;
    std::string fromArrayElem;
    if (parse_array_type(to.name, toArrayElem)) {
        if (from.name == "array<any>") return true;
        if (!parse_array_type(from.name, fromArrayElem)) return false;
        return element_ok(toArrayElem, fromArrayElem);
    }

    std::string toMapKey;
    std::string toMapValue;
    std::string fromMapKey;
    std::string fromMapValue;
    if (parse_map_type(to.name, toMapKey, toMapValue)) {
        if (from.name == "map<any,any>") return true;
        if (!parse_map_type(from.name, fromMapKey, fromMapValue)) return false;
        return element_ok(toMapKey, fromMapKey) && element_ok(toMapValue, fromMapValue);
    }

    if (to.name.rfind("struct:", 0) == 0 && from.name.rfind("dict:", 0) == 0) return true;
    if (to.name.rfind("struct:", 0) == 0 && (from.name.rfind("map", 0) == 0 || from.name == "map<any,any>")) return true;
    return false;
}

bool TypeChecker::is_convertible(const TypeInfo& from, const TypeInfo& to) const {
    return to.name == "int" && from.name == "bool";
}

TypeInfo TypeChecker::resolve_type(const std::string& syntax, const Program* program, bool* known) const {
    if (known) *known = true;
    std::string decl = syntax;
    std::transform(decl.begin(), decl.end(), decl.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    decl.erase(std::remove_if(decl.begin(), decl.end(), [](unsigned char c){ return std::isspace(c) != 0; }), decl.end());

    // Preserve original casing for generic application via TypeRef.
    TypeRef applied;
    try {
        applied = parse_type_ref_string(syntax);
    } catch (...) {
        applied = make_type_ref(syntax);
    }
    std::string baseName = applied.name;
    std::string baseLower = baseName;
    std::transform(baseLower.begin(), baseLower.end(), baseLower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    baseLower.erase(std::remove_if(baseLower.begin(), baseLower.end(), [](unsigned char c){ return std::isspace(c) != 0; }), baseLower.end());

    if (program) {
        for (const auto& alias : program->typeAliases) {
            std::string aliasName = alias.name;
            std::transform(aliasName.begin(), aliasName.end(), aliasName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            aliasName.erase(std::remove_if(aliasName.begin(), aliasName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), aliasName.end());
            if (aliasName == baseLower) {
                if (!alias.typeParams.empty()) {
                    if (applied.args.size() != alias.typeParams.size()) {
                        if (known) *known = false;
                        return TypeInfo{"unknown"};
                    }
                    std::unordered_map<std::string, std::string> subst;
                    for (size_t i = 0; i < alias.typeParams.size(); ++i) {
                        subst[alias.typeParams[i].name] = type_ref_canonical(applied.args[i]);
                    }
                    return resolve_type(substitute_type_string(alias.targetType, subst), program, known);
                }
                if (applied.args.empty()) {
                    return resolve_type(alias.targetType, program, known);
                }
            }
        }
    }

    const std::string declNormalized = baseLower;
    if (declNormalized == "auto") return TypeInfo{"auto"};
    if (declNormalized == "any") return TypeInfo{"any"};
    if (declNormalized == "void") return TypeInfo{"void"};
    if (declNormalized == "int") return TypeInfo{"int"};
    if (declNormalized == "u8" || declNormalized == "u16" || declNormalized == "u32" || declNormalized == "u64" ||
        declNormalized == "i8" || declNormalized == "i16" || declNormalized == "i32" || declNormalized == "i64" ||
        declNormalized == "uint" || declNormalized == "unsigned" || declNormalized == "unsignedint") {
        return TypeInfo{"int"};
    }
    if (declNormalized == "double" || declNormalized == "float") return TypeInfo{"double"};
    if (declNormalized == "bool") return TypeInfo{"bool"};
    if (declNormalized == "pointer") return TypeInfo{"pointer"};
    if (declNormalized == "string" || declNormalized == "str" || declNormalized == "char") return TypeInfo{"string"};
    if (declNormalized == "array") return TypeInfo{"array<any>"};
    if (baseLower == "array" || baseLower == "Array") {
        if (applied.args.empty()) return TypeInfo{"array<any>"};
        TypeInfo elem = resolve_type(type_ref_canonical(applied.args[0]), program, known);
        return TypeInfo{"array<" + elem.name + ">"};
    }
    if (decl.rfind("array<", 0) == 0) return TypeInfo{decl};
    if (declNormalized == "map" || declNormalized == "dictionary" || declNormalized == "hashmap") {
        if (applied.args.size() >= 2) {
            TypeInfo k = resolve_type(type_ref_canonical(applied.args[0]), program, known);
            TypeInfo v = resolve_type(type_ref_canonical(applied.args[1]), program, known);
            return TypeInfo{"map<" + k.name + "," + v.name + ">"};
        }
        return TypeInfo{"map<any,any>"};
    }
    if (decl.rfind("map<", 0) == 0) return TypeInfo{decl};
    if (!decl.empty() && (decl.back() == '*' || decl.back() == '&')) return TypeInfo{"pointer"};

    if (program) {
        for (const auto& sd : program->structs) {
            std::string structName = sd.name;
            std::transform(structName.begin(), structName.end(), structName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            structName.erase(std::remove_if(structName.begin(), structName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), structName.end());
            if (structName == declNormalized) {
                if (!sd.typeParams.empty()) {
                    if (applied.args.size() != sd.typeParams.size()) {
                        if (known) *known = false;
                        return TypeInfo{"unknown"};
                    }
                    std::string canon = sd.name + "<";
                    for (size_t i = 0; i < applied.args.size(); ++i) {
                        if (i) canon += ", ";
                        canon += type_ref_canonical(applied.args[i]);
                    }
                    canon += ">";
                    return TypeInfo{"struct:" + canon};
                }
                if (!applied.args.empty()) {
                    if (known) *known = false;
                    return TypeInfo{"unknown"};
                }
                return TypeInfo{"struct:" + sd.name};
            }
        }
        for (const auto& en : program->entities) {
            std::string entityName = en.name;
            std::transform(entityName.begin(), entityName.end(), entityName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            entityName.erase(std::remove_if(entityName.begin(), entityName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), entityName.end());
            if (entityName == declNormalized) {
                if (!en.typeParams.empty()) {
                    if (applied.args.size() != en.typeParams.size()) {
                        if (known) *known = false;
                        return TypeInfo{"unknown"};
                    }
                    std::string canon = en.name + "<";
                    for (size_t i = 0; i < applied.args.size(); ++i) {
                        if (i) canon += ", ";
                        canon += type_ref_canonical(applied.args[i]);
                    }
                    canon += ">";
                    return TypeInfo{"entity:" + canon};
                }
                return TypeInfo{"entity:" + en.name};
            }
        }
        for (const auto& en : program->enums) {
            std::string enumName = en.name;
            std::transform(enumName.begin(), enumName.end(), enumName.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            enumName.erase(std::remove_if(enumName.begin(), enumName.end(), [](unsigned char c){ return std::isspace(c) != 0; }), enumName.end());
            if (enumName == declNormalized) {
                if (!en.typeParams.empty()) {
                    if (applied.args.size() != en.typeParams.size()) {
                        if (known) *known = false;
                        return TypeInfo{"unknown"};
                    }
                    std::string canon = en.name + "<";
                    for (size_t i = 0; i < applied.args.size(); ++i) {
                        if (i) canon += ", ";
                        canon += type_ref_canonical(applied.args[i]);
                    }
                    canon += ">";
                    return TypeInfo{"enum:" + canon};
                }
                return TypeInfo{"enum:" + en.name};
            }
        }
    }

    if (known) *known = false;
    return TypeInfo{"unknown"};
}

bool TypeChecker::is_opaque_type(const TypeInfo& t, const CheckContext& ctx) const {
    return ctx.opaqueTypeParams.count(t.name) != 0;
}

void TypeChecker::push_opaque_params(CheckContext& ctx, const std::vector<TypeParam>& params) const {
    for (const auto& tp : params) {
        ctx.opaqueTypeParams.insert(tp.name);
        ctx.typeParamConstraints[tp.name] = tp.constraints;
    }
}

std::string TypeChecker::specialize_action_name(const std::string& name, const std::vector<std::string>& typeArgs) const {
    if (typeArgs.empty()) return name;
    std::string out = name + "<";
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i) out += ", ";
        out += typeArgs[i];
    }
    out += ">";
    return out;
}

bool TypeChecker::unify_type_args(const std::string& pattern, const std::string& concrete,
                                  std::unordered_map<std::string, std::string>& out,
                                  const std::vector<TypeParam>& typeParams) const {
    auto strip_prefix = [](std::string s) {
        if (s.rfind("struct:", 0) == 0) s = s.substr(7);
        else if (s.rfind("entity:", 0) == 0) s = s.substr(7);
        else if (s.rfind("enum:", 0) == 0) s = s.substr(5);
        return s;
    };
    TypeRef pat;
    TypeRef con;
    try {
        pat = parse_type_ref_string(pattern);
        con = parse_type_ref_string(strip_prefix(concrete));
    } catch (...) {
        return false;
    }
    auto isParam = [&](const std::string& n) {
        for (const auto& tp : typeParams) if (tp.name == n) return true;
        return false;
    };
    std::function<bool(const TypeRef&, const TypeRef&)> unify = [&](const TypeRef& p, const TypeRef& c) -> bool {
        if (isParam(p.name) && p.args.empty()) {
            auto it = out.find(p.name);
            const std::string canon = type_ref_canonical(c);
            if (it == out.end()) {
                out[p.name] = canon;
                return true;
            }
            return it->second == canon;
        }
        if (p.name != c.name || p.args.size() != c.args.size()) return false;
        for (size_t i = 0; i < p.args.size(); ++i) {
            if (!unify(p.args[i], c.args[i])) return false;
        }
        return true;
    };
    return unify(pat, con);
}

bool TypeChecker::check_constraints(const std::vector<TypeParam>& typeParams,
                                    const std::unordered_map<std::string, std::string>& subst,
                                    const Program* program, TCResult& out, const std::string& ctxName) const {
    if (!program) return true;
    bool ok = true;
    for (const auto& tp : typeParams) {
        if (tp.constraints.empty()) continue;
        auto sit = subst.find(tp.name);
        if (sit == subst.end()) continue;
        const std::string& concrete = sit->second;
        for (const auto& constraint : tp.constraints) {
            TypeRef needed = constraint;
            // Substitute trait type args too (Comparable<T>).
            needed = parse_type_ref_string(substitute_type_string(type_ref_canonical(constraint), subst));
            const TraitDecl* trait = nullptr;
            for (const auto& tr : program->traits) {
                if (tr.name == needed.name) { trait = &tr; break; }
            }
            if (!trait) {
                DiagBuilder(out, Severity::Error, "Unknown trait constraint: " + needed.name, "TC142", ctxName).emit();
                ok = false;
                continue;
            }
            // Structural: look for methods on struct/entity matching concrete type.
            std::string bare = concrete;
            const StructDecl* sd = nullptr;
            const Entity* ent = nullptr;
            if (bare.rfind("struct:", 0) == 0) bare = bare.substr(7);
            if (bare.rfind("entity:", 0) == 0) bare = bare.substr(7);
            std::string base = bare;
            auto lt = base.find('<');
            if (lt != std::string::npos) base = base.substr(0, lt);
            for (const auto& s : program->structs) if (s.name == base) { sd = &s; break; }
            for (const auto& e : program->entities) if (e.name == base) { ent = &e; break; }
            for (const auto& method : trait->methods) {
                bool found = false;
                auto check_action = [&](const Action& a) {
                    if (a.name != method.name) return;
                    if (a.params.size() != method.params.size()) return;
                    found = true;
                };
                if (sd) for (const auto& m : sd->methods) check_action(m);
                if (ent) for (const auto& m : ent->methods) check_action(m);
                // Primitive types: no structural methods unless builtin later.
                if (!found && concrete != "int" && concrete != "string" && concrete != "bool" && concrete != "double") {
                    DiagBuilder(out, Severity::Error,
                        "Type '" + concrete + "' does not satisfy trait '" + needed.name + "' (missing method '" + method.name + "')",
                        "TC143", ctxName).emit();
                    ok = false;
                }
            }
        }
    }
    return ok;
}

TypeInfo TypeChecker::instantiate_generic_type(const TypeRef& applied, const Program* program, bool* known) const {
    return resolve_type(type_ref_canonical(applied), program, known);
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
                            for (const auto& variant : en.variants) {
                                const auto& member = variant.name;
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
            if (tc_.is_opaque_type(lt, ctx) || tc_.is_opaque_type(rt, ctx)) {
                DiagBuilder(result_, Severity::Error,
                    "Opaque type parameter used in operator without a trait constraint",
                    "TC140", ctx.actionName())
                    .hint("Add a trait constraint such as `<T: Addable<T>>` and call a trait method instead of using '+' / arithmetic")
                    .emit();
                inferred = {"unknown"};
            } else
            switch (node.op) {
                case BinOp::Add:
                    if (TypeChecker::is_int(lt) && TypeChecker::is_int(rt)) { inferred = {"int"}; break; }
                    if (TypeChecker::is_string(lt) && TypeChecker::is_string(rt)) { inferred={"string"}; break; }
                    if ((TypeChecker::is_string(lt) && TypeChecker::is_int(rt)) || (TypeChecker::is_int(lt) && TypeChecker::is_string(rt))) {
                        DiagBuilder(result_, Severity::Error, "Illegal '+' operands", "TC011", ctx.actionName()).emit();
                    }
                    DiagBuilder(result_, Severity::Error, "Invalid '+' operand types: " + lt.name + " + " + rt.name, "TC011", ctx.actionName())
                        .hint("'+' requires int+int or string+string operands").emit();
                    inferred={"unknown"};
                    break;
                case BinOp::Sub: case BinOp::Mul: case BinOp::Div: case BinOp::Mod: case BinOp::Pow:
                    if (TypeChecker::is_int(lt) && TypeChecker::is_int(rt)) { inferred = {"int"}; break; }
                    DiagBuilder(result_, Severity::Error, "Invalid arithmetic operand types: " + lt.name + " " + rt.name, "TC013", ctx.actionName())
                        .hint("Arithmetic operators require int operands").emit();
                    inferred = {"unknown"};
                    break;
                case BinOp::BitAnd: case BinOp::BitXor: case BinOp::BitOr: case BinOp::Shl: case BinOp::Shr:
                    if (TypeChecker::is_int(lt) && TypeChecker::is_int(rt)) { inferred = {"int"}; break; }
                    DiagBuilder(result_, Severity::Error, "Invalid bitwise operand types: " + lt.name + " " + rt.name, "TC013", ctx.actionName())
                        .hint("Bitwise operators require int operands").emit();
                    inferred = {"unknown"};
                    break;
                case BinOp::And: case BinOp::Or:
                    if (TypeChecker::is_bool(lt) && TypeChecker::is_bool(rt)) { inferred={"bool"}; break; }
                    DiagBuilder(result_, Severity::Error, "Invalid logical operand types: " + lt.name + " " + rt.name, "TC014", ctx.actionName())
                        .hint("'&&' and '||' require bool operands").emit();
                    inferred = {"unknown"};
                    break;
                case BinOp::EQ: case BinOp::NE: case BinOp::StrictEQ: case BinOp::StrictNE:
                case BinOp::LT: case BinOp::LE: case BinOp::GT: case BinOp::GE:
                    inferred={"bool"}; break;
                default: inferred={"bool"}; break;
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
            } else if (node.op == UnOp::BitNot) {
                auto t = check(node.expr, ctx);
                if (t.name != "unknown" && !TypeChecker::is_int(t)) {
                    DiagBuilder(result_, Severity::Error, "Bitwise NOT requires an int operand, got " + t.name, "TC013", ctx.actionName()).emit();
                }
                inferred = {"int"};
            } else inferred = check(node.expr, ctx);
        } else if constexpr (std::is_same_v<T, NewExpr>) {
            bool known = true;
            inferred = tc_.resolve_type(node.typeName, ctx.program, &known);
            if (!known) inferred = TypeInfo{"entity:" + node.typeName};
            for (const auto& arg : node.args) (void)check(arg, ctx);
        } else if constexpr (std::is_same_v<T, MemberExpr>) {
            inferred = {"unknown"};
            bool handled = false;
            if (ctx.program) {
                TypeRef applied;
                try { applied = parse_type_ref_string(node.objectName); } catch (...) { applied = make_type_ref(node.objectName); }
                for (const auto& en : ctx.program->enums) {
                    if (en.name != applied.name && node.objectName != en.name) continue;
                    for (const auto& variant : en.variants) {
                        if (variant.name != node.field) continue;
                        if (!variant.payloads.empty()) {
                            DiagBuilder(result_, Severity::Error,
                                "Enum variant '" + variant.name + "' requires payload arguments",
                                "TC146", ctx.actionName()).emit();
                        }
                        bool known = true;
                        inferred = tc_.resolve_type(node.objectName, ctx.program, &known);
                        if (!known) {
                            if (en.typeParams.empty()) inferred = TypeInfo{"enum:" + en.name};
                            else inferred = TypeInfo{"enum:" + type_ref_canonical(applied)};
                        }
                        handled = true;
                        break;
                    }
                    if (handled) break;
                }
            }
            if (!handled && ctx.scopes) {
                if (auto* owner = ctx.scopes->lookup(node.objectName)) {
                    owner->used = true;
                    std::string otype = owner->type.name;
                    const std::string prefix = "struct:";
                    if (otype.rfind(prefix, 0) == 0 && ctx.program) {
                        std::string structName = otype.substr(prefix.size());
                        std::unordered_map<std::string, std::string> subst;
                        TypeRef applied;
                        try { applied = parse_type_ref_string(structName); } catch (...) { applied = make_type_ref(structName); }
                        std::string base = applied.name;
                        for (const auto& sd : ctx.program->structs) {
                            if (sd.name != base) continue;
                            if (!sd.typeParams.empty() && applied.args.size() == sd.typeParams.size()) {
                                for (size_t i = 0; i < sd.typeParams.size(); ++i) {
                                    subst[sd.typeParams[i].name] = type_ref_canonical(applied.args[i]);
                                }
                            }
                            for (const auto& field : sd.fields) {
                                if (field.name == node.field) {
                                    std::string ft = field.type.empty() ? "unknown" : field.type;
                                    if (!subst.empty()) ft = substitute_type_string(ft, subst);
                                    if (ctx.opaqueTypeParams.count(ft)) {
                                        inferred = TypeInfo{ft};
                                    } else {
                                        inferred = tc_.resolve_type(ft, ctx.program, nullptr);
                                    }
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
        } else if constexpr (std::is_same_v<T, LambdaExpr>) {
            inferred = {"string"}; // lambda returns a func:N handle string
            return;
        } else if constexpr (std::is_same_v<T, FunctionCallExpr>) {

            // Walk arguments so identifiers inside them are marked used (TC120)
            // and argument expressions are type-checked. The collection-literal
            // and removed-builtin branches above handle their own args.
            for (const auto& arg : node.args) {
                (void)check(arg, ctx);
            }

            // Enum variant construction: Option<int>.Some(1) or Status.Active as call with args
            bool enumConstructed = false;
            if (ctx.program) {
                const auto dot = node.name.rfind('.');
                if (dot != std::string::npos) {
                    const std::string typePart = node.name.substr(0, dot);
                    const std::string variantName = node.name.substr(dot + 1);
                    TypeRef applied;
                    try { applied = parse_type_ref_string(typePart); } catch (...) { applied = make_type_ref(typePart); }
                    for (const auto& en : ctx.program->enums) {
                        if (en.name != applied.name && typePart != en.name) continue;
                        std::unordered_map<std::string, std::string> subst;
                        if (!en.typeParams.empty() && applied.args.size() == en.typeParams.size()) {
                            for (size_t i = 0; i < en.typeParams.size(); ++i) {
                                subst[en.typeParams[i].name] = type_ref_canonical(applied.args[i]);
                            }
                        }
                        const EnumVariant* foundVariant = nullptr;
                        for (const auto& variant : en.variants) {
                            if (variant.name == variantName) { foundVariant = &variant; break; }
                        }
                        if (!foundVariant) {
                            DiagBuilder(result_, Severity::Error,
                                "Unknown variant `" + variantName + "` for enum `" + en.name + "`",
                                "TC150", ctx.actionName()).emit();
                            inferred = {"unknown"};
                            enumConstructed = true;
                            break;
                        }
                        if (foundVariant->payloads.size() != node.args.size()) {
                            DiagBuilder(result_, Severity::Error,
                                "Enum variant '" + variantName + "' expects " + std::to_string(foundVariant->payloads.size()) +
                                " payload(s), got " + std::to_string(node.args.size()),
                                "TC146", ctx.actionName()).emit();
                        } else {
                            for (size_t i = 0; i < node.args.size(); ++i) {
                                std::string pt = type_ref_canonical(foundVariant->payloads[i]);
                                if (!subst.empty()) pt = substitute_type_string(pt, subst);
                                TypeInfo expected = tc_.resolve_type(pt, ctx.program, nullptr);
                                TypeInfo actual = check(node.args[i], ctx);
                                if (!tc_.is_assignable(actual, expected) && actual.name != "unknown" && expected.name != "unknown") {
                                    DiagBuilder(result_, Severity::Error,
                                        "Enum variant '" + variantName + "' payload " + std::to_string(i) +
                                        " type mismatch: expected `" + expected.name + "`, got `" + actual.name + "`",
                                        "TC151", ctx.actionName()).emit();
                                }
                            }
                        }
                        bool known = true;
                        TypeInfo et = tc_.resolve_type(typePart, ctx.program, &known);
                        if (!known && en.typeParams.empty()) et = TypeInfo{"enum:" + en.name};
                        else if (!known) et = TypeInfo{"enum:" + type_ref_canonical(applied)};
                        inferred = et;
                        enumConstructed = true;
                        break;
                    }
                }
            }

            if (!enumConstructed) {
            // Trait method on opaque param: a.compare(b) as FunctionCallExpr "a.compare"
            {
                const auto dot = node.name.rfind('.');
                if (dot != std::string::npos && ctx.scopes) {
                    const std::string objectName = node.name.substr(0, dot);
                    const std::string methodName = node.name.substr(dot + 1);
                    if (auto* owner = ctx.scopes->lookup(objectName)) {
                        if (ctx.opaqueTypeParams.count(owner->type.name)) {
                            bool found = false;
                            std::string ret = "unknown";
                            auto cit = ctx.typeParamConstraints.find(owner->type.name);
                            if (cit != ctx.typeParamConstraints.end() && ctx.program) {
                                for (const auto& constraint : cit->second) {
                                    for (const auto& tr : ctx.program->traits) {
                                        if (tr.name != constraint.name) continue;
                                        for (const auto& m : tr.methods) {
                                            if (m.name != methodName) continue;
                                            found = true;
                                            ret = m.returnType.empty() ? "void" : m.returnType;
                                            break;
                                        }
                                    }
                                    if (found) break;
                                }
                            }
                            if (!found) {
                                DiagBuilder(result_, Severity::Error,
                                    "Method '" + methodName + "' not provided by constraints on type parameter '" + owner->type.name + "'",
                                    "TC147", ctx.actionName()).emit();
                                inferred = {"unknown"};
                            } else {
                                inferred = {ret};
                            }
                            enumConstructed = true; // skip normal call resolve
                        }
                    }
                }
            }
            if (!enumConstructed) {
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
                const Action& action = *aIt->second;
                std::vector<std::string> typeArgs = node.typeArgs;
                std::unordered_map<std::string, std::string> subst;
                if (!action.typeParams.empty()) {
                    if (typeArgs.empty()) {
                        // Infer from argument types.
                        for (size_t i = 0; i < action.params.size() && i < node.args.size(); ++i) {
                            if (action.params[i].type.empty()) continue;
                            TypeInfo argT = check(node.args[i], ctx);
                            if (!tc_.unify_type_args(action.params[i].type, argT.name, subst, action.typeParams)) {
                                // keep going; may fail later
                            }
                        }
                        for (const auto& tp : action.typeParams) {
                            if (!subst.count(tp.name)) {
                                DiagBuilder(result_, Severity::Error,
                                    "Cannot infer type parameter '" + tp.name + "' for call to " + node.name,
                                    "TC141", ctx.actionName())
                                    .hint("Pass explicit type arguments: `" + node.name + "<...>(...)`")
                                    .emit();
                            } else {
                                typeArgs.push_back(subst[tp.name]);
                            }
                        }
                        // Rebuild typeArgs in param order
                        typeArgs.clear();
                        for (const auto& tp : action.typeParams) {
                            auto it = subst.find(tp.name);
                            if (it != subst.end()) typeArgs.push_back(it->second);
                        }
                    } else if (typeArgs.size() != action.typeParams.size()) {
                        DiagBuilder(result_, Severity::Error,
                            "Type argument count mismatch calling " + node.name + ": got " +
                            std::to_string(typeArgs.size()) + ", expected " + std::to_string(action.typeParams.size()),
                            "TC144", ctx.actionName()).emit();
                    } else {
                        for (size_t i = 0; i < action.typeParams.size(); ++i) {
                            subst[action.typeParams[i].name] = typeArgs[i];
                        }
                    }
                    (void)tc_.check_constraints(action.typeParams, subst, ctx.program, result_, ctx.actionName());
                    (void)tc_.specialize_action_name(action.name, typeArgs);
                }
                if (action.params.size() != node.args.size()) {
                    DiagBuilder(result_, Severity::Error,
                        "Param count mismatch calling action " + node.name + ": got " + std::to_string(node.args.size()) +
                        ", expected " + std::to_string(action.params.size()),
                        "TC020", ctx.actionName())
                        .hint("Declare params in `action " + node.name + "(...)` or pass the correct number of arguments")
                        .emit();
                } else if (subst.empty()) {
                    check_action_arg_types(tc_, result_, action, node.args, *this, ctx, node.name);
                } else {
                    // Check args against substituted param types
                    for (size_t i = 0; i < action.params.size(); ++i) {
                        TypeInfo expected = tc_.resolve_type(substitute_type_string(action.params[i].type, subst), ctx.program, nullptr);
                        TypeInfo actual = check(node.args[i], ctx);
                        if (!tc_.is_assignable(actual, expected)) {
                            DiagBuilder(result_, Severity::Error,
                                "Argument type mismatch for " + node.name + " param '" + action.params[i].name +
                                "': expected " + expected.name + ", got " + actual.name,
                                "TC021", ctx.actionName()).emit();
                        }
                    }
                }
                std::string ret = action.returnType.empty() ? "void" : substitute_type_string(action.returnType, subst);
                inferred = { ret.empty() || ret == "void" ? "void" : tc_.resolve_type(ret, ctx.program, nullptr).name };
            } else {
                std::string builtinLookup = node.name;
                if (auto mapped = resolve_builtin_module_alias_call(ctx.program, node.name)) {
                    builtinLookup = *mapped;
                }
                auto bIt = tc_.builtins_.find(builtinLookup);
                if (bIt == tc_.builtins_.end()) {
                    // Check dotted imports for module action calls in expression context
                    bool resolvedModuleAction = false;
                    const auto dotPos = builtinLookup.find('.');
                    if (dotPos != std::string::npos && dotPos > 0 && ctx.program) {
                        const std::string alias = builtinLookup.substr(0, dotPos);
                        const std::string method = builtinLookup.substr(dotPos + 1);
                        for (const auto& importDecl : ctx.program->imports) {
                            if (!importDecl.alias || *importDecl.alias != alias) continue;
                            auto actionIt = tc_.actions_.find(method);
                            if (actionIt != tc_.actions_.end()) {
                                if (actionIt->second->params.size() != node.args.size()) {
                                    DiagBuilder(result_, Severity::Error,
                                        "Param count mismatch calling action " + node.name + ": got " + std::to_string(node.args.size()) +
                                        ", expected " + std::to_string(actionIt->second->params.size()),
                                        "TC020", ctx.actionName()).emit();
                                } else {
                                    check_action_arg_types(tc_, result_, *actionIt->second, node.args, *this, ctx, node.name);
                                }
                                inferred = { actionIt->second->returnType.empty() ? "void" : tc_.resolve_type(actionIt->second->returnType, ctx.program, nullptr).name };
                                resolvedModuleAction = true;
                            }
                            break;
                        }
                    }
                    if (!resolvedModuleAction) {
                        // Dotted/qualified names resolve at runtime as handle
                        // method calls (object.method). All handle dispatch
                        // methods return string values by convention.
                        if (node.name.find('.') == std::string::npos &&
                            node.name.find("::") == std::string::npos) {
                            // Check if name is a variable in scope (possibly a func:N handle)
                            if (ctx.scopes && ctx.scopes->lookup(node.name)) {
                                // Variable exists in scope — treat as function handle call
                                inferred = {"string"};
                            } else {
                                emit_unknown_call(result_, node.name, ctx.actionName());
                                inferred = {"unknown"};
                            }
                        } else {
                            // Handle/struct method call - returns string
                            inferred = {"string"};
                        }
                    }
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
            } // !enumConstructed inner
            } // !enumConstructed outer
        } else if constexpr (std::is_same_v<T, PostfixExpr>) {
            auto ot = check(node.operand, ctx);
            if (ot.name != "unknown" && !TypeChecker::is_int(ot)) {
                DiagBuilder(result_, Severity::Error, "Increment/decrement requires an int operand, got " + ot.name, "TC065", ctx.actionName()).emit();
            }
            inferred = ot;
        } else if constexpr (std::is_same_v<T, PrefixExpr>) {
            auto ot = check(node.operand, ctx);
            if (ot.name != "unknown" && !TypeChecker::is_int(ot)) {
                DiagBuilder(result_, Severity::Error, "Increment/decrement requires an int operand, got " + ot.name, "TC065", ctx.actionName()).emit();
            }
            inferred = ot;
        } else if constexpr (std::is_same_v<T, CompoundAssignExpr>) {
            auto lt = check(node.left, ctx);
            auto rt = check(node.right, ctx);
            if (node.op != BinOp::Add || (!TypeChecker::is_string(lt) && !TypeChecker::is_string(rt))) {
                if (lt.name != "unknown" && rt.name != "unknown" && !TypeChecker::is_int(lt)) {
                    DiagBuilder(result_, Severity::Error, "Compound assignment requires numeric operands, got " + lt.name, "TC066", ctx.actionName()).emit();
                }
            }
            inferred = lt;
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
            if (it != tc_.actions_.end() && it->second->params.size() == stmt.args.size()) {
                check_action_arg_types(tc_, result_, *it->second, stmt.args, expr_, ctx, stmt.name);
            }
        } else if constexpr (std::is_same_v<T, MethodCallStmt>) {
            for (auto& a : stmt.args) expr_.check(a, ctx);

            // Opaque type param: method must come from a constrained trait.
            if (ctx.scopes) {
                if (auto* owner = ctx.scopes->lookup(stmt.objectName)) {
                    if (ctx.opaqueTypeParams.count(owner->type.name)) {
                        bool found = false;
                        auto cit = ctx.typeParamConstraints.find(owner->type.name);
                        if (cit != ctx.typeParamConstraints.end() && ctx.program) {
                            for (const auto& constraint : cit->second) {
                                for (const auto& tr : ctx.program->traits) {
                                    if (tr.name != constraint.name) continue;
                                    for (const auto& m : tr.methods) {
                                        if (m.name == stmt.method) { found = true; break; }
                                    }
                                }
                                if (found) break;
                            }
                        }
                        if (!found) {
                            DiagBuilder(result_, Severity::Error,
                                "Method '" + stmt.method + "' not provided by constraints on type parameter '" + owner->type.name + "'",
                                "TC147", ctx.actionName())
                                .hint("Add a trait constraint that declares this method")
                                .emit();
                        }
                        return;
                    }
                }
            }

            const std::string callName = stmt.objectName + "." + stmt.method;
            auto mark_route_action_refs = [&]() {
                static const std::unordered_set<std::string> kRouteMethods = {
                    "get", "post", "put", "patch", "del", "ws", "sse", "use",
                };
                if (!kRouteMethods.count(stmt.method)) return;
                for (const auto& arg : stmt.args) {
                    if (!arg) continue;
                    if (auto* lit = std::get_if<ExprString>(&arg->node)) {
                        auto it = tc_.actionUsage_.find(lit->v);
                        if (it != tc_.actionUsage_.end()) it->second.referenced = true;
                    }
                }
            };

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
                mark_route_action_refs();
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
                    } else {
                        check_action_arg_types(tc_, result_, *actionIt->second, stmt.args, expr_, ctx, callName);
                    }
                    mark_route_action_refs();
                    return;
                }
            }

            // Allow method syntax in manual mode:
            // defer exact receiver/member validation to runtime for now.
            if (ctx.scopes) {
                if (auto* recv = ctx.scopes->lookup(stmt.objectName)) {
                    recv->used = true;
                } else {
                    DiagBuilder(result_, Severity::Error,
                        "Use before declaration: " + stmt.objectName,
                        "TC010", ctx.actionName())
                        .hint("Declare `" + stmt.objectName + " = ...;` before use, or check import alias spelling")
                        .emit();
                }
            }
            mark_route_action_refs();
        } else if constexpr (std::is_same_v<T, LetStmt>) {
            if (scopes.lookup(stmt.name)) DiagBuilder(result_, Severity::Error, "Variable redeclaration: " + stmt.name, "TC030", ctx.actionName()).emit();
            VarInfo vi; vi.type = expr_.check(stmt.value, ctx); vi.isConst = stmt.isConst; vi.assigned=true;
            if (!stmt.declaredType.empty()) {
                bool known = true;
                TypeInfo expected = tc_.resolve_type(stmt.declaredType, ctx.program, &known);
                if (!known) {
                    DiagBuilder(result_, Severity::Error, "Unknown declared type: " + stmt.declaredType, "TC041", ctx.actionName())
                        .hint("Known primitives: int, string, bool, double, void, pointer, array<T>, map<K,V>, struct:Name, enum:Name")
                        .emit();
                } else if (expected.name != "auto") {
                    const bool defaultNullInit =
                        stmt.value && std::holds_alternative<ExprNull>(stmt.value->node) && vi.type.name == "pointer";
                    if (!tc_.is_assignable(vi.type, expected) && !defaultNullInit) {
                        DiagBuilder(result_, Severity::Error,
                            "Type mismatch in declaration: " + stmt.name,
                            "TC042", ctx.actionName()).emit();
                    }
                    if (expected.name != "unknown") {
                        vi.type = expected;
                    }
                }
            }
            scopes.declare(stmt.name, vi);
        } else if constexpr (std::is_same_v<T, ReturnStmt>) {
            if (retType == "void") {
                if (stmt.value) expr_.check(*stmt.value, ctx);
            } else {
                auto t = stmt.value ? expr_.check(*stmt.value, ctx) : TypeInfo{"void"};
                bool known = true;
                TypeInfo expected = tc_.resolve_type(retType, ctx.program, &known);
                if (!known) expected = TypeInfo{retType};
                if (!tc_.is_assignable(t, expected) && !tc_.is_convertible(t, expected)) {
                    DiagBuilder(result_, Severity::Error,
                        "Return type mismatch: expected " + expected.name + ", got " + t.name,
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
                    if (v->type.name == "unknown") v->type = valT;
                    else if (!tc_.is_assignable(valT, v->type)) {
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
        } else if constexpr (std::is_same_v<T, MatchStmt>) {
            TypeInfo selT = expr_.check(stmt.selector, ctx);
            std::string enumType = selT.name;
            if (enumType.rfind("enum:", 0) == 0) enumType = enumType.substr(5);
            const EnumDecl* ed = nullptr;
            TypeRef applied;
            if (ctx.program) {
                try { applied = parse_type_ref_string(enumType); } catch (...) { applied = make_type_ref(enumType); }
                for (const auto& en : ctx.program->enums) {
                    if (en.name != applied.name) continue;
                    ed = &en;
                    break;
                }
            }
            if (!ed && selT.name != "unknown") {
                DiagBuilder(result_, Severity::Error,
                    "Match requires an enum value, got `" + selT.name + "`",
                    "TC148", ctx.actionName())
                    .hint("Use `match` on Option/Result or other enum types")
                    .emit();
            }

            std::function<void(const PatternPtr&, const std::string&)> check_pattern;
            check_pattern = [&](const PatternPtr& pat, const std::string& expectedType) {
                if (!pat) return;
                std::visit([&](const auto& node) {
                    using N = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<N, PatWildcard>) {
                        return;
                    } else if constexpr (std::is_same_v<N, PatBinding>) {
                        VarInfo vi;
                        vi.type = tc_.resolve_type(expectedType, ctx.program, nullptr);
                        vi.assigned = true;
                        if (!scopes.declare(node.name, vi)) {
                            DiagBuilder(result_, Severity::Error,
                                "Variable redeclared in match binding: " + node.name,
                                "TC030", ctx.actionName()).emit();
                        }
                    } else if constexpr (std::is_same_v<N, PatCtor>) {
                        std::string typeName = expectedType;
                        if (typeName.rfind("enum:", 0) == 0) typeName = typeName.substr(5);
                        const EnumDecl* nestEd = nullptr;
                        std::unordered_map<std::string, std::string> nestSubst;
                        TypeRef nestApplied;
                        if (ctx.program) {
                            try { nestApplied = parse_type_ref_string(typeName); } catch (...) { nestApplied = make_type_ref(typeName); }
                            for (const auto& en : ctx.program->enums) {
                                if (en.name != nestApplied.name) continue;
                                nestEd = &en;
                                if (!en.typeParams.empty() && nestApplied.args.size() == en.typeParams.size()) {
                                    for (size_t i = 0; i < en.typeParams.size(); ++i) {
                                        nestSubst[en.typeParams[i].name] = type_ref_canonical(nestApplied.args[i]);
                                    }
                                }
                                break;
                            }
                        }
                        if (!nestEd) {
                            DiagBuilder(result_, Severity::Error,
                                "Pattern variant `" + node.name + "` used where enum type `" + expectedType + "` expected",
                                "TC149", ctx.actionName()).emit();
                            return;
                        }
                        const EnumVariant* found = nullptr;
                        for (const auto& variant : nestEd->variants) {
                            if (variant.name == node.name) { found = &variant; break; }
                        }
                        if (!found) {
                            DiagBuilder(result_, Severity::Error,
                                "Unknown variant `" + node.name + "` for enum `" + nestEd->name + "`",
                                "TC150", ctx.actionName()).emit();
                            return;
                        }
                        if (found->payloads.size() != node.args.size()) {
                            DiagBuilder(result_, Severity::Error,
                                "Match binding count mismatch for variant '" + node.name + "'",
                                "TC145", ctx.actionName()).emit();
                            return;
                        }
                        for (size_t i = 0; i < node.args.size(); ++i) {
                            std::string pt = type_ref_canonical(found->payloads[i]);
                            if (!nestSubst.empty()) pt = substitute_type_string(pt, nestSubst);
                            check_pattern(node.args[i], pt);
                        }
                    }
                }, pat->node);
            };

            for (auto& c : stmt.cases) {
                auto caseScope = scopes.push();
                if (ed) {
                    check_pattern(c.pattern, enumType);
                }
                if (c.body) check_block(*c.body, ctx, scopes, retType);
            }
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ParallelStmt>>) {
            if (stmt) { auto guardP = scopes.push(); check_block(stmt->body, ctx, scopes, retType); }
        } else if constexpr (std::is_same_v<T, UnsafeStmt>) {
            auto guardU = scopes.push();
            check_block(*stmt.body, ctx, scopes, retType);
        } else if constexpr (std::is_same_v<T, PointerSetStmt>) {
            expr_.check(stmt.pointer, ctx);
            expr_.check(stmt.value, ctx);
        } else if constexpr (std::is_same_v<T, ExprStmt>) {
            expr_.check(stmt.expr, ctx);
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
    structs_.clear(); enums_.clear(); aliases_.clear(); traits_.clear();
    specializedTypeCache_.clear();
    init_builtins();
    for (const auto& a : program.actions) { actions_[a.name] = &a; actionUsage_[a.name]; }
    for (const auto& e : program.externs) { externActions_.insert(e.name); }
    for (const auto& s : program.structs) structs_[s.name] = &s;
    for (const auto& e : program.enums) enums_[e.name] = &e;
    for (const auto& a : program.typeAliases) aliases_[a.name] = &a;
    for (const auto& t : program.traits) traits_[t.name] = &t;
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
        push_opaque_params(ctx, a.typeParams);
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
            bool known = true;
            TypeInfo paramType;
            bool isOpaqueParam = false;
            for (const auto& tp : a.typeParams) {
                if (tp.name == p.type) { isOpaqueParam = true; break; }
            }
            if (isOpaqueParam) {
                paramType = TypeInfo{p.type};
                known = true;
            } else {
                paramType = p.type.empty()
                    ? TypeInfo{"unknown"}
                    : resolve_type(p.type, &program, &known);
            }
            if (!p.type.empty() && !known) {
                DiagBuilder(out, Severity::Error,
                    "Unknown parameter type '" + p.type + "' on '" + p.name + "' in action '" + a.name + "'",
                    "TC041", a.name)
                    .hint("Known primitives: int, string, bool, double, void, pointer, array<T>, map<K,V>, struct:Name, enum:Name")
                    .emit();
            }
            VarInfo vi; vi.type = paramType; vi.assigned = true; scopes.declare(p.name, vi);
        }
        // Register globals with explicit type annotations so action bodies can reference them
        for (auto& g : program.globals) {
            if (!g.typeName.empty()) {
                bool known = true;
                VarInfo gi; gi.type = resolve_type(g.typeName, &program, &known); gi.assigned = true; gi.used = true;
                scopes.declare(g.name, gi);
            }
        }
        // Implicit runtime injectables (handle result + HTTP request/response)
        {
            VarInfo ri; ri.type = {"string"}; ri.assigned = true; ri.used = true;
            scopes.declare("_", ri);
            VarInfo resInfo; resInfo.type = {"string"}; resInfo.assigned = true; resInfo.used = true;
            scopes.declare("res", resInfo);
            VarInfo reqInfo; reqInfo.type = {"string"}; reqInfo.assigned = true; reqInfo.used = true;
            scopes.declare("req", reqInfo);
        }
        auto rf = stmt.check_block(a.body, ctx, scopes, [&]() -> std::string {
            if (a.returnType.empty()) return "void";
            for (const auto& tp : a.typeParams) if (tp.name == a.returnType) return a.returnType;
            return a.returnType;
        }());
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
        add("http_get_auth", 2, 2, "string");
        add("http_post", 3, 3, "string");
        add("http_post_auth", 4, 4, "string");
        add("http_put_auth", 4, 4, "string");
        add("http_patch_auth", 4, 4, "string");
        add("http_delete_auth", 2, 2, "string");
        add("http_status", 1, 1, "string");
        add("http_download", 2, 2, "bool");
        add("hls_download_best", 2, 2, "bool");
        add("url_encode", 1, 1, "string");
        add("http_create_server", 1, 3, "string");
        add("http_create_server_tls", 3, 3, "string");
        // Also register the alias-qualified forms so expressions like
        //   string s = net.create_server("8080")
        // resolve without needing a separate FunctionCallExpr -> alias lookup
        add("net.create_server", 1, 3, "string");
        add("net.create_server_tls", 3, 3, "string");
        add("net.get", 1, 1, "string");
        add("net.get_auth", 2, 2, "string");
        add("net.post", 3, 3, "string");
        add("net.post_auth", 4, 4, "string");
        add("net.put", 3, 3, "string");
        add("net.put_auth", 4, 4, "string");
        add("net.patch", 4, 4, "string");
        add("net.patch_auth", 4, 4, "string");
        add("net.delete", 1, 1, "string");
        add("net.delete_auth", 2, 2, "string");
        add("net.head", 1, 1, "string");
        add("net.status", 1, 1, "string");
        add("net.download", 2, 2, "bool");
        add("net.encode", 1, 1, "string");
        add("net.json_encode", 1, 1, "string");
        add("net.json_decode", 1, 1, "string");
        add("net.get_resp", 1, 1, "string");
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
    }
    if (program_imports_module(&program, "builtin/tcp") || program_imports_module(&program, "builtin/rawtcp")) {
        add("tcp.connect", 2, 2, "string");
    }
    if (program_imports_module(&program, "builtin/websocket") || program_imports_module(&program, "builtin/ws")) {
        add("ws_connect", 1, 1, "string");
        add("ws_send", 2, 2, "bool");
        add("ws_recv", 1, 1, "string");
        add("ws_recv_timeout", 2, 2, "string");
        add("ws_close", 1, 1, "void");
        add("ws_state", 1, 1, "string");
        // Alias-qualified forms
        add("ws.connect", 1, 1, "string");
        add("ws.recv_timeout", 2, 2, "string");
        add("ws.broadcast", 1, 1, "bool");
        add("ws.send_binary", 1, 1, "bool");
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
        add("thread_count", 0, 0, "string");
        add("thread_yield", 0, 0, "void");
        add("thread_gc", 0, 0, "void");
        add("thread_gc_all", 0, 0, "void");
        add("thread_purge", 0, 0, "void");
        add("thread_remove", 1, 2, "string");
        add("thread_state", 1, 1, "string");
        add("thread_sleep", 1, 1, "void");
        add("thread_result", 1, 1, "any");
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
    if (program_imports_module(&program, "builtin/system") || program_imports_module(&program, "builtin/process") || program_imports_module(&program, "builtin/proc")) {
        add("system.cmd", 1, 2, "string");
        add("system.execute", 1, 3, "int");
        add("system.output", 0, 0, "string");
        add("system.last_exit", 0, 0, "int");
        add("system.ip.flush", 0, 0, "string");
    }
    if (program_imports_module(&program, "builtin/performance") || program_imports_module(&program, "builtin/perf")) {
        add("perf.profile.begin", 1, 1, "void");
        add("perf.profile.end", 1, 1, "void");
        add("perf.profile.duration", 1, 1, "string");
        add("perf.profile.calls", 1, 1, "string");
        add("perf.profile.report", 0, 0, "string");
        add("perf.mem.usage", 0, 0, "string");
        add("perf.mem.peak", 0, 0, "string");
        add("perf.gc.collect", 0, 0, "void");
        add("perf.gc.threshold", 1, 1, "void");
        add("perf.gc.pause", 0, 0, "void");
        add("perf.gc.resume", 0, 0, "void");
    }
    if (program_imports_module(&program, "builtin/path") || program_imports_module(&program, "builtin/erepath")) {
        add("path_join", 1, -1, "string"); add("path_dirname", 1, 1, "string");
        add("path_basename", 1, 1, "string"); add("path_ext", 1, 1, "string");
        add("file_exists", 1, 1, "bool");
    }
    if (program_imports_module(&program, "builtin/fs") || program_imports_module(&program, "builtin/erefs")) {
        add("read_text", 1, 1, "string"); add("write_text", 2, 2, "void"); add("append_text", 2, 2, "void");
        add("file_exists", 1, 1, "bool"); add("is_dir", 1, 1, "bool"); add("is_file", 1, 1, "bool");
        add("mkdirs", 1, 1, "void"); add("copy_file", 2, 2, "bool");
        add("move_file", 2, 2, "bool"); add("delete_file", 1, 1, "bool");
        add("list_files", 1, 1, "array<string>"); add("list_dirs", 1, 1, "array<string>");
        add("list_regular_files", 1, 1, "array<string>");
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
    // type constructors
    add("int",1,1,"int");
    add("float",1,1,"double");
    add("string",1,1,"string");
    add("bool",1,1,"bool");
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
    add("from_json",1,1,"string");
    add("json.encode",1,1,"string");
    add("json.decode",1,1,"string");
    add("url_encode",1,1,"string");
    add("json_encode",1,1,"string");
    add("json_decode",1,1,"string");
    add("http_get_resp",1,1,"string");
    add("tcp_connect",2,2,"string");
    add("tcp.connect",2,2,"string");
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
    add("map",2,2,"string");   // map(list, func) -> new list handle
    add("filter",2,2,"string"); // filter(list, func) -> new list handle
    add("reduce",3,3,"string"); // reduce(list, func, initial) -> accumulated value
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
    // Handle method dispatch types for req:/res:/ws:/http:/sse: handles
    // req: handle methods
    add("req.body",0,0,"string");
    add("req.query",1,1,"string");
    add("req.header",1,1,"string");
    add("req.method",0,0,"string");
    add("req.path",0,0,"string");
    add("req.cookie",1,1,"string");
    add("req.file",1,1,"string");
    add("req.save_upload",2,2,"string");
    // res: handle methods
    add("res.html",1,1,"void");
    add("res.json",1,1,"void");
    add("res.text",1,1,"void");
    add("res.write",1,1,"void");
    add("res.status",1,1,"void");
    add("res.header",2,2,"void");
    add("res.cookie",3,6,"void");
    add("res.end",0,0,"void");
    // ws: handle methods
    add("ws.send",1,1,"bool");
    add("ws.send_binary",1,1,"bool");
    add("ws.recv",0,0,"string");
    add("ws.recv_timeout",1,1,"string");
    add("ws.close",0,2,"void");
    add("ws.broadcast",1,1,"bool");
    add("ws.state",0,0,"string");
    // resp: handle methods
    add("resp.status",0,0,"int");
    add("resp.body",0,0,"string");
    add("resp.header",1,1,"string");
    add("resp.json",0,0,"string");
    // tcp: handle methods
    add("tcp.send",1,1,"int");
    add("tcp.recv",0,0,"string");
    add("tcp.recv_timeout",0,1,"string");
    add("tcp.close",0,0,"bool");
    add("tcp.state",0,0,"string");
    // sse: handle methods
    add("sse.emit",2,2,"void");
    add("sse.close",0,0,"void");

}

} // namespace erelang
