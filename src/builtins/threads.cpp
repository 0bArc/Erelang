// Production-grade thread utilities with:
// - Smart pointer ownership
// - Internal manager API (threadmgr)
// - Error codes instead of silent failures (error:<reason>)
// - Optional detach (policy guarded via builtin name "thread_detach")
// - Join timeout, list, count, yield, wait_all, gc (purge detached finished)
// - Extended GC (gc_all) and state querying (thread_state)
// - No static global destructor; explicit GC path
// - Optional cross-platform thread naming (define ERELANG_ENABLE_THREAD_NAMING)
//
// Thread lifecycle summary:
//   Attached:   Running -> (Joining) -> Done -> Joined -> (gc_all removes)
//   Detached:   DetachedRunning -> DetachedDone -> (gc / gc_all removes)
// States:
//   Running, DetachedRunning: executing user action
//   Joining: join in progress (removal rejected)
//   Done: finished work, not yet formally joined
//   DetachedDone: finished detached thread awaiting GC
//   Joined: thread joined, record kept until gc_all/remove
//
// Builtins:
//   thread_run <action> [detach]            -> thread:ID or error:*
//   thread_join <thread:ID>
//   thread_join_timeout <thread:ID> <ms>
//   thread_done <thread:ID>
//   thread_list -> list:ID
//   thread_wait_all
//   thread_count
//   thread_yield
//   thread_gc (detached finished only)
//   thread_gc_all (detached finished + joined)
//   thread_remove <thread:ID> [force|kill]
//   thread_state <thread:ID>  -> state:<StateName>
//
// Error semantics additions:
//   thread_state returns error:invalid_handle if not found.
//   Removal conditions enforced (see implementation).

#include "erelang/runtime.hpp"
#include "erelang/policy.hpp"
#include "erelang/runtime_internals.hpp"
#include <thread>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <sstream>
#include <chrono>
#include <condition_variable>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#if defined(ERELANG_ENABLE_THREAD_NAMING) && (defined(__linux__) || defined(__APPLE__))
#include <pthread.h>
#endif
#endif

namespace erelang {

enum class ThreadState { Running, DetachedRunning, Done, DetachedDone, Joining, Joined };

struct ThreadInfo {
    std::thread th;
    std::atomic<ThreadState> state{ThreadState::Running};
    std::string action;
    int id = 0;
    bool detached = false;
    std::mutex m;                // protects cv waiters
    std::condition_variable cv;  // signalled on completion
};

static std::unordered_map<int, std::unique_ptr<ThreadInfo>> g_threads; 
static std::atomic<int> g_nextThread{1};
static std::mutex g_threadMutex;

namespace threadmgr {
    static inline std::string handle(int id) noexcept { return "thread:" + std::to_string(id); }
    static inline bool parse_handle(const std::string& h, int& out) noexcept {
        if (h.rfind("thread:",0)==0) {
            try {
                auto tail = h.substr(7);
                if (tail.empty()) return false;
                size_t pos = 0;
                int v = std::stoi(tail,&pos);
                if (pos != tail.size()) return false;
                out = v; return true;
            } catch (...) { return false; }
        }
        return false;
    }

    // Remove finished detached threads (they cannot be joined)
    void gc_detached() noexcept {
        std::lock_guard<std::mutex> lg(g_threadMutex);
        for (auto it = g_threads.begin(); it != g_threads.end(); ) {
            ThreadInfo* ti = it->second.get();
            if (ti && ti->detached && (ti->state.load()==ThreadState::DetachedDone)) it = g_threads.erase(it); else ++it;
        }
    }

    void gc_all() noexcept {
        std::lock_guard<std::mutex> lg(g_threadMutex);
        for (auto it = g_threads.begin(); it != g_threads.end(); ) {
            ThreadState s = it->second->state.load();
            if (s == ThreadState::DetachedDone || s == ThreadState::Joined) it = g_threads.erase(it); else ++it;
        }
    }

    [[nodiscard]] int create(Runtime* rt, const Program* prog, const std::string& action, bool detachRequest, std::string& err) {
        err.clear();
        if (!PolicyManager::instance().is_allowed("thread_run")) { err = "policy"; return -1; }
        if (action.empty()) { err = "action_empty"; return -1; }
        if (!prog) { err = "no_program"; return -1; }
        bool found=false; for (auto & a : prog->actions) if (a.name == action) { found=true; break; }
        if (!found) { err = "action_not_found"; return -1; }
        {
            std::scoped_lock lg(g_threadMutex);
            if ((int)g_threads.size() >= PolicyManager::instance().policy().maxThreads) { err = "max_threads"; return -1; }
        }
    if (detachRequest && !PolicyManager::instance().is_allowed("thread_detach")) { err = "detach_not_allowed"; return -1; }
        int id = g_nextThread++;
        auto ti = std::make_unique<ThreadInfo>();
        ti->action = action; ti->id = id; ti->detached = detachRequest;
        ti->state.store(detachRequest ? ThreadState::DetachedRunning : ThreadState::Running);
        ThreadInfo* raw = ti.get();
        try {
            raw->th = std::thread([rt, prog, raw]{
                try {
#ifdef _WIN32
                    // Thread naming disabled on MinGW to avoid SetThreadDescription symbol issues.
#endif // _WIN32
                    if (rt && prog) {
                        // Copy of program action name already stored; assuming Runtime outlives threads.
                        rt->run_single_action(*prog, raw->action);
                    }
                } catch (...) { /* swallow */ }
                // Transition state to finished variant
                auto prev = raw->state.load();
                if (prev == ThreadState::Running) raw->state.store(ThreadState::Done);
                else if (prev == ThreadState::DetachedRunning) raw->state.store(ThreadState::DetachedDone);
                else if (prev == ThreadState::Joining) raw->state.store(ThreadState::Done); // joiner will flip to Joined
                {
                    std::scoped_lock<std::mutex> lk(raw->m);
                    raw->cv.notify_all();
                }
            });
        } catch (...) {
            err = "spawn_failed"; return -1; }
        if (detachRequest) {
            try { raw->th.detach(); } catch (...) { err = "detach_failed"; return -1; }
        }
        {
            std::scoped_lock lg(g_threadMutex);
            g_threads[id] = std::move(ti);
        }
        return id;
    }

    [[nodiscard]] bool join(int id, std::string& err) {
        err.clear();
        std::unique_lock<std::mutex> lk(g_threadMutex);
        auto it = g_threads.find(id); if (it==g_threads.end()) { err = "invalid_handle"; return false; }
        ThreadInfo* ti = it->second.get();
        if (ti->detached) { err = "detached"; return false; }
        // Mark joining to block remove
        auto st = ti->state.load();
        while (st == ThreadState::Running) {
            if (ti->state.compare_exchange_weak(st, ThreadState::Joining)) break; // now joining
        }
        if (ti->state.load() == ThreadState::Joining) {
            std::thread local;
            if (ti->th.joinable()) local = std::move(ti->th);
            lk.unlock();
            // Wait for worker completion
            {
                std::unique_lock<std::mutex> tlk(ti->m);
                ti->cv.wait(tlk, [&]{ auto s=ti->state.load(); return s==ThreadState::Done || s==ThreadState::DetachedDone || s==ThreadState::Joined; });
            }
            if (local.joinable()) { try { local.join(); } catch (...) { err = "join_exception"; } }
            lk.lock();
            // Finalize state
            ti->state.store(ThreadState::Joined);
        } else if (st==ThreadState::Done || st==ThreadState::Joined) {
            // Already finished; just ensure joined if still joinable
            std::thread local;
            if (ti->th.joinable()) { local = std::move(ti->th); }
            lk.unlock(); if (local.joinable()) { try { local.join(); } catch (...) { err="join_exception"; } } lk.lock();
            ti->state.store(ThreadState::Joined);
        } else if (st==ThreadState::DetachedRunning || st==ThreadState::DetachedDone) {
            err = "detached"; return false; }
        // retain record until explicit GC or join removal so subsequent thread_done still returns true
        return err.empty();
    }

    [[nodiscard]] bool join_timeout(int id, uint64_t ms, std::string& err) {
        err.clear();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        ThreadInfo* ti = nullptr;
        std::thread local;
        {
            std::lock_guard<std::mutex> lg(g_threadMutex);
            auto it = g_threads.find(id); if (it==g_threads.end()) { err="invalid_handle"; return false; }
            ti = it->second.get();
            if (ti->detached) { err="detached"; return false; }
            auto st = ti->state.load();
            if (st == ThreadState::Running) {
                ti->state.store(ThreadState::Joining);
            }
            else if (st == ThreadState::Joining) {
                // Another join in progress
                err = "joining"; return false;
            }
            else if (st == ThreadState::DetachedRunning || st == ThreadState::DetachedDone) { err="detached"; return false; }
            else if (st == ThreadState::Joined) {
                // already joined
                return true;
            }
            else if (st == ThreadState::Done) {
                // We can join immediately
                if (ti->th.joinable()) local = std::move(ti->th);
            }
        }
        if (!local.joinable()) {
            // Wait until done or timeout
            std::unique_lock<std::mutex> ulk(ti->m);
            if (!ti->cv.wait_until(ulk, deadline, [&]{ auto s=ti->state.load(); return s==ThreadState::Done || s==ThreadState::DetachedDone; })) {
                // timeout
                // revert joining state if still joining
                auto st = ti->state.load();
                if (st == ThreadState::Joining) ti->state.store(ThreadState::Running);
                err = "timeout"; return false;
            }
            // finished now
            if (ti->th.joinable()) local = std::move(ti->th);
        }
        if (local.joinable()) { try { local.join(); } catch (...) { err="join_exception"; } }
        ti->state.store(ThreadState::Joined);
        return err.empty();
    }

    bool is_done(int id, bool& exists, bool& detached) noexcept {
        std::scoped_lock lg(g_threadMutex);
        auto it = g_threads.find(id); if (it==g_threads.end()) { exists=false; detached=false; return false; }
        exists = true; detached = it->second->detached; auto st = it->second->state.load();
        return st==ThreadState::Done || st==ThreadState::DetachedDone || st==ThreadState::Joined;
    }

    void wait_all() {
        std::vector<ThreadInfo*> toJoin;
        {
            std::lock_guard<std::mutex> lg(g_threadMutex);
            for (auto & kv : g_threads) if (!kv.second->detached) toJoin.push_back(kv.second.get());
        }
        for (auto* ti : toJoin) {
            if (ti->th.joinable()) { try { ti->th.join(); } catch (...) {} }
            auto prev = ti->state.load();
            if (prev==ThreadState::Running) ti->state.store(ThreadState::Joined);
            else if (prev==ThreadState::Done) ti->state.store(ThreadState::Joined);
        }
    }

    int count() noexcept {
        std::scoped_lock lg(g_threadMutex); return (int)g_threads.size();
    }

    [[nodiscard]] std::string remove(int id, bool force) {
        std::scoped_lock lg(g_threadMutex);
        auto it = g_threads.find(id); if (it==g_threads.end()) return "error:invalid_handle";
        ThreadInfo* ti = it->second.get();
        auto st = ti->state.load();
        if (st == ThreadState::Joining) return "error:joining";
        if (!force) {
            if (ti->detached) {
                if (st != ThreadState::DetachedDone) return "error:still_running"; // wait until finished unless force
            } else {
                // attached
                if (ti->th.joinable()) return "error:still_joinable"; // would std::terminate if destroyed
            }
        }
        g_threads.erase(it);
        return force ? "forced" : std::string();
    }
}

static std::string threads_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };

    if (name == "thread_run") {
        std::string err; bool detach = (argv.size() > 1 && argv[1] == "detach");
        std::string action = argS(0);
        int id = threadmgr::create(rt, rt?rt->currentProgram():nullptr, action, detach, err);
        if (id < 0) return std::string("error:") + err;
        return threadmgr::handle(id);
    }
    if (name == "thread_join") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        std::string err; if (!threadmgr::join(id, err)) return std::string("error:") + err; return "true";
    }
    if (name == "thread_join_timeout") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        uint64_t ms = 0; if (argv.size()>1) { try { ms = std::stoull(argv[1]); } catch (...) { return "error:bad_timeout"; } }
        if (ms == 0) return "error:bad_timeout";
        std::string err; if (!threadmgr::join_timeout(id, ms, err)) return std::string("error:") + err; return "true";
    }
    if (name == "thread_done") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        bool exists=false, detached=false; bool done = threadmgr::is_done(id, exists, detached);
        if (!exists) return "error:invalid_handle"; return done?"true":"false";
    }
    if (name == "thread_list") {
        std::lock_guard<std::mutex> lg(g_threadMutex);
        int listId = g_nextListId++; g_lists[listId] = {};
        for (auto & kv : g_threads) g_lists[listId].push_back(threadmgr::handle(kv.first));
        return std::string("list:") + std::to_string(listId);
    }
    if (name == "thread_wait_all") { threadmgr::wait_all(); return {}; }
    if (name == "thread_count") { return std::to_string(threadmgr::count()); }
    if (name == "thread_yield") { std::this_thread::yield(); return {}; }
    if (name == "thread_gc" || name == "thread_purge") { threadmgr::gc_detached(); return {}; }
    if (name == "thread_gc_all") { threadmgr::gc_all(); return {}; }
    if (name == "thread_remove") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        bool force = (argv.size()>1 && (argv[1]=="force" || argv[1]=="kill"));
        return threadmgr::remove(id, force);
    }
    if (name == "thread_state") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        std::lock_guard<std::mutex> lg(g_threadMutex);
        auto it = g_threads.find(id); if (it==g_threads.end()) return "error:invalid_handle";
        ThreadState st = it->second->state.load();
        const char* s = "Unknown";
        switch(st) {
            case ThreadState::Running: s="Running"; break;
            case ThreadState::DetachedRunning: s="DetachedRunning"; break;
            case ThreadState::Done: s="Done"; break;
            case ThreadState::DetachedDone: s="DetachedDone"; break;
            case ThreadState::Joining: s="Joining"; break;
            case ThreadState::Joined: s="Joined"; break;
        }
        return std::string("state:") + s;
    }
    return {}; // unknown builtin name
}

std::string __erelang_builtin_threads_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv) {
    return threads_dispatch(rt, name, argv);
}

} // namespace erelang
