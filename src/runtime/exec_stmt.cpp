// Statement execution: control flow, loops, match, return and the simple statements.
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

void Runtime::exec_import(const ImportStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    (void)program;
    (void)ctx;
    (void)env;
    return;
}

void Runtime::exec_print(const PrintStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    std::cout << eval_string(*st.value, env) << '\n';
    return;
}

void Runtime::exec_sleep(const SleepStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    std::this_thread::sleep_for(std::chrono::milliseconds(st.ms));
    return;
}

void Runtime::exec_parallel(const std::shared_ptr<ParallelStmt>& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& p = node;
    ctx.threads.emplace_back([this, &program, p, env]() mutable {
        ExecContext child;
        Env childEnv = env;
        exec_block(p->body, program, child, childEnv);
        for (auto& th : child.threads) if (th.joinable()) th.join();
    });
    return;
}

void Runtime::exec_wait_all(const WaitAllStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    for (auto& th : ctx.threads) if (th.joinable()) th.join();
    ctx.threads.clear();
    return;
}

void Runtime::exec_pause(const PauseStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    std::string dummy; std::getline(std::cin, dummy);
    return;
}

void Runtime::exec_input(const InputStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& is = node;
    std::string line; std::getline(std::cin, line);
    env_set(env, is.name, value_from_legacy_string(std::move(line)));
    return;
}

void Runtime::exec_return(const ReturnStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& rs = node;
    Value rv;
    ctx.returnFields.clear();
    if (!rs.value || !*rs.value) {
        rv = Value::from_string("");
    } else {
        if (std::holds_alternative<ExprIdent>((*rs.value)->node)) {
            const std::string& name = std::get<ExprIdent>((*rs.value)->node).name;
            if (mem_try_move_own_ident(env.vars, name, rv)) {
                // moved
            } else {
                const Value* bound = env_find(env, name);
                if (bound && value_is_struct_marker(*bound)) {
                    rv = *bound;
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
            if (value_is_struct_marker(rv) && !lastReturnFields_.empty())
                ctx.returnFields = lastReturnFields_;
        }
    }
    ctx.returned = true;
    ctx.returnValue = std::move(rv);
    return;
}

void Runtime::exec_fire(const FireStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    if (const Hook* h = find_hook(program, st.name)) exec_block(h->body, program, ctx, env);
    return;
}

void Runtime::exec_if(const IfStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    if (is_truthy(eval_value(*st.cond, env))) exec_block(*st.thenBlk, program, ctx, env);
    else if (st.elseBlk) exec_block(*st.elseBlk, program, ctx, env);
    return;
}

void Runtime::exec_break(const BreakStmt& node, const Program& program, ExecContext& ctx, Env& env) const {

ctx.breakSignal = true;

return;
}

void Runtime::exec_continue(const ContinueStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    ctx.continueSignal = true;
    return;
}

void Runtime::exec_while(const WhileStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
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

void Runtime::exec_repeat(const RepeatStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
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

void Runtime::exec_for(const ForStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
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

void Runtime::exec_do_while(const DoWhileStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
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

void Runtime::exec_for_in(const ForInStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    const Value iterValue = eval_value(*st.iterable, env);
    if (value_is_handle(iterValue, HandleKind::List)) {
        if (st.valueVar) {
            throw std::runtime_error("List iteration supports only one variable: for (item : list)");
        }
        const int id = static_cast<int>(value_handle_id(iterValue));
        auto it = g_lists.find(id);
        if (it != g_lists.end()) {
            // Index loop: the body may push to this list (reallocation) or create lists (rehash).
            const std::size_t count = it->second.size();
            for (std::size_t i = 0; i < count; ++i) {
                auto cur = g_lists.find(id);
                if (cur == g_lists.end() || i >= cur->second.size()) break;
                env_set(env, st.var, cur->second[i]);
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
    } else if (value_is_handle(iterValue, HandleKind::Dict)) {
        const int id = static_cast<int>(value_handle_id(iterValue));
        auto it = g_dicts.find(id);
        if (it != g_dicts.end()) {
            // Snapshot: the body may insert keys (rehash invalidates live iterators).
            const auto entries = it->second;
            for (const auto& kv : entries) {
                if (st.valueVar) {
                    env_set(env, st.var, value_from_legacy_string(kv.first));
                    env_set(env, *st.valueVar, kv.second);
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

void Runtime::exec_try_catch(const TryCatchStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    try {
        exec_block(*st.tryBlk, program, ctx, env);
    } catch (const std::exception& ex) {
        env_set(env, st.catchVar, Value::from_string(ex.what()));
        exec_block(*st.catchBlk, program, ctx, env);
    }
    return;
}

void Runtime::exec_unsafe(const UnsafeStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& st = node;
    exec_block(*st.body, program, ctx, env);
    return;
}

void Runtime::exec_switch(const SwitchStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& sw = node;
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

void Runtime::exec_match(const MatchStmt& node, const Program& program, ExecContext& ctx, Env& env) const {
    const auto& ms = node;
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
                    caseEnv.vars[name + "." + kv.first] = kv.second;
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

} // namespace erelang
