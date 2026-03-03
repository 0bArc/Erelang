#include "erelang/ffi.hpp"
namespace erelang {
FFIResult ffi_call(const std::string& symbol, const std::vector<std::string>& args) {
    (void)symbol; (void)args; return {"", false};
}
}
