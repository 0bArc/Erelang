// WebSocket client builtin for erelang.
// Windows uses the WinHTTP WebSocket API. Other platforms speak RFC 6455 directly over a
// TCP socket, with OpenSSL for wss://.
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>

#include "net_platform.hpp"

#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <poll.h>
#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace erelang {

constexpr std::size_t kWsMaxMessage = 16u * 1024u * 1024u;
constexpr int kWsNormalClose = 1000;

#ifdef _WIN32

static std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), required);
    return wide;
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

#else

// TCP socket with optional TLS. The socket is non-blocking once the handshake is done, and
// every SSL call holds ioMutex: an SSL object must not be read and written from two threads
// at once, and the reader thread and senders share one.
struct WsStream {
    SOCKET sock = INVALID_SOCKET;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    std::mutex ioMutex;
    std::atomic<bool> aborted{false};

    WsStream() = default;
    WsStream(const WsStream&) = delete;
    WsStream& operator=(const WsStream&) = delete;
    ~WsStream() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        if (sock != INVALID_SOCKET) closesocket(sock);
    }

    // Unblocks a reader waiting in read_some; the descriptor itself is closed by the destructor.
    void abort() {
        aborted = true;
        if (sock != INVALID_SOCKET) ::shutdown(sock, SHUT_RDWR);
    }

    bool wait(short events, int timeoutMs) {
        pollfd pfd{sock, events, 0};
        return poll(&pfd, 1, timeoutMs) > 0;
    }

    bool write_all(const char* data, std::size_t len) {
        std::size_t sent = 0;
        while (sent < len) {
            if (aborted) return false;
            short waitFor = POLLOUT;
            {
                std::lock_guard<std::mutex> lock(ioMutex);
                if (ssl) {
                    const int n = SSL_write(ssl, data + sent, static_cast<int>(std::min<std::size_t>(len - sent, 1 << 30)));
                    if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
                    const int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ) waitFor = POLLIN;
                    else if (err != SSL_ERROR_WANT_WRITE) return false;
                } else {
                    const ssize_t n = ::send(sock, data + sent, len - sent, 0);
                    if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
                    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
                }
            }
            wait(waitFor, 1000);
        }
        return true;
    }

    // > 0: bytes read, 0: peer closed or stream aborted, < 0: error.
    int read_some(char* dest, int cap) {
        for (;;) {
            if (aborted) return 0;
            short waitFor = POLLIN;
            {
                std::lock_guard<std::mutex> lock(ioMutex);
                if (ssl) {
                    const int n = SSL_read(ssl, dest, cap);
                    if (n > 0) return n;
                    const int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_ZERO_RETURN) return 0;
                    if (err == SSL_ERROR_WANT_WRITE) waitFor = POLLOUT;
                    else if (err != SSL_ERROR_WANT_READ) return -1;
                } else {
                    const ssize_t n = ::recv(sock, dest, static_cast<std::size_t>(cap), 0);
                    if (n > 0) return static_cast<int>(n);
                    if (n == 0) return 0;
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
                }
            }
            wait(waitFor, 250);
        }
    }
};

#endif

struct WebSocketConnection {
#ifdef _WIN32
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle wsHandle;
#else
    WsStream stream;
    std::string pending;  // bytes read past the HTTP upgrade response, parsed as frames first
#endif
    std::string url;
    std::mutex sendMutex;
    std::atomic<bool> closed{false};
    std::string state = "connecting";  // "connecting" | "open" | "closing" | "closed"
    std::atomic<int> refCount{1};

    // Reader thread output. Neither transport has a timed receive the interpreter can use, so
    // one thread per connection blocks reading and queues whole messages here.
    std::mutex inboxMutex;
    std::condition_variable inboxCv;
    std::deque<std::string> inbox;
    bool readerDone = false;     // guarded by inboxMutex
    int closeCode = 0;           // last close status received from the peer (0 = none)
    std::string closeReason;

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

static void ws_queue_message(WebSocketConnection* conn, std::string message) {
    {
        std::lock_guard<std::mutex> lock(conn->inboxMutex);
        conn->inbox.push_back(std::move(message));
    }
    conn->inboxCv.notify_all();
}

static void ws_record_close(WebSocketConnection* conn, int code, std::string reason) {
    std::lock_guard<std::mutex> lock(conn->inboxMutex);
    conn->closeCode = code;
    conn->closeReason = std::move(reason);
}

// Marks the reader finished and drops its reference to `conn`.
static void ws_reader_finished(WebSocketConnection* conn) {
    {
        std::lock_guard<std::mutex> lock(conn->inboxMutex);
        conn->closed = true;
        conn->state = "closed";
        conn->readerDone = true;
    }
    conn->inboxCv.notify_all();
    conn->release();
}

#ifdef _WIN32

static bool ws_send_message(WebSocketConnection* conn, const std::string& data, bool binary) {
    std::lock_guard<std::mutex> lock(conn->sendMutex);
    const DWORD err = WinHttpWebSocketSend(conn->wsHandle,
                                           binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
                                                  : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                           const_cast<char*>(data.data()),
                                           static_cast<DWORD>(data.size()));
    return err == ERROR_SUCCESS;
}

static void ws_send_close(WebSocketConnection* conn, int code, const std::string& reason) {
    std::lock_guard<std::mutex> lock(conn->sendMutex);
    WinHttpWebSocketClose(conn->wsHandle, static_cast<USHORT>(code),
                          reason.empty() ? nullptr : const_cast<char*>(reason.data()),
                          static_cast<DWORD>(reason.size()));
}

// Cancels the reader's pending receive.
static void ws_abort(WebSocketConnection* conn) {
    WinHttpCloseHandle(conn->wsHandle.handle);
    conn->wsHandle.handle = nullptr;
}

// Blocks in WinHttpWebSocketReceive, joins fragments into whole messages and queues them.
// Owns one reference to `conn` (taken before the thread starts, released on exit).
static void ws_reader_loop(WebSocketConnection* conn) {
    std::vector<char> chunk(16384);
    std::string message;
    for (;;) {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        const DWORD err = WinHttpWebSocketReceive(conn->wsHandle, chunk.data(),
                                                  static_cast<DWORD>(chunk.size()), &bytes, &type);
        if (err != ERROR_SUCCESS) break;  // handle closed locally, or the connection dropped
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = 0;
            char reason[123]{};
            DWORD reasonLen = 0;
            if (WinHttpWebSocketQueryCloseStatus(conn->wsHandle, &status, reason, sizeof(reason), &reasonLen) ==
                ERROR_SUCCESS) {
                ws_record_close(conn, status, std::string(reason, reasonLen));
            }
            // Peer started the closing handshake: answer it.
            if (!conn->closed.load()) ws_send_close(conn, status ? status : kWsNormalClose, "");
            break;
        }
        message.append(chunk.data(), bytes);
        if (message.size() > kWsMaxMessage) break;  // refuse unbounded messages
        const bool fragment = type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                              type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
        if (!fragment) {
            ws_queue_message(conn, std::move(message));
            message.clear();
        }
    }
    ws_reader_finished(conn);
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

    auto conn = new WebSocketConnection();
    conn->url = url;

    conn->session.handle = WinHttpOpen(
        L"erelang-ws/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!conn->session.valid()) { fprintf(stderr, "[ws] WinHttpOpen failed: %lu\n", GetLastError()); delete conn; return -1; }
    // The default receive timeout (30 s) would abort a socket that is merely idle between
    // heartbeats. 0 = infinite; the reader thread owns waiting, recv_timeout is done on the inbox.
    WinHttpSetTimeouts(conn->session, 0, 60000, 30000, 0);

    conn->connection.handle = WinHttpConnect(conn->session, uc.lpszHostName, uc.nPort, 0);
    if (!conn->connection.valid()) { fprintf(stderr, "[ws] WinHttpConnect failed: %lu\n", GetLastError()); delete conn; return -1; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET req = WinHttpOpenRequest(
        conn->connection, L"GET", uc.lpszUrlPath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { fprintf(stderr, "[ws] WinHttpOpenRequest failed: %lu\n", GetLastError()); delete conn; return -1; }

    // WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET takes no arguments (NULL, 0); WinHTTP then adds
    // Sec-WebSocket-Key, Upgrade and Connection itself.
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        fprintf(stderr, "[ws] WinHttpSetOption(WEB_SOCKET) failed: %lu\n", GetLastError());
        WinHttpCloseHandle(req);
        delete conn; return -1;
    }

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
    conn->state = "open";
    conn->addRef();  // the reader thread's reference; dropped when it exits
    std::thread(ws_reader_loop, conn).detach();
    return id;
}

#else

constexpr unsigned char kOpContinuation = 0x0;
constexpr unsigned char kOpText = 0x1;
constexpr unsigned char kOpBinary = 0x2;
constexpr unsigned char kOpClose = 0x8;
constexpr unsigned char kOpPing = 0x9;

// Client frames are always masked (RFC 6455 section 5.3).
static bool ws_write_frame(WebSocketConnection* conn, unsigned char opcode, const std::string& payload) {
    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80 | opcode));
    const std::uint64_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
    unsigned char mask[4];
    if (RAND_bytes(mask, sizeof(mask)) != 1) return false;
    frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ static_cast<char>(mask[i % 4])));
    }
    std::lock_guard<std::mutex> lock(conn->sendMutex);
    return conn->stream.write_all(frame.data(), frame.size());
}

static bool ws_send_message(WebSocketConnection* conn, const std::string& data, bool binary) {
    return ws_write_frame(conn, binary ? kOpBinary : kOpText, data);
}

static void ws_send_close(WebSocketConnection* conn, int code, const std::string& reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason.substr(0, 123);
    ws_write_frame(conn, kOpClose, payload);
}

static void ws_abort(WebSocketConnection* conn) { conn->stream.abort(); }

// Reads exactly n bytes, consuming leftover handshake bytes first.
static bool ws_read_exact(WebSocketConnection* conn, char* dest, std::size_t n) {
    std::size_t got = 0;
    if (!conn->pending.empty()) {
        got = std::min(n, conn->pending.size());
        std::memcpy(dest, conn->pending.data(), got);
        conn->pending.erase(0, got);
    }
    while (got < n) {
        const int r = conn->stream.read_some(dest + got, static_cast<int>(std::min<std::size_t>(n - got, 1 << 20)));
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

// Parses frames, answers pings, joins fragments and queues whole messages.
// Owns one reference to `conn` (taken before the thread starts, released on exit).
static void ws_reader_loop(WebSocketConnection* conn) {
    std::string message;
    for (;;) {
        unsigned char head[2];
        if (!ws_read_exact(conn, reinterpret_cast<char*>(head), 2)) break;
        const bool fin = (head[0] & 0x80) != 0;
        const unsigned char opcode = head[0] & 0x0F;
        const bool masked = (head[1] & 0x80) != 0;
        std::uint64_t len = head[1] & 0x7F;
        if (len == 126) {
            unsigned char ext[2];
            if (!ws_read_exact(conn, reinterpret_cast<char*>(ext), 2)) break;
            len = (std::uint64_t(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8];
            if (!ws_read_exact(conn, reinterpret_cast<char*>(ext), 8)) break;
            len = 0;
            for (unsigned char b : ext) len = (len << 8) | b;
        }
        if (len > kWsMaxMessage || message.size() + len > kWsMaxMessage) break;  // refuse unbounded messages
        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked && !ws_read_exact(conn, reinterpret_cast<char*>(mask), 4)) break;
        std::string payload(static_cast<std::size_t>(len), '\0');
        if (len > 0 && !ws_read_exact(conn, payload.data(), payload.size())) break;
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }

        if (opcode == kOpClose) {
            int code = 0;
            std::string reason;
            if (payload.size() >= 2) {
                code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
                reason = payload.substr(2);
            }
            ws_record_close(conn, code, reason);
            // Peer started the closing handshake: answer it.
            if (!conn->closed.load()) ws_send_close(conn, code ? code : kWsNormalClose, "");
            break;
        }
        if (opcode == kOpPing) {
            ws_write_frame(conn, 0xA, payload);
            continue;
        }
        if (opcode != kOpText && opcode != kOpBinary && opcode != kOpContinuation) continue;  // pong or reserved
        message += payload;
        if (fin) {
            ws_queue_message(conn, std::move(message));
            message.clear();
        }
    }
    ws_reader_finished(conn);
}

struct WsUrl {
    bool secure = false;
    std::string host;
    int port = 0;
    std::string target = "/";
};

static bool parse_ws_url(const std::string& url, WsUrl& out) {
    std::string rest;
    if (url.rfind("wss://", 0) == 0) { out.secure = true; rest = url.substr(6); }
    else if (url.rfind("https://", 0) == 0) { out.secure = true; rest = url.substr(8); }
    else if (url.rfind("ws://", 0) == 0) { rest = url.substr(5); }
    else if (url.rfind("http://", 0) == 0) { rest = url.substr(7); }
    else return false;

    const std::size_t slash = rest.find_first_of("/?");
    std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) out.target = rest.substr(slash);
    if (!out.target.empty() && out.target[0] == '?') out.target = "/" + out.target;
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);

    std::string portText;
    if (!authority.empty() && authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') portText = authority.substr(close + 2);
    } else {
        const std::size_t colon = authority.rfind(':');
        out.host = authority.substr(0, colon);
        if (colon != std::string::npos) portText = authority.substr(colon + 1);
    }
    out.port = out.secure ? 443 : 80;
    if (!portText.empty()) {
        try { out.port = std::stoi(portText); } catch (...) { return false; }
    }
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

static bool tls_start(WsStream& stream, const std::string& host) {
    stream.ctx = SSL_CTX_new(TLS_client_method());
    if (!stream.ctx) return false;
    SSL_CTX_set_min_proto_version(stream.ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(stream.ctx);
    SSL_CTX_set_verify(stream.ctx, SSL_VERIFY_PEER, nullptr);
    stream.ssl = SSL_new(stream.ctx);
    if (!stream.ssl) return false;
    SSL_set_tlsext_host_name(stream.ssl, host.c_str());
    SSL_set1_host(stream.ssl, host.c_str());
    SSL_set_fd(stream.ssl, stream.sock);
    if (SSL_connect(stream.ssl) != 1) {
        const long verify = SSL_get_verify_result(stream.ssl);
        if (verify != X509_V_OK) {
            fprintf(stderr, "[ws] TLS certificate check failed: %s\n", X509_verify_cert_error_string(verify));
        } else {
            fprintf(stderr, "[ws] TLS handshake failed\n");
        }
        return false;
    }
    return true;
}

static std::string lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static int ws_connect_impl(const std::string& url) {
    WsUrl target;
    if (!parse_ws_url(url, target)) { fprintf(stderr, "[ws] invalid URL: %s\n", url.c_str()); return -1; }

    std::unique_ptr<WebSocketConnection> conn(new WebSocketConnection());
    conn->url = url;
    WsStream& stream = conn->stream;

    stream.sock = net::tcp_connect(target.host, target.port, 10000);
    if (stream.sock == INVALID_SOCKET) {
        fprintf(stderr, "[ws] connect to %s:%d failed\n", target.host.c_str(), target.port);
        return -1;
    }
    // Bound the handshake; the socket goes non-blocking for the reader afterwards.
    net::set_socket_timeout(stream.sock, SO_RCVTIMEO, 30000);
    net::set_socket_timeout(stream.sock, SO_SNDTIMEO, 30000);
    if (target.secure && !tls_start(stream, target.host)) return -1;

    unsigned char keyBytes[16];
    if (RAND_bytes(keyBytes, sizeof(keyBytes)) != 1) return -1;
    const std::string key = net::base64_encode(keyBytes, sizeof(keyBytes));
    const bool defaultPort = target.port == (target.secure ? 443 : 80);
    std::string hostHeader = target.host.find(':') != std::string::npos ? "[" + target.host + "]" : target.host;
    if (!defaultPort) hostHeader += ":" + std::to_string(target.port);
    const std::string request = "GET " + target.target + " HTTP/1.1\r\n"
                                "Host: " + hostHeader + "\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: " + key + "\r\n"
                                "Sec-WebSocket-Version: 13\r\n"
                                "User-Agent: erelang-ws/1.0\r\n\r\n";

    auto blocking_write = [&](const std::string& data) {
        if (stream.ssl) return SSL_write(stream.ssl, data.data(), static_cast<int>(data.size())) == static_cast<int>(data.size());
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(stream.sock, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    };
    auto blocking_read = [&](char* buf, int cap) -> int {
        if (stream.ssl) return SSL_read(stream.ssl, buf, cap);
        return static_cast<int>(::recv(stream.sock, buf, static_cast<std::size_t>(cap), 0));
    };

    if (!blocking_write(request)) { fprintf(stderr, "[ws] sending upgrade request failed\n"); return -1; }

    std::string response;
    char buf[4096];
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const int n = blocking_read(buf, sizeof(buf));
        if (n <= 0) { fprintf(stderr, "[ws] no upgrade response\n"); return -1; }
        response.append(buf, static_cast<std::size_t>(n));
        if (response.size() > 64 * 1024) { fprintf(stderr, "[ws] upgrade response too large\n"); return -1; }
        headerEnd = response.find("\r\n\r\n");
    }
    conn->pending = response.substr(headerEnd + 4);

    std::istringstream head(response.substr(0, headerEnd));
    std::string statusLine;
    std::getline(head, statusLine);
    int status = 0;
    {
        std::istringstream sl(statusLine);
        std::string version;
        sl >> version >> status;
    }
    if (status != 101) { fprintf(stderr, "[ws] unexpected status=%d\n", status); return -1; }
    std::string accept;
    std::string line;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lower_ascii(line.substr(0, colon)) != "sec-websocket-accept") continue;
        accept = line.substr(colon + 1);
        accept.erase(0, accept.find_first_not_of(" \t"));
        accept.erase(accept.find_last_not_of(" \t") + 1);
    }
    if (accept != net::ws_accept_key(key)) { fprintf(stderr, "[ws] server sent a wrong Sec-WebSocket-Accept\n"); return -1; }

    const int flags = fcntl(stream.sock, F_GETFL, 0);
    fcntl(stream.sock, F_SETFL, flags | O_NONBLOCK);

    WebSocketConnection* raw = conn.release();
    int id = ws_register(raw);
    raw->state = "open";
    raw->addRef();  // the reader thread's reference; dropped when it exits
    std::thread(ws_reader_loop, raw).detach();
    return id;
}

#endif

static std::string ws_send_impl(int id, const std::string& message, bool binary = false) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "false";
    const bool ok = !conn->closed && ws_send_message(conn, message, binary);
    conn->release();
    return ok ? "true" : "false";
}

static void ws_broadcast(const std::string& data) {
    std::lock_guard<std::mutex> lock(g_wsMutex);
    for (auto& [cid, c] : g_wsConnections) {
        if (!c->closed) ws_send_message(c, data, false);
    }
}

// Waits for the next whole message. timeoutMs < 0 waits until one arrives or the socket closes.
// Messages received before a close are still delivered; "" means timeout or closed.
static std::string ws_recv_wait(int id, int timeoutMs) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "";
    std::string out;
    {
        std::unique_lock<std::mutex> lock(conn->inboxMutex);
        auto ready = [&] { return !conn->inbox.empty() || conn->readerDone; };
        if (timeoutMs < 0) {
            conn->inboxCv.wait(lock, ready);
        } else {
            conn->inboxCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
        }
        if (!conn->inbox.empty()) {
            out = std::move(conn->inbox.front());
            conn->inbox.pop_front();
        }
    }
    conn->release();
    return out;
}

static std::string ws_recv_impl(int id) { return ws_recv_wait(id, -1); }

static std::string ws_recv_timeout_impl(int id, int timeoutMs) {
    return ws_recv_wait(id, timeoutMs < 0 ? 0 : timeoutMs);
}

// Closes with a status code and tears the connection down. Sends the close frame, gives the
// peer up to 1.5 s to answer (the reader thread then exits by itself), and otherwise cancels
// the pending receive.
static std::string ws_close_with(int id, int code, const std::string& reason) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "true";
    conn->state = "closing";
    if (!conn->closed.load()) ws_send_close(conn, code, reason);
    {
        std::unique_lock<std::mutex> lock(conn->inboxMutex);
        conn->inboxCv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return conn->readerDone; });
        if (!conn->readerDone) ws_abort(conn);
    }
    conn->closed = true;
    conn->state = "closed";
    conn->release();
    ws_unregister(id);
    return "true";
}

static std::string ws_close_impl(int id) { return ws_close_with(id, kWsNormalClose, ""); }

static std::string ws_state(int id) {
    WebSocketConnection* conn = ws_get_connection(id);
    if (!conn) return "closed";
    std::string st = conn->state;
    conn->release();
    return st;
}

static std::string ws_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < argv.size() ? argv[i] : empty;
    };

    if (name == "ws_connect") {
        int id = ws_connect_impl(argS(0));
        if (id < 0) return "null";
        return std::string("ws:") + std::to_string(id);
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
    if (name == "ws_state") {
        int id = 0;
        try { id = std::stoi(argS(0)); } catch (...) { return "closed"; }
        return ws_state(id);
    }
    if (name == "ws_broadcast") {
        ws_broadcast(argS(0));
        return "true";
    }
    if (name == "ws_send_binary") {
        int id = 0;
        try { id = std::stoi(argS(0)); } catch (...) { return "false"; }
        return ws_send_impl(id, argS(1), true);
    }

    return {};
}

std::string __erelang_builtin_websocket_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return ws_dispatch(name, argv);
}

// Handle method dispatch for ws: handles (called from actions.cpp)
// id is the raw int (prefix already stripped)
std::string __erelang_ws_handle_method(int id, const std::string& method, const std::vector<std::string>& args) {
    auto argS = [&](size_t i) -> const std::string& {
        static const std::string empty;
        return i < args.size() ? args[i] : empty;
    };

    if (method == "send")        return ws_send_impl(id, argS(0));
    if (method == "send_binary") return ws_send_impl(id, argS(0), true);
    if (method == "recv")        return ws_recv_impl(id);
    if (method == "recv_timeout") {
        int ms = 5000;
        try { ms = std::stoi(argS(0)); } catch (...) {}
        return ws_recv_timeout_impl(id, ms);
    }
    if (method == "close") {
        int code = kWsNormalClose;
        std::string reason;
        if (!argS(0).empty()) {
            try { code = std::stoi(argS(0)); } catch (...) {}
            reason = argS(1);
        }
        return ws_close_with(id, code, reason);
    }
    if (method == "close_code" || method == "close_reason") {
        // What the peer said when it closed (Discord: 4004 bad token, 4014 missing intents, ...).
        WebSocketConnection* conn = ws_get_connection(id);
        if (!conn) return method == "close_code" ? "0" : "";
        std::string out;
        {
            std::lock_guard<std::mutex> lock(conn->inboxMutex);
            out = method == "close_code" ? std::to_string(conn->closeCode) : conn->closeReason;
        }
        conn->release();
        return out;
    }
    if (method == "broadcast") {
        ws_broadcast(argS(0));
        return "true";
    }
    if (method == "state") return ws_state(id);
    return {};
}

} // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("ws",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_websocket_dispatch(name, argv);
    });
}  // namespace
const bool kWsHandleRegistered = erelang::modules::register_handle("ws",
    [](erelang::Runtime*, int id, const std::string& method, const std::vector<std::string>& args, std::string& out) {
        out = erelang::__erelang_ws_handle_method(id, method, args);
        return true;
    }, 20);
ERELANG_MODULE_ANCHOR(ws)
