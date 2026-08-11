#pragma once
#include <string>
#include <vector>
namespace erelang {
struct FFIResult { std::string value; bool ok{true}; };
inline FFIResult ffi_call(const std::string& symbol, const std::vector<std::string>& args) {
    (void)symbol;
    (void)args;
    return {"", false};
}
}
