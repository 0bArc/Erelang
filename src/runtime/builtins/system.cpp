// Built-in system helpers for erelang
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <filesystem>
#include <sstream>
#include <limits>
#include <utility>
#include <chrono>

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
#ifdef _WIN32
    if (s.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], size);
    return out;
#else
    std::wstring out(s.size(), L'\0');
    const size_t written = mbstowcs(out.data(), s.c_str(), s.size());
    if (written == static_cast<size_t>(-1)) {
        return std::wstring(s.begin(), s.end());
    }
    out.resize(written);
    return out;
#endif
}

#ifdef _WIN32

// Quotes a single command-line argument per the C runtime's CreateProcessW
// rules (double every backslash preceding a quote; wrap in quotes when the
// argument contains whitespace or quotes).
std::wstring quote_windows_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(const std::vector<std::wstring>& argv) {
    std::wstring line;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) line.push_back(L' ');
        line += quote_windows_arg(argv[i]);
    }
    return line;
}

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

ProcessResult run_process(const std::vector<std::wstring>& argv, const std::optional<std::wstring>& workingDir) {
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
    std::wstring cmdLine = build_command_line(argv);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
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

    // Cap total child runtime so a hung process (or one whose pipe handle
    // leaked to a grandchild) cannot block the caller forever.
    constexpr auto kPipeReadTimeout = std::chrono::seconds(60);
    const auto readStart = std::chrono::steady_clock::now();

    std::string output;
    constexpr DWORD chunk = 4096;
    char buffer[chunk];
    DWORD bytesRead = 0;
    while (true) {
        DWORD avail = 0;
        if (PeekNamedPipe(readGuard.get(), nullptr, 0, nullptr, &avail, nullptr)) {
            if (avail > 0) {
                if (ReadFile(readGuard.get(), buffer, chunk, &bytesRead, nullptr) && bytesRead > 0) {
                    output.append(buffer, buffer + bytesRead);
                }
            }
        }

        if (WaitForSingleObject(process.get(), 200) == WAIT_OBJECT_0) {
            // Process exited: drain whatever remains in the pipe with a short
            // grace period, then stop reading even if a grandchild still
            // holds the write end.
            const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < drainDeadline) {
                DWORD remainingAvail = 0;
                if (!PeekNamedPipe(readGuard.get(), nullptr, 0, nullptr, &remainingAvail, nullptr) || remainingAvail == 0) {
                    break;
                }
                if (!ReadFile(readGuard.get(), buffer, chunk, &bytesRead, nullptr) || bytesRead == 0) {
                    break;
                }
                output.append(buffer, buffer + bytesRead);
            }
            break;
        }

        if (std::chrono::steady_clock::now() - readStart > kPipeReadTimeout) {
            TerminateProcess(process.get(), static_cast<UINT>(-1));
            break;
        }
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

#include <unistd.h>
#include <sys/wait.h>

ProcessResult run_process(const std::vector<std::wstring>& argv, const std::optional<std::wstring>& workingDir) {
    ProcessResult result;
    std::vector<std::string> narrow;
    narrow.reserve(argv.size());
    for (const auto& w : argv) {
        std::string s(w.size() * MB_CUR_MAX + 1, '\0');
        const size_t written = wcstombs(s.data(), w.c_str(), s.size());
        if (written == static_cast<size_t>(-1)) {
            result.exitCode = std::numeric_limits<unsigned long>::max();
            return result;
        }
        s.resize(written);
        narrow.push_back(std::move(s));
    }
    std::vector<char*> cargv;
    cargv.reserve(narrow.size() + 1);
    for (auto& s : narrow) cargv.push_back(s.data());
    cargv.push_back(nullptr);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.exitCode = std::numeric_limits<unsigned long>::max();
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        result.exitCode = std::numeric_limits<unsigned long>::max();
        return result;
    }
    if (pid == 0) {
        // Child: redirect stdout/stderr into the pipe, then exec (no shell).
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        if (workingDir) {
            std::string dir;
            const size_t dirSize = workingDir->size() * MB_CUR_MAX + 1;
            dir.resize(dirSize);
            const size_t dirWritten = wcstombs(dir.data(), workingDir->c_str(), dirSize);
            if (dirWritten != static_cast<size_t>(-1)) {
                dir.resize(dirWritten);
                if (chdir(dir.c_str()) != 0) _exit(126);
            }
        }
        execv(cargv[0], cargv.data());
        _exit(127);
    }
    close(pipefd[1]);
    std::string output;
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<std::size_t>(n));
    }
    close(pipefd[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
        result.exitCode = static_cast<unsigned long>(WEXITSTATUS(status));
    } else {
        result.exitCode = 127;
    }
    result.output = std::move(output);
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

    if (name == "system.cmd" || name == "proc.shell" || name == "sys.cmd") {
        std::wstring command = widen(argS(0));
        if (command.empty()) {
            return {};
        }
        std::vector<std::wstring> argv;
#ifdef _WIN32
        argv = {L"cmd.exe", L"/C", std::move(command)};
#else
        argv = {L"/bin/sh", L"-c", std::move(command)};
#endif
        auto workingDir = optional_widen(argS(1));
        ProcessResult result = run_process(argv, workingDir);
        if (!result.ran) {
            store_result(result);
            return {};
        }
        store_result(result);
        return result.output;
    }

    if (name == "system.execute" || name == "proc.execute" || name == "sys.execute" || name == "proc.spawn") {
        std::wstring target = widen(argS(0));
        if (target.empty()) {
            return {};
        }
        // Each argument is passed and quoted individually so embedded
        // whitespace cannot inject extra arguments.
        std::vector<std::wstring> argv;
        argv.push_back(std::move(target));
        std::string tail = argS(1);
        if (!tail.empty()) {
            argv.push_back(widen(tail));
        }
        auto workingDir = optional_widen(argS(2));
        ProcessResult result = run_process(argv, workingDir);
        store_result(result);
        if (!result.ran) {
            return std::string("-1");
        }
        return std::to_string(static_cast<long long>(result.exitCode));
    }

    if (name == "system.output" || name == "proc.output" || name == "sys.output") {
        auto& state = system_state();
        std::scoped_lock lock(state.mutex);
        return state.lastOutput;
    }

    if (name == "system.last_exit" || name == "system.last_exit_code" || name == "proc.exit_code" || name == "sys.last_exit") {
        auto& state = system_state();
        std::scoped_lock lock(state.mutex);
        return std::to_string(static_cast<long long>(state.lastExitCode));
    }

    // Process protection stubs
    if (name == "sys_opts") {
        // Returns an empty options handle string
        return std::string("opts:0");
    }
    if (name == "sys_kill" || name == "proc.kill") {
        return {}; // stub — process handle tracking not implemented yet
    }
    if (name == "sys_wait" || name == "proc.wait") {
        return {}; // stub
    }
    if (name == "sys_alive" || name == "proc.alive") {
        return "false"; // stub
    }

    if (name == "system.ip.flush") {
#ifdef _WIN32
        ProcessResult result = run_process({L"ipconfig.exe", L"/flushdns"}, std::nullopt);
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
