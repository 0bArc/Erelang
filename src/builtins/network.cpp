// Built-in network module for erelang
// NOTE: Intentionally avoid including heavy runtime headers here to prevent
// namespace / macro interference with libstdc++ standard headers (observed
// pathological parse of <filesystem> after recent include hygiene changes).
// If runtime types are needed in the future, prefer forward declarations.
// Order: standard library first, then Windows, then local declarations.
#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <utility>
#include <array>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Lean Windows headers to minimize macro pollution
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace erelang {

namespace {

struct NetworkDebugState {
    std::mutex mutex;
    bool enabled = false;
    std::filesystem::path logPath = std::filesystem::path("network_debug.log");
    std::string lastOp;
    std::vector<std::string> lastArgs;
    std::string lastResult;
    bool lastSuccess = false;
};

NetworkDebugState& network_debug_state() {
    static NetworkDebugState state;
    return state;
}

std::string debug_timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto tt = clock::to_time_t(now);
#ifdef _WIN32
    std::tm localTm{};
    localtime_s(&localTm, &tt);
#else
    std::tm localTm{};
    localtime_r(&tt, &localTm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string join_args(const std::vector<std::string>& args) {
    if (args.empty()) {
        return "[]";
    }
    std::ostringstream oss;
    oss << '[';
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << '"';
        for (char ch : args[i]) {
            switch (ch) {
                case '\\': oss << "\\\\"; break;
                case '\"': oss << "\\\""; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;
                default: oss << ch; break;
            }
        }
        oss << '"';
    }
    oss << ']';
    return oss.str();
}

void network_debug_record(const std::string& op, const std::vector<std::string>& args, const std::string& result, bool success) {
    auto& state = network_debug_state();
    std::scoped_lock lock(state.mutex);
    state.lastOp = op;
    state.lastArgs = args;
    state.lastResult = result;
    state.lastSuccess = success;
    if (!state.enabled) {
        return;
    }
    std::ofstream out(state.logPath, std::ios::app);
    if (!out) {
        return;
    }
    const std::string ts = debug_timestamp();
    constexpr std::size_t kMaxResultPreview = 512;
    std::string preview = result;
    if (preview.size() > kMaxResultPreview) {
        preview.resize(kMaxResultPreview);
        preview.append("... <truncated>");
    }
    out << '[' << ts << "] op=" << op << " success=" << (success ? "true" : "false") << '\n';
    out << " args=" << join_args(args) << '\n';
    out << " result=";
    if (preview.find_first_not_of(" \t\r\n") == std::string::npos) {
        out << "<empty>";
    } else {
        out << preview;
    }
    out << "\n\n";
}

} // namespace

struct ExecResult {
    bool success = false;
    unsigned long exitCode = 0;
    std::string output;
};

static std::string format_exec_result(const ExecResult& result) {
    std::ostringstream oss;
    oss << "success=" << (result.success ? "true" : "false")
        << "\nexit_code=" << result.exitCode
        << "\noutput=";
    if (result.output.find_first_not_of(" \t\r\n") == std::string::npos) {
        oss << "<empty>";
    } else {
        oss << result.output;
    }
    return oss.str();
}

#ifdef _WIN32

struct UniqueHandle {
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle(h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }
    void reset(HANDLE h = nullptr) noexcept {
        if (handle) {
            CloseHandle(handle);
        }
        handle = h;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle, nullptr); }
    explicit operator bool() const noexcept { return handle != nullptr; }

private:
    HANDLE handle = nullptr;
};

static std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), required);
    return wide;
}

static std::string narrow_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), required, nullptr, nullptr);
    return narrow;
}

static std::string windows_error_message(DWORD error) {
    if (error == 0) {
        return {};
    }
    LPWSTR buffer = nullptr;
    const DWORD chars = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message;
    if (chars != 0 && buffer) {
        message = narrow_utf8(std::wstring(buffer, chars));
        LocalFree(buffer);
    }
    return message;
}

static void append_quoted_argument(std::wstring& target, const std::wstring& argument) {
    target.push_back(L'"');
    unsigned int backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            target.append(backslashes * 2 + 1, L'\\');
            target.push_back(L'"');
            backslashes = 0;
        } else {
            target.append(backslashes, L'\\');
            target.push_back(ch);
            backslashes = 0;
        }
    }
    target.append(backslashes * 2, L'\\');
    target.push_back(L'"');
}

static std::wstring build_command_line(const std::vector<std::string>& args) {
    std::wstring cmd;
    bool first = true;
    for (const auto& arg : args) {
        if (!first) {
            cmd.push_back(L' ');
        }
        first = false;
        append_quoted_argument(cmd, widen_utf8(arg));
    }
    return cmd;
}

static ExecResult run_process_with_command_line(const std::wstring& commandLine) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readTmp = nullptr;
    HANDLE writeTmp = nullptr;
    if (!CreatePipe(&readTmp, &writeTmp, &sa, 0)) {
        const DWORD err = GetLastError();
        return {false, err, windows_error_message(err)};
    }

    UniqueHandle readPipe(readTmp);
    UniqueHandle writePipe(writeTmp);

    if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        const DWORD err = GetLastError();
        return {false, err, windows_error_message(err)};
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe.get();
    si.hStdError = writePipe.get();
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        const DWORD err = GetLastError();
        return {false, err, windows_error_message(err)};
    }

    UniqueHandle process(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    writePipe.reset();

    std::string output;
    std::array<char, 4096> buffer{};
    DWORD bytesRead = 0;
    while (ReadFile(readPipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead != 0) {
        output.append(buffer.data(), static_cast<std::size_t>(bytesRead));
    }

    const DWORD waitResult = WaitForSingleObject(process.get(), INFINITE);
    if (waitResult == WAIT_FAILED) {
        const DWORD err = GetLastError();
        return {false, err, windows_error_message(err)};
    }

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.get(), &exitCode)) {
        const DWORD err = GetLastError();
        return {false, err, windows_error_message(err)};
    }

    return {exitCode == 0, exitCode, output};
}

static ExecResult execute_program_args(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {false, 1, "no program specified"};
    }
    if (args[0].find_first_not_of(" \t\r\n") == std::string::npos) {
        return {false, 1, "program name is empty"};
    }
    std::wstring cmdLine = build_command_line(args);
    return run_process_with_command_line(cmdLine);
}

static ExecResult execute_ipconfig(const std::vector<std::string>& extraArgs) {
    std::vector<std::string> args;
    args.reserve(extraArgs.size() + 1);
    args.emplace_back("ipconfig");
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return execute_program_args(args);
}

#else

static ExecResult run_process_with_command_line(const std::wstring&) {
    return {false, 1, "unsupported platform"};
}

static ExecResult execute_program_args(const std::vector<std::string>&) {
    return {false, 1, "unsupported platform"};
}

static ExecResult execute_ipconfig(const std::vector<std::string>&) {
    return {false, 1, "unsupported platform"};
}

#endif

static std::string url_encode_impl(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') o<<c;
        else { o<<'%'<<std::uppercase<<std::hex<<(int)c<<std::nouppercase<<std::dec; }
    }
    return o.str();
}

static std::string http_get_impl(const std::string& url) {
#ifdef _WIN32
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256]; wchar_t path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = sizeof(host)/sizeof(wchar_t);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = sizeof(path)/sizeof(wchar_t);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {};
    HINTERNET h = WinHttpOpen(L"erelang/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) return {};
    HINTERNET c = WinHttpConnect(h, uc.lpszHostName, uc.nPort, 0);
    if (!c) { WinHttpCloseHandle(h); return {}; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", uc.lpszUrlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(h); return {}; }
    BOOL ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(r, NULL);
    std::string out;
    if (ok) {
        DWORD avail = 0;
        do {
            if (!WinHttpQueryDataAvailable(r, &avail)) break;
            if (!avail) break;
            std::string buf; buf.resize(avail);
            DWORD read = 0;
            if (!WinHttpReadData(r, buf.data(), avail, &read)) break;
            buf.resize(read); out += buf;
        } while (avail > 0);
    }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(h);
    return out;
#else
    return {};
#endif
}

// Download a URL to a local file (best-effort). Returns true on success.
static bool http_download_impl(const std::string& url, const std::filesystem::path& outPath) {
#ifdef _WIN32
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256]; wchar_t path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = sizeof(host)/sizeof(wchar_t);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = sizeof(path)/sizeof(wchar_t);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    HINTERNET h = WinHttpOpen(L"erelang/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) return false;
    HINTERNET c = WinHttpConnect(h, uc.lpszHostName, uc.nPort, 0);
    if (!c) { WinHttpCloseHandle(h); return false; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", uc.lpszUrlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(h); return false; }
    BOOL ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(r, NULL);
    bool success = false;
    if (ok) {
        std::error_code ec; std::filesystem::create_directories(outPath.parent_path(), ec);
        std::ofstream out(outPath, std::ios::binary);
        if (out) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                std::string buf; buf.resize(avail);
                DWORD read = 0; if (!WinHttpReadData(r, buf.data(), avail, &read) || read == 0) break;
                out.write(buf.data(), static_cast<std::streamsize>(read));
            }
            success = static_cast<bool>(out);
        }
    }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(h);
    return success;
#else
    (void)url; (void)outPath; return false;
#endif
}

// Very simple M3U8 parser helpers for HLS; supports master playlist variant selection and VOD media download.
struct HlsVariant { std::string uri; int bandwidth = 0; };

static std::string dirname_url(const std::string& url) {
    auto pos = url.find_last_of('/');
    if (pos == std::string::npos) return url;
    return url.substr(0, pos+1);
}

static std::string resolve_url(const std::string& base, const std::string& rel) {
    if (rel.rfind("http://",0)==0 || rel.rfind("https://",0)==0) return rel;
    if (!base.empty() && rel.size() && rel[0] == '/') {
        // derive scheme+host from base
        auto p = base.find("//"); if (p==std::string::npos) return base + rel; p+=2;
        auto q = base.find('/', p); if (q==std::string::npos) return base + rel;
        return base.substr(0, q) + rel;
    }
    return dirname_url(base) + rel;
}

static std::vector<HlsVariant> parse_master_m3u8(const std::string& text, const std::string& base) {
    std::vector<HlsVariant> v;
    std::istringstream in(text); std::string line; int lastBw = 0;
    while (std::getline(in, line)) {
        if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            // find BANDWIDTH attribute
            lastBw = 0; auto k = line.find("BANDWIDTH=");
            if (k != std::string::npos) {
                k += 10; size_t e=k; while (e<line.size() && isdigit(static_cast<unsigned char>(line[e]))) ++e; lastBw = std::atoi(line.substr(k, e-k).c_str());
            }
        } else if (!line.empty() && line[0] != '#') {
            HlsVariant hv; hv.uri = resolve_url(base, line); hv.bandwidth = lastBw; v.push_back(std::move(hv)); lastBw = 0;
        }
    }
    return v;
}

static std::vector<std::string> parse_media_m3u8_segments(const std::string& text, const std::string& base) {
    std::vector<std::string> segs; std::istringstream in(text); std::string line;
    bool hasKey = false; // basic guard; we don't handle encrypted streams
    while (std::getline(in, line)) {
        if (line.rfind("#EXT-X-KEY:", 0) == 0) { hasKey = true; break; }
        if (!line.empty() && line[0] != '#') segs.push_back(resolve_url(base, line));
    }
    if (hasKey) return {}; // unsupported
    return segs;
}

// Download HLS stream selecting highest BANDWIDTH variant (VOD only). Returns true on success.
static bool hls_download_best_impl(const std::string& m3u8Url, const std::filesystem::path& outPath) {
    std::string master = http_get_impl(m3u8Url);
    if (master.empty()) return false;
    auto variants = parse_master_m3u8(master, m3u8Url);
    std::string mediaUrl;
    if (!variants.empty()) {
        const HlsVariant* best = &variants[0];
        for (const auto& v : variants) if (v.bandwidth > best->bandwidth) best = &v;
        mediaUrl = best->uri;
    } else {
        // already a media playlist
        mediaUrl = m3u8Url;
    }
    std::string media = http_get_impl(mediaUrl);
    if (media.empty()) return false;
    auto segs = parse_media_m3u8_segments(media, mediaUrl);
    if (segs.empty()) return false;
    std::error_code ec; std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary);
    if (!out) return false;
    for (const auto& s : segs) {
        std::string chunk = http_get_impl(s);
        if (chunk.empty()) return false; // abort on failure
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    return static_cast<bool>(out);
}

static std::string net_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i < argv.size() ? argv[i] : std::string(); };
    if (name == "network.debug.enable") {
        auto& state = network_debug_state();
        std::scoped_lock lock(state.mutex);
        state.enabled = true;
        if (!argS(0).empty()) {
            state.logPath = std::filesystem::path(argS(0));
        }
        return std::string("true");
    }
    if (name == "network.debug.disable") {
        auto& state = network_debug_state();
        std::scoped_lock lock(state.mutex);
        state.enabled = false;
        return std::string("true");
    }
    if (name == "network.debug.status") {
        auto& state = network_debug_state();
        std::scoped_lock lock(state.mutex);
        std::ostringstream oss;
        oss << "enabled=" << (state.enabled ? "true" : "false") << ", log=" << state.logPath.string();
        return oss.str();
    }
    if (name == "network.debug.last") {
        auto& state = network_debug_state();
        std::scoped_lock lock(state.mutex);
        if (state.lastOp.empty()) return {};
        std::ostringstream oss;
        oss << "op=" << state.lastOp << '\n';
        oss << "success=" << (state.lastSuccess ? "true" : "false") << '\n';
        oss << "args=" << join_args(state.lastArgs) << '\n';
        oss << "result=" << state.lastResult;
        return oss.str();
    }
    if (name == "network.debug.clear") {
        auto& state = network_debug_state();
        std::scoped_lock lock(state.mutex);
        state.lastOp.clear();
        state.lastArgs.clear();
        state.lastResult.clear();
        state.lastSuccess = false;
        return std::string("true");
    }
    if (name == "network.debug.log_tail") {
        std::size_t lines = 20;
        if (!argS(0).empty()) {
            try {
                lines = static_cast<std::size_t>(std::stoul(argS(0)));
            } catch (...) {
                lines = 20;
            }
        }
        auto& state = network_debug_state();
        std::scoped_lock lock(state.mutex);
        std::ifstream in(state.logPath);
        if (!in) return {};
        std::deque<std::string> buffer;
        std::string line;
        while (std::getline(in, line)) {
            if (buffer.size() == lines) buffer.pop_front();
            buffer.push_back(line);
        }
        std::ostringstream oss;
        bool first = true;
        for (const auto& l : buffer) {
            if (!first) oss << '\n';
            first = false;
            oss << l;
        }
        return oss.str();
    }
    if (name == "network.ip.flush") {
        ExecResult res = execute_ipconfig({"/flushdns"});
        std::string formatted = format_exec_result(res);
        network_debug_record(name, argv, formatted, res.success);
        return formatted;
    }
    if (name == "network.ip.release") {
        std::vector<std::string> args{"/release"};
        if (!argS(0).empty()) {
            args.push_back(argS(0));
        }
        ExecResult res = execute_ipconfig(args);
        std::string formatted = format_exec_result(res);
        network_debug_record(name, argv, formatted, res.success);
        return formatted;
    }
    if (name == "network.ip.renew") {
        std::vector<std::string> args{"/renew"};
        if (!argS(0).empty()) {
            args.push_back(argS(0));
        }
        ExecResult res = execute_ipconfig(args);
        std::string formatted = format_exec_result(res);
        network_debug_record(name, argv, formatted, res.success);
        return formatted;
    }
    if (name == "network.ip.registerdns") {
        ExecResult res = execute_ipconfig({"/registerdns"});
        std::string formatted = format_exec_result(res);
        network_debug_record(name, argv, formatted, res.success);
        return formatted;
    }
    if (name == "http_get") {
        std::string r = http_get_impl(argS(0));
        network_debug_record(name, argv, r, !r.empty());
        return r;
    }
    if (name == "http_download") {
        bool ok = http_download_impl(argS(0), std::filesystem::path(argS(1)));
        network_debug_record(name, argv, ok ? "true" : "false", ok);
        return ok ? std::string("true") : std::string("false");
    }
    if (name == "hls_download_best") {
        bool ok = hls_download_best_impl(argS(0), std::filesystem::path(argS(1)));
        network_debug_record(name, argv, ok ? "true" : "false", ok);
        return ok ? std::string("true") : std::string("false");
    }
    if (name == "url_encode") {
        std::string encoded = url_encode_impl(argS(0));
        network_debug_record(name, argv, encoded, true);
        return encoded;
    }
    return {};
}

std::string __erelang_builtin_network_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return net_dispatch(name, argv);
}

} // namespace erelang
