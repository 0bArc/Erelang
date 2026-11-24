#include "erelang/runtime.hpp"

namespace erelang {

std::string __erelang_builtin_threads_dispatch(Runtime*, const std::string&, const std::vector<std::string>&) {
    return "error:experimental_disabled";
}

std::string __erelang_builtin_monitor_dispatch(Runtime*, const std::string&, const std::vector<std::string>&) {
    return "error:experimental_disabled";
}

} // namespace erelang

// Experimental modules are off: keep the anchors so a full link still resolves.
#include "erelang/module_registry.hpp"
namespace {
const bool kThreads = erelang::modules::register_dispatch("threads",
    [](erelang::Runtime* rt, const std::string& n, const std::vector<std::string>& a) {
        return erelang::__erelang_builtin_threads_dispatch(rt, n, a);
    });
const bool kMonitor = erelang::modules::register_dispatch("monitor",
    [](erelang::Runtime* rt, const std::string& n, const std::vector<std::string>& a) {
        return erelang::__erelang_builtin_monitor_dispatch(rt, n, a);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(threads)
ERELANG_MODULE_ANCHOR(monitor)
