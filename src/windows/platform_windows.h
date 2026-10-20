/* platform_windows.h - system headers and types for Windows (Winsock 2). */
#ifndef PLATFORM_WINDOWS_H
#define PLATFORM_WINDOWS_H

#define WIN32_LEAN_AND_MEAN
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024          /* Windows' default of 64 would cap the server at ~60 players */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#endif
