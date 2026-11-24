// Simple permissions/capabilities registry (coarse)
#include "erelang/runtime.hpp"
#include <mutex>
#include <set>
#include <string>

namespace erelang {
// In memory only. Ordered so perm.list() is stable.
static std::set<std::string> g_perms_granted;
static std::mutex g_permMutex;

static std::string perm_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    std::lock_guard<std::mutex> lock(g_permMutex);
    if (name == "perm_grant") { g_perms_granted.insert(argS(0)); return {}; }
    if (name == "perm_revoke") { g_perms_granted.erase(argS(0)); return {}; }
    if (name == "perm_has") { return g_perms_granted.count(argS(0))?"true":"false"; }
    if (name == "perm_list") {
        std::string out; bool first=true; for(auto &p: g_perms_granted){ if(!first) out+=","; first=false; out+=p; } return out;
    }
    return {};
}

std::string __erelang_builtin_perm_dispatch(const std::string& name, const std::vector<std::string>& argv) { return perm_dispatch(name, argv); }

} // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("perm",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_perm_dispatch(name, argv);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(perm)
