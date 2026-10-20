/* net_common.c - OS-neutral networking: listening and connecting (IPv4 + IPv6). */
#include "platform.h"
#include "platform_internal.h"
#include <stdio.h>
#include <string.h>

uint64_t now_ms(void) { return now_us() / 1000; }

void term_write(const char *buf, size_t n) {
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
}

sock_t net_listen(int port, char *err, size_t errlen) {
    sock_t s;
    char es[160];
    int off = 0;

    /* Prefer a dual-stack IPv6 socket (also accepts IPv4); fall back to IPv4. */
    s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s != SOCK_BAD) {
        struct sockaddr_in6 a6;
        net_set_reuseaddr(s);
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
        memset(&a6, 0, sizeof a6);
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons((unsigned short)port);
        if (bind(s, (struct sockaddr *)&a6, sizeof a6) == 0 && listen(s, 128) == 0) {
            net_set_nonblock(s);
            return s;
        }
        net_close(s);
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_BAD) { net_strerror(es, sizeof es); snprintf(err, errlen, "socket: %s", es); return SOCK_BAD; }
    {
        struct sockaddr_in a4;
        net_set_reuseaddr(s);
        memset(&a4, 0, sizeof a4);
        a4.sin_family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_ANY);
        a4.sin_port = htons((unsigned short)port);
        if (bind(s, (struct sockaddr *)&a4, sizeof a4) != 0 || listen(s, 128) != 0) {
            net_strerror(es, sizeof es);
            snprintf(err, errlen, "cannot listen on port %d: %s", port, es);
            net_close(s);
            return SOCK_BAD;
        }
    }
    net_set_nonblock(s);
    return s;
}

sock_t net_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen) {
    struct addrinfo hints, *res = NULL, *ai;
    char ps[16], es[160];
    int gr;

    snprintf(ps, sizeof ps, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    gr = getaddrinfo(host, ps, &hints, &res);
    if (gr != 0) {
        snprintf(err, errlen, "cannot resolve '%s': %s", host, gai_strerror(gr));
        return SOCK_BAD;
    }
    snprintf(err, errlen, "no usable address for '%s'", host);

    for (ai = res; ai; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        int ok;
        if (s == SOCK_BAD) {
            net_strerror(es, sizeof es);
            snprintf(err, errlen, "cannot create socket for '%s': %s", host, es);
            continue;
        }
        net_set_nonblock(s);
        ok = (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0);
        if (!ok && net_in_progress()) {
            fd_set w, x;
            struct timeval tv;
            int n;
            FD_ZERO(&w); FD_ZERO(&x);
            FD_SET(s, &w); FD_SET(s, &x);
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            n = select((int)s + 1, NULL, &w, &x, &tv);
            if (n > 0 && FD_ISSET(s, &w) && !FD_ISSET(s, &x)) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
                if (soerr == 0) ok = 1;
                else { err_text(soerr, es, sizeof es); snprintf(err, errlen, "connect to %s:%d failed: %s", host, port, es); }
            } else if (n == 0) {
                snprintf(err, errlen, "connect to %s:%d timed out", host, port);
            } else {
                snprintf(err, errlen, "connect to %s:%d failed", host, port);
            }
        } else if (!ok) {
            net_strerror(es, sizeof es);
            snprintf(err, errlen, "connect to %s:%d failed: %s", host, port, es);
        }
        if (ok) {
            net_set_nodelay(s);
            freeaddrinfo(res);
            return s;
        }
        net_close(s);
    }
    freeaddrinfo(res);
    return SOCK_BAD;
}
