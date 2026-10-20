/* platform_linux.h - system headers and types for Linux (and other POSIX systems). */
#ifndef PLATFORM_LINUX_H
#define PLATFORM_LINUX_H

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef int sock_t;
#define SOCK_BAD (-1)

#endif
