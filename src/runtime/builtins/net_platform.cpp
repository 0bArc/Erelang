#include "net_platform.hpp"

#include <cstring>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#endif

namespace erelang::net {

void ensure_init() {
    static const bool ready = [] {
#ifdef _WIN32
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
        std::signal(SIGPIPE, SIG_IGN);
        return true;
#endif
    }();
    (void)ready;
}

void set_socket_timeout(SOCKET s, int option, int timeoutMs) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeoutMs < 0 ? 0 : timeoutMs);
    setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval tv{};
    if (timeoutMs > 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
    }
    setsockopt(s, SOL_SOCKET, option, &tv, sizeof(tv));
#endif
}

// Waits for a non-blocking connect() to finish within timeoutMs.
static bool finish_connect(SOCKET s, int timeoutMs) {
#ifdef _WIN32
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(s, &writeSet);
    fd_set errorSet = writeSet;
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &writeSet, &errorSet, &tv) <= 0) return false;
    if (FD_ISSET(s, &errorSet)) return false;
#else
    pollfd pfd{s, POLLOUT, 0};
    if (poll(&pfd, 1, timeoutMs) <= 0) return false;
#endif
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) return false;
    return err == 0;
}

static void set_blocking(SOCKET s, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

SOCKET tcp_connect(const std::string& host, int port, int timeoutMs) {
    ensure_init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || !results) return INVALID_SOCKET;

    SOCKET connected = INVALID_SOCKET;
    for (addrinfo* ai = results; ai && connected == INVALID_SOCKET; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        set_blocking(s, false);
        const int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = rc == 0;
        if (!ok) {
#ifdef _WIN32
            const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool pending = errno == EINPROGRESS;
#endif
            ok = pending && finish_connect(s, timeoutMs);
        }
        if (ok) {
            set_blocking(s, true);
            connected = s;
        } else {
            closesocket(s);
        }
    }
    freeaddrinfo(results);
    return connected;
}

std::array<std::uint8_t, 20> sha1(const std::string& data) {
    auto rol = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    std::vector<std::uint8_t> msg(data.begin(), data.end());
    const std::uint64_t bitLen = static_cast<std::uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bitLen >> (i * 8)));

    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(msg[off + i * 4]) << 24) | (std::uint32_t(msg[off + i * 4 + 1]) << 16) |
                   (std::uint32_t(msg[off + i * 4 + 2]) << 8) | std::uint32_t(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

std::string base64_encode(const unsigned char* data, std::size_t len) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? kTable[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? kTable[n & 63] : '=');
    }
    return out;
}

std::string ws_accept_key(const std::string& clientKey) {
    const auto digest = sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64_encode(digest.data(), digest.size());
}

} // namespace erelang::net
