#pragma once
#include <memory>
#include <vector>
#include <string>
#include <variant>
#include <optional>

namespace erelang {
// Lightweight AST wrapper layer (future richer nodes can migrate here)
struct AstNodeBase { virtual ~AstNodeBase() = default; };
using AstNode = std::shared_ptr<AstNodeBase>;

// Placeholder for future bytecode emission handle
struct BytecodeChunk { std::vector<uint8_t> code; };

AstNode ast_wrap_program(); // stub
}
