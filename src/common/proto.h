/*
 * proto.h - wire protocol and shared game constants.
 *
 * Transport : TCP (TCP_NODELAY on both ends).
 * Framing   : [u16 big-endian length][u8 type][payload]
 *             'length' counts the type byte plus the payload (1..PP_MAX_FRAME).
 * Integers  : big-endian. Strings are [u8 length][bytes], printable ASCII only.
 *
 * The server is authoritative: clients only send paddle direction (-1/0/+1),
 * the server simulates the game and streams snapshots back.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PP_VERSION        1
#define PP_DEFAULT_PORT   7777
#define PP_MAX_FRAME      1024      /* type byte + payload */
#define PP_NAME_MAX       16
#define PP_ROOM_NAME_MAX  24
#define PP_CHAT_MAX       90
#define PP_MAX_QUEUE      12        /* waiting players per match */
#define PP_MAX_LIST       32        /* matches returned by one LIST */

/* client -> server */
enum { C_HELLO = 1, C_LIST, C_CREATE, C_JOIN, C_LEAVE, C_INPUT, C_CHAT, C_PING };

/* server -> client */
enum {
    S_WELCOME = 101, S_ROOMS, S_JOINED, S_ROOM_INFO, S_STATE,
    S_CHAT, S_NOTICE, S_ERROR, S_PONG, S_LEFT_ROOM
};

/* error codes (1..ERR_SHUTDOWN are fatal: the server closes the connection) */
enum {
    ERR_VERSION = 1, ERR_FULL, ERR_LIMIT, ERR_FLOOD, ERR_PROTO, ERR_TIMEOUT, ERR_SHUTDOWN,
    ERR_NO_ROOM, ERR_ROOM_FULL, ERR_STATE, ERR_MAX_ROOMS
};
static inline int err_is_fatal(int code) { return code >= ERR_VERSION && code <= ERR_SHUTDOWN; }

/* match phases */
enum { PH_WAITING, PH_COUNTDOWN, PH_PLAYING, PH_POINT, PH_GAMEOVER };

/* a member's role in a match */
enum { ROLE_LEFT, ROLE_RIGHT, ROLE_QUEUE };

/* ---------- shared game constants (server physics + client rendering) ---------- */
#define TICK_HZ            60
#define SNAPSHOT_DIV       2         /* send a snapshot every N ticks (30 Hz) */
#define FIELD_W            60.0f
#define FIELD_H            16.0f
#define PAD_H              4.0f
#define PAD_FACE           2.0f      /* x of the left paddle's hitting face */
#define PAD_SPEED          22.0f     /* units/second */
#define BALL_R             0.5f
#define SERVE_SPEED        22.0f
#define MAX_SPEED          48.0f
#define SPEED_UP           1.06f     /* per paddle hit */
#define MAX_ANGLE          0.95f     /* radians, at the paddle edge */
#define DEFAULT_WIN_SCORE  7
#define FP                 100.0f    /* fixed-point scale on the wire */

/* ---------- buffer writer ---------- */
typedef struct { uint8_t *p; size_t len, cap; int err; } Wr;

static inline void wr_init(Wr *w, uint8_t *buf, size_t cap) { w->p = buf; w->len = 0; w->cap = cap; w->err = 0; }
static inline void wr_u8(Wr *w, unsigned v) {
    if (w->len + 1 > w->cap) { w->err = 1; return; }
    w->p[w->len++] = (uint8_t)v;
}
static inline void wr_u16(Wr *w, unsigned v) { wr_u8(w, (v >> 8) & 0xff); wr_u8(w, v & 0xff); }
static inline void wr_i16(Wr *w, int v) { wr_u16(w, (unsigned)(v & 0xffff)); }
static inline void wr_u32(Wr *w, uint32_t v) { wr_u16(w, v >> 16); wr_u16(w, v & 0xffff); }
static inline void wr_str(Wr *w, const char *s, size_t maxlen) {
    size_t n = strlen(s);
    if (n > maxlen) n = maxlen;
    wr_u8(w, (unsigned)n);
    if (w->len + n > w->cap) { w->err = 1; return; }
    memcpy(w->p + w->len, s, n);
    w->len += n;
}
static inline void wr_begin(Wr *w, int type) { wr_u16(w, 0); wr_u8(w, (unsigned)type); }
static inline void wr_end(Wr *w) {
    if (w->err || w->len < 3 || w->len - 2 > PP_MAX_FRAME) { w->err = 1; return; }
    size_t flen = w->len - 2;
    w->p[0] = (uint8_t)(flen >> 8);
    w->p[1] = (uint8_t)(flen & 0xff);
}

/* ---------- bounds-checked buffer reader ---------- */
typedef struct { const uint8_t *p; size_t len, pos; int err; } Rd;

static inline void rd_init(Rd *r, const uint8_t *p, size_t len) { r->p = p; r->len = len; r->pos = 0; r->err = 0; }
static inline unsigned rd_u8(Rd *r) {
    if (r->pos + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->pos++];
}
static inline unsigned rd_u16(Rd *r) { unsigned hi = rd_u8(r); return (hi << 8) | rd_u8(r); }
static inline int rd_i16(Rd *r) { return (int16_t)rd_u16(r); }
static inline uint32_t rd_u32(Rd *r) { uint32_t hi = rd_u16(r); return (hi << 16) | rd_u16(r); }

/* Reads a length-prefixed string into out (always NUL-terminated).
 * Anything outside printable ASCII becomes '?', so a hostile peer can never
 * inject terminal escape sequences through names or chat. */
static inline void rd_str(Rd *r, char *out, size_t outcap) {
    size_t n = rd_u8(r), i, o = 0;
    if (r->pos + n > r->len) { r->err = 1; out[0] = 0; return; }
    for (i = 0; i < n; i++) {
        unsigned char c = r->p[r->pos + i];
        if (o + 1 < outcap) out[o++] = (c < 32 || c > 126) ? '?' : (char)c;
    }
    out[o] = 0;
    r->pos += n;
}

static inline void pp_trim(char *s) {
    size_t n = strlen(s), i = 0;
    while (s[i] == ' ') i++;
    if (i) { memmove(s, s + i, n - i + 1); n -= i; }
    while (n && s[n - 1] == ' ') s[--n] = 0;
}

#endif
