// Socket portability layer shared by network.cpp and websocket.cpp.
// On POSIX it provides the WinSock names the socket code is written against
// (SOCKET, INVALID_SOCKET, closesocket, ...) so one implementation serves every platform.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace erelang {

#ifndef _WIN32
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
inline int closesocket(SOCKET s) { return ::close(s); }
inline int WSAGetLastError() { return errno; }
#endif

namespace net {

// Starts WinSock on Windows. On POSIX, ignores SIGPIPE so writing to a socket the peer
// closed returns an error instead of terminating the interpreter. Safe to call repeatedly.
void ensure_init();

// SO_RCVTIMEO / SO_SNDTIMEO take milliseconds as a DWORD on Windows and a timeval on POSIX.
void set_socket_timeout(SOCKET s, int option, int timeoutMs);

// Opens a TCP connection to host:port (name lookup included). INVALID_SOCKET on failure.
SOCKET tcp_connect(const std::string& host, int port, int timeoutMs);

std::array<std::uint8_t, 20> sha1(const std::string& data);
std::string base64_encode(const unsigned char* data, std::size_t len);

// Sec-WebSocket-Accept value for a Sec-WebSocket-Key (RFC 6455 section 4.2.2).
std::string ws_accept_key(const std::string& clientKey);

} // namespace net
} // namespace erelang
