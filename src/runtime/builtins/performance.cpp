// Performance profiling and memory API (runtime only — compiler directives are parser-level)
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <mutex>

namespace erelang {

namespace {
    struct ProfileEntry {
        std::chrono::steady_clock::time_point begin;
        std::chrono::steady_clock::time_point end;
        unsigned long long cumulativeMs = 0;
        unsigned long long callCount = 0;
        bool running = false;
    };
    std::unordered_map<std::string, ProfileEntry> g_profiles;
    std::mutex g_profileMutex;

    // Memory stubs — in a real implementation these would query the OS
    unsigned long long g_peakBytes = 0;
}

static std::string performance_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };

    // Profiling regions
    if (name == "perf.profile.begin") {
        std::string label = argS(0);
        if (label.empty()) return "false";
        std::lock_guard<std::mutex> lg(g_profileMutex);
        auto& entry = g_profiles[label];
        entry.begin = std::chrono::steady_clock::now();
        entry.running = true;
        return "true";
    }
    if (name == "perf.profile.end") {
        std::string label = argS(0);
        if (label.empty()) return "0";
        std::lock_guard<std::mutex> lg(g_profileMutex);
        auto it = g_profiles.find(label);
        if (it == g_profiles.end() || !it->second.running) return "0";
        auto& entry = it->second;
        entry.end = std::chrono::steady_clock::now();
        entry.running = false;
        using namespace std::chrono;
        auto elapsed = duration_cast<milliseconds>(entry.end - entry.begin).count();
        entry.cumulativeMs += static_cast<unsigned long long>(elapsed);
        entry.callCount++;
        return std::to_string(elapsed);
    }
    if (name == "perf.profile.duration") {
        std::string label = argS(0);
        std::lock_guard<std::mutex> lg(g_profileMutex);
        auto it = g_profiles.find(label);
        if (it == g_profiles.end()) return "0";
        return std::to_string(it->second.cumulativeMs);
    }
    if (name == "perf.profile.calls") {
        std::string label = argS(0);
        std::lock_guard<std::mutex> lg(g_profileMutex);
        auto it = g_profiles.find(label);
        if (it == g_profiles.end()) return "0";
        return std::to_string(it->second.callCount);
    }
    if (name == "perf.profile.report") {
        std::lock_guard<std::mutex> lg(g_profileMutex);
        std::ostringstream ss;
        for (const auto& kv : g_profiles) {
            ss << kv.first << ": " << kv.second.cumulativeMs << "ms ("
               << kv.second.callCount << " calls)\n";
        }
        return ss.str();
    }

    // Memory API
    if (name == "perf.mem.usage") {
        // Stub — would query OS for RSS
        return "Not implemented yet";
    }
    if (name == "perf.mem.peak") {
        // Stub — would query OS for peak RSS
        return std::to_string(g_peakBytes);
    }

    // GC controls (stubs — the runtime doesn't have a real GC)
    if (name == "perf.gc.collect") {
        // Force cleanup of container state
        return "Not implemented yet";
    }
    if (name == "perf.gc.threshold") {
        // Stub
        return argS(0);
    }
    if (name == "perf.gc.pause") {
        // Stub
        return "Not implemented yet";
    }
    if (name == "perf.gc.resume") {
        // Stub
        return "Not implemented yet";
    }

    return {};
}

std::string __erelang_builtin_performance_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return performance_dispatch(name, argv);
}

} // namespace erelang
