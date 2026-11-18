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
