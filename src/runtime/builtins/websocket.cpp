// WebSocket client builtin for erelang
// Uses WinHTTP WebSocket API on Windows, or platform stubs elsewhere.
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>

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

struct WinHttpHandle {
    HINTERNET handle = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : handle(h) {}
    ~WinHttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& o) noexcept : handle(o.handle) { o.handle = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        if (this != &o) { if (handle) WinHttpCloseHandle(handle); handle = o.handle; o.handle = nullptr; }
        return *this;
    }
    operator HINTERNET() const { return handle; }
    bool valid() const { return handle != nullptr; }
};

struct WebSocketConnection {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle wsHandle;
    std::string url;
    std::mutex sendMutex;
    std::mutex recvMutex;
    bool closed = false;
    std::atomic<int> refCount{1};

    void addRef() { ++refCount; }
    void release() { if (--refCount == 0) delete this; }
};

static std::unordered_map<int, WebSocketConnection*> g_wsConnections;
static std::mutex g_wsMutex;
static std::atomic<int> g_wsNextId{1};

static WebSocketConnection* ws_get_connection(int id) {
    std::lock_guard<std::mutex> lock(g_wsMutex);
    auto it = g_wsConnections.find(id);
    if (it != g_wsConnections.end()) {
        it->second->addRef();
        return it->second;
    }
    return nullptr;
}

static int ws_register(WebSocketConnection* conn) {
    int id = g_wsNextId.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_wsMutex);
    g_wsConnections[id] = conn;
    return id;
}

static void ws_unregister(int id) {
    std::lock_guard<std::mutex> lock(g_wsMutex);
    auto it = g_wsConnections.find(id);
    if (it != g_wsConnections.end()) {
        it->second->release();
        g_wsConnections.erase(it);
    }
}

static int ws_connect_impl(const std::string& url) {
    // WinHttpCrackUrl doesn't recognize wss:// as HTTPS. Normalize wss:// -> https://
    std::string normalizedUrl = url;
    if (normalizedUrl.rfind("wss://", 0) == 0) {
        normalizedUrl = "https://" + normalizedUrl.substr(6);
    } else if (normalizedUrl.rfind("ws://", 0) == 0) {
        normalizedUrl = "http://" + normalizedUrl.substr(5);
    }
    std::wstring wurl = widen_utf8(normalizedUrl);
    if (wurl.empty()) { fprintf(stderr, "[ws] widen_utf8 failed\n"); return -1; }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512]{};
    wchar_t path[4096]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        fprintf(stderr, "[ws] WinHttpCrackUrl failed: %lu\n", GetLastError());
        return -1;
    }
    // Note: keeping only error-level stderr output

    auto conn = new WebSocketConnection();
    conn->url = url;

    conn->session.handle = WinHttpOpen(
        L"erelang-ws/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!conn->session.valid()) { fprintf(stderr, "[ws] WinHttpOpen failed: %lu\n", GetLastError()); delete conn; return -1; }

    conn->connection.handle = WinHttpConnect(conn->session, uc.lpszHostName, uc.nPort, 0);
    if (!conn->connection.valid()) { fprintf(stderr, "[ws] WinHttpConnect failed: %lu\n", GetLastError()); delete conn; return -1; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET req = WinHttpOpenRequest(
        conn->connection, L"GET", uc.lpszUrlPath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { fprintf(stderr, "[ws] WinHttpOpenRequest failed: %lu\n", GetLastError()); delete conn; return -1; }

    // Set WebSocket upgrade option before sending request.
    // WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET takes no arguments (NULL, 0).
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        DWORD err = GetLastError();
        fprintf(stderr, "[ws] WinHttpSetOption(WEB_SOCKET) failed: %lu\n", err);
        WinHttpCloseHandle(req);
        delete conn; return -1;
    }

    // WinHTTP automatically adds Sec-WebSocket-Key, Upgrade, and Connection headers
    // when WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET is set.
    std::wstring headers = L"Sec-WebSocket-Version: 13\r\n";
    if (!WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        fprintf(stderr, "[ws] WinHttpSendRequest failed: %lu\n", GetLastError());
        WinHttpCloseHandle(req);
        delete conn; return -1;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        fprintf(stderr, "[ws] WinHttpReceiveResponse failed: %lu\n", GetLastError());
        WinHttpCloseHandle(req);
        delete conn; return -1;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 101) {
        fprintf(stderr, "[ws] unexpected status=%lu\n", statusCode); WinHttpCloseHandle(req); delete conn; return -1; }

    conn->wsHandle.handle = WinHttpWebSocketCompleteUpgrade(req, 0);
    // Close the request handle after upgrade (per MSDN sample code)
    WinHttpCloseHandle(req);
    if (!conn->wsHandle.valid()) {
        fprintf(stderr, "[ws] WinHttpWebSocketCompleteUpgrade failed: %lu\n", GetLastError());
        delete conn; return -1;
    }

    int id = ws_register(conn);
    // Don't release - the map owns the reference now.
    // ws_unregister will release when the connection is closed.
    return id;
}

static std::string ws_send_impl(int id, const std::string& message) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "false";
    if (conn->closed) { conn->release(); return "false"; }

    std::lock_guard<std::mutex> lock(conn->sendMutex);
    DWORD err = WinHttpWebSocketSend(conn->wsHandle,
                                      WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                      const_cast<char*>(message.data()),
                                      static_cast<DWORD>(message.size()));
    conn->release();
    return err == ERROR_SUCCESS ? "true" : "false";
}

static std::string ws_recv_impl(int id) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "";
    if (conn->closed) { conn->release(); return ""; }

    std::lock_guard<std::mutex> lock(conn->recvMutex);
    const DWORD bufSize = 65536;
    char* buf = static_cast<char*>(malloc(bufSize));
    if (!buf) { conn->release(); return ""; }

    DWORD bytesRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    DWORD err = WinHttpWebSocketReceive(conn->wsHandle, buf, bufSize, &bytesRead, &bufferType);
    conn->release();

    if (err != ERROR_SUCCESS) {
        free(buf);
        return "";
    }

    std::string result;
    if (bytesRead > 0 && bufferType != WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        result.assign(buf, bytesRead);
    } else if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        conn->closed = true;
    }
    free(buf);
    return result;
}

static std::string ws_close_impl(int id) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "true";

    {
        std::lock_guard<std::mutex> lock(conn->sendMutex);
        WinHttpWebSocketClose(conn->wsHandle, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        conn->closed = true;
    }

    conn->release();
    ws_unregister(id);
    return "true";
}

static std::string ws_recv_timeout_impl(int id, int timeoutMs) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "";
    if (conn->closed) { conn->release(); return ""; }

    // Temporarily set receive timeout on the session
    WinHttpSetTimeouts(conn->session, 0, 0, timeoutMs, timeoutMs);

    std::lock_guard<std::mutex> lock(conn->recvMutex);
    const DWORD bufSize = 65536;
    char* buf = static_cast<char*>(malloc(bufSize));
    if (!buf) {
        WinHttpSetTimeouts(conn->session, 0, 0, 0, 0);
        conn->release();
        return "";
    }

    DWORD bytesRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    DWORD err = WinHttpWebSocketReceive(conn->wsHandle, buf, bufSize, &bytesRead, &bufferType);

    // Restore default timeouts (no timeout = blocking)
    WinHttpSetTimeouts(conn->session, 0, 0, 0, 0);

    conn->release();

    if (err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_OPERATION_CANCELLED) {
        free(buf);
        return "";
    }

    if (err != ERROR_SUCCESS) {
        free(buf);
        return "";
    }

    std::string result;
    if (bytesRead > 0 && bufferType != WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        result.assign(buf, bytesRead);
    } else if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        conn->closed = true;
    }
    free(buf);
    return result;
}

#else

// Non-Windows stubs
static int ws_connect_impl(const std::string&) { return -1; }
static std::string ws_send_impl(int, const std::string&) { return "false"; }
static std::string ws_recv_impl(int) { return ""; }
static std::string ws_close_impl(int) { return "false"; }

#endif

static std::string ws_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < argv.size() ? argv[i] : empty;
    };

    if (name == "ws_connect") {
        int id = ws_connect_impl(argS(0));
        return std::to_string(id);
    }
    if (name == "ws_send") {
        int id = 0;
        try { id = std::stoi(argS(0)); } catch (...) { return "false"; }
        return ws_send_impl(id, argS(1));
    }
    if (name == "ws_recv") {
        int id = 0;
        try { id = std::stoi(argS(0)); } catch (...) { return ""; }
        return ws_recv_impl(id);
    }
    if (name == "ws_recv_timeout") {
        int id = 0;
        int ms = 5000;
        try { id = std::stoi(argS(0)); } catch (...) { return ""; }
        try { ms = std::stoi(argS(1)); } catch (...) {}
        return ws_recv_timeout_impl(id, ms);
    }
    if (name == "ws_close") {
        int id = 0;
        try { id = std::stoi(argS(0)); } catch (...) { return "true"; }
        return ws_close_impl(id);
    }

    return {};
}

std::string __erelang_builtin_websocket_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return ws_dispatch(name, argv);
}

} // namespace erelang
