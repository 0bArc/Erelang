// SPDX-License-Identifier: Apache-2.0
#include "erelang/bytecode.hpp"

#include "erelang/parser.hpp"

#include <algorithm>
#include <stdexcept>
#include <variant>

namespace erelang {
namespace {

void emit_u8(Chunk& chunk, uint8_t v) { chunk.code.push_back(v); }

void emit_u16(Chunk& chunk, uint16_t v) {
    chunk.code.push_back(static_cast<uint8_t>(v & 0xff));
    chunk.code.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

uint16_t read_u16(const Chunk& chunk, size_t& ip) {
    if (ip + 1 >= chunk.code.size()) throw std::runtime_error("bytecode: truncated u16");
    const uint16_t lo = chunk.code[ip++];
    const uint16_t hi = chunk.code[ip++];
    return static_cast<uint16_t>(lo | (hi << 8));
}

int16_t read_i16(const Chunk& chunk, size_t& ip) {
    return static_cast<int16_t>(read_u16(chunk, ip));
}

uint16_t add_constant(Chunk& chunk, Value v) {
    if (chunk.constants.size() >= 0xffff) throw std::runtime_error("bytecode: too many constants");
    const auto idx = static_cast<uint16_t>(chunk.constants.size());
    chunk.constants.push_back(std::move(v));
    return idx;
}

uint16_t add_name(Chunk& chunk, std::string name) {
    for (size_t i = 0; i < chunk.names.size(); ++i) {
        if (chunk.names[i] == name) return static_cast<uint16_t>(i);
    }
    if (chunk.names.size() >= 0xffff) throw std::runtime_error("bytecode: too many names");
    const auto idx = static_cast<uint16_t>(chunk.names.size());
    chunk.names.push_back(std::move(name));
    return idx;
}

ValueBinOp to_value_bin_op(BinOp op) {
    switch (op) {
        case BinOp::Add: return ValueBinOp::Add;
        case BinOp::Sub: return ValueBinOp::Sub;
        case BinOp::Mul: return ValueBinOp::Mul;
        case BinOp::Div: return ValueBinOp::Div;
        case BinOp::Mod: return ValueBinOp::Mod;
        case BinOp::Pow: return ValueBinOp::Pow;
        case BinOp::EQ: return ValueBinOp::Eq;
        case BinOp::NE: return ValueBinOp::Ne;
        case BinOp::LT: return ValueBinOp::Lt;
        case BinOp::LE: return ValueBinOp::Le;
        case BinOp::GT: return ValueBinOp::Gt;
        case BinOp::GE: return ValueBinOp::Ge;
        case BinOp::And: return ValueBinOp::And;
        case BinOp::Or: return ValueBinOp::Or;
        case BinOp::Coalesce: return ValueBinOp::Coalesce;
        case BinOp::BitAnd: return ValueBinOp::BitAnd;
        case BinOp::BitXor: return ValueBinOp::BitXor;
        case BinOp::BitOr: return ValueBinOp::BitOr;
        case BinOp::Shl: return ValueBinOp::Shl;
        case BinOp::Shr: return ValueBinOp::Shr;
        case BinOp::StrictEQ: return ValueBinOp::StrictEq;
        case BinOp::StrictNE: return ValueBinOp::StrictNe;
    }
    return ValueBinOp::Add;
}

bool compile_node(const Expr& e, Chunk& out, const Runtime::Env* env);

bool compile_node(const Expr& e, Chunk& out, const Runtime::Env* env) {
    if (std::holds_alternative<ExprNull>(e.node)) {
        emit_u8(out, static_cast<uint8_t>(OpCode::LoadConst));
        emit_u16(out, add_constant(out, Value::null_value()));
        return true;
    }
    if (std::holds_alternative<ExprBool>(e.node)) {
        emit_u8(out, static_cast<uint8_t>(OpCode::LoadConst));
        emit_u16(out, add_constant(out, Value::from_bool(std::get<ExprBool>(e.node).v)));
        return true;
    }
    if (std::holds_alternative<ExprNumber>(e.node)) {
        const auto& number = std::get<ExprNumber>(e.node);
        Value v;
        if (number.isFloatLiteral) {
            try {
                v = Value::from_float(std::stod(number.raw));
            } catch (...) {
                v = Value::from_float(static_cast<double>(number.v));
            }
        } else {
            v = Value::from_int(number.v);
        }
        emit_u8(out, static_cast<uint8_t>(OpCode::LoadConst));
        emit_u16(out, add_constant(out, std::move(v)));
        return true;
    }
    if (std::holds_alternative<ExprString>(e.node)) {
        emit_u8(out, static_cast<uint8_t>(OpCode::LoadConst));
        emit_u16(out, add_constant(out, Value::from_string(std::get<ExprString>(e.node).v)));
        return true;
    }
    if (std::holds_alternative<ExprIdent>(e.node)) {
        const std::string& name = std::get<ExprIdent>(e.node).name;
        if (env && env->useSlots) {
            auto it = env->slotIndex.find(name);
            if (it != env->slotIndex.end() && it->second >= 0) {
                emit_u8(out, static_cast<uint8_t>(OpCode::LoadSlot));
                emit_u16(out, static_cast<uint16_t>(it->second));
                return true;
            }
        }
        emit_u8(out, static_cast<uint8_t>(OpCode::LoadName));
        emit_u16(out, add_name(out, name));
        return true;
    }
    if (std::holds_alternative<UnaryExpr>(e.node)) {
        const auto& unary = std::get<UnaryExpr>(e.node);
        if (!unary.expr) return false;
        if (unary.op != UnOp::Neg && unary.op != UnOp::Not && unary.op != UnOp::BitNot) return false;
        if (!compile_node(*unary.expr, out, env)) return false;
        emit_u8(out, static_cast<uint8_t>(OpCode::UnOp));
        ValueUnOp un = ValueUnOp::Not;
        if (unary.op == UnOp::Neg) un = ValueUnOp::Neg;
        else if (unary.op == UnOp::BitNot) un = ValueUnOp::BitNot;
        emit_u8(out, static_cast<uint8_t>(un));
        return true;
    }
    if (std::holds_alternative<BinaryExpr>(e.node)) {
        const auto& binary = std::get<BinaryExpr>(e.node);
        if (!binary.left || !binary.right) return false;
        switch (binary.op) {
            case BinOp::Add:
            case BinOp::Sub:
            case BinOp::Mul:
            case BinOp::Div:
            case BinOp::EQ:
            case BinOp::NE:
                return false;
            default:
                break;
        }
        if (!compile_node(*binary.left, out, env)) return false;
        if (!compile_node(*binary.right, out, env)) return false;
        emit_u8(out, static_cast<uint8_t>(OpCode::BinOp));
        emit_u8(out, static_cast<uint8_t>(to_value_bin_op(binary.op)));
        return true;
    }
    return false;
}

} // namespace

bool try_compile_expr(const Expr& e, Chunk& out, const Runtime::Env* env) {
    out.code.clear();
    out.constants.clear();
    out.names.clear();
    if (!compile_node(e, out, env)) {
        out.code.clear();
        out.constants.clear();
        out.names.clear();
        return false;
    }
    emit_u8(out, static_cast<uint8_t>(OpCode::Return));
    return true;
}

namespace {

bool emit_store_name(Chunk& out, const std::string& name, const Runtime::Env* env) {
    if (env && env->useSlots) {
        auto it = env->slotIndex.find(name);
        if (it != env->slotIndex.end() && it->second >= 0) {
            emit_u8(out, static_cast<uint8_t>(OpCode::StoreSlot));
            emit_u16(out, static_cast<uint16_t>(it->second));
            return true;
        }
    }
    emit_u8(out, static_cast<uint8_t>(OpCode::StoreName));
    emit_u16(out, add_name(out, name));
    return true;
}

bool compile_inc_dec(Chunk& out, const std::string& name, bool isInc, const Runtime::Env* env) {
    if (env && env->useSlots) {
        auto it = env->slotIndex.find(name);
        if (it != env->slotIndex.end() && it->second >= 0) {
            emit_u8(out, static_cast<uint8_t>(isInc ? OpCode::IncSlot : OpCode::DecSlot));
            emit_u16(out, static_cast<uint16_t>(it->second));
            return true;
        }
    }
    if (!compile_node(Expr{ExprIdent{name}}, out, env)) return false;
    emit_u8(out, static_cast<uint8_t>(OpCode::LoadConst));
    emit_u16(out, add_constant(out, Value::from_int(1)));
    emit_u8(out, static_cast<uint8_t>(OpCode::BinOp));
    emit_u8(out, static_cast<uint8_t>(isInc ? ValueBinOp::Add : ValueBinOp::Sub));
    return emit_store_name(out, name, env);
}

void patch_i16(Chunk& out, size_t at, int16_t rel) {
    out.code[at] = static_cast<uint8_t>(static_cast<uint16_t>(rel) & 0xff);
    out.code[at + 1] = static_cast<uint8_t>((static_cast<uint16_t>(rel) >> 8) & 0xff);
}

} // namespace

bool try_compile_for_loop(const ForStmt& st, Chunk& out, const Runtime::Env* env) {
    if (!st.cond || !*st.cond || !st.body || !st.step) return false;
    if (st.body->stmts.empty()) return false;

    for (const auto& bs : st.body->stmts) {
        if (!std::holds_alternative<SetStmt>(bs)) return false;
        const auto& set = std::get<SetStmt>(bs);
        if (set.isMember || !set.value) return false;
        Chunk probe;
        if (!compile_node(*set.value, probe, env)) return false;
    }

    if (st.step->stmts.size() != 1) return false;
    const auto& stepStmt = st.step->stmts[0];

    out.code.clear();
    out.constants.clear();
    out.names.clear();

    const size_t loopTop = out.code.size();
    if (!compile_node(**st.cond, out, env)) {
        out.code.clear();
        return false;
    }
    emit_u8(out, static_cast<uint8_t>(OpCode::JumpIfFalse));
    const size_t exitPatch = out.code.size();
    emit_u16(out, 0);

    for (const auto& bs : st.body->stmts) {
        const auto& set = std::get<SetStmt>(bs);
        if (!compile_node(*set.value, out, env)) {
            out.code.clear();
            return false;
        }
        if (!emit_store_name(out, set.varOrField, env)) {
            out.code.clear();
            return false;
        }
    }

    if (std::holds_alternative<ExprStmt>(stepStmt)) {
        const auto& es = std::get<ExprStmt>(stepStmt);
        if (!es.expr) { out.code.clear(); return false; }
        if (std::holds_alternative<PostfixExpr>(es.expr->node)) {
            const auto& pe = std::get<PostfixExpr>(es.expr->node);
            if (!pe.operand || !std::holds_alternative<ExprIdent>(pe.operand->node)) {
                out.code.clear();
                return false;
            }
            if (!compile_inc_dec(out, std::get<ExprIdent>(pe.operand->node).name, pe.isInc, env)) {
                out.code.clear();
                return false;
            }
        } else if (std::holds_alternative<PrefixExpr>(es.expr->node)) {
            const auto& pe = std::get<PrefixExpr>(es.expr->node);
            if (!pe.operand || !std::holds_alternative<ExprIdent>(pe.operand->node)) {
                out.code.clear();
                return false;
            }
            if (!compile_inc_dec(out, std::get<ExprIdent>(pe.operand->node).name, pe.isInc, env)) {
                out.code.clear();
                return false;
            }
        } else {
            out.code.clear();
            return false;
        }
    } else if (std::holds_alternative<SetStmt>(stepStmt)) {
        const auto& set = std::get<SetStmt>(stepStmt);
        if (set.isMember || !set.value) { out.code.clear(); return false; }
        if (!compile_node(*set.value, out, env) || !emit_store_name(out, set.varOrField, env)) {
            out.code.clear();
            return false;
        }
    } else {
        out.code.clear();
        return false;
    }

    emit_u8(out, static_cast<uint8_t>(OpCode::Jump));
    const size_t backPatch = out.code.size();
    emit_u16(out, 0);

    const size_t exitIp = out.code.size();
    const int64_t exitRel = static_cast<int64_t>(exitIp) - static_cast<int64_t>(exitPatch + 2);
    if (exitRel < INT16_MIN || exitRel > INT16_MAX) { out.code.clear(); return false; }
    patch_i16(out, exitPatch, static_cast<int16_t>(exitRel));

    const int64_t backRel = static_cast<int64_t>(loopTop) - static_cast<int64_t>(backPatch + 2);
    if (backRel < INT16_MIN || backRel > INT16_MAX) { out.code.clear(); return false; }
    patch_i16(out, backPatch, static_cast<int16_t>(backRel));

    emit_u8(out, static_cast<uint8_t>(OpCode::Return));
    return true;
}

Value run_chunk(const Chunk& chunk, Runtime& rt, Runtime::Env& env) {
    std::vector<Value> stack;
    stack.reserve(16);
    size_t ip = 0;
    std::vector<uint8_t> dirty;
    if (env.useSlots) dirty.assign(env.slots.size(), 0);

    auto mark_dirty = [&](uint16_t idx) {
        if (!env.useSlots) return;
        if (idx >= dirty.size()) dirty.resize(static_cast<size_t>(idx) + 1, 0);
        dirty[idx] = 1;
    };

    auto pop = [&]() -> Value {
        if (stack.empty()) throw std::runtime_error("bytecode: stack underflow");
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    };

    auto sync_dirty_slots = [&]() {
        if (!env.useSlots) return;
        const size_t n = std::min({dirty.size(), env.slots.size(), env.slotNames.size()});
        for (size_t i = 0; i < n; ++i) {
            if (!dirty[i]) continue;
            const std::string& name = env.slotNames[i];
            env.vars[name] = env.slots[i];
            if (rt.globalNames_.count(name)) rt.globalVars_[name] = env.slots[i];
        }
    };

    while (ip < chunk.code.size()) {
        const auto op = static_cast<OpCode>(chunk.code[ip++]);
        switch (op) {
            case OpCode::LoadConst: {
                const uint16_t idx = read_u16(chunk, ip);
                if (idx >= chunk.constants.size()) throw std::runtime_error("bytecode: bad const");
                stack.push_back(chunk.constants[idx]);
                break;
            }
            case OpCode::LoadSlot: {
                const uint16_t idx = read_u16(chunk, ip);
                if (!env.useSlots || idx >= env.slots.size()) {
                    stack.push_back(Value::null_value());
                } else {
                    stack.push_back(env.slots[idx]);
                }
                break;
            }
            case OpCode::StoreSlot: {
                const uint16_t idx = read_u16(chunk, ip);
                Value v = pop();
                if (env.useSlots) {
                    if (idx >= env.slots.size()) env.slots.resize(static_cast<size_t>(idx) + 1);
                    env.slots[idx] = std::move(v);
                    mark_dirty(idx);
                }
                break;
            }
            case OpCode::IncSlot:
            case OpCode::DecSlot: {
                const uint16_t idx = read_u16(chunk, ip);
                if (env.useSlots && idx < env.slots.size()) {
                    Value& slot = env.slots[idx];
                    if (slot.kind == ValueKind::Int) {
                        if (op == OpCode::IncSlot) ++slot.i;
                        else --slot.i;
                    } else {
                        const int64_t cur = value_as_int(slot);
                        slot = Value::from_int(cur + (op == OpCode::IncSlot ? 1 : -1));
                    }
                    mark_dirty(idx);
                }
                break;
            }
            case OpCode::LoadName: {
                const uint16_t idx = read_u16(chunk, ip);
                if (idx >= chunk.names.size()) throw std::runtime_error("bytecode: bad name");
                const std::string& name = chunk.names[idx];
                const bool found =
                    (env.useSlots && env.slotIndex.count(name) > 0) ||
                    env.vars.count(name) > 0 ||
                    rt.globalVars_.count(name) > 0;
                if (found) {
                    stack.push_back(rt.env_get(env, name));
                } else {
                    stack.push_back(Value::from_string(name));
                }
                break;
            }
            case OpCode::StoreName: {
                const uint16_t idx = read_u16(chunk, ip);
                if (idx >= chunk.names.size()) throw std::runtime_error("bytecode: bad name");
                rt.env_set(env, chunk.names[idx], pop());
                break;
            }
            case OpCode::BinOp: {
                if (ip >= chunk.code.size()) throw std::runtime_error("bytecode: truncated BinOp");
                const auto bin = static_cast<ValueBinOp>(chunk.code[ip++]);
                Value right = pop();
                Value left = pop();
                if (left.kind == ValueKind::Int && right.kind == ValueKind::Int) {
                    const int64_t a = left.i, b = right.i;
                    switch (bin) {
                        case ValueBinOp::Add: stack.push_back(Value::from_int(a + b)); break;
                        case ValueBinOp::Sub: stack.push_back(Value::from_int(a - b)); break;
                        case ValueBinOp::Mul: stack.push_back(Value::from_int(a * b)); break;
                        case ValueBinOp::Div:
                            if (b == 0) throw std::runtime_error("Division by zero");
                            stack.push_back(Value::from_int(a / b));
                            break;
                        case ValueBinOp::Mod:
                            if (b == 0) throw std::runtime_error("Modulo by zero");
                            stack.push_back(Value::from_int(a % b));
                            break;
                        case ValueBinOp::Eq: stack.push_back(Value::from_bool(a == b)); break;
                        case ValueBinOp::Ne: stack.push_back(Value::from_bool(a != b)); break;
                        case ValueBinOp::Lt: stack.push_back(Value::from_bool(a < b)); break;
                        case ValueBinOp::Le: stack.push_back(Value::from_bool(a <= b)); break;
                        case ValueBinOp::Gt: stack.push_back(Value::from_bool(a > b)); break;
                        case ValueBinOp::Ge: stack.push_back(Value::from_bool(a >= b)); break;
                        default: stack.push_back(apply_binary(bin, left, right)); break;
                    }
                } else {
                    stack.push_back(apply_binary(bin, left, right));
                }
                break;
            }
            case OpCode::UnOp: {
                if (ip >= chunk.code.size()) throw std::runtime_error("bytecode: truncated UnOp");
                const auto un = static_cast<ValueUnOp>(chunk.code[ip++]);
                stack.push_back(apply_unary(un, pop()));
                break;
            }
            case OpCode::Jump: {
                const int16_t rel = read_i16(chunk, ip);
                ip = static_cast<size_t>(static_cast<int64_t>(ip) + rel);
                break;
            }
            case OpCode::JumpIfFalse: {
                const int16_t rel = read_i16(chunk, ip);
                Value cond = pop();
                const bool truthy = (cond.kind == ValueKind::Bool) ? cond.b
                    : (cond.kind == ValueKind::Int) ? (cond.i != 0)
                    : is_truthy(cond);
                if (!truthy) {
                    ip = static_cast<size_t>(static_cast<int64_t>(ip) + rel);
                }
                break;
            }
            case OpCode::JumpIfTrue: {
                const int16_t rel = read_i16(chunk, ip);
                Value cond = pop();
                const bool truthy = (cond.kind == ValueKind::Bool) ? cond.b
                    : (cond.kind == ValueKind::Int) ? (cond.i != 0)
                    : is_truthy(cond);
                if (truthy) {
                    ip = static_cast<size_t>(static_cast<int64_t>(ip) + rel);
                }
                break;
            }
            case OpCode::Pop:
                (void)pop();
                break;
            case OpCode::Dup: {
                if (stack.empty()) throw std::runtime_error("bytecode: stack underflow");
                stack.push_back(stack.back());
                break;
            }
            case OpCode::Return: {
                Value result = stack.empty() ? Value::null_value() : pop();
                sync_dirty_slots();
                return result;
            }
        }
    }
    sync_dirty_slots();
    return stack.empty() ? Value::null_value() : pop();
}

} // namespace erelang
