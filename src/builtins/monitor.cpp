#include "erelang/runtime.hpp"
#include "erelang/policy.hpp"
#include "erelang/runtime_internals.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <optional>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

// Monitor subsystem goals (phase 1):
// - Allow scripts to register processes (by exe path or name) to watch.
// - Periodically sample file modification time + size + a lightweight hash (fnv1a) to detect tampering.
// - Provide query functions to fetch current state and last change reason.
// - Provide ability to stop monitoring a given handle.
// - Tamper protection scaffold: when monitoring is active during compilation (future hook), we can lock file attributes (placeholder now).
// Security considerations:
// - Access gated by policy: builtins monitor_add/monitor_info/monitor_list/monitor_remove/monitor_last_change
// - No elevated operations performed; only read access and hashing.
// - Future phases: code signing checks, module hash chain, in-memory patch detection.

namespace {
struct MonRecord {
    std::string key;              // user provided identifier
    std::filesystem::path path;   // resolved path
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mtx;
    uint64_t lastWriteTime = 0;   // coarse
    uint64_t lastSize = 0;
    std::string lastHash;         // fnv1a hex
    std::string lastChange;       // human text of last change
    uint32_t intervalMs = 2000;   // default poll interval
};

std::mutex g_monitorsMutex;
std::unordered_map<int, std::unique_ptr<MonRecord>> g_monitors; // id -> record
int g_nextMonId = 1;

static uint64_t file_time_to_uint64(const std::filesystem::file_time_type& ft) {
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(ft - std::filesystem::file_time_type::clock::now()
        + system_clock::now());
    return (uint64_t)duration_cast<milliseconds>(sctp.time_since_epoch()).count();
}

static std::string hash_fnv1a(const std::string& data) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ULL; }
    std::ostringstream ss; ss << std::hex;
    ss.width(16); ss.fill('0'); ss << h;
    return ss.str();
}

static std::string hash_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream buf; buf << in.rdbuf();
    return hash_fnv1a(buf.str());
}

void run_monitor_loop(MonRecord* rec) {
    rec->running = true;
    while (rec->running) {
        std::error_code ec;
        auto exists = std::filesystem::exists(rec->path, ec);
        uint64_t size = 0; uint64_t writeMs = 0; std::string h;
        if (exists && !ec) {
            try {
                size = (uint64_t)std::filesystem::file_size(rec->path);
                writeMs = file_time_to_uint64(std::filesystem::last_write_time(rec->path));
                h = hash_file(rec->path);
            } catch (...) {}
        }
        bool changed=false; std::string reason;
        {
            std::lock_guard<std::mutex> lg(rec->mtx);
            if (writeMs != rec->lastWriteTime) { changed=true; reason += "mtime "; }
            if (size != rec->lastSize) { changed=true; reason += "size "; }
            if (!h.empty() && h != rec->lastHash) { changed=true; reason += "hash "; }
            if (changed) {
                rec->lastWriteTime = writeMs;
                rec->lastSize = size;
                if (!h.empty()) rec->lastHash = h;
                if (!reason.empty()) rec->lastChange = reason;
            }
        }
        for (uint32_t i=0;i<rec->intervalMs/100;i++) {
            if (!rec->running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
} // anon

namespace erelang {

std::string __erelang_builtin_monitor_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv) {
    // Gate by policy: treat all monitor_* as a single group; rely on per-builtin policy later if desired
    if (!PolicyManager::instance().is_allowed(name)) return {};
    if (name == "monitor_add") {
        if (argv.empty()) return {};
        std::filesystem::path p = argv[0];
        std::error_code ec; auto abs = std::filesystem::absolute(p, ec);
        auto rec = std::make_unique<MonRecord>();
        rec->path = abs; rec->key = argv.size()>1? argv[1] : abs.filename().string();
        int id;
        {
            std::lock_guard<std::mutex> lg(g_monitorsMutex);
            id = g_nextMonId++;
            g_monitors[id] = std::move(rec);
        }
        MonRecord* rptr;
        {
            std::lock_guard<std::mutex> lg(g_monitorsMutex);
            rptr = g_monitors[id].get();
        }
        rptr->worker = std::thread(run_monitor_loop, rptr);
        return std::string("monitor:") + std::to_string(id);
    }
    if (name == "monitor_remove") {
        if (argv.empty()) return {};
        int id = std::atoi(argv[0].c_str());
        std::unique_ptr<MonRecord> rec;
        {
            std::lock_guard<std::mutex> lg(g_monitorsMutex);
            auto it = g_monitors.find(id);
            if (it != g_monitors.end()) { rec = std::move(it->second); g_monitors.erase(it); }
        }
        if (rec) { rec->running=false; if (rec->worker.joinable()) rec->worker.join(); }
        return {};
    }
    if (name == "monitor_list") {
        std::lock_guard<std::mutex> lg(g_monitorsMutex);
        int listId = g_nextListId++;
        g_lists[listId] = {};
        for (auto& kv : g_monitors) {
            g_lists[listId].push_back(std::to_string(kv.first));
        }
        return std::string("list:") + std::to_string(listId);
    }
    if (name == "monitor_info") {
        if (argv.empty()) return {};
        int id = std::atoi(argv[0].c_str());
        std::lock_guard<std::mutex> lg(g_monitorsMutex);
        auto it = g_monitors.find(id);
        if (it == g_monitors.end()) return {};
        MonRecord* r = it->second.get();
        std::lock_guard<std::mutex> rg(r->mtx);
        std::ostringstream ss;
        ss << "path=" << r->path.string() << "\n";
        ss << "last_change=" << r->lastChange << "\n";
        ss << "mtime=" << r->lastWriteTime << "\n";
        ss << "size=" << r->lastSize << "\n";
        ss << "hash=" << r->lastHash;
        return ss.str();
    }
    if (name == "monitor_last_change") {
        if (argv.empty()) return {};
        int id = std::atoi(argv[0].c_str());
        std::lock_guard<std::mutex> lg(g_monitorsMutex);
        auto it = g_monitors.find(id);
        if (it == g_monitors.end()) return {};
        MonRecord* r = it->second.get();
        std::lock_guard<std::mutex> rg(r->mtx);
        return r->lastChange;
    }
    if (name == "monitor_set_interval") {
        if (argv.size()<2) return {};
        int id = std::atoi(argv[0].c_str());
        uint32_t ms = (uint32_t)std::stoul(argv[1]);
        std::lock_guard<std::mutex> lg(g_monitorsMutex);
        auto it = g_monitors.find(id);
        if (it == g_monitors.end()) return {};
        it->second->intervalMs = ms < 200 ? 200 : ms; // enforce minimum 200ms
        return {};
    }
    return {};
}

} // namespace erelang

// NOTE: Cleanup threads on static destruction (optional). We'll rely on process exit to tear down.
