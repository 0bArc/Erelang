// Runtime hookup of builtin modules; see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace erelang::modules {

namespace {

struct DispatchEntry {
    std::string moduleId;
    DispatchFn fn;
};

struct HandleEntry {
    std::string prefix;
    HandleFn fn;
    int priority;
};

// Function-local statics: registration runs during static initialization of other objects.
std::vector<DispatchEntry>& dispatchers() {
    static std::vector<DispatchEntry> d;
    return d;
}

std::vector<HandleEntry>& handlers() {
    static std::vector<HandleEntry> h;
    return h;
}

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

}  // namespace

bool register_dispatch(const char* moduleId, DispatchFn fn) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    dispatchers().push_back({moduleId, fn});
    return true;
}

bool register_handle(const char* prefix, HandleFn fn, int priority) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& h = handlers();
    h.push_back({prefix, fn, priority});
    std::stable_sort(h.begin(), h.end(), [](const HandleEntry& a, const HandleEntry& b) {
        return a.priority < b.priority;
    });
    return true;
}

std::string dispatch(Runtime* rt, const std::vector<std::string>& moduleIds, const std::string& name,
                     const std::vector<std::string>& argv) {
    const auto& d = dispatchers();
    for (const auto& id : moduleIds) {
        for (const auto& e : d) {
            if (e.moduleId != id) continue;
            std::string r = e.fn(rt, name, argv);
            if (!r.empty()) return r;
        }
    }
    return {};
}

bool has_handle_prefix(std::string_view prefix) {
    for (const auto& e : handlers()) {
        if (e.prefix == prefix) return true;
    }
    return false;
}

bool dispatch_handle(Runtime* rt, std::string_view prefix, int id, const std::string& method,
                     const std::vector<std::string>& args, std::string& out) {
    for (const auto& e : handlers()) {
        if (e.prefix != prefix) continue;
        if (e.fn(rt, id, method, args, out)) return true;
    }
    return false;
}

const char* implementation_of(std::string_view moduleId) {
    static const std::pair<const char*, const char*> kImpl[] = {
        {"net", "net"}, {"ws", "ws"}, {"tcp", "net"}, {"regex", "regex"}, {"ai", "ai"}, {"crypto", "crypto"},
        {"perf", "perf"}, {"binary", "binary"}, {"threads", "threads"}, {"monitor", "monitor"},
        {"math", "math"}, {"system", "system"}, {"process", "system"}, {"data", "data"}, {"perm", "perm"},
        {"json", "json"},
    };
    for (const auto& [mod, impl] : kImpl) {
        if (moduleId == mod) return impl;
    }
    return nullptr;
}

}  // namespace erelang::modules
