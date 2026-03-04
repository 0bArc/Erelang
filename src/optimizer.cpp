#include "erelang/optimizer.hpp"
#include <memory>
#include <optional>

namespace erelang {
namespace {

template <typename... Lambdas>
struct Overloaded : Lambdas... {
	using Lambdas::operator()...;
};

template <typename... Lambdas>
Overloaded(Lambdas...)->Overloaded<Lambdas...>;

[[nodiscard]] inline ExprPtr make_number_expr(int64_t value) {
	return std::make_shared<Expr>(Expr{ ExprNumber{ value, false, std::to_string(value) } });
}

struct FoldStats {
	int folds{0};
	int nodesReplaced{0};

	void record_fold() noexcept {
		++folds;
		++nodesReplaced;
	}
};

[[nodiscard]] std::optional<int64_t> as_number(const ExprPtr& expr) noexcept {
	if (!expr) {
		return std::nullopt;
	}
	if (auto* number = std::get_if<ExprNumber>(&expr->node)) {
		return number->v;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<int64_t> apply_binary(BinOp op, int64_t lhs, int64_t rhs) noexcept {
	switch (op) {
		case BinOp::Add: return lhs + rhs;
		case BinOp::Sub: return lhs - rhs;
		case BinOp::Mul: return lhs * rhs;
		case BinOp::Div:
			if (rhs == 0) return std::nullopt;
			return lhs / rhs;
		case BinOp::Mod:
			if (rhs == 0) return std::nullopt;
			return lhs % rhs;
		case BinOp::Pow: {
			if (rhs < 0) return std::nullopt;
			int64_t value = 1;
			for (int64_t i = 0; i < rhs; ++i) value *= lhs;
			return value;
		}
		default: break;
	}
	return std::nullopt;
}

void fold_expr(ExprPtr& expr, FoldStats& stats);
void fold_block(Block& block, FoldStats& stats);

void fold_expr(ExprPtr& expr, FoldStats& stats) {
	if (!expr) {
		return;
	}

	std::visit(
		Overloaded{
			[&](BinaryExpr& bin) {
				fold_expr(bin.left, stats);
				fold_expr(bin.right, stats);

				const auto lhs = as_number(bin.left);
				const auto rhs = as_number(bin.right);
				if (!lhs || !rhs) {
					return;
				}

				if (const auto result = apply_binary(bin.op, *lhs, *rhs)) {
					expr = make_number_expr(*result);
					stats.record_fold();
				}
			},
			[&](UnaryExpr& un) {
				fold_expr(un.expr, stats);

				const auto value = as_number(un.expr);
				if (!value) {
					return;
				}

				if (un.op == UnOp::Neg) {
					expr = make_number_expr(-*value);
					stats.record_fold();
				}
			},
			[](auto&) {}
		},
		expr->node);
}

void fold_block(Block& block, FoldStats& stats) {
	auto fold_expr_inplace = [&](ExprPtr& e) { fold_expr(e, stats); };

	for (auto& stmtVariant : block.stmts) {
		std::visit(
			Overloaded{
				[&](PrintStmt& stmt) { fold_expr_inplace(stmt.value); },
				[&](ActionCallStmt& stmt) {
					for (auto& arg : stmt.args) {
						fold_expr_inplace(arg);
					}
				},
				[&](LetStmt& stmt) { fold_expr_inplace(stmt.value); },
				[&](ReturnStmt& stmt) {
					if (stmt.value) {
						fold_expr_inplace(*stmt.value);
					}
				},
				[&](SetStmt& stmt) { fold_expr_inplace(stmt.value); },
				[&](MethodCallStmt& stmt) {
					for (auto& arg : stmt.args) {
						fold_expr_inplace(arg);
					}
				},
				[&](IfStmt& stmt) {
					fold_expr_inplace(stmt.cond);
					if (stmt.thenBlk) {
						fold_block(*stmt.thenBlk, stats);
					}
					if (stmt.elseBlk) {
						fold_block(*stmt.elseBlk, stats);
					}
				},
				[&](WhileStmt& stmt) {
					fold_expr_inplace(stmt.cond);
					if (stmt.body) {
						fold_block(*stmt.body, stats);
					}
				},
				[&](ForStmt& stmt) {
					if (stmt.init) {
						fold_block(*stmt.init, stats);
					}
					if (stmt.cond && *stmt.cond) {
						fold_expr_inplace(*stmt.cond);
					}
					if (stmt.step) {
						fold_block(*stmt.step, stats);
					}
					if (stmt.body) {
						fold_block(*stmt.body, stats);
					}
				},
				[&](ForInStmt& stmt) {
					fold_expr_inplace(stmt.iterable);
					if (stmt.body) {
						fold_block(*stmt.body, stats);
					}
				},
				[&](std::shared_ptr<ParallelStmt>& stmt) {
					if (stmt) {
						fold_block(stmt->body, stats);
					}
				},
				[&](SwitchStmt& stmt) {
					fold_expr_inplace(stmt.selector);
					for (auto& c : stmt.cases) {
						if (c.body) {
							fold_block(*c.body, stats);
						}
					}
					if (stmt.defaultBlk) {
						fold_block(*stmt.defaultBlk, stats);
					}
				},
				[](auto&) {}
			},
			stmtVariant);
	}
}

} // namespace

OptimizeResult optimize_program(Program& program) {
	OptimizeResult result;

	auto process_block_fixpoint = [&](Block& block) {
		while (true) {
			FoldStats passStats{};
			fold_block(block, passStats);
			if (passStats.folds == 0) {
				break;
			}

			result.folds += passStats.folds;
			result.nodesReplaced += passStats.nodesReplaced;
			++result.passes;
		}
	};

	for (auto& action : program.actions) {
		process_block_fixpoint(action.body);
	}
	for (auto& entity : program.entities) {
		for (auto& method : entity.methods) {
			process_block_fixpoint(method.body);
		}
	}

	return result;
}

} // namespace erelang
