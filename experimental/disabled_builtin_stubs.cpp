#include "erelang/runtime.hpp"

namespace erelang {

std::string __erelang_builtin_threads_dispatch(Runtime*, const std::string&, const std::vector<std::string>&) {
    return "error:experimental_disabled";
}

std::string __erelang_builtin_monitor_dispatch(Runtime*, const std::string&, const std::vector<std::string>&) {
    return "error:experimental_disabled";
}

} // namespace erelang
