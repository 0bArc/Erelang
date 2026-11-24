// AST optimizer: constant folding and removal of branches that can never run.
//
// Folding follows the runtime's semantics exactly (src/runtime/value.cpp). Integer arithmetic that
// would overflow is left alone so the runtime's wrapping rule decides; float literals are never
// folded with integer rules; string concatenation only folds literals that contain no `{`, so
// interpolation still happens at run time.
#include "erelang/optimizer.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace erelang {
namespace {

template <typename... Lambdas>
struct Overloaded : Lambdas... {
    using Lambdas::operator()...;
};
template <typename... Lambdas>
Overloaded(Lambdas...) -> Overloaded<Lambdas...>;

ExprPtr make_number(int64_t value) {
    return std::make_shared<Expr>(Expr{ExprNumber{value, false, std::to_string(value)}});
}
ExprPtr make_bool(bool value) {
    return std::make_shared<Expr>(Expr{ExprBool{value}});
}
ExprPtr make_string(std::string value) {
    return std::make_shared<Expr>(Expr{ExprString{std::move(value), false}});
}

struct FoldStats {
    int folds{0};
    void record() noexcept { ++folds; }
};

std::optional<int64_t> as_int(const ExprPtr& e) noexcept {
    if (!e) return std::nullopt;
    if (const auto* n = std::get_if<ExprNumber>(&e->node)) {
        if (n->isFloatLiteral) return std::nullopt;
        return n->v;
    }
    return std::nullopt;
}

std::optional<bool> as_bool(const ExprPtr& e) noexcept {
    if (!e) return std::nullopt;
    if (const auto* b = std::get_if<ExprBool>(&e->node)) return b->v;
    return std::nullopt;
}

// A string literal that is safe to fold: no interpolation braces.
const std::string* as_plain_string(const ExprPtr& e) noexcept {
    if (!e) return nullptr;
    if (const auto* s = std::get_if<ExprString>(&e->node)) {
        if (s->v.find('{') == std::string::npos && s->v.find('}') == std::string::npos) return &s->v;
    }
    return nullptr;
}

std::optional<int64_t> checked_mul(int64_t lhs, int64_t rhs) noexcept {
    int64_t out = 0;
    if (__builtin_mul_overflow(lhs, rhs, &out)) return std::nullopt;
    return out;
}

std::optional<int64_t> fold_int(BinOp op, int64_t lhs, int64_t rhs) noexcept {
    int64_t out = 0;
    switch (op) {
        case BinOp::Add: if (__builtin_add_overflow(lhs, rhs, &out)) return std::nullopt; return out;
        case BinOp::Sub: if (__builtin_sub_overflow(lhs, rhs, &out)) return std::nullopt; return out;
        case BinOp::Mul: return checked_mul(lhs, rhs);
        case BinOp::Div:
            if (rhs == 0 || (lhs == INT64_MIN && rhs == -1)) return std::nullopt;
            return lhs / rhs;
        case BinOp::Mod:
            if (rhs == 0 || (lhs == INT64_MIN && rhs == -1)) return std::nullopt;
            return lhs % rhs;
        case BinOp::Pow: {
            if (rhs < 0 || rhs > 62) return std::nullopt;  // negative exponents give floats at run time
            int64_t value = 1;
            for (int64_t i = 0; i < rhs; ++i) {
                const auto p = checked_mul(value, lhs);
                if (!p) return std::nullopt;
                value = *p;
            }
            return value;
        }
        case BinOp::BitAnd: return lhs & rhs;
        case BinOp::BitXor: return lhs ^ rhs;
        case BinOp::BitOr: return lhs | rhs;
        case BinOp::Shl:
            if (rhs < 0 || rhs >= 64) return std::nullopt;
            return static_cast<int64_t>(static_cast<uint64_t>(lhs) << static_cast<unsigned>(rhs));
        case BinOp::Shr:
            if (rhs < 0 || rhs >= 64) return std::nullopt;
            return lhs >> static_cast<unsigned>(rhs);
        default: return std::nullopt;
    }
}

std::optional<bool> fold_compare(BinOp op, int64_t lhs, int64_t rhs) noexcept {
    switch (op) {
        case BinOp::EQ: case BinOp::StrictEQ: return lhs == rhs;
        case BinOp::NE: case BinOp::StrictNE: return lhs != rhs;
        case BinOp::LT: return lhs < rhs;
        case BinOp::LE: return lhs <= rhs;
        case BinOp::GT: return lhs > rhs;
        case BinOp::GE: return lhs >= rhs;
        default: return std::nullopt;
    }
}

void fold_expr(ExprPtr& expr, FoldStats& stats);
void fold_block(Block& block, FoldStats& stats);

void fold_all(std::vector<ExprPtr>& exprs, FoldStats& stats) {
    for (auto& e : exprs) fold_expr(e, stats);
}

void fold_expr(ExprPtr& expr, FoldStats& stats) {
    if (!expr) return;
    std::visit(
        Overloaded{
            [&](BinaryExpr& bin) {
                fold_expr(bin.left, stats);
                fold_expr(bin.right, stats);
                const auto li = as_int(bin.left), ri = as_int(bin.right);
                if (li && ri) {
                    if (const auto v = fold_int(bin.op, *li, *ri)) { expr = make_number(*v); stats.record(); return; }
                    if (const auto b = fold_compare(bin.op, *li, *ri)) { expr = make_bool(*b); stats.record(); return; }
                    return;
                }
                const auto lb = as_bool(bin.left), rb = as_bool(bin.right);
                if (lb && rb) {
                    if (bin.op == BinOp::And) { expr = make_bool(*lb && *rb); stats.record(); return; }
                    if (bin.op == BinOp::Or) { expr = make_bool(*lb || *rb); stats.record(); return; }
                    if (bin.op == BinOp::EQ || bin.op == BinOp::StrictEQ) { expr = make_bool(*lb == *rb); stats.record(); return; }
                    if (bin.op == BinOp::NE || bin.op == BinOp::StrictNE) { expr = make_bool(*lb != *rb); stats.record(); return; }
                }
                // `false && x` and `true || x` short-circuit at run time: x never runs.
                if (lb && bin.op == BinOp::And && !*lb) { expr = make_bool(false); stats.record(); return; }
                if (lb && bin.op == BinOp::Or && *lb) { expr = make_bool(true); stats.record(); return; }
                if (bin.op == BinOp::Add) {
                    const std::string* ls = as_plain_string(bin.left);
                    const std::string* rs = as_plain_string(bin.right);
                    if (ls && rs) { expr = make_string(*ls + *rs); stats.record(); return; }
                }
            },
            [&](UnaryExpr& un) {
                fold_expr(un.expr, stats);
                if (const auto v = as_int(un.expr)) {
                    if (un.op == UnOp::Neg && *v != INT64_MIN) { expr = make_number(-*v); stats.record(); }
                    else if (un.op == UnOp::BitNot) { expr = make_number(~*v); stats.record(); }
                    return;
                }
                if (const auto b = as_bool(un.expr); b && un.op == UnOp::Not) { expr = make_bool(!*b); stats.record(); }
            },
            [&](TernaryExpr& t) {
                fold_expr(t.cond, stats);
                fold_expr(t.thenExpr, stats);
                fold_expr(t.elseExpr, stats);
                if (const auto b = as_bool(t.cond)) {
                    ExprPtr chosen = *b ? t.thenExpr : t.elseExpr;
                    if (chosen) { expr = std::move(chosen); stats.record(); }
                }
            },
            [&](FunctionCallExpr& call) { fold_all(call.args, stats); },
            [&](NewExpr& n) { fold_all(n.args, stats); },
            [&](ListLiteralExpr& l) { fold_all(l.elements, stats); },
            [&](DictLiteralExpr& d) { fold_all(d.entries, stats); },
            [&](TupleLiteralExpr& t) { fold_all(t.elements, stats); },
            [&](IndexExpr& ix) { fold_expr(ix.object, stats); fold_expr(ix.index, stats); },
            [&](RangeExpr& r) { fold_expr(r.start, stats); fold_expr(r.end, stats); },
            [&](CompoundAssignExpr& c) { fold_expr(c.right, stats); },
            [&](TryExpr& t) { fold_expr(t.inner, stats); },
            [&](AwaitExpr& a) { fold_expr(a.inner, stats); },
            [](auto&) {}},
        expr->node);
}

// True when a statement can be dropped: `if (false) { ... }` with no else, `while (false)`.
bool is_dead(const Statement& s) {
    if (const auto* st = std::get_if<IfStmt>(&s)) {
        const auto b = as_bool(st->cond);
        return b && !*b && !st->elseBlk;
    }
    if (const auto* st = std::get_if<WhileStmt>(&s)) {
        const auto b = as_bool(st->cond);
        return b && !*b;
    }
    return false;
}

void fold_block(Block& block, FoldStats& stats) {
    auto fold = [&](ExprPtr& e) { fold_expr(e, stats); };
    for (auto& stmtVariant : block.stmts) {
        std::visit(
            Overloaded{
                [&](PrintStmt& st) { fold(st.value); },
                [&](ActionCallStmt& st) { fold_all(st.args, stats); },
                [&](LetStmt& st) { fold(st.value); fold(st.sizeExpr); },
                [&](ReturnStmt& st) { if (st.value) fold(*st.value); },
                [&](SetStmt& st) { fold(st.value); },
                [&](MethodCallStmt& st) { fold_all(st.args, stats); },
                [&](ExprStmt& st) { fold(st.expr); },
                [&](IfStmt& st) {
                    fold(st.cond);
                    if (st.thenBlk) fold_block(*st.thenBlk, stats);
                    if (st.elseBlk) fold_block(*st.elseBlk, stats);
                    // `if (true) A else B`: B can never run. (A keeps its own block scope.)
                    if (const auto b = as_bool(st.cond); b && *b && st.elseBlk) {
                        st.elseBlk.reset();
                        stats.record();
                    }
                },
                [&](WhileStmt& st) { fold(st.cond); if (st.body) fold_block(*st.body, stats); },
                [&](ForStmt& st) {
                    if (st.init) fold_block(*st.init, stats);
                    if (st.cond && *st.cond) fold(*st.cond);
                    if (st.step) fold_block(*st.step, stats);
                    if (st.body) fold_block(*st.body, stats);
                },
                [&](ForInStmt& st) { fold(st.iterable); if (st.body) fold_block(*st.body, stats); },
                [&](std::shared_ptr<ParallelStmt>& st) { if (st) fold_block(st->body, stats); },
                [&](SwitchStmt& st) {
                    fold(st.selector);
                    for (auto& c : st.cases) if (c.body) fold_block(*c.body, stats);
                    if (st.defaultBlk) fold_block(*st.defaultBlk, stats);
                },
                [&](MatchStmt& st) {
                    fold(st.selector);
                    for (auto& c : st.cases) if (c.body) fold_block(*c.body, stats);
                },
                [&](DoWhileStmt& st) { fold(st.cond); if (st.body) fold_block(*st.body, stats); },
                [&](RepeatStmt& st) { fold(st.count); if (st.body) fold_block(*st.body, stats); },
                [&](TryCatchStmt& st) {
                    if (st.tryBlk) fold_block(*st.tryBlk, stats);
                    if (st.catchBlk) fold_block(*st.catchBlk, stats);
                },
                [&](UnsafeStmt& st) { if (st.body) fold_block(*st.body, stats); },
                [&](PointerSetStmt& st) { fold(st.pointer); fold(st.value); },
                [&](IndexSetStmt& st) { fold(st.object); fold(st.index); fold(st.value); },
                [](auto&) {}},
            stmtVariant);
    }
    const auto before = block.stmts.size();
    std::erase_if(block.stmts, is_dead);
    for (std::size_t i = block.stmts.size(); i < before; ++i) stats.record();
}

}  // namespace

OptimizeResult optimize_program(Program& program) {
    OptimizeResult result;
    auto fixpoint = [&](Block& block) {
        for (int pass = 0; pass < 16; ++pass) {
            FoldStats stats{};
            fold_block(block, stats);
            if (stats.folds == 0) break;
            result.folds += stats.folds;
            result.nodesReplaced += stats.folds;
            ++result.passes;
        }
    };
    for (auto& action : program.actions) fixpoint(action.body);
    for (auto& entity : program.entities) {
        for (auto& method : entity.methods) fixpoint(method.body);
    }
    return result;
}

}  // namespace erelang
