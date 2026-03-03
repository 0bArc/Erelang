#include "erelang/typechecker.hpp"
#include <unordered_set>

namespace erelang {

// ================= ExprChecker =================
TypeInfo ExprChecker::check(const ExprPtr& e, CheckContext& ctx) {
    if (!e) return {"void"};
    auto itC = cache_.find(e.get()); if (itC!=cache_.end()) return itC->second;
    TypeInfo inferred{"unknown"};
    std::visit([&](auto&& node){
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ExprNumber>) inferred = {"int"};
        else if constexpr (std::is_same_v<T, ExprBool>) inferred = {"bool"};
        else if constexpr (std::is_same_v<T, ExprString>) inferred = {"string"};
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
                    if ((TypeChecker::is_string(lt) && (TypeChecker::is_string(rt)||TypeChecker::is_int(rt))) || (TypeChecker::is_string(rt) && (TypeChecker::is_string(lt)||TypeChecker::is_int(lt)))) { inferred={"string"}; break; }
                    inferred={"unknown"};
                    break;
                case BinOp::Sub: case BinOp::Mul: case BinOp::Div: case BinOp::Mod:
                    inferred = {"int"}; break;
                case BinOp::And: case BinOp::Or: inferred={"bool"}; break;
                default: inferred={"bool"}; break; // comparisons
            }
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            inferred = check(node.expr, ctx);
        } else if constexpr (std::is_same_v<T, NewExpr>) {
            inferred = TypeInfo{"entity:" + node.typeName};
        } else if constexpr (std::is_same_v<T, MemberExpr>) {
            inferred = {"unknown"};
        } else if constexpr (std::is_same_v<T, FunctionCallExpr>) {
            // action or builtin
            auto aIt = tc_.actions_.find(node.name);
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
            auto it = tc_.actions_.find(stmt.name);
            if (it != tc_.actions_.end()) {
                tc_.actionUsage_[stmt.name].referenced = true;
                if (it->second->params.size() != stmt.args.size()) DiagBuilder(result_, Severity::Error, "Param count mismatch calling action " + stmt.name, "TC020", ctx.actionName()).emit();
            } else {
                auto bIt = tc_.builtins_.find(stmt.name);
                if (bIt == tc_.builtins_.end()) {
                    DiagBuilder(result_, Severity::Error, "Unknown action: " + stmt.name, "TC001", ctx.actionName()).emit();
                } else {
                    auto& bi = bIt->second;
                    if ((int)stmt.args.size() < bi.minParams || (bi.maxParams>=0 && (int)stmt.args.size() > bi.maxParams)) DiagBuilder(result_, Severity::Error, "Param count mismatch calling builtin " + stmt.name, "TC021", ctx.actionName()).emit();
                }
            }
            for (auto& a : stmt.args) expr_.check(a, ctx);
        } else if constexpr (std::is_same_v<T, LetStmt>) {
            if (scopes.lookup(stmt.name)) DiagBuilder(result_, Severity::Error, "Variable redeclaration: " + stmt.name, "TC030", ctx.actionName()).emit();
            VarInfo vi; vi.type = expr_.check(stmt.value, ctx); vi.isConst = stmt.isConst; vi.assigned=true; scopes.declare(stmt.name, vi);
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
            expr_.check(stmt.iterable, ctx); // iterable validation later
            CheckContext inner = ctx; inner.loop = LoopCtx::InLoop; inner.scopes = ctx.scopes;
            check_block(*stmt.body, inner, scopes, retType);
        } else if constexpr (std::is_same_v<T, SwitchStmt>) {
            expr_.check(stmt.selector, ctx);
            for (auto& c : stmt.cases) { auto guardC = scopes.push(); check_block(*c.body, ctx, scopes, retType); }
            if (stmt.defaultBlk) { auto guardD = scopes.push(); check_block(*stmt.defaultBlk, ctx, scopes, retType); }
        } else if constexpr (std::is_same_v<T, ParallelStmt>) {
            auto guardP = scopes.push(); check_block(stmt.body, ctx, scopes, retType);
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
    init_builtins();
    for (const auto& a : program.actions) { actions_[a.name] = &a; actionUsage_[a.name]; }
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
    add("exec",1,1,"int"); add("run_file",1,1,"void"); add("run_bat",1,1,"void"); add("read_line",0,0,"string"); add("prompt",1,1,"string");
    // Filesystem
    add("read_text",1,1,"string"); add("write_text",2,2,"void"); add("append_text",2,2,"void"); add("file_exists",1,1,"bool"); add("mkdirs",1,1,"void"); add("copy_file",2,2,"bool"); add("move_file",2,2,"bool"); add("delete_file",1,1,"bool");
    add("list_files",1,1,"unknown"); add("cwd",0,0,"string"); add("chdir",1,1,"bool");
    add("path_join",1,-1,"string"); add("path_dirname",1,1,"string"); add("path_basename",1,1,"string"); add("path_ext",1,1,"string");
    // Collections
    add("list_new",0,-1,"unknown"); add("list_push",2,2,"void"); add("list_get",2,2,"unknown"); add("list_len",1,1,"int"); add("list_join",2,2,"string"); add("list_clear",1,1,"void"); add("list_remove_at",2,2,"void");
    add("dict_new",0,-1,"unknown"); add("dict_set",3,-1,"void"); add("dict_get",2,-1,"unknown"); add("dict_has",2,-1,"bool"); add("dict_keys",1,1,"unknown"); add("dict_values",1,1,"unknown");
    add("dict_get_or",3,-1,"unknown"); add("dict_remove",2,-1,"bool"); add("dict_clear",1,1,"void"); add("dict_size",1,1,"int");
    add("dict_merge",2,2,"void"); add("dict_clone",1,1,"unknown"); add("dict_items",1,1,"unknown"); add("dict_entries",1,1,"unknown");
    add("dict_set_path",3,3,"void"); add("dict_get_path",2,3,"unknown"); add("dict_has_path",2,2,"bool"); add("dict_remove_path",2,2,"bool");
    add("table_new",0,0,"unknown"); add("table_put",4,4,"void"); add("table_get",3,4,"unknown"); add("table_has",3,3,"bool");
    add("table_remove",3,3,"bool"); add("table_rows",1,1,"unknown"); add("table_columns",1,1,"unknown"); add("table_row_keys",2,2,"unknown");
    add("table_clear_row",2,2,"void"); add("table_count_row",2,2,"int");
    // Network
    add("http_get",1,1,"string"); add("http_download",2,2,"bool"); add("hls_download_best",2,2,"bool"); add("url_encode",1,1,"string");
    add("network.ip.flush",0,0,"string"); add("network.ip.release",0,1,"string"); add("network.ip.renew",0,1,"string"); add("network.ip.registerdns",0,0,"string");
    add("network.debug.enable",0,1,"string"); add("network.debug.disable",0,0,"string");
    add("network.debug.status",0,0,"string"); add("network.debug.last",0,0,"string");
    add("network.debug.clear",0,0,"string"); add("network.debug.log_tail",0,1,"string");
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
