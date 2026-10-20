/* platform_windows.c - Windows implementation: Winsock, QueryPerformanceCounter,
 * console with ANSI/VT processing. Needs Windows 10 or newer; link ws2_32 and winmm. */
#include "platform.h"
#include "platform_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mmsystem.h>
#include <conio.h>

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */
uint64_t now_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000ULL +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000ULL / (uint64_t)freq.QuadPart;
}

void sleep_ms(unsigned ms) { Sleep(ms); }

/* ------------------------------------------------------------------ */
/* sockets                                                            */
/* ------------------------------------------------------------------ */
int net_init(void) {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return -1;
    timeBeginPeriod(1);              /* 1 ms timer resolution for the game loop */
    return 0;
}

void net_close(sock_t s) { closesocket(s); }

int net_set_nonblock(sock_t s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on);
}

void net_set_nodelay(sock_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

/* Windows' SO_REUSEADDR lets another process steal the port, so don't set it. */
void net_set_reuseaddr(sock_t s) { (void)s; }

int net_would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
int net_interrupted(void) { return WSAGetLastError() == WSAEINTR; }
int net_in_progress(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }

void err_text(int code, char *buf, size_t n) {
    DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, (DWORD)code, 0, buf, (DWORD)n, NULL);
    while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) buf[--len] = 0;
    if (!len) snprintf(buf, n, "error %d", code);
}
void net_strerror(char *buf, size_t n) { err_text(WSAGetLastError(), buf, n); }

/* ------------------------------------------------------------------ */
/* terminal (console VT mode + _kbhit/_getch)                         */
/* ------------------------------------------------------------------ */
static int g_term_on = 0;
static DWORD g_old_out_mode, g_old_in_mode;
static int g_have_in_mode = 0;

int term_init(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE), hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD m, im;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &m)) return -1;
    g_old_out_mode = m;
    if (!SetConsoleMode(h, m | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return -1;
    /* Clicking in a console normally starts a QuickEdit text selection, which
     * FREEZES the program's output until a key is pressed. The client would stop
     * answering the server and get timed out, so switch QuickEdit/mouse off. */
    if (hin != INVALID_HANDLE_VALUE && GetConsoleMode(hin, &im)) {
        g_old_in_mode = im;
        g_have_in_mode = 1;
        SetConsoleMode(hin, (im & ~(DWORD)(ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT)) | ENABLE_EXTENDED_FLAGS);
    }
    fputs("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J", stdout);   /* alt screen, hide cursor, no autowrap */
    fflush(stdout);
    g_term_on = 1;
    return 0;
}

void term_restore(void) {
    if (!g_term_on) return;
    g_term_on = 0;
    fputs("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_old_out_mode);
    if (g_have_in_mode) SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_old_in_mode);
}

int term_size(int *cols, int *rows) {
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i)) { *cols = 80; *rows = 24; return -1; }
    *cols = i.srWindow.Right - i.srWindow.Left + 1;
    *rows = i.srWindow.Bottom - i.srWindow.Top + 1;
    return 0;
}

int term_wait(sock_t s, int ms, int *sock_ready) {
    uint64_t end = now_ms() + (uint64_t)ms;
    *sock_ready = 0;
    for (;;) {
        fd_set r;
        struct timeval tv = { 0, 0 };
        int k;
        FD_ZERO(&r);
        FD_SET(s, &r);
        if (select(0, &r, NULL, NULL, &tv) > 0) *sock_ready = 1;
        k = _kbhit();
        if (*sock_ready || k) return k ? 1 : 0;
        if (now_ms() >= end) return 0;
        Sleep(1);
    }
}

int term_read_key(void) {
    int c;
    if (!_kbhit()) return KEY_NONE;
    c = _getch();
    if (c == 0 || c == 0xE0) {
        switch (_getch()) {
            case 72: return KEY_UP;
            case 80: return KEY_DOWN;
            case 75: return KEY_LEFT;
            case 77: return KEY_RIGHT;
            default: return KEY_OTHER;
        }
    }
    if (c == 13) return KEY_ENTER;
    if (c == 8) return KEY_BACKSPACE;
    if (c == 3) return KEY_QUIT;
    return c;
}
