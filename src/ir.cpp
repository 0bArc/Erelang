#include "erelang/ir.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace erelang {
namespace {

std::string quote_string(std::string v) {
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('"');
    for (char c : v) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
            continue;
        }
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '\r') { out += "\\r"; continue; }
        if (c == '\t') { out += "\\t"; continue; }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool is_integer_text(std::string_view text) {
    if (text.empty()) return false;
    std::size_t i = 0;
    if (text.front() == '-' || text.front() == '+') i = 1;
    if (i >= text.size()) return false;
    for (; i < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) return false;
    }
    return true;
}

struct ValueRef {
    std::string value;
    std::string type; // int, bool, string, unknown
};

class IRLowerer {
public:
    explicit IRLowerer(IRFunction& fn) : fn_(fn) {}

    void seed_params(const std::vector<std::string>& params) {
        for (const auto& p : params) {
            varTypes_[p] = "unknown";
        }
    }

    void lower_block(const Block& block) {
        for (const auto& stmt : block.stmts) {
            lower_stmt(stmt);
        }
    }

private:
    static std::string to_var(const std::string& name) { return "$" + name; }

    std::string new_temp() {
        return "%t" + std::to_string(nextTemp_++);
    }

    std::string new_label(const std::string& hint) {
        return "L_" + hint + "_" + std::to_string(nextLabel_++);
    }

    void emit(std::string opcode, std::vector<std::string> operands = {}) {
        fn_.instructions.push_back(IRInstruction{std::move(opcode), std::move(operands)});
    }

    std::string type_of_var(const std::string& name) const {
        auto it = varTypes_.find(name);
        if (it == varTypes_.end()) return "unknown";
        return it->second;
    }

    ValueRef lower_expr(const ExprPtr& expr) {
        if (!expr) return {"#0", "unknown"};
        return std::visit([&](const auto& node) -> ValueRef {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ExprString>) {
                return {quote_string(node.v), "string"};
            } else if constexpr (std::is_same_v<T, ExprNull>) {
                return {"#0", "unknown"};
            } else if constexpr (std::is_same_v<T, ExprNumber>) {
                if (!node.raw.empty() && is_integer_text(node.raw)) return {"#" + node.raw, "int"};
                return {"#" + std::to_string(node.v), "int"};
            } else if constexpr (std::is_same_v<T, ExprBool>) {
                return {node.v ? "#1" : "#0", "bool"};
            } else if constexpr (std::is_same_v<T, ExprIdent>) {
                return {to_var(node.name), type_of_var(node.name)};
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                auto v = lower_expr(node.expr);
                if (node.op == UnOp::Neg) {
                    const std::string dst = new_temp();
                    emit("neg", {dst, v.value});
                    return {dst, "int"};
                }
                if (node.op == UnOp::Not) {
                    const std::string dst = new_temp();
                    emit("not", {dst, v.value});
                    return {dst, "bool"};
                }
                return {"#0", "unknown"};
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                auto l = lower_expr(node.left);
                auto r = lower_expr(node.right);
                const std::string dst = new_temp();
                switch (node.op) {
                    case BinOp::Add: emit("add", {dst, l.value, r.value}); return {dst, l.type == "string" || r.type == "string" ? "string" : "int"};
                    case BinOp::Sub: emit("sub", {dst, l.value, r.value}); return {dst, "int"};
                    case BinOp::Mul: emit("mul", {dst, l.value, r.value}); return {dst, "int"};
                    case BinOp::Div: emit("div", {dst, l.value, r.value}); return {dst, "int"};
                    case BinOp::Mod: emit("mod", {dst, l.value, r.value}); return {dst, "int"};
                    case BinOp::EQ: emit("cmp_eq", {dst, l.value, r.value}); return {dst, "bool"};
                    case BinOp::NE: emit("cmp_ne", {dst, l.value, r.value}); return {dst, "bool"};
                    case BinOp::LT: emit("cmp_lt", {dst, l.value, r.value}); return {dst, "bool"};
                    case BinOp::LE: emit("cmp_le", {dst, l.value, r.value}); return {dst, "bool"};
                    case BinOp::GT: emit("cmp_gt", {dst, l.value, r.value}); return {dst, "bool"};
                    case BinOp::GE: emit("cmp_ge", {dst, l.value, r.value}); return {dst, "bool"};
                    case BinOp::And: emit("and", {dst, l.value, r.value}); return {dst, "bool"};
                    case BinOp::Or: emit("or", {dst, l.value, r.value}); return {dst, "bool"};
                    default: return {"#0", "unknown"};
                }
            } else if constexpr (std::is_same_v<T, TernaryExpr>) {
                auto cond = lower_expr(node.cond);
                const std::string thenLabel = new_label("tern_then");
                const std::string elseLabel = new_label("tern_else");
                const std::string endLabel = new_label("tern_end");
                const std::string dst = new_temp();
                emit("jnz", {cond.value, thenLabel});
                emit("jmp", {elseLabel});
                emit("label", {thenLabel});
                auto t = lower_expr(node.thenExpr);
                emit("mov", {dst, t.value});
                emit("jmp", {endLabel});
                emit("label", {elseLabel});
                auto e = lower_expr(node.elseExpr);
                emit("mov", {dst, e.value});
                emit("label", {endLabel});
                return {dst, t.type == e.type ? t.type : "unknown"};
            } else if constexpr (std::is_same_v<T, FunctionCallExpr>) {
                std::vector<std::string> ops;
                ops.push_back(node.name);
                for (const auto& a : node.args) ops.push_back(lower_expr(a).value);
                const std::string dst = new_temp();
                emit("call_name", std::move(ops));
                emit("call", {dst});
                return {dst, "unknown"};
            } else if constexpr (std::is_same_v<T, MemberExpr>) {
                return {"#0", "unknown"};
            } else if constexpr (std::is_same_v<T, NewExpr>) {
                return {"#0", "unknown"};
            } else {
                return {"#0", "unknown"};
            }
        }, expr->node);
    }

    void lower_stmt(const Statement& stmt) {
        std::visit([&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, PrintStmt>) {
                if (s.value) {
                    if (const auto* strNode = std::get_if<ExprString>(&s.value->node)) {
                        std::string fmt;
                        std::vector<std::string> args;
                        bool hadPlaceholder = false;
                        for (std::size_t i = 0; i < strNode->v.size();) {
                            if (strNode->v[i] == '{') {
                                const std::size_t close = strNode->v.find('}', i + 1);
                                if (close != std::string::npos) {
                                    std::string name = strNode->v.substr(i + 1, close - i - 1);
                                    if (!name.empty()) {
                                        hadPlaceholder = true;
                                        const std::string t = type_of_var(name);
                                        fmt += (t == "string") ? "%s" : "%lld";
                                        args.push_back(to_var(name));
                                        i = close + 1;
                                        continue;
                                    }
                                }
                            }
                            fmt.push_back(strNode->v[i]);
                            ++i;
                        }
                        if (hadPlaceholder) {
                            if (fmt.empty() || fmt.back() != '\n') fmt.push_back('\n');
                            std::vector<std::string> ops;
                            ops.push_back(quote_string(fmt));
                            ops.insert(ops.end(), args.begin(), args.end());
                            emit("print_fmt", std::move(ops));
                            return;
                        }
                    }
                }
                auto v = lower_expr(s.value);
                if (v.type == "string") emit("print_s", {v.value});
                else emit("print_i", {v.value});
            } else if constexpr (std::is_same_v<T, LetStmt>) {
                auto v = lower_expr(s.value);
                emit("mov", {to_var(s.name), v.value});
                varTypes_[s.name] = v.type;
            } else if constexpr (std::is_same_v<T, SetStmt>) {
                if (!s.isMember) {
                    auto v = lower_expr(s.value);
                    emit("mov", {to_var(s.varOrField), v.value});
                    varTypes_[s.varOrField] = v.type;
                } else {
                    emit("todo", {"member-set"});
                }
            } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                if (s.value.has_value()) emit("ret", {lower_expr(*s.value).value});
                else emit("ret");
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                auto cond = lower_expr(s.cond);
                const std::string elseLabel = new_label("if_else");
                const std::string endLabel = new_label("if_end");
                emit("jz", {cond.value, elseLabel});
                if (s.thenBlk) lower_block(*s.thenBlk);
                emit("jmp", {endLabel});
                emit("label", {elseLabel});
                if (s.elseBlk) lower_block(*s.elseBlk);
                emit("label", {endLabel});
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                const std::string startLabel = new_label("while_start");
                const std::string endLabel = new_label("while_end");
                emit("label", {startLabel});
                auto cond = lower_expr(s.cond);
                emit("jz", {cond.value, endLabel});
                if (s.body) lower_block(*s.body);
                emit("jmp", {startLabel});
                emit("label", {endLabel});
            } else if constexpr (std::is_same_v<T, DoWhileStmt>) {
                const std::string startLabel = new_label("do_start");
                emit("label", {startLabel});
                if (s.body) lower_block(*s.body);
                auto cond = lower_expr(s.cond);
                emit("jnz", {cond.value, startLabel});
            } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                auto count = lower_expr(s.count);
                const std::string counter = new_temp();
                emit("mov", {counter, "#0"});
                const std::string loopLabel = new_label("repeat_loop");
                const std::string endLabel = new_label("repeat_end");
                emit("label", {loopLabel});
                const std::string cond = new_temp();
                emit("cmp_lt", {cond, counter, count.value});
                emit("jz", {cond, endLabel});
                if (s.body) lower_block(*s.body);
                const std::string one = new_temp();
                emit("mov", {one, "#1"});
                const std::string next = new_temp();
                emit("add", {next, counter, one});
                emit("mov", {counter, next});
                emit("jmp", {loopLabel});
                emit("label", {endLabel});
            } else if constexpr (std::is_same_v<T, SwitchStmt>) {
                auto sel = lower_expr(s.selector);
                const std::string endLabel = new_label("switch_end");
                std::vector<std::string> caseLabels;
                caseLabels.reserve(s.cases.size());
                for (std::size_t i = 0; i < s.cases.size(); ++i) caseLabels.push_back(new_label("switch_case"));
                const std::string defaultLabel = s.defaultBlk ? new_label("switch_default") : endLabel;

                for (std::size_t i = 0; i < s.cases.size(); ++i) {
                    const auto& c = s.cases[i];
                    std::string caseImm;
                    if (is_integer_text(c.value)) caseImm = "#" + c.value;
                    else caseImm = quote_string(c.value);
                    const std::string cmp = new_temp();
                    emit("cmp_eq", {cmp, sel.value, caseImm});
                    emit("jnz", {cmp, caseLabels[i]});
                }
                emit("jmp", {defaultLabel});

                for (std::size_t i = 0; i < s.cases.size(); ++i) {
                    emit("label", {caseLabels[i]});
                    if (s.cases[i].body) lower_block(*s.cases[i].body);
                    emit("jmp", {endLabel});
                }
                if (s.defaultBlk) {
                    emit("label", {defaultLabel});
                    lower_block(*s.defaultBlk);
                }
                emit("label", {endLabel});
            } else if constexpr (std::is_same_v<T, ActionCallStmt>) {
                std::vector<std::string> ops;
                ops.push_back(s.name);
                for (const auto& a : s.args) ops.push_back(lower_expr(a).value);
                emit("call_name", std::move(ops));
            } else if constexpr (std::is_same_v<T, MethodCallStmt>) {
                emit("todo", {"method-call", s.objectName + ":" + s.method});
            } else if constexpr (std::is_same_v<T, ForInStmt>) {
                emit("todo", {"for-in"});
            } else if constexpr (std::is_same_v<T, ForStmt>) {
                emit("todo", {"for"});
            } else if constexpr (std::is_same_v<T, TryCatchStmt>) {
                emit("todo", {"try-catch"});
            } else if constexpr (std::is_same_v<T, UnsafeStmt>) {
                if (s.body) lower_block(*s.body);
            } else if constexpr (std::is_same_v<T, PointerSetStmt>) {
                emit("todo", {"pointer-set"});
            } else if constexpr (std::is_same_v<T, InputStmt>) {
                emit("todo", {"input"});
            } else if constexpr (std::is_same_v<T, FireStmt>) {
                emit("todo", {"fire"});
            } else if constexpr (std::is_same_v<T, WaitAllStmt>) {
                emit("todo", {"wait-all"});
            } else if constexpr (std::is_same_v<T, PauseStmt>) {
                emit("todo", {"pause"});
            } else if constexpr (std::is_same_v<T, SleepStmt>) {
                emit("todo", {"sleep"});
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ParallelStmt>>) {
                emit("todo", {"parallel"});
            }
        }, stmt);
    }

private:
    IRFunction& fn_;
    int nextTemp_{0};
    int nextLabel_{0};
    std::unordered_map<std::string, std::string> varTypes_;
};

} // namespace

IRModule IRBuilder::build(const Program& program) const {
    IRModule module;
    module.entry = program.runTarget;
    if (!module.entry.has_value()) {
        for (const auto& a : program.actions) {
            if (a.name == "main") {
                module.entry = "main";
                break;
            }
        }
    }

    std::vector<const Action*> sorted;
    sorted.reserve(program.actions.size());
    for (const auto& action : program.actions) sorted.push_back(&action);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Action* a, const Action* b) { return a->name < b->name; });

    for (const Action* action : sorted) {
        IRFunction fn;
        fn.name = action->name;
        for (const auto& p : action->params) fn.params.push_back(p.name);
        IRLowerer lowerer(fn);
        lowerer.seed_params(fn.params);
        lowerer.lower_block(action->body);
        if (fn.instructions.empty() || fn.instructions.back().opcode != "ret") {
            fn.instructions.push_back(IRInstruction{"ret", {"#0"}});
        }
        module.functions.push_back(std::move(fn));
    }

    return module;
}

std::string ir_to_text(const IRModule& module) {
    std::ostringstream out;
    out << "; erelang IR v1\n";
    if (module.entry.has_value()) out << "entry " << *module.entry << "\n";
    out << "\n";

    for (const auto& fn : module.functions) {
        out << "func " << fn.name << "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) out << ", ";
            out << fn.params[i];
        }
        out << ")\n";
        for (const auto& ins : fn.instructions) {
            out << "  " << ins.opcode;
            if (!ins.operands.empty()) {
                out << " ";
                for (size_t i = 0; i < ins.operands.size(); ++i) {
                    if (i) out << ", ";
                    out << ins.operands[i];
                }
            }
            out << "\n";
        }
        out << "end\n\n";
    }

    return out.str();
}

} // namespace erelang
