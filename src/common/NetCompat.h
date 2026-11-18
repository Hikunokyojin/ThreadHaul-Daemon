#pragma once

// Socket names shared by the Windows and Linux builds. Windows code keeps
// using the Winsock spellings; on Linux they map onto BSD sockets.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define TH_SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
inline int closesocket(SOCKET s) { return ::close(s); }
// A peer that disconnects must give an error, not kill the process.
#define TH_SEND_FLAGS MSG_NOSIGNAL
#endif

#include <cerrno>

namespace netcompat {

// WSAStartup/WSACleanup on Windows; nothing to do elsewhere.
inline bool Startup() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

inline void Cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

inline int LastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Receive timeout on a connected socket.
inline void SetRecvTimeout(SOCKET s, unsigned ms) {
#ifdef _WIN32
    DWORD t = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval t{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
}

// Stop another process taking over a listening port.
inline void ExclusivePort(SOCKET s) {
#ifdef _WIN32
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    // Linux never lets a second socket listen on a port that is being
    // listened on, even with SO_REUSEADDR; the option only allows rebinding
    // while old connections sit in TIME_WAIT, as after a quick restart.
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
}

}
