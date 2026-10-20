/* platform.h - portability API used by server.c and client.c.
 *
 * The API is declared here once; each OS implements it in its own file:
 *   src/linux/platform_linux.c      (Linux, also builds on macOS/BSD)
 *   src/windows/platform_windows.c  (Windows 10+, mingw-w64 or MSVC-style libc)
 * OS-neutral networking code lives in src/common/net_common.c.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #include "platform_windows.h"
#else
  #include "platform_linux.h"
#endif

/* ---- networking (implemented per OS) ---- */
int    net_init(void);
void   net_close(sock_t s);
int    net_set_nonblock(sock_t s);
void   net_set_nodelay(sock_t s);
void   net_set_reuseaddr(sock_t s);        /* no-op on Windows (different semantics) */
int    net_would_block(void);              /* last socket error was EWOULDBLOCK/EAGAIN */
int    net_interrupted(void);              /* last socket error was EINTR */
void   net_strerror(char *buf, size_t n);  /* text for the last socket error */

/* ---- networking (OS-neutral, src/common/net_common.c) ---- */
sock_t net_listen(int port, char *err, size_t errlen);
sock_t net_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen);

/* ---- time ---- */
uint64_t now_us(void);                     /* implemented per OS */
uint64_t now_ms(void);                     /* common */
void     sleep_ms(unsigned ms);            /* implemented per OS */

/* ---- terminal (client; implemented per OS, term_write is common) ---- */
enum { KEY_NONE = -1, KEY_OTHER = -2, KEY_ENTER = 10, KEY_ESC = 27, KEY_BACKSPACE = 127,
       KEY_UP = 1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_QUIT };

int  term_init(void);                      /* raw mode, alt screen, ANSI on; 0 = ok */
void term_restore(void);
int  term_size(int *cols, int *rows);
/* Waits up to ms for socket data or a key. Returns 1 if a key is ready;
 * *sock_ready is set if the socket is readable. */
int  term_wait(sock_t s, int ms, int *sock_ready);
int  term_read_key(void);                  /* KEY_NONE when nothing is pending */
void term_write(const char *buf, size_t n);

#endif
