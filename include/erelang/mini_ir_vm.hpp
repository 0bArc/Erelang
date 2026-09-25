#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace erelang {
namespace mini_ir {

struct RunResult {
    bool ok = false;
    bool did_print = false;
    std::int64_t last_print = 0;
    std::string last_print_str;
    std::int64_t value = 0;
    std::string error;
};

// Text format: "# mini-ir v1" module with .entry / .func blocks.
[[nodiscard]] RunResult run_text(const std::string& text);
[[nodiscard]] RunResult run_file(const std::string& path);

// Encode for Elan: "1|did|print|rest..." or "0|error"
[[nodiscard]] std::string encode_result(const RunResult& r);

} // namespace mini_ir
} // namespace erelang
