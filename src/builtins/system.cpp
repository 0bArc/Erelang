// Built-in system helpers for erelang
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <filesystem>
#include <sstream>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace erelang {

namespace {

struct SystemState {
    std::mutex mutex;
    std::string lastOutput;
    unsigned long lastExitCode = 0;
};

SystemState& system_state() {
    static SystemState state;
    return state;
}

struct ProcessResult {
    std::string output;
    unsigned long exitCode = 0;
    bool ran = false;
};

std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

#ifdef _WIN32

struct HandleGuard {
    HANDLE handle{nullptr};
    HandleGuard() = default;
    explicit HandleGuard(HANDLE h) : handle(h) {}
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HandleGuard(HandleGuard&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this != &other) {
            close();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }
    ~HandleGuard() { close(); }
    void close() noexcept {
        if (handle) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }
    HANDLE get() const noexcept { return handle; }
    HANDLE release() noexcept {
        HANDLE h = handle;
        handle = nullptr;
        return h;
    }
};

ProcessResult run_process(const std::wstring& commandLine, const std::optional<std::wstring>& workingDir) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return {};
    }
    HandleGuard readGuard(readPipe);
    HandleGuard writeGuard(writePipe);

    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        return {};
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    BOOL ok = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        flags,
        nullptr,
        workingDir ? workingDir->c_str() : nullptr,
        &si,
        &pi
    );

    if (!ok) {
        return {};
    }

    HandleGuard process(pi.hProcess);
    HandleGuard thread(pi.hThread);
    writeGuard.close();

    std::string output;
    constexpr DWORD chunk = 4096;
    char buffer[chunk];
    DWORD bytesRead = 0;
    while (ReadFile(readGuard.get(), buffer, chunk, &bytesRead, nullptr) && bytesRead != 0) {
        output.append(buffer, buffer + bytesRead);
    }

    WaitForSingleObject(process.get(), INFINITE);
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.get(), &exitCode)) {
        exitCode = static_cast<DWORD>(-1);
    }
    ProcessResult result;
    result.output = std::move(output);
    result.exitCode = exitCode;
    result.ran = true;
    return result;
}

#else

ProcessResult run_process(const std::wstring& commandLine, const std::optional<std::wstring>&) {
    std::string narrow(commandLine.begin(), commandLine.end());
    int code = std::system(narrow.c_str());
    ProcessResult result;
    result.exitCode = static_cast<unsigned long>(code);
    result.ran = true;
    return result;
}

#endif

void store_result(const ProcessResult& result) {
    auto& state = system_state();
    std::scoped_lock lock(state.mutex);
    if (result.ran) {
        state.lastOutput = result.output;
        state.lastExitCode = result.exitCode;
    } else {
        state.lastOutput.clear();
        state.lastExitCode = std::numeric_limits<unsigned long>::max();
    }
}

std::optional<std::wstring> optional_widen(const std::string& s) {
    if (s.empty()) {
        return std::nullopt;
    }
    return widen(s);
}

} // namespace

std::string __erelang_builtin_system_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](std::size_t i) -> std::string {
        return i < argv.size() ? argv[i] : std::string();
    };

    if (name == "system.cmd") {
        std::wstring command = widen(argS(0));
        if (command.empty()) {
            return {};
        }
        std::wstring cmdLine = L"cmd.exe /C " + command;
        auto workingDir = optional_widen(argS(1));
        ProcessResult result = run_process(cmdLine, workingDir);
        if (!result.ran) {
            store_result(result);
            return {};
        }
        store_result(result);
        return result.output;
    }

    if (name == "system.execute") {
        std::wstring target = widen(argS(0));
        if (target.empty()) {
            return {};
        }
        std::wstring cmdLine = target;
        std::string tail = argS(1);
        if (!tail.empty()) {
            cmdLine.push_back(L' ');
            cmdLine.append(widen(tail));
        }
        auto workingDir = optional_widen(argS(2));
        ProcessResult result = run_process(cmdLine, workingDir);
        store_result(result);
        if (!result.ran) {
            return std::string("-1");
        }
        return std::to_string(static_cast<long long>(result.exitCode));
    }

    if (name == "system.output") {
        auto& state = system_state();
        std::scoped_lock lock(state.mutex);
        return state.lastOutput;
    }

    if (name == "system.last_exit_code") {
        auto& state = system_state();
        std::scoped_lock lock(state.mutex);
        return std::to_string(static_cast<long long>(state.lastExitCode));
    }

    if (name == "system.ip.flush") {
#ifdef _WIN32
        ProcessResult result = run_process(L"ipconfig.exe /flushdns", std::nullopt);
        store_result(result);
        if (!result.ran) {
            return std::string("false");
        }
        return result.exitCode == 0 ? std::string("true") : std::string("false");
#else
        (void)argv;
        (void)argS;
        return std::string("false");
#endif
    }

    return {};
}

} // namespace erelang
