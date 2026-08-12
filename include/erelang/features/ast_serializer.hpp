#pragma once
#include "erelang/parser.hpp"
#include <cstdint>
#include <vector>

namespace erelang::features {

// Serialize a fully resolved Program to a compact binary blob.
// The caller should have already run lex+parse+typecheck+optimize.
[[nodiscard]] std::vector<uint8_t> serialize_program(const Program& prog);

// Deserialize a Program from a binary blob produced by serialize_program.
// Returns nullopt on corruption.
[[nodiscard]] std::optional<Program> deserialize_program(const uint8_t* data, size_t size);

} // namespace erelang::features
