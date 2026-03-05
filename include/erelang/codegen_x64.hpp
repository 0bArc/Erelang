#pragma once

#include <string>

#include "erelang/ir.hpp"

namespace erelang {

class X64Codegen {
public:
    std::string emit_nasm_win64(const IRModule& module) const;
    std::string emit_gas_win64_demo(const IRModule& module) const;
};

} // namespace erelang
