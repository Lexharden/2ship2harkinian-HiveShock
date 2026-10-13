#pragma once

// Portable socket helpers for HiveShock (Winsock on Windows, BSD sockets elsewhere).
// Header-only and meant to be included from HiveShock.cpp only: on Windows winsock2.h must come before windows.h,
// which the precompiled header already pulls in, so that file is built without the PCH (see mm/CMakeLists.txt).

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <string>

namespace HiveShockNet {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

inline void CloseSocket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

// Initializes the platform socket layer for the lifetime of the object (Winsock refcounts per call).
struct NetSession {
    bool ok = true;
#ifdef _WIN32
    NetSession() {
        WSADATA wsaData;
        ok = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }
    ~NetSession() {
        if (ok) {
            WSACleanup();
        }
    }
#endif
};

inline int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline bool LastSocketErrorIsTransient() {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEINTR || err == WSAEWOULDBLOCK;
#else
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED;
#endif
}

// macOS/BSD have no MSG_NOSIGNAL, so a write to a closed peer would raise SIGPIPE and kill the whole game;
// SO_NOSIGPIPE turns that into a plain EPIPE error instead.
inline void ConfigureSocket(SocketHandle sock) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)sock;
#endif
}

inline void SetNonBlocking(SocketHandle sock, bool enable) {
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
    }
#endif
}

// Waits until the socket is readable (or, if !forRead, writable). poll() has no FD_SETSIZE limit, unlike select().
inline bool WaitForSocket(SocketHandle sock, bool forRead, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = sock;
    pfd.events = forRead ? POLLRDNORM : POLLWRNORM;
    return WSAPoll(&pfd, 1, timeoutMs) > 0;
#else
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = forRead ? POLLIN : POLLOUT;
    return poll(&pfd, 1, timeoutMs) > 0;
#endif
}

// Polls two sockets for readability. Returns a bitmask: 1 = a readable, 2 = b readable.
inline int WaitForEitherReadable(SocketHandle a, SocketHandle b, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD fds[2]{};
    fds[0].fd = a;
    fds[0].events = POLLRDNORM;
    fds[1].fd = b;
    fds[1].events = POLLRDNORM;
    if (WSAPoll(fds, 2, timeoutMs) <= 0) {
        return 0;
    }
    return ((fds[0].revents & (POLLRDNORM | POLLHUP | POLLERR)) ? 1 : 0) |
           ((fds[1].revents & (POLLRDNORM | POLLHUP | POLLERR)) ? 2 : 0);
#else
    pollfd fds[2]{};
    fds[0].fd = a;
    fds[0].events = POLLIN;
    fds[1].fd = b;
    fds[1].events = POLLIN;
    if (poll(fds, 2, timeoutMs) <= 0) {
        return 0;
    }
    return ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0) |
           ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) ? 2 : 0);
#endif
}

inline void SendAll(SocketHandle sock, const std::string& data) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), flags);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

inline sockaddr_in LoopbackAddress(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

inline sockaddr_in AnyAddress(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return addr;
}

// Address of the outgoing LAN interface (127.x is ignored). Empty when it cannot be determined.
inline std::string ResolvePreferredLanIpv4() {
    NetSession session;
    SocketHandle sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) {
        return "";
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    // connect() on UDP sends nothing; it only picks the outgoing interface.
    if (connect(sock, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
        CloseSocket(sock);
        return "";
    }

    sockaddr_in local{};
#ifdef _WIN32
    int len = sizeof(local);
#else
    socklen_t len = sizeof(local);
#endif
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
        CloseSocket(sock);
        return "";
    }
    CloseSocket(sock);

    char ip[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)) == nullptr) {
        return "";
    }
    std::string result = ip;
    if (result.rfind("127.", 0) == 0) {
        return "";
    }
    return result;
}

} // namespace HiveShockNet
