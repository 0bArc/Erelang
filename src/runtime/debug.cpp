#include "erelang/runtime.hpp"
#include "erelang/value.hpp"

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace erelang {
namespace {

struct DebugState {
    std::atomic<bool> enabled{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> stepMode{false};
    std::atomic<bool> paused{false};
    std::mutex bpMutex;
    std::unordered_set<int> breakpoints;
    std::mutex pathMutex;
    std::string sourcePath;
};

thread_local bool g_debugMainExec = false;

DebugState& state() {
    static DebugState s;
    return s;
}

void emit_event(const std::string& payload) {
    std::cerr << "!dap " << payload << std::endl;
}

std::string locals_json(const Runtime::Env& env) {
    std::ostringstream oss;
    oss << '{';
    bool first = true;
    auto emit = [&](const std::string& name, const Value& v) {
        if (!first) oss << ',';
        first = false;
        oss << '"';
        for (char c : name) {
            if (c == '"' || c == '\\') oss << '\\';
            oss << c;
        }
        oss << "\":\"";
        const std::string disp = to_display_string(v);
        for (char c : disp) {
            if (c == '"' || c == '\\') oss << '\\';
            else if (c == '\n') { oss << "\\n"; continue; }
            else if (c == '\r') { oss << "\\r"; continue; }
            oss << c;
        }
        oss << '"';
    };
    if (env.useSlots) {
        for (size_t i = 0; i < env.slots.size() && i < env.slotNames.size(); ++i) {
            emit(env.slotNames[i], env.slots[i]);
        }
    } else {
        for (const auto& kv : env.vars) {
            if (kv.first.find('.') != std::string::npos) continue;
            emit(kv.first, kv.second);
        }
    }
    oss << '}';
    return oss.str();
}

} // namespace

void Runtime::debug_enable(bool on) {
    state().enabled.store(on);
    if (on) emit_event("ready");
}

void Runtime::debug_wait_attach() {
    if (!state().enabled.load()) return;
    while (!state().quit.load()) {
        std::string lineIn;
        if (!std::getline(std::cin, lineIn)) {
            state().quit.store(true);
            break;
        }
        if (lineIn.rfind("!dap ", 0) != 0) continue;
        const std::string cmd = lineIn.substr(5);
        if (cmd.rfind("break ", 0) == 0) {
            try {
                debug_add_breakpoint(std::stoi(cmd.substr(6)));
            } catch (...) {}
        } else if (cmd == "clear") {
            debug_clear_breakpoints();
        } else if (cmd == "go" || cmd == "continue") {
            break;
        } else if (cmd == "quit") {
            state().quit.store(true);
            break;
        }
    }
}

bool Runtime::debug_enabled() {
    return state().enabled.load();
}

void Runtime::debug_set_breakpoints(std::unordered_set<int> lines) {
    std::lock_guard<std::mutex> lock(state().bpMutex);
    state().breakpoints = std::move(lines);
}

void Runtime::debug_add_breakpoint(int line) {
    if (line <= 0) return;
    std::lock_guard<std::mutex> lock(state().bpMutex);
    state().breakpoints.insert(line);
}

void Runtime::debug_clear_breakpoints() {
    std::lock_guard<std::mutex> lock(state().bpMutex);
    state().breakpoints.clear();
}

void Runtime::debug_set_source_path(std::string path) {
    std::lock_guard<std::mutex> lock(state().pathMutex);
    state().sourcePath = std::move(path);
}

void Runtime::debug_request_continue() {
    auto& s = state();
    s.stepMode.store(false);
    s.paused.store(false);
}

void Runtime::debug_request_step() {
    auto& s = state();
    s.stepMode.store(true);
    s.paused.store(false);
}

void Runtime::debug_request_quit() {
    auto& s = state();
    s.quit.store(true);
    s.paused.store(false);
}

void Runtime::debug_enter_main() {
    g_debugMainExec = true;
}

void Runtime::debug_leave_main() {
    g_debugMainExec = false;
}

void Runtime::debug_hook(int line, const Env& env) {
    auto& s = state();
    if (!s.enabled.load() || line <= 0) return;
    if (!g_debugMainExec) return;
    if (s.quit.load()) {
        throw std::runtime_error("debug quit");
    }

    bool hit = false;
    {
        std::lock_guard<std::mutex> lock(s.bpMutex);
        hit = s.breakpoints.count(line) > 0;
    }
    const bool step = s.stepMode.load();
    if (!hit && !step) return;

    s.stepMode.store(false);
    s.paused.store(true);
    std::string path;
    {
        std::lock_guard<std::mutex> lock(s.pathMutex);
        path = s.sourcePath;
    }
    emit_event("stopped line=" + std::to_string(line) +
               " reason=" + (hit ? std::string("breakpoint") : std::string("step")) +
               " file=" + path);
    emit_event("locals " + locals_json(env));

    while (s.paused.load() && !s.quit.load()) {
        std::string lineIn;
        if (!std::getline(std::cin, lineIn)) {
            s.quit.store(true);
            break;
        }
        if (lineIn.rfind("!dap ", 0) != 0) continue;
        const std::string cmd = lineIn.substr(5);
        if (cmd.rfind("break ", 0) == 0) {
            try {
                debug_add_breakpoint(std::stoi(cmd.substr(6)));
            } catch (...) {}
        } else if (cmd == "clear") {
            debug_clear_breakpoints();
        } else if (cmd == "continue") {
            debug_request_continue();
            break;
        } else if (cmd == "step") {
            debug_request_step();
            break;
        } else if (cmd == "quit") {
            debug_request_quit();
            break;
        } else if (cmd == "locals") {
            emit_event("locals " + locals_json(env));
        }
    }
    if (s.quit.load()) {
        throw std::runtime_error("debug quit");
    }
}

} // namespace erelang
