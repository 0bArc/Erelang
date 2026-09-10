// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "erelang/runtime.hpp"
#include "erelang/value.hpp"

namespace erelang {

struct Expr;

enum class OpCode : uint8_t {
    LoadConst = 0,
    LoadSlot,
    StoreSlot,
    LoadName,
    StoreName,
    BinOp,
    UnOp,
    Jump,
    JumpIfFalse,
    JumpIfTrue,
    Pop,
    Dup,
    Return,
    IncSlot,
    DecSlot
};

struct Chunk {
    std::vector<uint8_t> code;
    std::vector<Value> constants;
    std::vector<std::string> names;
};

[[nodiscard]] bool try_compile_expr(const Expr& e, Chunk& out,
                                    const Runtime::Env* env = nullptr);

[[nodiscard]] bool try_compile_for_loop(const ForStmt& st, Chunk& out,
                                        const Runtime::Env* env);

[[nodiscard]] Value run_chunk(const Chunk& chunk, Runtime& rt, Runtime::Env& env);

} // namespace erelang
