#pragma once

#include <optional>
#include <string>
#include <vector>

#include "erelang/parser.hpp"

namespace erelang {

struct IRInstruction {
    std::string opcode;
    std::vector<std::string> operands;
};

struct IRFunction {
    std::string name;
    std::vector<std::string> params;
    std::vector<IRInstruction> instructions;
};

struct IRModule {
    std::vector<IRFunction> functions;
    std::optional<std::string> entry;
};

class IRBuilder {
public:
    IRModule build(const Program& program) const;
};

std::string ir_to_text(const IRModule& module);

} // namespace erelang
