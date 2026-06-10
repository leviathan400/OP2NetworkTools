// socket_compat.h - tiny cross-platform UDP socket shim (Windows Winsock <-> POSIX).
// Keeps the rest of the net layer free of #ifdefs. No GUI or app dependencies.
#pragma once

#include <cstdint>
#include <cstring>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET op2_sock_t;
  #define OP2_INVALID_SOCK INVALID_SOCKET
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  typedef int op2_sock_t;
  #define OP2_INVALID_SOCK (-1)
#endif

namespace op2 {

// Process-wide socket subsystem init/teardown (no-op on POSIX). Call once each.
inline bool net_startup() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}
inline void net_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

inline void close_sock(op2_sock_t s) {
    if (s == OP2_INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

// Set the receive timeout in milliseconds (so the recv loop can poll for stop/commands).
inline void set_rcv_timeout(op2_sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
    timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

inline void set_broadcast(op2_sock_t s, bool on) {
    int yes = on ? 1 : 0;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
}

} // namespace op2
