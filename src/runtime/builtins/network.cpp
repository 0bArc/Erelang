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
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
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
    if (name == "hls_download_best") {
        const bool ok = hls_download_best_impl(argS(0), std::filesystem::path(argS(1)));
        return ok ? "true" : "false";
    }
    if (name == "url_encode") return url_encode_impl(argS(0));

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
