// SPDX-License-Identifier: Apache-2.0
// Split from runtime.cpp

#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"
#include "erelang/runtime_builtins.hpp"
#include "erelang/runtime_imports.hpp"
#include "erelang/parser.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

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
    for (const auto& e : program.entities) if (e.name == name) return &e;
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
        env.vars[is.name] = line;
        if (globalNames_.count(is.name)) globalVars_[is.name] = line;
        return;
    }
    if (std::holds_alternative<LetStmt>(s)) {
        const auto& st = std::get<LetStmt>(s);
        if (!st.declaredType.empty()) {
            if (const StructDecl* sd = find_struct_decl(program, st.declaredType)) {
                env.vars[st.name] = std::string("struct:") + sd->name;
                for (const auto& f : sd->fields) {
                    env.vars[st.name + "." + f.name] = std::string{};
                }

                const std::string initValue = eval_string(*st.value, env);
                if (initValue.rfind("dict:", 0) == 0) {
                    const int id = to_int(initValue.substr(5));
                    auto dit = g_dicts.find(id);
                    if (dit != g_dicts.end()) {
                        for (const auto& f : sd->fields) {
                            auto fit = dit->second.find(f.name);
                            if (fit != dit->second.end()) {
                                env.vars[st.name + "." + f.name] = fit->second;
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
                                env.vars[st.name + "." + f.name] = srcField->second;
                            }
                        }
                    }
                }

                if (globalNames_.count(st.name)) globalVars_[st.name] = env.vars[st.name];
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
                    env.vars[st.name] = st.name;
                    if (globalNames_.count(st.name)) globalVars_[st.name] = env.vars[st.name];
                    return;
                }
            }
        }
        // Support object construction on right-hand side: let x = new Type(args)
        if (std::holds_alternative<FunctionCallExpr>(st.value->node)) {
            const auto& fc = std::get<FunctionCallExpr>(st.value->node);
            if (const StructDecl* sd = find_struct_decl(program, fc.name)) {
                env.vars[st.name] = std::string("struct:") + sd->name;
                for (const auto& f : sd->fields) {
                    env.vars[st.name + "." + f.name] = "0";
                }
                if (globalNames_.count(st.name)) globalVars_[st.name] = env.vars[st.name];
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
            // also store a string representation
            env.vars[st.name] = st.name;
        } else {
            int64_t iv = 0;
            const bool wantInt = st.declaredType.empty() || normalize_runtime_type_name(st.declaredType) == "int";
            if (wantInt && try_eval_int(*st.value, env, iv)) {
                if (!st.declaredType.empty() && normalize_runtime_type_name(st.declaredType) != "int") {
                    // fall through to typed string path
                } else {
                    set_var_int(env, st.name, iv);
                    return;
                }
            }
            std::string v = eval_string(*st.value, env);
            if (!st.declaredType.empty()) {
                auto infer_runtime_value_type = [&](const std::string& value) {
                    if (value.rfind("list:", 0) == 0) return std::string("array<any>");
                    if (value.rfind("dict:", 0) == 0) return std::string("map<string,any>");
                    if (value.rfind("struct:", 0) == 0) return normalize_runtime_type_name(value);
                    if (value == "true" || value == "false") return std::string("bool");
                    if (is_int_string(value)) return std::string("int");
                    if (is_float_string(value)) return std::string("double");
                    if (auto it = env.objects.find(value); it != env.objects.end() && it->second) {
                        return normalize_runtime_type_name(it->second->typeName);
                    }
                    return std::string("string");
                };
                const std::string actualType = infer_runtime_value_type(v);
                const std::string declaredNorm = normalize_runtime_type_name(st.declaredType);
                // string declarations accept anything printable (snowflake IDs look like ints)
                if (declaredNorm == "string" || declaredNorm == "str") {
                    // keep v as-is
                } else if (!runtime_declared_type_matches(st.declaredType, actualType)) {
                    throw std::runtime_error(
                        "Type mismatch in declaration '" + st.name + "': declared " + st.declaredType + " but got " + actualType
                    );
                }
            }
            if (is_int_string(v) && (st.declaredType.empty() || normalize_runtime_type_name(st.declaredType) == "int")) {
                set_var_int(env, st.name, to_int(v));
            } else {
                set_var_str(env, st.name, v);
            }
            if (std::holds_alternative<FunctionCallExpr>(st.value->node)) {
                const auto& fc = std::get<FunctionCallExpr>(st.value->node);
                if (fc.name == "dynamic_cast") {
                    auto source = env.objects.find(v);
                    if (source != env.objects.end()) {
                        env.objects[st.name] = source->second;
                        set_var_str(env, st.name, st.name);
                    }
                }
            }
        }
        return;
    }
    if (std::holds_alternative<ReturnStmt>(s)) {
        const auto& rs = std::get<ReturnStmt>(s);
        // Evaluate return expression if provided; propagate evaluation errors
        // so a failing return expression is not silently converted to "".
        std::string rv;
        if (rs.value && *rs.value) rv = eval_string(**rs.value, env);
        ctx.returned = true;
        ctx.returnValue = rv;
        return;
    }
    if (std::holds_alternative<FireStmt>(s)) {
        const auto& st = std::get<FireStmt>(s);
        if (const Hook* h = find_hook(program, st.name)) exec_block(h->body, program, ctx, env);
        return;
    }
    if (std::holds_alternative<IfStmt>(s)) {
        const auto& st = std::get<IfStmt>(s);
        std::string c = eval_string(*st.cond, env);
        bool truthy = is_truthy(c);
        if (truthy) exec_block(*st.thenBlk, program, ctx, env);
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

    while (is_truthy(eval_string(*st.cond, env))) {

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
        // init
        if (st.init) exec_block(*st.init, program, ctx, env);

        // Fast path: counted int for-loop with pure int body → native C++ loop.
        // Pattern: for (i = ...; i < N; i++) { x = <int expr>; ... }
        // Eliminates per-iteration AST / hash-map / exec_stmt dispatch (hypothesis F/G/H).
        auto ident_name = [](const ExprPtr& e) -> std::optional<std::string> {
            if (e && std::holds_alternative<ExprIdent>(e->node)) return std::get<ExprIdent>(e->node).name;
            return std::nullopt;
        };
        auto is_inc_step_on = [&](const std::string& var) -> bool {
            if (!st.step || st.step->stmts.size() != 1) return false;
            const auto& stepStmt = st.step->stmts[0];
            if (!std::holds_alternative<ExprStmt>(stepStmt)) return false;
            const auto& es = std::get<ExprStmt>(stepStmt);
            if (!es.expr) return false;
            if (std::holds_alternative<PostfixExpr>(es.expr->node)) {
                const auto& pe = std::get<PostfixExpr>(es.expr->node);
                auto n = ident_name(pe.operand);
                return pe.isInc && n && *n == var;
            }
            if (std::holds_alternative<PrefixExpr>(es.expr->node)) {
                const auto& pe = std::get<PrefixExpr>(es.expr->node);
                auto n = ident_name(pe.operand);
                return pe.isInc && n && *n == var;
            }
            return false;
        };
        auto body_is_int_assigns = [&]() -> bool {
            if (!st.body) return false;
            for (const auto& bs : st.body->stmts) {
                if (!std::holds_alternative<SetStmt>(bs)) return false;
                const auto& set = std::get<SetStmt>(bs);
                if (set.isMember) return false;
                int64_t ignore = 0;
                if (!try_eval_int(*set.value, env, ignore)) return false;
            }
            return !st.body->stmts.empty();
        };

        if (st.cond && std::holds_alternative<BinaryExpr>((*st.cond)->node)) {
            const auto& cond = std::get<BinaryExpr>((*st.cond)->node);
            auto leftName = ident_name(cond.left);
            if (leftName && (cond.op == BinOp::LT || cond.op == BinOp::LE) &&
                is_inc_step_on(*leftName) && body_is_int_assigns()) {
                int64_t* iPtr = nullptr;
                if (auto it = env.intVars.find(*leftName); it != env.intVars.end()) {
                    iPtr = &it->second;
                }
                int64_t limit = 0;
                const bool limitOk = try_eval_int(*cond.right, env, limit);
                if (iPtr && limitOk) {
                    // Collect assignment targets + RHS expressions (stable for loop duration).
                    struct Assign { std::string name; const Expr* rhs; int64_t* slot; };
                    std::vector<Assign> assigns;
                    assigns.reserve(st.body->stmts.size());
                    bool slotsOk = true;
                    for (const auto& bs : st.body->stmts) {
                        const auto& set = std::get<SetStmt>(bs);
                        auto vit = env.intVars.find(set.varOrField);
                        if (vit == env.intVars.end()) {
                            // Ensure target exists as int slot (e.g. first write).
                            int64_t cur = 0;
                            (void)try_get_int_var(env, set.varOrField, cur);
                            set_var_int(env, set.varOrField, cur);
                            vit = env.intVars.find(set.varOrField);
                        }
                        if (vit == env.intVars.end()) { slotsOk = false; break; }
                        assigns.push_back(Assign{set.varOrField, set.value.get(), &vit->second});
                    }
                    // Re-resolve iPtr after possible rehash from set_var_int.
                    iPtr = &env.intVars.find(*leftName)->second;
                    for (auto& a : assigns) {
                        a.slot = &env.intVars.find(a.name)->second;
                    }
                    if (slotsOk) {
                        // Specialize sum = sum + i (or sum = i + sum) to pointer arithmetic.
                        bool directAdd = false;
                        int64_t* accPtr = nullptr;
                        if (assigns.size() == 1 && assigns[0].rhs &&
                            std::holds_alternative<BinaryExpr>(assigns[0].rhs->node)) {
                            const auto& be = std::get<BinaryExpr>(assigns[0].rhs->node);
                            if (be.op == BinOp::Add) {
                                auto ln = ident_name(be.left);
                                auto rn = ident_name(be.right);
                                if (ln && rn &&
                                    ((*ln == assigns[0].name && *rn == *leftName) ||
                                     (*rn == assigns[0].name && *ln == *leftName))) {
                                    accPtr = assigns[0].slot;
                                    directAdd = true;
                                }
                            }
                        }
                        if (directAdd && accPtr) {
                            if (cond.op == BinOp::LT) {
                                for (; *iPtr < limit; ++(*iPtr)) {
                                    *accPtr = *accPtr + *iPtr;
                                }
                            } else {
                                for (; *iPtr <= limit; ++(*iPtr)) {
                                    *accPtr = *accPtr + *iPtr;
                                }
                            }
                            return;
                        }
                        auto run_body = [&]() {
                            for (auto& a : assigns) {
                                int64_t v = 0;
                                if (!try_eval_int(*a.rhs, env, v)) {
                                    set_var_str(env, a.name, eval_string(*a.rhs, env));
                                    return false;
                                }
                                *a.slot = v;
                            }
                            return true;
                        };
                        if (cond.op == BinOp::LT) {
                            for (; *iPtr < limit; ++(*iPtr)) {
                                if (!run_body()) break;
                            }
                        } else {
                            for (; *iPtr <= limit; ++(*iPtr)) {
                                if (!run_body()) break;
                            }
                        }
                        return;
                    }
                }
            }
        }

        while (true) {
            if (st.cond) {
                int64_t cv = 0;
                if (try_eval_int(**st.cond, env, cv)) {
                    if (cv == 0) break;
                } else {
                    std::string c = eval_string(**st.cond, env);
                    if (!is_truthy(c)) break;
                }
            }
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
            std::string c = eval_string(*st.cond, env);
            if (!is_truthy(c)) {
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
                    env.vars[st.var] = item;
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
                        env.vars[st.var] = kv.first;
                        env.vars[*st.valueVar] = kv.second;
                    } else {
                        env.vars[st.var] = kv.first;
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
            env.vars[st.catchVar] = ex.what();
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
        const std::string target = eval_string(*st.pointer, env);
        const std::string newValue = eval_string(*st.value, env);
        if (target.rfind("ref:", 0) == 0) {
            const std::string varName = target.substr(4);
            if (globalNames_.count(varName)) {
                globalVars_[varName] = newValue;
            }
            env.vars[varName] = newValue;
            return;
        }
        if (auto idOpt = parse_pointer_handle(target); idOpt.has_value()) {
            const int id = *idOpt;
            auto it = g_ptrs.find(id);
            if (it != g_ptrs.end()) {
                if (it->second.rfind("ref:", 0) == 0) {
                    const std::string varName = it->second.substr(4);
                    if (globalNames_.count(varName)) {
                        globalVars_[varName] = newValue;
                    }
                    env.vars[varName] = newValue;
                    return;
                }
                it->second = newValue;
            }
            return;
        }
        throw std::runtime_error("Pointer assignment target is not a pointer");
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
    if (std::holds_alternative<ExprStmt>(s)) {
        const auto& st = std::get<ExprStmt>(s);
        if (!st.expr) return;
        auto read_var = [&](const std::string& name) -> std::string {
            int64_t iv = 0;
            if (try_get_int_var(env, name, iv)) return std::to_string(iv);
            auto it = env.vars.find(name);
            if (it != env.vars.end()) return it->second;
            auto git = globalVars_.find(name);
            if (git != globalVars_.end()) return git->second;
            return {};
        };
        auto write_var = [&](const std::string& name, const std::string& val) {
            if (is_int_string(val)) set_var_int(env, name, to_int(val));
            else set_var_str(env, name, val);
        };
        auto lvalue_name = [](const ExprPtr& e) -> std::optional<std::string> {
            if (e && std::holds_alternative<ExprIdent>(e->node)) return std::get<ExprIdent>(e->node).name;
            return std::nullopt;
        };
        if (std::holds_alternative<PostfixExpr>(st.expr->node)) {
            const auto& pe = std::get<PostfixExpr>(st.expr->node);
            if (auto name = lvalue_name(pe.operand)) {
                int64_t cur = 0;
                if (!try_get_int_var(env, *name, cur)) cur = to_int(read_var(*name));
                set_var_int(env, *name, cur + (pe.isInc ? 1 : -1));
            } else {
                (void)eval_string(*st.expr, env);
            }
            return;
        }
        if (std::holds_alternative<PrefixExpr>(st.expr->node)) {
            const auto& pe = std::get<PrefixExpr>(st.expr->node);
            if (auto name = lvalue_name(pe.operand)) {
                int64_t cur = 0;
                if (!try_get_int_var(env, *name, cur)) cur = to_int(read_var(*name));
                set_var_int(env, *name, cur + (pe.isInc ? 1 : -1));
            } else {
                (void)eval_string(*st.expr, env);
            }
            return;
        }
        if (std::holds_alternative<CompoundAssignExpr>(st.expr->node)) {
            const auto& ca = std::get<CompoundAssignExpr>(st.expr->node);
            if (auto name = lvalue_name(ca.left)) {
                auto binExpr = std::make_shared<Expr>(Expr{ BinaryExpr{ ca.op, std::make_shared<Expr>(Expr{ExprIdent{*name}}), ca.right } });
                int64_t iv = 0;
                if (try_eval_int(*binExpr, env, iv)) {
                    set_var_int(env, *name, iv);
                } else {
                    write_var(*name, eval_string(*binExpr, env));
                }
            } else {
                (void)eval_string(*st.expr, env);
            }
            return;
        }
        (void)eval_string(*st.expr, env);
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
                    env.vars[st.objectName + "." + st.varOrField] = val;
                    return;
                }
                throw std::runtime_error("Unknown object: " + st.objectName);
            }
            std::string val = eval_string(*st.value, env);
            it->second->fields[st.varOrField] = val;
            if (st.objectName == "self") {
                env.vars[st.varOrField] = val;
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
                env.vars[st.varOrField] = st.varOrField;
                if (globalNames_.count(st.varOrField)) globalVars_[st.varOrField] = env.vars[st.varOrField];
                return;
            }
            int64_t iv = 0;
            if (try_eval_int(*st.value, env, iv)) {
                set_var_int(env, st.varOrField, iv);
            } else {
                set_var_str(env, st.varOrField, eval_string(*st.value, env));
            }
        }
        return;
    }
    if (std::holds_alternative<MethodCallStmt>(s)) {
        const auto& mc = std::get<MethodCallStmt>(s);
        std::string methodName = mc.method;
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
                                         normalizedPath == "builtin/path" || normalizedPath == "builtin/erepath");
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
                ExecContext calleeCtx;
                exec_block(aliased->body, program, calleeCtx, calleeEnv);
                for (auto& th : calleeCtx.threads) if (th.joinable()) th.join();
                return;
            }
            break;
        }

        // Support dynamic list/dict method calls using handles stored in variables
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
            if (handle.rfind("file:", 0) == 0 && methodName == "read") {
                int id = to_int(handle.substr(5));
                auto fit = g_fileStreams.find(id);
                if (fit != g_fileStreams.end() && fit->second && fit->second->is_open()) {
                    if (mc.args.empty()) {
                        std::ostringstream ss;
                        ss << fit->second->rdbuf();
                        env.vars["_"] = ss.str();
                    } else {
                        int count = to_int(eval_string(*mc.args[0], env));
                        std::string buf(static_cast<size_t>(count), '\0');
                        fit->second->read(&buf[0], count);
                        buf.resize(static_cast<size_t>(fit->second->gcount()));
                        env.vars["_"] = buf;
                    }
                } else {
                    env.vars["_"] = std::string();
                }
                return;
            }
            if (handle.rfind("file:", 0) == 0 && methodName == "write") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    auto fit = g_fileStreams.find(id);
                    if (fit != g_fileStreams.end() && fit->second && fit->second->is_open()) {
                        std::string data = eval_string(*mc.args[0], env);
                        fit->second->write(data.data(), static_cast<std::streamsize>(data.size()));
                        env.vars["_"] = std::to_string(static_cast<int>(data.size()));
                    }
                }
                return;
            }
            if (handle.rfind("file:", 0) == 0 && methodName == "seek") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    auto fit = g_fileStreams.find(id);
                    if (fit != g_fileStreams.end() && fit->second && fit->second->is_open()) {
                        int64_t off = to_int(eval_string(*mc.args[0], env));
                        std::ios::seekdir dir = std::ios::beg;
                        if (mc.args.size() >= 2) {
                            std::string whence = eval_string(*mc.args[1], env);
                            if (whence == "cur" || whence == "current") dir = std::ios::cur;
                            else if (whence == "end") dir = std::ios::end;
                        }
                        fit->second->seekg(static_cast<std::streamoff>(off), dir);
                        fit->second->seekp(static_cast<std::streamoff>(off), dir);
                        env.vars["_"] = "true";
                    }
                }
                return;
            }
            if (handle.rfind("file:", 0) == 0 && methodName == "tell") {
                int id = to_int(handle.substr(5));
                auto fit = g_fileStreams.find(id);
                if (fit != g_fileStreams.end() && fit->second && fit->second->is_open()) {
                    env.vars["_"] = std::to_string(static_cast<int64_t>(fit->second->tellg()));
                } else {
                    env.vars["_"] = "0";
                }
                return;
            }
            if (handle.rfind("file:", 0) == 0 && methodName == "flush") {
                int id = to_int(handle.substr(5));
                auto fit = g_fileStreams.find(id);
                if (fit != g_fileStreams.end() && fit->second && fit->second->is_open()) {
                    fit->second->flush();
                    env.vars["_"] = "true";
                } else {
                    env.vars["_"] = "false";
                }
                return;
            }
            if (handle.rfind("file:", 0) == 0 && methodName == "close") {
                int id = to_int(handle.substr(5));
                auto fit = g_fileStreams.find(id);
                if (fit != g_fileStreams.end() && fit->second && fit->second->is_open()) {
                    fit->second->close();
                }
                g_fileStreams.erase(id);
                env.vars["_"] = "true";
                return;
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
                std::string result = __erelang_ws_handle_method(id, wsMethod, args);
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
                std::string result = __erelang_http_handle_method(id, methodName, args);
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
            // Closure dispatch (handle prefix "func:")
            if (handle.rfind("func:", 0) == 0) {
                const std::string funcIdStr = handle.substr(5);
                int funcId = 0;
                try { funcId = std::stoi(funcIdStr); }
                catch (...) { throw std::runtime_error("Invalid func handle: " + handle); }
                auto it = g_closures.find(funcId);
                if (it == g_closures.end() || !it->second) {
                    // Allow call on null — silently return empty string
                    env.vars["_"] = "";
                    return;
                }
                ClosureData* cd = it->second;
                Env callEnv;
                // Seed captured values
                for (const auto& cv : cd->captured) {
                    callEnv.vars[cv.name] = cv.value;
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
                        env.vars[mc.objectName + "." + f.name] = selfIt->second;
                    } else if (fieldIt != callEnv.vars.end()) {
                        env.vars[mc.objectName + "." + f.name] = fieldIt->second;
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
        if (const Action* a = find_action(program, call.name)) {
            if (program.strict && a->visibility != Visibility::Public) {
                throw std::runtime_error("Action not public: " + a->name);
            }
            Env calleeEnv;
            // Seed with current shared globals
            for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
            // Bind positional args into parameter names
            for (size_t i=0; i<a->params.size() && i<call.args.size(); ++i) {
                const std::string paramName = a->params[i].name;
                calleeEnv.vars[paramName] = eval_string(*call.args[i], env);
                if (std::holds_alternative<ExprIdent>(call.args[i]->node)) {
                    const auto& id = std::get<ExprIdent>(call.args[i]->node).name;
                    auto oit = env.objects.find(id);
                    if (oit != env.objects.end()) {
                        calleeEnv.objects[paramName] = oit->second;
                    }
                }
            }
            ExecContext calleeCtx;
            exec_block(a->body, program, calleeCtx, calleeEnv);
            for (auto& th : calleeCtx.threads) {
                if (th.joinable()) {
                    th.join();
                }
            }
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
                    for (const auto& cv : cd->captured) callEnv.vars[cv.name] = cv.value;
                    for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
                    for (size_t i = 0; i < cd->body.params.size() && i < call.args.size(); ++i) {
                        callEnv.vars[cd->body.params[i].name] = eval_string(*call.args[i], env);
                    }
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
    for (const auto& st : b.stmts) {
        exec_stmt(st, program, ctx, env);
        if (ctx.returned || ctx.breakSignal || ctx.continueSignal) {
            break;
        }
    }
}

} // namespace erelang
