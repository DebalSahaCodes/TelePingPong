/* platform_linux.c - Linux/POSIX implementation: sockets, clock, raw terminal. */
#include "platform.h"
#include "platform_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */
uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

void sleep_ms(unsigned ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* sockets                                                            */
/* ------------------------------------------------------------------ */
int net_init(void) {
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void net_close(sock_t s) { close(s); }

int net_set_nonblock(sock_t s) {
    int fl = fcntl(s, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(s, F_SETFL, fl | O_NONBLOCK);
}

void net_set_nodelay(sock_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

void net_set_reuseaddr(sock_t s) {
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
}

int net_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
int net_interrupted(void) { return errno == EINTR; }
int net_in_progress(void) { return errno == EINPROGRESS || errno == EINTR; }

void err_text(int code, char *buf, size_t n) { snprintf(buf, n, "%s", strerror(code)); }
void net_strerror(char *buf, size_t n) { err_text(errno, buf, n); }

/* ------------------------------------------------------------------ */
/* terminal (termios raw mode + ANSI escape sequences)                */
/* ------------------------------------------------------------------ */
static int g_term_on = 0;
static struct termios g_old_tio;

int term_init(void) {
    struct termios t;
    if (!isatty(0) || tcgetattr(0, &g_old_tio) != 0) return -1;
    t = g_old_tio;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_iflag &= ~(tcflag_t)IXON;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
    fflush(stdout);
    g_term_on = 1;
    return 0;
}

void term_restore(void) {
    if (!g_term_on) return;
    g_term_on = 0;
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    tcsetattr(0, TCSANOW, &g_old_tio);
}

int term_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) { *cols = 80; *rows = 24; return -1; }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

int term_wait(sock_t s, int ms, int *sock_ready) {
    struct pollfd p[2];
    int n;
    p[0].fd = 0; p[0].events = POLLIN; p[0].revents = 0;
    p[1].fd = s; p[1].events = POLLIN; p[1].revents = 0;
    *sock_ready = 0;
    n = poll(p, 2, ms);
    if (n <= 0) return 0;
    if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) *sock_ready = 1;
    return (p[0].revents & (POLLIN | POLLHUP)) ? 1 : 0;
}

static unsigned char g_kb[64];
static int g_kbn = 0;

int term_read_key(void) {
    unsigned char c;
    int used = 1, key;
    if (g_kbn == 0) {
        struct pollfd p;
        ssize_t n;
        p.fd = 0; p.events = POLLIN; p.revents = 0;
        if (poll(&p, 1, 0) <= 0) return KEY_NONE;
        n = read(0, g_kb, sizeof g_kb);
        if (n == 0) return KEY_QUIT;            /* stdin closed */
        if (n < 0) return KEY_NONE;
        g_kbn = (int)n;
    }
    c = g_kb[0];
    key = c;
    if (c == 27 && g_kbn >= 2 && (g_kb[1] == '[' || g_kb[1] == 'O')) {
        int fin = 0;
        used = 2;
        while (used < g_kbn) {                  /* consume up to the final byte */
            unsigned char d = g_kb[used++];
            if (d >= 0x40 && d <= 0x7e) { fin = d; break; }
        }
        switch (fin) {
            case 'A': key = KEY_UP; break;
            case 'B': key = KEY_DOWN; break;
            case 'C': key = KEY_RIGHT; break;
            case 'D': key = KEY_LEFT; break;
            default:  key = KEY_OTHER; break;
        }
    } else if (c == 127 || c == 8) {
        key = KEY_BACKSPACE;
    } else if (c == '\r') {
        key = KEY_ENTER;
    }
    memmove(g_kb, g_kb + used, (size_t)(g_kbn - used));
    g_kbn -= used;
    return key;
}
