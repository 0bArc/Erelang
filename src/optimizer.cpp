#include "erelang/optimizer.hpp"
#include <memory>
#include <optional>
#include <cstdint>

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
		// Floating-point literals must not be folded with integer arithmetic:
		// the runtime evaluates them via string/float rules, so folding would
		// change program output (e.g. 1.5 + 2.5 must stay "1.5" "2.5" concat).
		if (number->isFloatLiteral) {
			return std::nullopt;
		}
		return number->v;
	}
	return std::nullopt;
}

// Returns the product lhs*rhs, or nullopt on signed overflow / exponent too large.
[[nodiscard]] std::optional<int64_t> checked_mul(int64_t lhs, int64_t rhs) noexcept {
#if defined(__GNUC__) || defined(__clang__)
	__int128 wide = static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
	if (wide < static_cast<__int128>(INT64_MIN) || wide > static_cast<__int128>(INT64_MAX)) {
		return std::nullopt;
	}
	return static_cast<int64_t>(wide);
#else
	if (lhs == 0 || rhs == 0) return 0;
	if (lhs == -1 && rhs == INT64_MIN) return std::nullopt;
	if (rhs == -1 && lhs == INT64_MIN) return std::nullopt;
	int64_t result = lhs * rhs;
	if (result / rhs != lhs) return std::nullopt;
	return result;
#endif
}

[[nodiscard]] std::optional<int64_t> apply_binary(BinOp op, int64_t lhs, int64_t rhs) noexcept {
	switch (op) {
		case BinOp::Add:
			if ((rhs > 0 && lhs > INT64_MAX - rhs) || (rhs < 0 && lhs < INT64_MIN - rhs)) return std::nullopt;
			return lhs + rhs;
		case BinOp::Sub:
			if ((rhs < 0 && lhs > INT64_MAX + rhs) || (rhs > 0 && lhs < INT64_MIN + rhs)) return std::nullopt;
			return lhs - rhs;
		case BinOp::Mul:
			return checked_mul(lhs, rhs);
		case BinOp::Div:
			if (rhs == 0) return std::nullopt;
			if (lhs == INT64_MIN && rhs == -1) return std::nullopt;
			return lhs / rhs;
		case BinOp::Mod:
			if (rhs == 0) return std::nullopt;
			if (lhs == INT64_MIN && rhs == -1) return std::nullopt;
			return lhs % rhs;
		case BinOp::Pow: {
			if (rhs < 0) return std::nullopt;
			if (rhs > 62) return std::nullopt; // 2^63 overflows int64; bail on anything larger
			int64_t value = 1;
			for (int64_t i = 0; i < rhs; ++i) {
				auto product = checked_mul(value, lhs);
				if (!product) return std::nullopt;
				value = *product;
			}
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
				if (*value == INT64_MIN) {
					return; // -INT64_MIN would be signed-overflow UB; skip folding
				}
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
				[&](DoWhileStmt& stmt) {
					fold_expr_inplace(stmt.cond);
					if (stmt.body) {
						fold_block(*stmt.body, stats);
					}
				},
				[&](RepeatStmt& stmt) {
					fold_expr_inplace(stmt.count);
					if (stmt.body) {
						fold_block(*stmt.body, stats);
					}
				},
				[&](TryCatchStmt& stmt) {
					if (stmt.tryBlk) {
						fold_block(*stmt.tryBlk, stats);
					}
					if (stmt.catchBlk) {
						fold_block(*stmt.catchBlk, stats);
					}
				},
				[&](UnsafeStmt& stmt) {
					if (stmt.body) {
						fold_block(*stmt.body, stats);
					}
				},
				[&](PointerSetStmt& stmt) {
					fold_expr_inplace(stmt.pointer);
					fold_expr_inplace(stmt.value);
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
