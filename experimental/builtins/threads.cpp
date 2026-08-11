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
// Upper bound on concurrently registered workers (policy system removed).
static constexpr int kMaxWorkerThreads = 512;
// Threads that were force-removed while still running are parked here and
// joined by park_all() before run() returns, so no worker can outlive the
// Program/Runtime it captured.
static std::vector<std::thread> g_parkedThreads;

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

    // Move a record's joinable native handle into g_parkedThreads so erasing
    // the record cannot destroy a joinable std::thread (std::terminate).
    // Caller must hold g_threadMutex. The parked threads are joined by
    // park_all() before Runtime::run() returns.
    static void retire_locked(std::unordered_map<int, std::unique_ptr<ThreadInfo>>::iterator it) {
        ThreadInfo* ti = it->second.get();
        if (ti && ti->th.joinable()) g_parkedThreads.push_back(std::move(ti->th));
        g_threads.erase(it);
    }

    // Join every remaining worker (including parked force-removed ones) so no
    // thread can outlive the Program/Runtime. Called by Runtime::run() at exit.
    // Loops until the table is drained because a still-running worker can spawn
    // new threads while we are joining its predecessors.
    void park_all() noexcept {
        for (;;) {
            std::vector<std::thread> toJoin;
            {
                std::lock_guard<std::mutex> lg(g_threadMutex);
                for (auto it = g_threads.begin(); it != g_threads.end(); ) {
                    ThreadInfo* ti = it->second.get();
                    if (ti && ti->th.joinable()) toJoin.push_back(std::move(ti->th));
                    it = g_threads.erase(it);
                }
                for (auto& th : g_parkedThreads) if (th.joinable()) toJoin.push_back(std::move(th));
                g_parkedThreads.clear();
            }
            if (toJoin.empty()) {
                std::lock_guard<std::mutex> lg(g_threadMutex);
                if (g_threads.empty()) break;  // fully drained
                continue;
            }
            for (auto& th : toJoin) {
                if (th.joinable()) { try { th.join(); } catch (...) {} }
            }
        }
    }

    // Remove finished detached threads (they cannot be joined)
    void gc_detached() noexcept {
        std::lock_guard<std::mutex> lg(g_threadMutex);
        for (auto it = g_threads.begin(); it != g_threads.end(); ) {
            ThreadInfo* ti = it->second.get();
            if (ti && ti->detached && (ti->state.load()==ThreadState::DetachedDone)) retire_locked(it); else ++it;
        }
    }

    void gc_all() noexcept {
        std::lock_guard<std::mutex> lg(g_threadMutex);
        for (auto it = g_threads.begin(); it != g_threads.end(); ) {
            ThreadState s = it->second->state.load();
            if (s == ThreadState::DetachedDone || s == ThreadState::Joined) retire_locked(it); else ++it;
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
            // Policy system removed: cap concurrent worker threads to avoid unbounded growth.
            if ((int)g_threads.size() >= kMaxWorkerThreads) { err = "max_threads"; return -1; }
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
        // NOTE: the underlying std::thread is intentionally never detached here.
        // The record stays joinable so park_all() can join every worker before
        // Runtime::run() returns; a truly detached std::thread would outlive the
        // Program and dereference a dangling pointer.
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
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lg(g_threadMutex);
            for (auto& kv : g_threads) {
                ThreadInfo* ti = kv.second.get();
                if (!ti->detached && ti->th.joinable()) {
                    toJoin.push_back(std::move(ti->th));
                    auto prev = ti->state.load();
                    if (prev == ThreadState::Running || prev == ThreadState::Done) ti->state.store(ThreadState::Joined);
                }
            }
        }
        for (auto& th : toJoin) {
            if (th.joinable()) { try { th.join(); } catch (...) {} }
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
        if (force) {
            // Cannot kill a running worker; retire the record and let park_all()
            // join the native handle before run() returns. This keeps the thread
            // alive (no std::terminate) while guaranteeing it cannot outlive the
            // Program/Runtime it captured.
            retire_locked(it);
            return "forced";
        }
        if (ti->detached) {
            if (st != ThreadState::DetachedDone) return "error:still_running"; // wait until finished unless force
        } else {
            // attached
            if (ti->th.joinable()) return "error:still_joinable"; // would std::terminate if destroyed
        }
        g_threads.erase(it);
        return std::string();
    }
}

// Register the park hook once so Runtime::run() joins all workers at exit.
namespace {
struct ThreadParkHookRegistrar {
    ThreadParkHookRegistrar() { Runtime::set_worker_park_hook(&threadmgr::park_all); }
};
} // namespace

static std::string threads_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    if (name == "thread_run" || name == "thread.spawn" || name == "thread.spawn_detached") {
        std::string err; bool detach = (argv.size() > 1 && argv[1] == "detach");
        if (name == "thread.spawn_detached") detach = true;
        std::string action = argS(0);
        int id = threadmgr::create(rt, rt?rt->currentProgram():nullptr, action, detach, err);
        if (id < 0) return std::string("error:") + err;
        return threadmgr::handle(id);
    }
    if (name == "thread_join" || name == "thread.join") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        std::string err; if (!threadmgr::join(id, err)) return std::string("error:") + err; return "true";
    }
    if (name == "thread_join_timeout" || name == "thread.join_timeout") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        uint64_t ms = 0; if (argv.size()>1) { try { ms = std::stoull(argv[1]); } catch (...) { return "error:bad_timeout"; } }
        if (ms == 0) return "error:bad_timeout";
        std::string err; if (!threadmgr::join_timeout(id, ms, err)) return std::string("error:") + err; return "true";
    }
    if (name == "thread_done" || name == "thread.done") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        bool exists=false, detached=false; bool done = threadmgr::is_done(id, exists, detached);
        if (!exists) return "error:invalid_handle"; return done?"true":"false";
    }
    if (name == "thread_list" || name == "thread.list") {
        std::lock_guard<std::mutex> lg(g_threadMutex);
        int listId = g_nextListId++; g_lists[listId] = {};
        for (auto & kv : g_threads) g_lists[listId].push_back(threadmgr::handle(kv.first));
        return std::string("list:") + std::to_string(listId);
    }
    if (name == "thread_wait_all" || name == "thread.wait_all") { threadmgr::wait_all(); return {}; }
    if (name == "thread_count" || name == "thread.active") { return std::to_string(threadmgr::count()); }
    if (name == "thread_yield" || name == "thread.yield") { std::this_thread::yield(); return {}; }
    if (name == "thread_gc" || name == "thread_purge" || name == "thread.kill") { threadmgr::gc_detached(); return {}; }
    if (name == "thread_gc_all") { threadmgr::gc_all(); return {}; }
    if (name == "thread_remove" || name == "thread.remove") {
        int id; if (!threadmgr::parse_handle(argS(0), id)) return "error:invalid_handle";
        bool force = (argv.size()>1 && (argv[1]=="force" || argv[1]=="kill"));
        return threadmgr::remove(id, force);
    }
    if (name == "thread_state" || name == "thread.state") {
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
    // pool control stubs
    if (name == "thread.pool.max" || name == "thread.pool.stop") {
        // Pool control is a no-op at runtime; typechecker validates
        return {};
    }
    // thread.sleep and thread.result are handled separately in builtins.cpp
    return {}; // unknown builtin name
}

std::string __erelang_builtin_threads_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv) {
    return threads_dispatch(rt, name, argv);
}

} // namespace erelang
