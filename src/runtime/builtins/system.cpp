// Built-in system helpers for erelang
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <filesystem>
#include <sstream>
#include <limits>
#include <cstdint>
#include <utility>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>

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
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
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
    // Scripts are UTF-8 whatever the C locale says, so decode by hand (wchar_t holds a code point here).
    std::wstring out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (extra < 0 || i + static_cast<std::size_t>(extra) >= s.size() + (extra == 0)) {
            out.push_back(static_cast<wchar_t>(c));  // invalid byte: keep it as-is
            ++i;
            continue;
        }
        std::uint32_t cp = extra == 0 ? c : (c & (0x3F >> extra));
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(static_cast<wchar_t>(cp));
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
#endif
}

#ifndef _WIN32
std::string narrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (const wchar_t wc : w) {
        const auto cp = static_cast<std::uint32_t>(wc);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// The parent's pipe ends must not leak into later children, or a child's stdin never sees EOF.
void set_cloexec(int fd) {
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}
#endif

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

ProcessResult run_process(const std::vector<std::wstring>& argv, const std::optional<std::wstring>& workingDir) {
    ProcessResult result;
    std::vector<std::string> args;
    args.reserve(argv.size());
    for (const auto& w : argv) args.push_back(narrow(w));
    const std::string dir = workingDir ? narrow(*workingDir) : std::string();
    std::vector<char*> cargv;
    cargv.reserve(args.size() + 1);
    for (auto& s : args) cargv.push_back(s.data());
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
        if (!dir.empty() && chdir(dir.c_str()) != 0) _exit(126);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(pipefd[1]);
    set_cloexec(pipefd[0]);
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

// ---- Background processes (proc: handles) ----------------------------------------------------
//
// sys.start(cmd) runs a command line through the shell without waiting. The handle reads stdout and
// stderr on background threads, so a chatty child never blocks on a full pipe, and the script can
// poll with read(), wait with a timeout, write to stdin or kill it.
namespace {

class Process {
public:
    ~Process() {
        close_input();
#ifdef _WIN32
        if (process_) {
            // Detach: the child keeps running if the script forgot it, but our handles go away.
            CloseHandle(process_);
        }
#endif
        for (auto& t : readers_) {
            if (t.joinable()) t.join();
        }
    }

    bool start(const std::wstring& commandLine, const std::optional<std::wstring>& workingDir) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        HANDLE inRead = nullptr, inWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return false;
        HandleGuard outR(outRead), outW(outWrite);
        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) return false;
        HandleGuard errR(errRead), errW(errWrite);
        if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return false;
        HandleGuard inR(inRead), inW(inWrite);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"cmd.exe /C " + commandLine;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                            nullptr, workingDir ? workingDir->c_str() : nullptr, &si, &pi)) {
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        pid_ = static_cast<long long>(pi.dwProcessId);
        // The child owns its ends now.
        outW.close();
        errW.close();
        inR.close();
        stdin_ = inW.release();
        HANDLE so = outR.release();
        HANDLE se = errR.release();
        readers_.emplace_back([this, so] { pump(so, out_); });
        readers_.emplace_back([this, se] { pump(se, err_); });
        return true;
#else
        int outPipe[2], errPipe[2], inPipe[2];
        if (pipe(outPipe) != 0) return false;
        if (pipe(errPipe) != 0) return false;
        if (pipe(inPipe) != 0) return false;
        const std::string cmd = narrow(commandLine);
        const std::string dir = workingDir ? narrow(*workingDir) : std::string();
        const pid_t pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);
            ::close(inPipe[0]); ::close(inPipe[1]); ::close(outPipe[0]); ::close(outPipe[1]);
            ::close(errPipe[0]); ::close(errPipe[1]);
            if (!dir.empty() && chdir(dir.c_str()) != 0) _exit(126);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(inPipe[0]); ::close(outPipe[1]); ::close(errPipe[1]);
        set_cloexec(inPipe[1]); set_cloexec(outPipe[0]); set_cloexec(errPipe[0]);
        pid_ = pid;
        stdinFd_ = inPipe[1];
        const int so = outPipe[0], se = errPipe[0];
        readers_.emplace_back([this, so] { pump(so, out_); });
        readers_.emplace_back([this, se] { pump(se, err_); });
        return true;
#endif
    }

    long long pid() const { return pid_; }

    bool alive() {
        return exit_code() == kRunning;
    }

    // Exit code, or kRunning while the process runs.
    long long exit_code() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exited_) return exitCode_;
#ifdef _WIN32
        if (!process_) return -1;
        DWORD code = 0;
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return kRunning;
        GetExitCodeProcess(process_, &code);
        exitCode_ = static_cast<long long>(code);
#else
        int status = 0;
        const pid_t r = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
        if (r == 0) return kRunning;
        exitCode_ = (r == static_cast<pid_t>(pid_) && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif
        exited_ = true;
        return exitCode_;
    }

    // Waits up to `timeoutMs` (negative = forever). Returns the exit code, or kRunning on timeout.
    long long wait(long long timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
        for (;;) {
            const long long code = exit_code();
            if (code != kRunning) {
                join_readers();
                return code;
            }
            if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return kRunning;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    bool kill() {
        if (!alive()) return false;
#ifdef _WIN32
        return TerminateProcess(process_, 1) != 0;
#else
        return ::kill(static_cast<pid_t>(pid_), SIGKILL) == 0;
#endif
    }

    bool write(const std::string& text) {
#ifdef _WIN32
        if (!stdin_) return false;
        DWORD written = 0;
        return WriteFile(stdin_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
#else
        if (stdinFd_ < 0) return false;
        return ::write(stdinFd_, text.data(), text.size()) == static_cast<ssize_t>(text.size());
#endif
    }

    bool close_input() {
#ifdef _WIN32
        if (!stdin_) return false;
        CloseHandle(stdin_);
        stdin_ = nullptr;
#else
        if (stdinFd_ < 0) return false;
        ::close(stdinFd_);
        stdinFd_ = -1;
#endif
        return true;
    }

    // New output since the previous call.
    std::string read_new(bool err) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string& all = err ? err_ : out_;
        std::size_t& seen = err ? errSeen_ : outSeen_;
        std::string fresh = all.substr(seen);
        seen = all.size();
        return fresh;
    }

    std::string all(bool err) {
        std::lock_guard<std::mutex> lock(mutex_);
        return err ? err_ : out_;
    }

    static constexpr long long kRunning = std::numeric_limits<long long>::min();

private:
#ifdef _WIN32
    void pump(HANDLE h, std::string& sink) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            sink.append(buf, n);
        }
        CloseHandle(h);
    }
    HANDLE process_ = nullptr;
    HANDLE stdin_ = nullptr;
#else
    void pump(int fd, std::string& sink) {
        char buf[4096];
        ssize_t n = 0;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            sink.append(buf, static_cast<std::size_t>(n));
        }
        ::close(fd);
    }
    int stdinFd_ = -1;
#endif
    void join_readers() {
        for (auto& t : readers_) {
            if (t.joinable()) t.join();
        }
    }

    std::mutex mutex_;
    std::vector<std::thread> readers_;
    std::string out_, err_;
    std::size_t outSeen_ = 0, errSeen_ = 0;
    long long pid_ = -1;
    long long exitCode_ = -1;
    bool exited_ = false;
};

std::mutex g_procMutex;
std::unordered_map<int, std::shared_ptr<Process>> g_procs;
int g_nextProcId = 1;

std::shared_ptr<Process> find_proc(int id) {
    std::lock_guard<std::mutex> lock(g_procMutex);
    const auto it = g_procs.find(id);
    return it == g_procs.end() ? nullptr : it->second;
}

int handle_id(const std::string& handle) {
    const std::string digits = handle.rfind("proc:", 0) == 0 ? handle.substr(5) : handle;
    try { return std::stoi(digits); } catch (...) { return -1; }
}

std::string code_text(long long code) {
    return code == Process::kRunning ? std::string() : std::to_string(code);
}

long long parse_ms(const std::string& s, long long fallback) {
    if (s.empty()) return fallback;
    try { return std::stoll(s); } catch (...) { return fallback; }
}

}  // namespace

std::string proc_start(const std::string& commandLine, const std::string& dir) {
    auto p = std::make_shared<Process>();
    if (commandLine.empty() || !p->start(widen(commandLine), optional_widen(dir))) return "error:start_failed";
    std::lock_guard<std::mutex> lock(g_procMutex);
    const int id = g_nextProcId++;
    g_procs[id] = std::move(p);
    return "proc:" + std::to_string(id);
}

// Methods of a proc: handle; see the "proc" type in the builtin registry.
bool proc_handle_method(int id, const std::string& method, const std::vector<std::string>& args, std::string& out) {
    auto p = find_proc(id);
    if (!p) {
        out = method == "alive" ? "false" : "error:invalid_handle";
        return true;
    }
    const auto arg = [&](std::size_t i) { return i < args.size() ? args[i] : std::string(); };
    if (method == "wait") { out = code_text(p->wait(parse_ms(arg(0), -1))); return true; }
    if (method == "alive") { out = p->alive() ? "true" : "false"; return true; }
    if (method == "kill") { out = p->kill() ? "true" : "false"; return true; }
    if (method == "pid") { out = std::to_string(p->pid()); return true; }
    if (method == "read") { out = p->read_new(false); return true; }
    if (method == "read_err") { out = p->read_new(true); return true; }
    if (method == "output") { out = p->all(false); return true; }
    if (method == "errors") { out = p->all(true); return true; }
    if (method == "write") { out = p->write(arg(0)) ? "true" : "false"; return true; }
    if (method == "close_input") { out = p->close_input() ? "true" : "false"; return true; }
    if (method == "exit_code") {
        const long long c = p->exit_code();
        out = std::to_string(c == Process::kRunning ? -1 : c);
        return true;
    }
    if (method == "forget") {
        std::lock_guard<std::mutex> lock(g_procMutex);
        g_procs.erase(id);
        out = "true";
        return true;
    }
    out = "error:unknown_method:" + method;
    return true;
}

// ---- dispatch --------------------------------------------------------------------------------

std::string __erelang_builtin_system_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](std::size_t i) -> std::string {
        return i < argv.size() ? argv[i] : std::string();
    };
    const auto exit_text = [](unsigned long code) {
        return code == std::numeric_limits<unsigned long>::max() ? std::string("-1")
                                                                 : std::to_string(static_cast<long long>(code));
    };

    if (name == "system.cmd" || name == "proc.shell" || name == "sys.cmd") {
        std::wstring command = widen(argS(0));
        if (command.empty()) {
            return {};
        }
        std::vector<std::wstring> args;
#ifdef _WIN32
        args = {L"cmd.exe", L"/C", std::move(command)};
#else
        args = {L"/bin/sh", L"-c", std::move(command)};
#endif
        auto workingDir = optional_widen(argS(1));
        ProcessResult result = run_process(args, workingDir);
        store_result(result);
        return result.ran ? result.output : std::string();
    }

    // execute(program, arg1, arg2, ...) and execute_in(dir, program, args...): every argument is
    // passed and quoted on its own, no shell, so spaces in an argument cannot split it.
    if (name == "system.execute" || name == "proc.execute" || name == "sys.execute" ||
        name == "system.execute_in") {
        const bool inDir = name == "system.execute_in";
        const std::size_t first = inDir ? 1 : 0;
        std::wstring target = widen(argS(first));
        if (target.empty()) {
            return std::string("-1");
        }
        std::vector<std::wstring> args;
        args.push_back(std::move(target));
        for (std::size_t i = first + 1; i < argv.size(); ++i) args.push_back(widen(argv[i]));
        auto workingDir = inDir ? optional_widen(argS(0)) : std::optional<std::wstring>{};
        ProcessResult result = run_process(args, workingDir);
        store_result(result);
        if (!result.ran) {
            return std::string("-1");
        }
        return exit_text(result.exitCode);
    }

    if (name == "system.output" || name == "proc.output" || name == "sys.output") {
        auto& state = system_state();
        std::scoped_lock lock(state.mutex);
        return state.lastOutput;
    }

    if (name == "system.last_exit" || name == "system.last_exit_code" || name == "proc.exit_code" || name == "sys.last_exit") {
        auto& state = system_state();
        std::scoped_lock lock(state.mutex);
        return exit_text(state.lastExitCode);
    }

    if (name == "proc_start") return proc_start(argS(0), argS(1));
    if (name == "proc_kill" || name == "proc_wait" || name == "proc_alive") {
        std::string out;
        const std::string method = name.substr(5);
        std::vector<std::string> rest(argv.begin() + (argv.empty() ? 0 : 1), argv.end());
        proc_handle_method(handle_id(argS(0)), method, rest, out);
        return out;
    }

    if (name == "system.os") {
#if defined(_WIN32)
        return "windows";
#elif defined(__APPLE__)
        return "macos";
#else
        return "linux";
#endif
    }

    if (name == "system.ip.flush") {
#if defined(_WIN32)
        ProcessResult result = run_process({L"ipconfig.exe", L"/flushdns"}, std::nullopt);
#elif defined(__APPLE__)
        ProcessResult result = run_process({L"dscacheutil", L"-flushcache"}, std::nullopt);
#else
        ProcessResult result = run_process({L"resolvectl", L"flush-caches"}, std::nullopt);
#endif
        store_result(result);
        if (!result.ran) {
            return std::string("false");
        }
        return result.exitCode == 0 ? std::string("true") : std::string("false");
    }

    return {};
}

} // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("system",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_system_dispatch(name, argv);
    });
const bool kProcHandle = erelang::modules::register_handle("proc",
    [](erelang::Runtime*, int id, const std::string& method, const std::vector<std::string>& args, std::string& out) {
        return erelang::proc_handle_method(id, method, args, out);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(system)
