// Simple permissions/capabilities registry (coarse)
#include "erelang/runtime.hpp"
#include <unordered_set>
#include <string>

namespace erelang {
static std::unordered_set<std::string> g_perms_granted; // in-memory only

static std::string perm_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
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
