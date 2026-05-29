#include "erelang/runtime.hpp"

namespace erelang {

std::string __erelang_builtin_threads_dispatch(Runtime*, const std::string&, const std::vector<std::string>&) {
    return {};
}

std::string __erelang_builtin_monitor_dispatch(Runtime*, const std::string&, const std::vector<std::string>&) {
    return {};
}

} // namespace erelang
