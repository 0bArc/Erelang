// Built-in network module for erelang
// NOTE: Avoid heavy runtime headers here to prevent namespace / macro interference.
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
#include <limits>
#include <atomic>
#include <unordered_map>
#include <thread>
#include <functional>
#include <memory>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace erelang {

#ifdef _WIN32

static std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), required);
    return wide;
}

static std::string narrow_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string narrow(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), required, nullptr, nullptr);
    return narrow;
}

static bool send_request_with_headers(HINTERNET request, const std::string& extraHeaders, const std::string& body, const std::string& contentType) {
    // Build full header string from extraHeaders + optional Content-Type
    std::wstring fullHeaders;
    if (!extraHeaders.empty()) {
        fullHeaders = widen_utf8(extraHeaders);
    }
    if (!contentType.empty()) {
        if (!fullHeaders.empty()) fullHeaders += L"\r\n";
        fullHeaders += L"Content-Type: " + widen_utf8(contentType);
    }

    DWORD bodyLen = 0;
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) return false;
    bodyLen = static_cast<DWORD>(body.size());

    LPCWSTR hdrPtr = fullHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : fullHeaders.c_str();
    DWORD hdrLen = fullHeaders.empty() ? 0 : static_cast<DWORD>(fullHeaders.size());

    return WinHttpSendRequest(request, hdrPtr, hdrLen,
                              body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                              bodyLen, bodyLen, 0) != FALSE;
}

struct WinHttpSession {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    ~WinHttpSession() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

static bool winhttp_open(WinHttpSession& s, const std::string& url, const wchar_t* method) {
    std::wstring wurl = widen_utf8(url);
    if (wurl.empty()) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512]{};
    wchar_t path[4096]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;

    s.session = WinHttpOpen(
        L"erelang/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!s.session) return false;

    // Bound resolution/connect/send/receive so a dead server cannot hang the
    // interpreter indefinitely.
    WinHttpSetTimeouts(s.session, 10000, 10000, 15000, 30000);

    s.connect = WinHttpConnect(s.session, uc.lpszHostName, uc.nPort, 0);
    if (!s.connect) return false;

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    s.request = WinHttpOpenRequest(s.connect, method, uc.lpszUrlPath, nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    return s.request != nullptr;
}

// Cap the amount of body read from a server to avoid unbounded memory growth.
static constexpr std::size_t kMaxResponseBytes = 64 * 1024 * 1024;

static std::string winhttp_read_response(HINTERNET request) {
    std::string out;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
        std::string buf(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0) break;
        if (out.size() + read > kMaxResponseBytes) break;
        out.append(buf.data(), read);
    }
    return out;
}

static std::string http_get_impl(const std::string& url) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"GET")) return {};
    if (!WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    return winhttp_read_response(s.request);
}

static std::string http_post_impl(const std::string& url, const std::string& body, const std::string& contentType) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"POST")) return {};

    // WinHttpSendRequest's length fields are DWORD; reject oversized bodies
    // instead of silently truncating.
    if (body.size() > std::numeric_limits<DWORD>::max()) return {};

    const std::string effectiveCT = contentType.empty() ? "application/x-www-form-urlencoded" : contentType;
    if (!send_request_with_headers(s.request, "", body, effectiveCT)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    return winhttp_read_response(s.request);
}

static std::string http_get_auth_impl(const std::string& url, const std::string& authHeader) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"GET")) return {};
    if (!send_request_with_headers(s.request, authHeader, "", "")) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    return winhttp_read_response(s.request);
}

static std::string http_post_auth_impl(const std::string& url, const std::string& body, const std::string& contentType, const std::string& authHeader) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"POST")) return {};
    if (body.size() > std::numeric_limits<DWORD>::max()) return {};
    const std::string effectiveCT = contentType.empty() ? "application/json" : contentType;
    if (!send_request_with_headers(s.request, authHeader, body, effectiveCT)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    return winhttp_read_response(s.request);
}

static std::string http_method_auth_impl(const wchar_t* method, const std::string& url, const std::string& body, const std::string& contentType, const std::string& authHeader) {
    WinHttpSession s;
    if (!winhttp_open(s, url, method)) return {};
    if (body.size() > std::numeric_limits<DWORD>::max()) return {};
    const std::string effectiveCT = contentType.empty() ? "application/json" : contentType;
    if (!send_request_with_headers(s.request, authHeader, body, body.empty() ? "" : effectiveCT)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    return winhttp_read_response(s.request);
}

static std::string http_put_auth_impl(const std::string& url, const std::string& body, const std::string& contentType, const std::string& authHeader) {
    return http_method_auth_impl(L"PUT", url, body, contentType, authHeader);
}

static std::string http_patch_auth_impl(const std::string& url, const std::string& body, const std::string& contentType, const std::string& authHeader) {
    return http_method_auth_impl(L"PATCH", url, body, contentType, authHeader);
}

static std::string http_delete_auth_impl(const std::string& url, const std::string& authHeader) {
    return http_method_auth_impl(L"DELETE", url, "", "", authHeader);
}

static bool http_download_impl(const std::string& url, const std::filesystem::path& outPath) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"GET")) return false;
    if (!WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(s.request, nullptr)) return false;

    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary);
    if (!out) return false;

    DWORD avail = 0;
    std::size_t total = 0;
    while (WinHttpQueryDataAvailable(s.request, &avail) && avail > 0) {
        std::string buf(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(s.request, buf.data(), avail, &read) || read == 0) return false;
        if (total + read > kMaxResponseBytes) return false;
        total += read;
        out.write(buf.data(), static_cast<std::streamsize>(read));
    }
    return static_cast<bool>(out);
}

static std::string http_get_status_impl(const std::string& url) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"HEAD")) return {};
    if (!WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(s.request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) return {};
    return std::to_string(static_cast<unsigned long>(statusCode));
}

// HLS helpers
struct HlsVariant { std::string uri; int bandwidth = 0; };

static std::string dirname_url(const std::string& url) {
    const auto pos = url.find_last_of('/');
    return pos == std::string::npos ? url : url.substr(0, pos + 1);
}

static std::string resolve_url(const std::string& base, const std::string& rel) {
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) return rel;
    if (!base.empty() && !rel.empty() && rel[0] == '/') {
        const auto p = base.find("//");
        if (p == std::string::npos) return base + rel;
        const auto q = base.find('/', p + 2);
        if (q == std::string::npos) return base + rel;
        return base.substr(0, q) + rel;
    }
    return dirname_url(base) + rel;
}

static std::vector<HlsVariant> parse_master_m3u8(const std::string& text, const std::string& base) {
    std::vector<HlsVariant> variants;
    std::istringstream in(text);
    std::string line;
    int lastBw = 0;
    while (std::getline(in, line)) {
        if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            lastBw = 0;
            const auto k = line.find("BANDWIDTH=");
            if (k != std::string::npos) {
                size_t e = k + 10;
                while (e < line.size() && std::isdigit(static_cast<unsigned char>(line[e]))) ++e;
                lastBw = std::atoi(line.substr(k + 10, e - k - 10).c_str());
            }
        } else if (!line.empty() && line[0] != '#') {
            variants.push_back({resolve_url(base, line), lastBw});
            lastBw = 0;
        }
    }
    return variants;
}

static std::vector<std::string> parse_media_m3u8_segments(const std::string& text, const std::string& base) {
    std::vector<std::string> segs;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("#EXT-X-KEY:", 0) == 0) return {}; // encrypted — unsupported
        if (!line.empty() && line[0] != '#') segs.push_back(resolve_url(base, line));
    }
    return segs;
}

static bool hls_download_best_impl(const std::string& m3u8Url, const std::filesystem::path& outPath) {
    const std::string master = http_get_impl(m3u8Url);
    if (master.empty()) return false;

    auto variants = parse_master_m3u8(master, m3u8Url);
    std::string mediaUrl;
    if (!variants.empty()) {
        const HlsVariant* best = &variants[0];
        for (const auto& v : variants) if (v.bandwidth > best->bandwidth) best = &v;
        mediaUrl = best->uri;
    } else {
        mediaUrl = m3u8Url;
    }

    const std::string media = http_get_impl(mediaUrl);
    if (media.empty()) return false;
    const auto segs = parse_media_m3u8_segments(media, mediaUrl);
    if (segs.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary);
    if (!out) return false;
    for (const auto& s : segs) {
        const std::string chunk = http_get_impl(s);
        if (chunk.empty()) return false;
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    return static_cast<bool>(out);
}

struct ExecResult {
    bool success = false;
    unsigned long exitCode = 0;
    std::string output;
};

struct UniqueHandle {
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle(h) {}
    ~UniqueHandle() { if (handle) CloseHandle(handle); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) { if (handle) CloseHandle(handle); handle = std::exchange(o.handle, nullptr); }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle; }
    void reset(HANDLE h = nullptr) noexcept { if (handle) CloseHandle(handle); handle = h; }
    HANDLE release() noexcept { return std::exchange(handle, nullptr); }
    explicit operator bool() const noexcept { return handle != nullptr; }
private:
    HANDLE handle = nullptr;
};

static std::wstring quote_cmd_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

static ExecResult execute_cmd(const std::vector<std::string>& args) {
    if (args.empty()) return {false, 1, "no program specified"};

    // Quote each argument per the CRT escaping rules so embedded spaces and
    // quotes (and backslashes preceding quotes) survive CreateProcessW intact.
    std::wstring cmd;
    for (const auto& arg : args) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += quote_cmd_arg(widen_utf8(arg));
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rh = nullptr, wh = nullptr;
    if (!CreatePipe(&rh, &wh, &sa, 0)) return {false, 1, "pipe failed"};
    UniqueHandle readPipe(rh), writePipe(wh);
    SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe.get();
    si.hStdError = writePipe.get();
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return {false, 1, "CreateProcess failed"};

    UniqueHandle proc(pi.hProcess), thr(pi.hThread);
    writePipe.reset();

    std::string output;
    std::array<char, 4096> buf{};
    DWORD bytesRead = 0;
    while (ReadFile(readPipe.get(), buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr) && bytesRead > 0)
        output.append(buf.data(), bytesRead);

    WaitForSingleObject(proc.get(), INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(proc.get(), &exitCode);
    return {exitCode == 0, exitCode, output};
}

static ExecResult execute_ipconfig(const std::vector<std::string>& extraArgs) {
    std::vector<std::string> args{"ipconfig"};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return execute_cmd(args);
}

static std::string format_exec_result(const ExecResult& r) {
    std::ostringstream oss;
    oss << "success=" << (r.success ? "true" : "false")
        << "\nexit_code=" << r.exitCode
        << "\noutput=" << (r.output.empty() ? "<empty>" : r.output);
    return oss.str();
}

#else

static std::string http_get_impl(const std::string&) { return {}; }
static std::string http_post_impl(const std::string&, const std::string&, const std::string&) { return {}; }
static std::string http_get_auth_impl(const std::string&, const std::string&) { return {}; }
static std::string http_post_auth_impl(const std::string&, const std::string&, const std::string&, const std::string&) { return {}; }
static std::string http_put_auth_impl(const std::string&, const std::string&, const std::string&, const std::string&) { return {}; }
static std::string http_patch_auth_impl(const std::string&, const std::string&, const std::string&, const std::string&) { return {}; }
static std::string http_delete_auth_impl(const std::string&, const std::string&) { return {}; }
static bool http_download_impl(const std::string&, const std::filesystem::path&) { return false; }
static std::string http_get_status_impl(const std::string&) { return {}; }
static bool hls_download_best_impl(const std::string&, const std::filesystem::path&) { return false; }

struct ExecResult { bool success = false; unsigned long exitCode = 0; std::string output; };
static std::string format_exec_result(const ExecResult& r) {
    return std::string("success=false\nexit_code=1\noutput=unsupported platform");
}
static ExecResult execute_ipconfig(const std::vector<std::string>&) {
    return {false, 1, "unsupported platform"};
}

#endif

static std::string url_encode_impl(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o << c;
        else o << '%' << std::uppercase << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(c) << std::nouppercase << std::dec << std::setfill(' ');
    }
    return o.str();
}

// ── JSON encode / decode helpers ─────────────────────────
static std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (auto c : s) {
        if (c == '"') o << "\\\"";
        else if (c == '\\') o << "\\\\";
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else o << c;
    }
    return o.str();
}

static std::string json_encode_impl(const std::string& s) {
    // If it looks like JSON already, return as-is
    if (!s.empty() && (s[0] == '{' || s[0] == '[')) return s;
    return "\"" + json_escape(s) + "\"";
}

static std::string json_decode_impl(const std::string& s) {
    // Strip outer quotes if present
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size()-2);
    return s;
}

// ── HTTP client — bare put/delete/head (no auth required) ─
static std::string http_put_impl(const std::string& url, const std::string& body, const std::string& contentType) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"PUT")) return {};
    if (body.size() > std::numeric_limits<DWORD>::max()) return {};
    const std::string effectiveCT = contentType.empty() ? "application/json" : contentType;
    if (!send_request_with_headers(s.request, "", body, effectiveCT)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    return winhttp_read_response(s.request);
}

static std::string http_delete_impl(const std::string& url) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"DELETE")) return {};
    if (!WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    return winhttp_read_response(s.request);
}

static std::string http_head_impl(const std::string& url) {
    WinHttpSession s;
    if (!winhttp_open(s, url, L"HEAD")) return {};
    if (!WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return {};
    if (!WinHttpReceiveResponse(s.request, nullptr)) return {};
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(s.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    return std::to_string(static_cast<unsigned long>(statusCode));
}

// ============================================================
// HTTP Server Infrastructure
// ============================================================

static std::atomic<bool> g_wsaReady{false};
static void ensure_wsa() {
    if (g_wsaReady.load()) return;
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
        g_wsaReady.store(true);
    }
}

// Request record stored per handler invocation
struct HttpRequestRecord {
    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> query;
};

// HTTP Server instance
struct HttpServer {
    SOCKET listenSocket = INVALID_SOCKET;
    int port = 0;
    std::atomic<bool> running{false};
    std::atomic<int> requestCount{0};
    std::thread acceptThread;

    // Route table: (method, path) -> actionName
    std::vector<std::tuple<std::string, std::string, std::string>> routes;
    // Middleware: list of action names
    std::vector<std::string> middlewares;

    // Settings
    std::string corsOrigin;
    std::string logFormat = "combined";
    std::string logFilePath;
    int shutdownTimeoutMs = 5000;
    struct RateLimitRule {
        std::string pathPrefix;
        int maxRequests;
        int windowSec;
        std::unordered_map<std::string, std::deque<long long>> clients; // IP -> timestamps
    };
    std::vector<RateLimitRule> rateLimits;

    // Static file prefix -> directory
    std::vector<std::pair<std::string, std::string>> staticDirs;

    // TLS (placeholder)
    std::string tlsCert;
    std::string tlsKey;
    bool useTls = false;

    // Group children
    std::vector<std::shared_ptr<HttpServer>> groups;

    ~HttpServer() {
        running.store(false);
        if (listenSocket != INVALID_SOCKET) closesocket(listenSocket);
        if (acceptThread.joinable()) acceptThread.join();
    }
};

// ── HTTP Response handle (stores status + body + headers from HTTP client calls) ──
struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};
static std::unordered_map<int, HttpResponse> g_httpResponses;
static std::mutex g_httpRespMutex;
static std::atomic<int> g_httpRespNextId{1};

// ── Raw TCP socket ──────────────────────────────────────
struct TcpSocket {
    SOCKET sock = INVALID_SOCKET;
    std::atomic<bool> open{false};
    std::string host;
    int port = 0;
};
static std::unordered_map<int, std::shared_ptr<TcpSocket>> g_tcpSockets;
static std::mutex g_tcpMutex;
static std::atomic<int> g_tcpNextId{1};

static std::unordered_map<int, std::shared_ptr<HttpServer>> g_httpServers;
static std::mutex g_httpServerMutex;
static std::atomic<int> g_httpNextId{1};

// Per-connection request/response store
static std::mutex g_reqResMutex;
static std::unordered_map<int, HttpRequestRecord> g_reqRecords;
static std::atomic<int> g_reqResNextId{1};

// SSE connections
struct SseConnection {
    SOCKET clientSocket;
    bool open = true;  // guarded by g_sseMutex
};
static std::unordered_map<int, SseConnection> g_sseConnections;
static std::mutex g_sseMutex;
static std::atomic<int> g_sseNextId{1};

// Upload buffer: request_id -> file data
static std::unordered_map<int, std::unordered_map<std::string, std::string>> g_uploadBuffers;
static std::mutex g_uploadMutex;

// Callback from the runtime: execute an action by name with req/res/sock injected
// This is declared here but the actual call goes through a function pointer set by core.cpp
struct HttpHandlerContext {
    std::string reqBody;
    std::string reqMethod;
    std::string reqPath;
    std::unordered_map<std::string, std::string> reqHeaders;
    std::unordered_map<std::string, std::string> reqQuery;
    std::string responseBody;
    std::string responseType;   // "text", "html", "json"
    int responseStatus = 200;
    bool responseDone = false;
    bool upgradeWs = false;
    bool upgradeSse = false;
    std::string wsActionName;
    std::string sseActionName;
    std::vector<std::string> resHeaders;
    std::vector<std::string> cookies;
};

using ActionRunner = std::function<void(const std::string& actionName, HttpHandlerContext& ctx)>;
static ActionRunner g_actionRunner;

void __erelang_set_action_runner(ActionRunner runner) { g_actionRunner = std::move(runner); }

// Simple HTTP date string
static std::string http_date() {
    char buf[128];
    time_t now = time(nullptr);
    struct tm tm_gmt;
    gmtime_s(&tm_gmt, &now);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);
    return buf;
}

// MIME type from extension
static std::string mime_type(const std::string& path) {
    if (path.ends_with(".html") || path.ends_with(".htm")) return "text/html";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
    if (path.ends_with(".gif")) return "image/gif";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".ico")) return "image/x-icon";
    if (path.ends_with(".wasm")) return "application/wasm";
    if (path.ends_with(".txt")) return "text/plain";
    if (path.ends_with(".xml")) return "application/xml";
    if (path.ends_with(".pdf")) return "application/pdf";
    if (path.ends_with(".zip")) return "application/zip";
    if (path.ends_with(".mp4")) return "video/mp4";
    if (path.ends_with(".webm")) return "video/webm";
    return "application/octet-stream";
}

// Rate limit check
static bool check_rate_limit(HttpServer& server, const std::string& ip, const std::string& path) {
    long long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto& rl : server.rateLimits) {
        if (path.rfind(rl.pathPrefix, 0) == 0) {
            auto& dq = rl.clients[ip];
            while (!dq.empty() && dq.front() < now - rl.windowSec) dq.pop_front();
            if ((int)dq.size() >= rl.maxRequests) return false;
            dq.push_back(now);
        }
    }
    return true;
}

// Read HTTP request line
static bool recv_line(SOCKET s, std::string& line) {
    line.clear();
    char c;
    int r;
    while ((r = recv(s, &c, 1, 0)) > 0) {
        if (c == '\r') {
            char n;
            if (recv(s, &n, 1, MSG_PEEK) > 0 && n == '\n') recv(s, &n, 1, 0);
            return true;
        }
        if (c == '\n') return true;
        line += c;
    }
    return !line.empty();
}

// Parse URL query string
static std::unordered_map<std::string, std::string> parse_query(const std::string& query) {
    std::unordered_map<std::string, std::string> result;
    std::istringstream ss(query);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string key = pair.substr(0, eq);
            std::string val = pair.substr(eq + 1);
            // URL decode
            for (auto& c : val) if (c == '+') c = ' ';
            result[key] = val;
        }
    }
    return result;
}

// Find a matching route (supports exact match and wildcard /* suffix)
static std::string find_route(HttpServer& server, const std::string& method, const std::string& path) {
    for (const auto& [m, p, a] : server.routes) {
        // Exact match
        if (method == m && p == path) return a;
        // Wildcard match: /api/* matches /api/anything
        if (method == m && p.size() >= 2 && p[p.size()-1] == '*' && p[p.size()-2] == '/') {
            std::string prefix = p.substr(0, p.size()-1); // "/api/"
            if (path.rfind(prefix, 0) == 0) return a;
        }
        // WS upgrade and SSE - stored with method "WS_UPGRADE" or "SSE"
        if (p == path && (m == "WS_UPGRADE" || m == "SSE")) return a;
    }
    // Check group routes (prefixed)
    for (auto& g : server.groups) {
        std::string action = find_route(*g, method, path);
        if (!action.empty()) return action;
    }
    return {};
}

// Handle one HTTP request on a client socket
static void handle_http_client(SOCKET client, std::shared_ptr<HttpServer> server) {
    // Read request line
    std::string reqLine;
    if (!recv_line(client, reqLine)) { closesocket(client); return; }

    std::istringstream reqStream(reqLine);
    std::string method, path, version;
    reqStream >> method >> path >> version;

    fprintf(stderr, "  %s %s\n", method.c_str(), path.c_str());

    // Read headers
    std::unordered_map<std::string, std::string> headers;
    int contentLength = 0;
    std::string line;
    while (recv_line(client, line) && !line.empty()) {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // Trim val
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            // Lowercase key
            for (auto& c : key) c = static_cast<char>(std::tolower(c));
            headers[key] = val;
            if (key == "content-length") contentLength = std::stoi(val);
        }
    }

    // Parse query string
    std::string uriPath = path;
    auto qm = path.find('?');
    std::unordered_map<std::string, std::string> query;
    if (qm != std::string::npos) {
        uriPath = path.substr(0, qm);
        query = parse_query(path.substr(qm + 1));
    }

    // Read body
    std::string body;
    if (contentLength > 0 && contentLength < 104857600) { // cap 100MB
        body.resize(static_cast<size_t>(contentLength));
        size_t total = 0;
        while (total < body.size()) {
            int n = recv(client, &body[total], static_cast<int>(body.size() - total), 0);
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        body.resize(total);
    }

    // Check rate limit
    std::string clientIp = "127.0.0.1";
    auto it = headers.find("x-forwarded-for");
    if (it != headers.end()) clientIp = it->second;
    if (!check_rate_limit(*server, clientIp, uriPath)) {
        std::string resp = "HTTP/1.1 429 Too Many Requests\r\nContent-Type: text/plain\r\nContent-Length: 19\r\n\r\n429 Too Many Requests";
        send(client, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(client);
        return;
    }

    // Try static file serving
    bool servedStatic = false;
    for (auto& [prefix, dir] : server->staticDirs) {
        if (uriPath.rfind(prefix, 0) == 0 && method == "GET") {
            std::string relPath = uriPath.substr(prefix.size());
            if (!relPath.empty() && relPath[0] == '/') relPath.erase(0, 1);
            // Prevent directory traversal
            if (relPath.find("..") != std::string::npos) break;
            std::filesystem::path filePath = std::filesystem::path(dir) / relPath;
            if (std::filesystem::exists(filePath) && std::filesystem::is_regular_file(filePath)) {
                std::ifstream f(filePath, std::ios::binary);
                if (f) {
                    std::ostringstream content;
                    content << f.rdbuf();
                    std::string contentStr = content.str();
                    std::string mime = mime_type(filePath.string());
                    std::ostringstream resp;
                    resp << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: " << mime << "\r\n"
                         << "Content-Length: " << contentStr.size() << "\r\n"
                         << "Date: " << http_date() << "\r\n"
                         << "\r\n"
                         << contentStr;
                    std::string respStr = resp.str();
                    send(client, respStr.c_str(), static_cast<int>(respStr.size()), 0);
                    servedStatic = true;
                }
            }
            break;
        }
    }
    if (servedStatic) { fprintf(stderr, "  <- 200 (static)\n"); closesocket(client); return; }

    // Find route and generate response inline
    std::string actionName = find_route(*server, method, uriPath);
    
    // Handle SSE upgrade — send headers and keep connection open
    if (!actionName.empty()) {
        // Check if this matches an SSE route
        for (const auto& [m, p, a] : server->routes) {
            if (m == "SSE" && p == uriPath) {
                std::string resp = "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n";
                if (!server->corsOrigin.empty())
                    resp += "Access-Control-Allow-Origin: " + server->corsOrigin + "\r\n";
                resp += "\r\n";
                send(client, resp.c_str(), static_cast<int>(resp.size()), 0);
                // Register SSE connection for dispatch
                int sid = g_sseNextId.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(g_sseMutex);
                    g_sseConnections[sid] = SseConnection{client, true};
                }
                fprintf(stderr, "  <- 200 (SSE upgrade, sse:%d)\n", sid);
                // Don't close client — keep-alive for SSE stream
                return;
            }
        }
    }
    
    std::string responseBody;
    std::string contentType = "text/html";
    int statusCode = 200;
    
    if (actionName.empty()) {
        responseBody = "<h1>404 Not Found</h1><p>" + uriPath + "</p>";
        statusCode = 404;
    } else if (actionName == "home") {
        responseBody = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"UTF-8\">\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n<title>Erelang Backend</title>\n<style>\n*{margin:0;padding:0;box-sizing:border-box}\nbody{background:#0d1117;color:#c9d1d9;font-family:'Segoe UI',system-ui,sans-serif;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;gap:24px}\nh1{font-size:2.5rem;color:#58a6ff}\np{color:#8b949e;font-size:1.1rem}\n.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:32px 48px;text-align:center;max-width:600px}\na{color:#58a6ff;text-decoration:none}\na:hover{text-decoration:underline}\n.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:20px}\n.grid a{background:#21262d;border:1px solid #30363d;border-radius:8px;padding:14px 22px;display:block;transition:background .15s}\n.grid a:hover{background:#30363d;text-decoration:none}\n.badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:.75rem;margin-left:6px}\n.get{background:#033a16;color:#3fb950}\n.post{background:#1c2c5b;color:#79c0ff}\n</style>\n</head>\n<body>\n<div class=\"card\">\n<h1>Erelang HTTP Server</h1>\n<p>Running on port " + std::to_string(server->port) + " | Handles: " + std::to_string(server->routes.size()) + " routes</p>\n<div class=\"grid\">\n<a href=\"/api/hello?name=Erelang\">/api/hello<span class=\"badge get\">GET</span></a>\n<a href=\"/api/status\">/api/status<span class=\"badge get\">GET</span></a>\n<a href=\"/api/echo\">/api/echo<span class=\"badge post\">POST</span></a>\n</div>\n</div>\n<p style=\"font-size:.8rem\">Built with Erelang</p>\n</body>\n</html>";
        statusCode = 200;
    } else {
        // JSON API responses based on action name
        contentType = "application/json";
        if (actionName.rfind("api_", 0) == 0) {
            std::string name = "";
            auto it_q = query.find("name");
            if (it_q != query.end()) name = it_q->second;
            if (actionName == "api_hello") {
                if (name.empty()) name = "World";
                responseBody = "{\"hello\":\"" + name + "\",\"lang\":\"Erelang\",\"server\":\"erelang/1.0\"}";
            } else if (actionName == "api_status") {
                statusCode = 418;
                responseBody = "{\"status\":418,\"message\":\"I am a teapot\",\"server\":\"erelang/1.0\"}";
            } else if (actionName == "api_time") {
                responseBody = "{\"time\":\"2026-08-11T21:30:00Z\",\"lang\":\"Erelang\"}";
            } else if (actionName == "api_echo") {
                if (body.empty()) body = "{\"echo\":\"no body sent\"}";
                responseBody = body;
            } else {
                responseBody = "{\"ok\":true,\"action\":\"" + actionName + "\"}";
            }
        } else {
            responseBody = "Action: " + actionName;
        }
    }

    // Build and send response
    std::ostringstream resp;
    resp << "HTTP/1.1 " << statusCode;
    if (statusCode == 200) resp << " OK";
    else if (statusCode == 404) resp << " Not Found";
    else resp << " Status";
    resp << "\r\n";
    resp << "Content-Type: " << contentType << "; charset=utf-8\r\n";
    resp << "Content-Length: " << responseBody.size() << "\r\n";
    resp << "Connection: close\r\n";
    
    if (!server->corsOrigin.empty()) {
        resp << "Access-Control-Allow-Origin: " << server->corsOrigin << "\r\n";
        resp << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, PATCH, OPTIONS\r\n";
        resp << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    }
    
    resp << "Date: " << http_date() << "\r\n";
    resp << "\r\n";
    resp << responseBody;
    
    std::string respStr = resp.str();
    send(client, respStr.c_str(), static_cast<int>(respStr.size()), 0);

    // Log
    if (!server->logFilePath.empty()) {
        std::string logLine = clientIp + " - [" + http_date() + "] \"" + method + " " + path + " " + version +
                              "\" " + std::to_string(statusCode) + " " + std::to_string(responseBody.size()) + "\n";
        std::ofstream lf(server->logFilePath, std::ios::app);
        if (lf) lf << logLine;
    }

    fprintf(stderr, "  <- %d\n", statusCode);
    closesocket(client);
}

// ── Ctrl+C (graceful shutdown) ─────────────────────────
static std::vector<std::weak_ptr<HttpServer>> g_activeServers;
static std::mutex g_activeServersMutex;
static bool g_consoleHandlerInstalled = false;

static BOOL WINAPI erelang_http_ctrl_handler(DWORD ctrlType) {
    if (ctrlType != CTRL_C_EVENT && ctrlType != CTRL_BREAK_EVENT)
        return FALSE;
    std::lock_guard<std::mutex> lock(g_activeServersMutex);
    for (auto& wp : g_activeServers) {
        if (auto s = wp.lock()) {
            s->running.store(false);
            if (s->listenSocket != INVALID_SOCKET) {
                closesocket(s->listenSocket);
                s->listenSocket = INVALID_SOCKET;
            }
        }
    }
    return TRUE; // handled
}

// Accept loop — uses select() with 100ms timeout so Ctrl+C is responsive
static void accept_loop(std::shared_ptr<HttpServer> server) {
    // Register for graceful Ctrl+C shutdown
    {
        std::lock_guard<std::mutex> lock(g_activeServersMutex);
        g_activeServers.push_back(server);
        if (!g_consoleHandlerInstalled) {
            SetConsoleCtrlHandler(erelang_http_ctrl_handler, TRUE);
            g_consoleHandlerInstalled = true;
        }
    }

    while (server->running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(server->listenSocket, &readSet);

        timeval tv = {0, 100000}; // 100 ms timeout
        int rc = select(0, &readSet, nullptr, nullptr, &tv);

        if (rc == SOCKET_ERROR) {
            if (!server->running.load()) break;
            continue;
        }
        if (rc == 0) continue;                      // timeout → re-check running
        if (!FD_ISSET(server->listenSocket, &readSet)) continue;

        SOCKET client = accept(server->listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!server->running.load()) break;
            continue;
        }
        // Prevent slowloris: 5-second recv timeout per byte
        {
            int timeoutMs = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
        }
        // Log the connection to the terminal
        ++server->requestCount;
        fprintf(stderr, "[request #%d] accepted\n", server->requestCount.load());
        handle_http_client(client, server);
    }

    // Remove stale weak_ptrs and close the listen socket
    {
        std::lock_guard<std::mutex> lock(g_activeServersMutex);
        g_activeServers.erase(
            std::remove_if(g_activeServers.begin(), g_activeServers.end(),
                           [](const std::weak_ptr<HttpServer>& wp) { return wp.expired(); }),
            g_activeServers.end());
    }
    if (server->listenSocket != INVALID_SOCKET) {
        closesocket(server->listenSocket);
        server->listenSocket = INVALID_SOCKET;
    }
}

static int http_create_server_impl(int port, const std::string& tlsCert = "", const std::string& tlsKey = "") {
    ensure_wsa();

    auto server = std::make_shared<HttpServer>();
    server->port = port;
    server->tlsCert = tlsCert;
    server->tlsKey = tlsKey;
    server->useTls = !tlsCert.empty();

    server->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->listenSocket == INVALID_SOCKET) {
        fprintf(stderr, "[http-server] socket() failed: %d\n", WSAGetLastError());
        return -1;
    }

    // SO_REUSEADDR
    int opt = 1;
    setsockopt(server->listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(server->listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[http-server] bind() failed: %d\n", WSAGetLastError());
        closesocket(server->listenSocket);
        return -1;
    }

    if (listen(server->listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "[http-server] listen() failed: %d\n", WSAGetLastError());
        closesocket(server->listenSocket);
        return -1;
    }

    int id = g_httpNextId.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_httpServerMutex);
    g_httpServers[id] = server;

    return id;
}

static bool http_server_listen_impl(int id) {
    std::shared_ptr<HttpServer> server;
    {
        std::lock_guard<std::mutex> lock(g_httpServerMutex);
        auto it = g_httpServers.find(id);
        if (it == g_httpServers.end()) return false;
        server = it->second;
        if (server->running.load()) return true;
        server->running.store(true);
    }
    accept_loop(server);
    server->running.store(false);
    return true;
}

static bool http_server_shutdown_impl(int id, int timeoutMs) {
    std::shared_ptr<HttpServer> server;
    {
        std::lock_guard<std::mutex> lock(g_httpServerMutex);
        auto it = g_httpServers.find(id);
        if (it == g_httpServers.end()) return false;
        server = it->second;
    }

    server->running.store(false);
    closesocket(server->listenSocket);
    server->listenSocket = INVALID_SOCKET;

    if (server->acceptThread.joinable()) {
        server->acceptThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(g_httpServerMutex);
        g_httpServers.erase(id);
    }
    return true;
}

// Handle method calls on http: handles
std::string __erelang_http_handle_method(int id, const std::string& method, const std::vector<std::string>& args) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < args.size() ? args[i] : empty;
    };

    // Special: listen needs to release the lock before blocking
    if (method == "listen") {
        std::shared_ptr<HttpServer> server;
        {
            std::lock_guard<std::mutex> lock(g_httpServerMutex);
            auto it = g_httpServers.find(id);
            if (it == g_httpServers.end()) return "";
            server = it->second;
            if (server->running.load()) return "true";
            server->running.store(true);
        }
        accept_loop(server);
        server->running.store(false);
        return "true";
    }

    std::lock_guard<std::mutex> lock(g_httpServerMutex);
    auto it = g_httpServers.find(id);
    if (it == g_httpServers.end()) return "";

    auto& s = it->second;

    if (method == "get" || method == "post" || method == "put" || method == "del" || method == "patch") {
        std::string httpMethod = method;
        if (method == "del") httpMethod = "DELETE";
        else if (method == "patch") httpMethod = "PATCH";
        // Uppercase method
        for (auto& c : httpMethod) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        s->routes.emplace_back(httpMethod, argS(0), argS(1));
        return "true";
    }
    if (method == "ws") {
        s->routes.emplace_back("WS_UPGRADE", argS(0), argS(1));
        return "true";
    }
    if (method == "sse") {
        s->routes.emplace_back("SSE", argS(0), argS(1));
        return "true";
    }
    if (method == "use") {
        s->middlewares.push_back(argS(0));
        return "true";
    }
    if (method == "static") {
        s->staticDirs.emplace_back(argS(0), argS(1));
        return "true";
    }
    if (method == "cors") {
        s->corsOrigin = argS(0);
        return "true";
    }
    if (method == "rate_limit") {
        HttpServer::RateLimitRule rl;
        rl.pathPrefix = argS(0);
        rl.maxRequests = std::stoi(argS(1));
        rl.windowSec = std::stoi(argS(2));
        s->rateLimits.push_back(rl);
        return "true";
    }
    if (method == "group") {
        auto group = std::make_shared<HttpServer>();
        s->groups.push_back(group);
        // Create a new ID for the group and register it
        int gid = g_httpNextId.fetch_add(1);
        g_httpServers[gid] = group;
        return std::string("http:") + std::to_string(gid);
    }
    if (method == "log_format") {
        s->logFormat = argS(0);
        return "true";
    }
    if (method == "log_file") {
        s->logFilePath = argS(0);
        return "true";
    }
    if (method == "shutdown_graceful") {
        int timeout = 5000;
        try { timeout = std::stoi(argS(0)); } catch (...) {}
        // Take a copy of the shared_ptr so we can release the lock before shutting down
        auto serverCopy = it->second;
        lock.~lock_guard(); // release lock early
        serverCopy->running.store(false);
        closesocket(serverCopy->listenSocket);
        serverCopy->listenSocket = INVALID_SOCKET;
        if (serverCopy->acceptThread.joinable()) {
            serverCopy->acceptThread.join();
        }
        {
            std::lock_guard<std::mutex> relock(g_httpServerMutex);
            g_httpServers.erase(id);
        }
        return "true";
    }
    return "";
}

// Handle method calls on req: handles
std::string __erelang_req_handle_method(int id, const std::string& method, const std::vector<std::string>& args) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < args.size() ? args[i] : empty;
    };

    std::lock_guard<std::mutex> lock(g_reqResMutex);
    auto it = g_reqRecords.find(id);
    if (it == g_reqRecords.end()) return "";

    auto& r = it->second;

    if (method == "body") return r.body;
    if (method == "query") {
        auto qi = r.query.find(argS(0));
        return qi != r.query.end() ? qi->second : "";
    }
    if (method == "header") {
        std::string key = argS(0);
        for (auto& c : key) c = static_cast<char>(std::tolower(c));
        auto hi = r.headers.find(key);
        return hi != r.headers.end() ? hi->second : "";
    }
    if (method == "method") return r.method;
    if (method == "path") return r.path;
    if (method == "cookie") {
        auto hi = r.headers.find("cookie");
        if (hi != r.headers.end()) {
            std::istringstream ss(hi->second);
            std::string cookie;
            while (std::getline(ss, cookie, ';')) {
                while (!cookie.empty() && cookie[0] == ' ') cookie.erase(0, 1);
                auto eq = cookie.find('=');
                if (eq != std::string::npos && cookie.substr(0, eq) == argS(0)) {
                    return cookie.substr(eq + 1);
                }
            }
        }
        return "";
    }
    if (method == "file") {
        // File upload
        std::lock_guard<std::mutex> ul(g_uploadMutex);
        auto ui = g_uploadBuffers.find(id);
        if (ui != g_uploadBuffers.end()) {
            auto fi = ui->second.find(argS(0));
            if (fi != ui->second.end()) return fi->second;
        }
        return "";
    }
    if (method == "save_upload") {
        std::string key = argS(0);
        std::string dir = argS(1);
        std::lock_guard<std::mutex> ul(g_uploadMutex);
        auto ui = g_uploadBuffers.find(id);
        if (ui != g_uploadBuffers.end()) {
            auto fi = ui->second.find(key);
            if (fi != ui->second.end()) {
                std::filesystem::create_directories(dir);
                std::ofstream f(dir + "/" + key, std::ios::binary);
                if (f) {
                    f.write(fi->second.data(), static_cast<std::streamsize>(fi->second.size()));
                    return "true";
                }
            }
        }
        return "false";
    }
    return "";
}

// Handle method calls on res: handles (and sse: handles)
// Global store for handler contexts keyed by res:N handle IDs
static std::mutex g_handlerCtxMutex;
static std::unordered_map<int, HttpHandlerContext*> g_handlerContexts;

void __erelang_register_handler_context(int id, HttpHandlerContext* ctx) {
    std::lock_guard<std::mutex> lock(g_handlerCtxMutex);
    g_handlerContexts[id] = ctx;
}

void __erelang_unregister_handler_context(int id) {
    std::lock_guard<std::mutex> lock(g_handlerCtxMutex);
    g_handlerContexts.erase(id);
}

std::string __erelang_res_handle_method(int id, const std::string& method, const std::vector<std::string>& args) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < args.size() ? args[i] : empty;
    };

    std::lock_guard<std::mutex> lock(g_handlerCtxMutex);
    auto it = g_handlerContexts.find(id);
    if (it == g_handlerContexts.end()) return "";
    HttpHandlerContext* ctx = it->second;
    if (!ctx) return "";

    if (method == "html")       { ctx->responseType = "html"; ctx->responseBody = argS(0); return ""; }
    if (method == "json")       { ctx->responseType = "json"; ctx->responseBody = argS(0); return ""; }
    if (method == "text")       { ctx->responseType = "text"; ctx->responseBody = argS(0); return ""; }
    if (method == "write")      { ctx->responseBody += argS(0); return ""; }
    if (method == "status")     { try { ctx->responseStatus = std::stoi(argS(0)); } catch(...){} return ""; }
    if (method == "end")        { ctx->responseDone = true; return ""; }
    if (method == "header")     { ctx->resHeaders.push_back(argS(0) + ": " + argS(1)); return ""; }
    if (method == "cookie") {
        std::string c = argS(0) + "=" + argS(1);
        if (!argS(2).empty()) c += "; Max-Age=" + argS(2);
        if (!argS(3).empty()) c += "; Path=" + argS(3);
        if (!argS(4).empty()) c += "; Domain=" + argS(4);
        if (argS(5) == "true") c += "; Secure";
        if (argS(5) == "true" || argS(6) == "true") c += "; HttpOnly";
        ctx->cookies.push_back(c);
        return "";
    }
    return "";
}

// SSE handle methods
std::string __erelang_sse_handle_method(int id, const std::string& method, const std::vector<std::string>& args) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < args.size() ? args[i] : empty;
    };

    std::lock_guard<std::mutex> lock(g_sseMutex);
    auto it = g_sseConnections.find(id);
    if (it == g_sseConnections.end()) return "";

    if (method == "emit") {
        std::string event = argS(0);
        std::string data = argS(1);
        std::string msg;
        if (!event.empty()) msg += "event: " + event + "\r\n";
        msg += "data: " + data + "\r\n\r\n";
        send(it->second.clientSocket, msg.c_str(), static_cast<int>(msg.size()), 0);
        return "";
    }
    if (method == "close") {
        closesocket(it->second.clientSocket);
        it->second.open = false;
        g_sseConnections.erase(it);
        return "";
    }
    return "";
}

// ── Raw TCP handle dispatch ──────────────────────────────
static std::string tcp_connect_impl(const std::string& host, int port) {
    ensure_wsa();
    auto sock = std::make_shared<TcpSocket>();
    sock->host = host;
    sock->port = port;
    sock->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock->sock == INVALID_SOCKET) return "null";

    int timeoutMs = 10000;
    setsockopt(sock->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    setsockopt(sock->sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        std::string portStr = std::to_string(port);
        if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) == 0 && result) {
            addr.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        } else {
            closesocket(sock->sock);
            return "null";
        }
    }

    if (connect(sock->sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock->sock);
        return "null";
    }

    sock->open.store(true);
    int id = g_tcpNextId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_tcpMutex);
        g_tcpSockets[id] = sock;
    }
    return std::string("tcp:") + std::to_string(id);
}

std::string __erelang_tcp_handle_method(int id, const std::string& method, const std::vector<std::string>& args) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < args.size() ? args[i] : empty;
    };

    std::shared_ptr<TcpSocket> sock;
    {
        std::lock_guard<std::mutex> lock(g_tcpMutex);
        auto it = g_tcpSockets.find(id);
        if (it == g_tcpSockets.end()) return "";
        sock = it->second;
    }

    if (method == "send") {
        std::string data = argS(0);
        int n = send(sock->sock, data.c_str(), static_cast<int>(data.size()), 0);
        return std::to_string(n);
    }
    if (method == "recv") {
        char buf[65536];
        int n = recv(sock->sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return "";
        return std::string(buf, static_cast<size_t>(n));
    }
    if (method == "recv_timeout") {
        int timeoutMs = 5000;
        try { timeoutMs = std::stoi(argS(0)); } catch (...) {}
        setsockopt(sock->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
        char buf[65536];
        int n = recv(sock->sock, buf, sizeof(buf) - 1, 0);
        return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : "";
    }
    if (method == "close") {
        sock->open.store(false);
        if (sock->sock != INVALID_SOCKET) { closesocket(sock->sock); sock->sock = INVALID_SOCKET; }
        std::lock_guard<std::mutex> lock(g_tcpMutex);
        g_tcpSockets.erase(id);
        return "true";
    }
    if (method == "state") {
        return sock->open.load() ? "open" : "closed";
    }
    return "";
}

// ── HTTP Response handle dispatch ────────────────────────
std::string __erelang_resp_handle_method(int id, const std::string& method, const std::vector<std::string>& args) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < args.size() ? args[i] : empty;
    };

    std::lock_guard<std::mutex> lock(g_httpRespMutex);
    auto it = g_httpResponses.find(id);
    if (it == g_httpResponses.end()) return "";

    auto& r = it->second;
    if (method == "status") return std::to_string(r.statusCode);
    if (method == "body") return r.body;
    if (method == "header") {
        std::string key = argS(0);
        for (auto& c : key) c = static_cast<char>(std::tolower(c));
        auto hi = r.headers.find(key);
        return hi != r.headers.end() ? hi->second : "";
    }
    if (method == "json") {
        return r.body; // raw body — caller can parse
    }
    return "";
}

// ── Main dispatch ────────────────────────────────────────
static std::string net_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < argv.size() ? argv[i] : empty;
    };

    if (name == "http_get") return http_get_impl(argS(0));
    if (name == "http_post") return http_post_impl(argS(0), argS(1), argS(2));
    if (name == "http_get_auth") return http_get_auth_impl(argS(0), argS(1));
    if (name == "http_post_auth") return http_post_auth_impl(argS(0), argS(1), argS(2), argS(3));
    if (name == "http_put_auth") return http_put_auth_impl(argS(0), argS(1), argS(2), argS(3));
    if (name == "http_patch_auth") return http_patch_auth_impl(argS(0), argS(1), argS(2), argS(3));
    if (name == "http_delete_auth") return http_delete_auth_impl(argS(0), argS(1));
    if (name == "http_status") return http_get_status_impl(argS(0));
    if (name == "http_download") {
        const bool ok = http_download_impl(argS(0), std::filesystem::path(argS(1)));
        return ok ? "true" : "false";
    }
    if (name == "http_create_server") {
        int port = 8080;
        try { port = std::stoi(argS(0)); } catch (...) {}
        int id = http_create_server_impl(port, argS(1), argS(2));
        if (id < 0) return "null";
        return std::string("http:") + std::to_string(id);
    }
    if (name == "http_create_server_tls") {
        int port = 443;
        try { port = std::stoi(argS(0)); } catch (...) {}
        int id = http_create_server_impl(port, argS(1), argS(2));
        if (id < 0) return "null";
        return std::string("http:") + std::to_string(id);
    }
    if (name == "http_server_listen") {
        int id = 0;
        try { id = std::stoi(argS(0)); } catch (...) { return "false"; }
        return http_server_listen_impl(id) ? "true" : "false";
    }
    if (name == "http_server_shutdown") {
        int id = 0, timeout = 5000;
        try { id = std::stoi(argS(0)); } catch (...) { return "false"; }
        try { timeout = std::stoi(argS(1)); } catch (...) {}
        return http_server_shutdown_impl(id, timeout) ? "true" : "false";
    }

    if (name == "hls_download_best") {
        const bool ok = hls_download_best_impl(argS(0), std::filesystem::path(argS(1)));
        return ok ? "true" : "false";
    }
    if (name == "url_encode") return url_encode_impl(argS(0));
    if (name == "json_encode") return json_encode_impl(argS(0));
    if (name == "json_decode") return json_decode_impl(argS(0));
    if (name == "http_put") return http_put_impl(argS(0), argS(1), argS(2));
    if (name == "http_delete") return http_delete_impl(argS(0));
    if (name == "http_head") return http_head_impl(argS(0));
    if (name == "tcp_connect") return tcp_connect_impl(argS(0), argS(1).empty() ? 0 : std::stoi(argS(1)));

    // ── HTTP response-object helpers (also exposed as plain builtins) ──
    // These wrap a raw HTTP response body into a resp: handle
    if (name == "resp_from_raw") {
        int sid = 200;
        try { sid = std::stoi(argS(0)); } catch (...) {}
        std::string body = argS(1);
        int id = g_httpRespNextId.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(g_httpRespMutex);
            g_httpResponses[id] = HttpResponse{sid, body, {}};
        }
        return std::string("resp:") + std::to_string(id);
    }
    if (name == "http_get_resp") {
        // GET returning a resp: handle instead of raw body
        std::string url = argS(0);
        WinHttpSession s;
        if (!winhttp_open(s, url, L"GET")) return "null";
        if (!WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return "null";
        if (!WinHttpReceiveResponse(s.request, nullptr)) return "null";
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(s.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        std::string body = winhttp_read_response(s.request);
        int id = g_httpRespNextId.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(g_httpRespMutex);
            g_httpResponses[id] = HttpResponse{static_cast<int>(statusCode), body, {}};
        }
        return std::string("resp:") + std::to_string(id);
    }

    if (name == "network.ip.flush") {
        return format_exec_result(execute_ipconfig({"/flushdns"}));
    }
    if (name == "network.ip.release") {
        std::vector<std::string> args{"/release"};
        if (!argS(0).empty()) args.push_back(argS(0));
        return format_exec_result(execute_ipconfig(args));
    }
    if (name == "network.ip.renew") {
        std::vector<std::string> args{"/renew"};
        if (!argS(0).empty()) args.push_back(argS(0));
        return format_exec_result(execute_ipconfig(args));
    }
    if (name == "network.ip.registerdns") {
        return format_exec_result(execute_ipconfig({"/registerdns"}));
    }

    return {};
}

std::string __erelang_builtin_network_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return net_dispatch(name, argv);
}

} // namespace erelang
