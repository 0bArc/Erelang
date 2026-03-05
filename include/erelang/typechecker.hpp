#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <variant>
#include <memory>
#include "erelang/parser.hpp"

namespace erelang {

// =========== Types & Diagnostics ===========

struct TypeInfo { std::string name; bool operator==(const TypeInfo& o) const { return name == o.name; } };

enum class Severity { Error, Warning, Note };
enum class ReturnFlow { NoReturn, MaybeReturn, AlwaysReturn };
enum class LoopCtx { NotInLoop, InLoop };

struct Diagnostic {
    std::string code;     // e.g. TC001
    std::string message;  // human readable
    std::string context;  // action/entity name
    int line{-1};
    int col{-1};
    Severity severity{Severity::Error};
};

struct VarInfo {
    TypeInfo type{ "unknown" };
    bool isConst{false};
    bool assigned{false};
    bool used{false};
};

struct ActionUsage { bool referenced{false}; };
struct MethodUsage { bool referenced{false}; };
struct EntityUsage { bool referenced{false}; };

struct TCResult { bool ok{true}; std::vector<Diagnostic> diagnostics; };

// Forward declarations for helper classes
class TypeChecker;

struct CheckContext {
    const Program* program{nullptr};
    const Action* currentAction{nullptr};
    LoopCtx loop{LoopCtx::NotInLoop};
    class ScopeManager* scopes{nullptr};
    std::string actionName() const { return currentAction ? currentAction->name : std::string(); }
};

// Scope manager with RAII
class ScopeManager {
public:
    struct Guard { ScopeManager* sm; Guard(ScopeManager* s):sm(s){} ~Guard(){ if(sm) sm->pop(); } };
    Guard push() { scopes_.emplace_back(); return Guard(this); }
    bool declare(const std::string& name, const VarInfo& vi) { return scopes_.back().emplace(name, vi).second; }
    VarInfo* lookup(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) { auto f = it->find(name); if (f!=it->end()) return &f->second; }
        return nullptr;
    }
    std::unordered_map<std::string, VarInfo>& current() { return scopes_.back(); }
    const std::vector<std::unordered_map<std::string,VarInfo>>& all() const { return scopes_; }
private:
    void pop() { if(!scopes_.empty()) scopes_.pop_back(); }
    std::vector<std::unordered_map<std::string,VarInfo>> scopes_{1}; // start with one scope
};

// Diagnostic builder utility
class DiagBuilder {
public:
    DiagBuilder(TCResult& r, Severity sev, std::string msg, std::string code, std::string ctx)
      : res_(r) { d_.severity=sev; d_.message=std::move(msg); d_.code=std::move(code); d_.context=std::move(ctx); }
    DiagBuilder& at(int line, int col) { d_.line=line; d_.col=col; return *this; }
    void emit() { if (d_.severity==Severity::Error) res_.ok=false; res_.diagnostics.push_back(std::move(d_)); }
private:
    TCResult& res_; Diagnostic d_;
};

// Expression checker (caches inferred types)
class ExprChecker {
public:
    ExprChecker(TypeChecker& tc, TCResult& r):tc_(tc), result_(r) {}
    TypeInfo check(const ExprPtr& e, CheckContext& ctx);
    TypeInfo require_bool(const ExprPtr& e, CheckContext& ctx, const std::string& code, const std::string& msg);
private:
    TypeChecker& tc_; TCResult& result_;
    std::unordered_map<const Expr*, TypeInfo> cache_;
};

class StmtChecker {
public:
    StmtChecker(TypeChecker& tc, ExprChecker& ec, TCResult& r):tc_(tc), expr_(ec), result_(r) {}
    ReturnFlow check_stmt(const Statement& s, CheckContext& ctx, ScopeManager& scopes, const std::string& retType);
    ReturnFlow check_block(const Block& b, CheckContext& ctx, ScopeManager& scopes, const std::string& retType);
private:
    TypeChecker& tc_; ExprChecker& expr_; TCResult& result_;
};

// =========== Main Type Checker ===========
class TypeChecker {
public:
    TCResult check(const Program& program);
    // helpers used by visitors
    static bool is_bool(const TypeInfo& t) { return t.name == "bool"; }
    static bool is_int(const TypeInfo& t) { return t.name == "int"; }
    static bool is_string(const TypeInfo& t) { return t.name == "string"; }
    bool returns_void(const Action& a) const { return a.returnType.empty() || a.returnType == "void"; }
private:
    friend class ExprChecker; friend class StmtChecker;
    void pass_collect(const Program& program);
    void pass_check_program(const Program& program, TCResult& out);
    void finalize_unused(const Program& program, TCResult& out);
    void init_builtins();
private:
    struct BuiltinInfo { int minParams; int maxParams; std::string returnType; };
    // symbol tables / caches
    std::unordered_map<std::string, const Action*> actions_;
    std::unordered_map<std::string, const Entity*> entities_;
    std::unordered_map<std::string, std::unordered_map<std::string, const Action*>> methods_;
    std::unordered_map<std::string, std::unordered_set<std::string>> entityFields_;
    std::unordered_map<std::string, ActionUsage> actionUsage_;
    std::unordered_map<std::string, EntityUsage> entityUsage_;
    std::unordered_map<std::string, std::unordered_map<std::string, MethodUsage>> methodUsage_;
    std::unordered_map<std::string, BuiltinInfo> builtins_;
    std::unordered_set<std::string> externActions_;
};

} // namespace erelang
