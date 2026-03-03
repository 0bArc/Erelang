#pragma once
#include <string>
#include <vector>
namespace erelang {
struct FFIResult { std::string value; bool ok{true}; };
FFIResult ffi_call(const std::string& symbol, const std::vector<std::string>& args);
}
