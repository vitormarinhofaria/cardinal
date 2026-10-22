#pragma once

#include <string>

// Raw loopback sockets for tests, on POSIX and Winsock. Only what the tests need.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cardinal::testing::net {

#ifdef _WIN32
using sock_t = SOCKET;
inline constexpr sock_t kBad = INVALID_SOCKET;
using socklen = int;
using ssize = int;
inline constexpr int kNoSignal = 0;
inline constexpr int kShutBoth = SD_BOTH;
inline constexpr int kCloexec = 0;

// Winsock must be started once per process; the inline variable does it before main().
inline const bool kStarted = [] {
    WSADATA d;
    return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
}();

inline void close(sock_t s) { ::closesocket(s); }
inline int pollIn(sock_t s, int ms) {
    WSAPOLLFD p{s, POLLRDNORM, 0};
    return ::WSAPoll(&p, 1, ms);
}
inline void setRecvTimeout(sock_t s, int seconds) {
    DWORD ms = static_cast<DWORD>(seconds) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}
// Windows has no SO_REUSEADDR semantics worth using for a test listener; do nothing.
inline void reuseAddr(sock_t) {}
#else
using sock_t = int;
inline constexpr sock_t kBad = -1;
using socklen = socklen_t;
using ssize = ssize_t;
inline constexpr int kNoSignal = MSG_NOSIGNAL;
inline constexpr int kShutBoth = SHUT_RDWR;
inline constexpr int kCloexec = SOCK_CLOEXEC;

inline void close(sock_t s) { ::close(s); }
inline int pollIn(sock_t s, int ms) {
    pollfd p{s, POLLIN, 0};
    return ::poll(&p, 1, ms);
}
inline void setRecvTimeout(sock_t s, int seconds) {
    timeval tv{seconds, 0};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
inline void reuseAddr(sock_t s) {
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}
#endif

inline int send(sock_t s, const std::string& data) {
    return static_cast<int>(::send(s, data.data(), static_cast<int>(data.size()), kNoSignal));
}

}  // namespace cardinal::testing::net
