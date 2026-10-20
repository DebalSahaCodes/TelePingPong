/*
 * client.c - terminal client: lobby (browse / host / join) and match view.
 *
 * The client never simulates the game. It sends paddle direction changes to
 * the server and renders the 30 Hz snapshots it gets back, extrapolating the
 * ball/paddles a few milliseconds between snapshots so motion looks smooth.
 */
#include "platform.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <signal.h>

#define GW        60            /* field cells, must match FIELD_W/FIELD_H */
#define GH        16
#define MIN_COLS  (GW + 2)
#define MIN_ROWS  24

enum { M_CONNECTING, M_LOBBY, M_ROOM };
enum { IN_NONE, IN_HOST, IN_CHAT };

typedef struct {
    uint16_t id;
    uint8_t  np, nq, phase;
    char     name[PP_ROOM_NAME_MAX + 1];
    char     host[PP_NAME_MAX + 1];
} RoomEntry;

typedef struct {
    int   phase, ms, score[2], dir[2];
    float bx, by, bvx, bvy, pad[2];
} Snap;

typedef struct {
    sock_t   s;
    uint8_t  rx[2 + PP_MAX_FRAME];
    size_t   rxlen;
    uint8_t  tx[8192];
    size_t   txlen;

    int      mode, input_mode, need_clear;
    char     input[PP_CHAT_MAX + 1];
    int      inlen;
    char     myname[PP_NAME_MAX + 1];
    char     server[160];
    int      online, srv_rooms;

    RoomEntry rooms[PP_MAX_LIST];
    int      nrooms, sel;

    char     roomname[PP_ROOM_NAME_MAX + 1];
    char     left[PP_NAME_MAX + 1], right[PP_NAME_MAX + 1];
    char     queue[PP_MAX_QUEUE][PP_NAME_MAX + 1];
    int      nq, role, qpos;

    Snap     snap;
    uint64_t snap_t;
    int      have_snap;
    int      dir;

    char     chat[8][128];
    int      nchat;

    char     status[120];
    uint64_t status_until;

    int      rtt;
    uint64_t t_ping, t_list, t_render;

    int      dead;
    char     dead_msg[220];
    char     last_err[200];
} App;

static volatile int g_quit = 0;
static void on_signal(int s) { (void)s; g_quit = 1; }

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
static void die(App *a, const char *msg) {
    if (a->dead) return;
    a->dead = 1;
    snprintf(a->dead_msg, sizeof a->dead_msg, "%s", a->last_err[0] ? a->last_err : msg);
}

#if defined(__GNUC__)
static void set_status(App *a, int ms, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void chat_add(App *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void set_status(App *a, int ms, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
    a->status_until = now_ms() + (uint64_t)ms;
}

static void chat_add(App *a, const char *fmt, ...) {
    va_list ap;
    if (a->nchat == 8) { memmove(a->chat[0], a->chat[1], sizeof a->chat[0] * 7); a->nchat = 7; }
    va_start(ap, fmt);
    vsnprintf(a->chat[a->nchat], sizeof a->chat[0], fmt, ap);
    va_end(ap);
    a->nchat++;
}

/* ------------------------------------------------------------------ */
/* sending                                                            */
/* ------------------------------------------------------------------ */
static void app_flush(App *a) {
    while (a->txlen > 0 && !a->dead) {
        int n = (int)send(a->s, (const char *)a->tx, (int)a->txlen, MSG_NOSIGNAL);
        if (n > 0) { memmove(a->tx, a->tx + n, a->txlen - (size_t)n); a->txlen -= (size_t)n; continue; }
        if (n < 0 && net_would_block()) return;
        if (n < 0 && net_interrupted()) continue;
        die(a, "connection lost");
        return;
    }
}

static void app_send(App *a, Wr *w) {
    wr_end(w);
    if (w->err || a->dead) return;
    if (a->txlen + w->len > sizeof a->tx) { die(a, "connection too slow (send buffer full)"); return; }
    memcpy(a->tx + a->txlen, w->p, w->len);
    a->txlen += w->len;
    app_flush(a);
}

static void send_simple(App *a, int type) {
    uint8_t b[8]; Wr w;
    wr_init(&w, b, sizeof b); wr_begin(&w, type); app_send(a, &w);
}
static void send_hello(App *a) {
    uint8_t b[64]; Wr w;
    wr_init(&w, b, sizeof b); wr_begin(&w, C_HELLO);
    wr_u8(&w, PP_VERSION); wr_str(&w, a->myname, PP_NAME_MAX);
    app_send(a, &w);
}
static void send_create(App *a, const char *name) {
    uint8_t b[64]; Wr w;
    wr_init(&w, b, sizeof b); wr_begin(&w, C_CREATE);
    wr_str(&w, name, PP_ROOM_NAME_MAX); app_send(a, &w);
}
static void send_join(App *a, unsigned id) {
    uint8_t b[16]; Wr w;
    wr_init(&w, b, sizeof b); wr_begin(&w, C_JOIN); wr_u16(&w, id); app_send(a, &w);
}
static void send_input(App *a, int dir) {
    uint8_t b[16]; Wr w;
    wr_init(&w, b, sizeof b); wr_begin(&w, C_INPUT); wr_u8(&w, (unsigned)(dir + 1)); app_send(a, &w);
}
static void send_chat(App *a, const char *text) {
    uint8_t b[160]; Wr w;
    wr_init(&w, b, sizeof b); wr_begin(&w, C_CHAT); wr_str(&w, text, PP_CHAT_MAX); app_send(a, &w);
}
static void send_ping(App *a) {
    uint8_t b[16]; Wr w;
    wr_init(&w, b, sizeof b); wr_begin(&w, C_PING); wr_u32(&w, (uint32_t)now_ms()); app_send(a, &w);
}

/* ------------------------------------------------------------------ */
/* receiving                                                          */
/* ------------------------------------------------------------------ */
static void on_frame(App *a, int type, Rd *r) {
    switch (type) {
    case S_WELCOME:
        (void)rd_u16(r);
        a->online = (int)rd_u16(r);
        a->srv_rooms = (int)rd_u16(r);
        a->mode = M_LOBBY;
        a->need_clear = 1;
        send_simple(a, C_LIST);
        break;
    case S_ROOMS: {
        int n = (int)rd_u8(r), i;
        if (n > PP_MAX_LIST) n = PP_MAX_LIST;
        for (i = 0; i < n && !r->err; i++) {
            RoomEntry *e = &a->rooms[i];
            e->id = (uint16_t)rd_u16(r);
            e->np = (uint8_t)rd_u8(r);
            e->nq = (uint8_t)rd_u8(r);
            e->phase = (uint8_t)rd_u8(r);
            rd_str(r, e->name, sizeof e->name);
            rd_str(r, e->host, sizeof e->host);
        }
        if (!r->err) {
            a->nrooms = i;
            if (a->sel >= a->nrooms) a->sel = a->nrooms ? a->nrooms - 1 : 0;
        }
        break;
    }
    case S_JOINED:
        (void)rd_u16(r);
        a->mode = M_ROOM;
        a->need_clear = 1;
        a->have_snap = 0;
        a->dir = 0;
        a->nchat = 0;
        a->role = ROLE_QUEUE;
        a->qpos = 0;
        break;
    case S_ROOM_INFO: {
        int role = (int)rd_u8(r), pos = (int)rd_u8(r), i, n;
        char rn[PP_ROOM_NAME_MAX + 1], l[PP_NAME_MAX + 1], rr[PP_NAME_MAX + 1];
        rd_str(r, rn, sizeof rn); rd_str(r, l, sizeof l); rd_str(r, rr, sizeof rr);
        n = (int)rd_u8(r);
        if (n > PP_MAX_QUEUE) n = PP_MAX_QUEUE;
        if (r->err || a->mode != M_ROOM) break;
        snprintf(a->roomname, sizeof a->roomname, "%s", rn);
        snprintf(a->left, sizeof a->left, "%s", l);
        snprintf(a->right, sizeof a->right, "%s", rr);
        for (i = 0; i < n; i++) rd_str(r, a->queue[i], sizeof a->queue[i]);
        a->nq = n;
        if (role != a->role) a->dir = 0;
        a->role = role;
        a->qpos = pos;
        break;
    }
    case S_STATE: {
        Snap s;
        s.phase = (int)rd_u8(r);
        s.ms = (int)rd_u16(r);
        s.score[0] = (int)rd_u8(r);
        s.score[1] = (int)rd_u8(r);
        s.bx = (float)rd_i16(r) / FP;
        s.by = (float)rd_i16(r) / FP;
        s.bvx = (float)rd_i16(r) / FP;
        s.bvy = (float)rd_i16(r) / FP;
        s.pad[0] = (float)rd_i16(r) / FP;
        s.pad[1] = (float)rd_i16(r) / FP;
        s.dir[0] = (int)rd_u8(r) - 1;
        s.dir[1] = (int)rd_u8(r) - 1;
        if (!r->err && a->mode == M_ROOM) { a->snap = s; a->snap_t = now_ms(); a->have_snap = 1; }
        break;
    }
    case S_CHAT: {
        char who[PP_NAME_MAX + 1], text[PP_CHAT_MAX + 1];
        rd_str(r, who, sizeof who); rd_str(r, text, sizeof text);
        if (!r->err) chat_add(a, "%s: %s", who, text);
        break;
    }
    case S_NOTICE: {
        char text[128];
        rd_str(r, text, sizeof text);
        if (!r->err) chat_add(a, "* %s", text);
        break;
    }
    case S_ERROR: {
        int code = (int)rd_u8(r);
        char text[128];
        rd_str(r, text, sizeof text);
        if (r->err) break;
        if (err_is_fatal(code)) snprintf(a->last_err, sizeof a->last_err, "Server: %s", text);
        else set_status(a, 4000, "%s", text);
        break;
    }
    case S_PONG: {
        uint32_t ts = rd_u32(r);
        if (!r->err) {
            int rtt = (int)((uint32_t)now_ms() - ts);
            a->rtt = a->rtt ? (a->rtt * 3 + rtt) / 4 : rtt;
        }
        break;
    }
    case S_LEFT_ROOM:
        if (a->mode == M_ROOM) { a->mode = M_LOBBY; a->need_clear = 1; }
        a->t_list = 0;
        break;
    default:
        break;
    }
}

static void pump_read(App *a) {
    int guard;
    for (guard = 0; guard < 8 && !a->dead; guard++) {
        int n = (int)recv(a->s, (char *)a->rx + a->rxlen, (int)(sizeof a->rx - a->rxlen), 0);
        if (n > 0) {
            a->rxlen += (size_t)n;
            for (;;) {
                size_t flen, total;
                Rd r;
                if (a->rxlen < 2) break;
                flen = ((size_t)a->rx[0] << 8) | a->rx[1];
                if (flen == 0 || flen > PP_MAX_FRAME) { die(a, "protocol error (bad frame)"); return; }
                total = 2 + flen;
                if (a->rxlen < total) break;
                rd_init(&r, a->rx + 3, flen - 1);
                on_frame(a, a->rx[2], &r);
                memmove(a->rx, a->rx + total, a->rxlen - total);
                a->rxlen -= total;
            }
        } else if (n == 0) {
            die(a, "server closed the connection");
            return;
        } else {
            if (net_would_block()) return;
            if (net_interrupted()) continue;
            die(a, "connection lost");
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* rendering                                                          */
/* ------------------------------------------------------------------ */
static char g_out[32768];
static size_t g_on;

#if defined(__GNUC__)
static void oput(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(int row, int cols, const char *sgr, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#endif
static void oput(const char *fmt, ...) {
    va_list ap;
    int n;
    if (g_on >= sizeof g_out - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(g_out + g_on, sizeof g_out - g_on, fmt, ap);
    va_end(ap);
    if (n > 0) g_on += ((size_t)n < sizeof g_out - g_on) ? (size_t)n : sizeof g_out - g_on - 1;
}

/* One full-width text row at an absolute position (never scrolls the screen). */
static void line(int row, int cols, const char *sgr, const char *fmt, ...) {
    char t[256];
    va_list ap;
    int lim = cols - 1;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (lim > 200) lim = 200;
    if (lim < 0) lim = 0;
    if ((int)strlen(t) > lim) t[lim] = 0;
    oput("\x1b[%d;1H%s%s\x1b[0m\x1b[K", row, sgr ? sgr : "", t);
}

static const char *phase_name(int p) {
    switch (p) {
        case PH_WAITING:   return "waiting";
        case PH_COUNTDOWN: return "starting";
        case PH_PLAYING:   return "playing";
        case PH_POINT:     return "playing";
        default:           return "game over";
    }
}

static void render_lobby(App *a, int cols, int rows) {
    int cap = rows - 8, i, first;
    line(1, cols, "\x1b[1;37;44m", " PING PONG - lobby    server %s    ping %dms", a->server, a->rtt);
    line(2, cols, "", " Playing as %s  |  %d online, %d open matches", a->myname, a->online, a->nrooms);
    line(3, cols, "\x1b[90m", " ------------------------------------------------------------------------");
    line(4, cols, "\x1b[1m", "   #  %-24s %-16s %-8s %-6s %s", "Match", "Host", "Players", "Queue", "Status");
    if (cap < 1) cap = 1;
    first = 0;
    if (a->sel >= cap) first = a->sel - cap + 1;
    for (i = 0; i < cap; i++) {
        int idx = first + i, row = 5 + i;
        if (idx < a->nrooms) {
            RoomEntry *e = &a->rooms[idx];
            line(row, cols, idx == a->sel ? "\x1b[7m" : "", " %c%2d  %-24s %-16s %d/2      %-6d %s",
                 idx == a->sel ? '>' : ' ', idx + 1, e->name, e->host, e->np, e->nq, phase_name(e->phase));
        } else if (idx == 0 && a->nrooms == 0) {
            line(row, cols, "\x1b[90m", "   No matches yet - press H to host one and wait for others to join.");
        } else {
            line(row, cols, "", "%s", "");
        }
    }
    line(rows - 3, cols, "\x1b[90m", " ------------------------------------------------------------------------");
    line(rows - 2, cols, "", " [Up/Down] select  [Enter/1-9] join  [H] host  [R] refresh  [Q] quit");
    if (a->status[0] && now_ms() < a->status_until) line(rows - 1, cols, "\x1b[1;33m", " %s", a->status);
    else line(rows - 1, cols, "", "%s", "");
    if (a->input_mode == IN_HOST) line(rows, cols, "\x1b[1;36m", " Name your match (Enter = default, Esc = cancel): %s_", a->input);
    else line(rows, cols, "", "%s", "");
}

enum { C_DEF, C_LP, C_RP, C_BALL, C_NET, C_TXT, C_LPM, C_RPM };
static const char *const COL[] = {
    "\x1b[0m", "\x1b[36m", "\x1b[35m", "\x1b[1;33m", "\x1b[90m", "\x1b[1;37m", "\x1b[1;36m", "\x1b[1;35m"
};

/* Ball/paddle positions for this frame: last snapshot + a little extrapolation. */
static void view_state(const App *a, float *bx, float *by, float *p0, float *p1) {
    const Snap *s = &a->snap;
    float dt = (float)(now_ms() - a->snap_t) / 1000.0f, x = s->bx, y = s->by;
    int i;
    float p[2];
    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    if (s->phase == PH_PLAYING) {
        x += s->bvx * dt;
        y += s->bvy * dt;
        if (y < 0) y = -y;
        if (y > FIELD_H) y = 2 * FIELD_H - y;
        if (s->bx >= PAD_FACE && s->bx <= FIELD_W - PAD_FACE) {
            if (x < PAD_FACE) x = PAD_FACE;
            if (x > FIELD_W - PAD_FACE) x = FIELD_W - PAD_FACE;
        }
    }
    for (i = 0; i < 2; i++) {
        p[i] = s->pad[i] + (float)s->dir[i] * PAD_SPEED * dt;
        if (p[i] < PAD_H / 2) p[i] = PAD_H / 2;
        if (p[i] > FIELD_H - PAD_H / 2) p[i] = FIELD_H - PAD_H / 2;
    }
    *bx = x; *by = y; *p0 = p[0]; *p1 = p[1];
}

static void render_room(App *a, int cols, int rows) {
    unsigned char ch[GH][GW], co[GH][GW];
    char ov[96] = "", tmp[256];
    float bx, by, p0, p1;
    int x, y, i, chat_lines = rows - 22, hint_row, bc, br;
    const Snap *s = &a->snap;

    if (chat_lines < 1) chat_lines = 1;
    if (chat_lines > 6) chat_lines = 6;
    hint_row = 22 + chat_lines;

    memset(ch, ' ', sizeof ch);
    memset(co, C_DEF, sizeof co);
    for (y = 0; y < GH; y += 2) { ch[y][GW / 2] = ':'; co[y][GW / 2] = C_NET; }

    if (a->have_snap) {
        view_state(a, &bx, &by, &p0, &p1);
        for (y = 0; y < GH; y++) {
            float c = (float)y + 0.5f;
            if (c >= p0 - PAD_H / 2 && c <= p0 + PAD_H / 2) { ch[y][1] = '#'; co[y][1] = (a->role == ROLE_LEFT) ? C_LPM : C_LP; }
            if (c >= p1 - PAD_H / 2 && c <= p1 + PAD_H / 2) { ch[y][GW - 2] = '#'; co[y][GW - 2] = (a->role == ROLE_RIGHT) ? C_RPM : C_RP; }
        }
        bc = (int)bx; br = (int)by;
        if (bc < 0) bc = 0;
        if (bc >= GW) bc = GW - 1;
        if (br < 0) br = 0;
        if (br >= GH) br = GH - 1;
        ch[br][bc] = 'O'; co[br][bc] = C_BALL;

        switch (s->phase) {
            case PH_WAITING:   snprintf(ov, sizeof ov, "Waiting for an opponent..."); break;
            case PH_COUNTDOWN: snprintf(ov, sizeof ov, "Get ready... %d", (s->ms + 999) / 1000); break;
            case PH_POINT:     snprintf(ov, sizeof ov, "Point!"); break;
            case PH_GAMEOVER:  snprintf(ov, sizeof ov, "%s wins!", s->score[0] > s->score[1] ? a->left : a->right); break;
            default: break;
        }
    } else {
        snprintf(ov, sizeof ov, "Joining...");
    }
    for (i = 0; ov[i] && i < GW; i++) {
        x = (GW - (int)strlen(ov)) / 2 + i;
        if (x >= 0 && x < GW) { ch[5][x] = (unsigned char)ov[i]; co[5][x] = C_TXT; }
    }

    if (a->role == ROLE_LEFT) snprintf(tmp, sizeof tmp, "you: LEFT paddle");
    else if (a->role == ROLE_RIGHT) snprintf(tmp, sizeof tmp, "you: RIGHT paddle");
    else snprintf(tmp, sizeof tmp, "you: waiting, #%d in queue", a->qpos);
    line(1, cols, "\x1b[1;37;44m", " PING PONG | %s | %s | ping %dms", a->roomname, tmp, a->rtt);

    oput("\x1b[2;1H\x1b[90m+");
    for (x = 0; x < GW; x++) oput("-");
    oput("+\x1b[0m");
    for (y = 0; y < GH; y++) {
        int cur = -1;
        oput("\x1b[%d;1H\x1b[90m|\x1b[0m", 3 + y);
        for (x = 0; x < GW; x++) {
            if (co[y][x] != cur) { cur = co[y][x]; oput("%s", COL[cur]); }
            oput("%c", ch[y][x]);
        }
        oput("\x1b[0m\x1b[90m|\x1b[0m");
    }
    oput("\x1b[%d;1H\x1b[90m+", 3 + GH);
    for (x = 0; x < GW; x++) oput("-");
    oput("+\x1b[0m");

    snprintf(tmp, sizeof tmp, " %s%s  %d  :  %d  %s%s", a->left[0] ? a->left : "(empty)", a->role == ROLE_LEFT ? " (you)" : "",
             s->score[0], s->score[1], a->right[0] ? a->right : "(empty)", a->role == ROLE_RIGHT ? " (you)" : "");
    line(4 + GH, cols, "\x1b[1m", "%s", a->have_snap ? tmp : "");

    if (a->nq == 0) {
        line(5 + GH, cols, "\x1b[90m", " Queue: empty");
    } else {
        char q[200];
        size_t n = 0;
        q[0] = 0;
        for (i = 0; i < a->nq && n + PP_NAME_MAX + 4 < sizeof q; i++)
            n += (size_t)snprintf(q + n, sizeof q - n, "%s%d.%s", i ? "  " : "", i + 1, a->queue[i]);
        line(5 + GH, cols, "", " Queue (next up first): %s", q);
    }

    for (i = 0; i < chat_lines; i++) {
        int idx = a->nchat - chat_lines + i;
        if (idx >= 0) line(22 + i, cols, a->chat[idx][0] == '*' ? "\x1b[33m" : "", " %s", a->chat[idx]);
        else line(22 + i, cols, "", "%s", "");
    }
    if (a->input_mode == IN_CHAT) line(hint_row, cols, "\x1b[1;36m", " say> %s_", a->input);
    else if (a->status[0] && now_ms() < a->status_until) line(hint_row, cols, "\x1b[1;33m", " %s", a->status);
    else line(hint_row, cols, "\x1b[90m", " W/S or Up/Down: move   Space: stop   T: chat   Q: leave match");
}

static void render(App *a) {
    int cols, rows;
    term_size(&cols, &rows);
    g_on = 0;
    if (a->need_clear) { oput("\x1b[2J"); a->need_clear = 0; }
    if (cols < MIN_COLS || rows < MIN_ROWS) {
        oput("\x1b[2J\x1b[1;1HPlease enlarge the terminal window to at least %dx%d (now %dx%d).", MIN_COLS, MIN_ROWS, cols, rows);
        a->need_clear = 1;
    } else if (a->mode == M_CONNECTING) {
        line(1, cols, "\x1b[1;37;44m", " PING PONG");
        line(3, cols, "", " Connecting to %s ...", a->server);
    } else if (a->mode == M_LOBBY) {
        render_lobby(a, cols, rows);
    } else {
        render_room(a, cols, rows);
    }
    term_write(g_out, g_on);
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */
static void set_dir(App *a, int d) {
    if (d < -1) d = -1;
    if (d > 1) d = 1;
    if (a->role == ROLE_QUEUE || d == a->dir) return;
    a->dir = d;
    send_input(a, d);
}

static void handle_text_input(App *a, int k) {
    int max = (a->input_mode == IN_HOST) ? PP_ROOM_NAME_MAX : PP_CHAT_MAX;
    if (k == KEY_ESC) {
        a->input_mode = IN_NONE;
    } else if (k == KEY_ENTER) {
        if (a->input_mode == IN_HOST) send_create(a, a->input);
        else if (a->inlen > 0) send_chat(a, a->input);
        a->input_mode = IN_NONE;
    } else if (k == KEY_BACKSPACE) {
        if (a->inlen > 0) a->input[--a->inlen] = 0;
    } else if (k >= 32 && k <= 126 && a->inlen < max) {
        a->input[a->inlen++] = (char)k;
        a->input[a->inlen] = 0;
    }
}

static void begin_input(App *a, int mode) {
    a->input_mode = mode;
    a->input[0] = 0;
    a->inlen = 0;
}

static void handle_key(App *a, int k) {
    if (k == KEY_QUIT) { g_quit = 1; return; }
    if (a->input_mode) { handle_text_input(a, k); return; }

    if (a->mode == M_LOBBY) {
        if (k == 'q' || k == 'Q' || k == KEY_ESC) g_quit = 1;
        else if (k == 'r' || k == 'R') { send_simple(a, C_LIST); a->t_list = now_ms(); }
        else if (k == 'h' || k == 'H') begin_input(a, IN_HOST);
        else if ((k == KEY_UP || k == 'w' || k == 'k') && a->sel > 0) a->sel--;
        else if ((k == KEY_DOWN || k == 's' || k == 'j') && a->sel + 1 < a->nrooms) a->sel++;
        else if (k == KEY_ENTER && a->nrooms > 0) send_join(a, a->rooms[a->sel].id);
        else if (k >= '1' && k <= '9' && (k - '1') < a->nrooms) send_join(a, a->rooms[k - '1'].id);
    } else if (a->mode == M_ROOM) {
        if (k == 'q' || k == 'Q' || k == KEY_ESC) {
            send_simple(a, C_LEAVE);
            a->mode = M_LOBBY;
            a->need_clear = 1;
            a->t_list = 0;
            a->have_snap = 0;
            a->dir = 0;
        }
        else if (k == 'w' || k == 'W' || k == 'k' || k == 'K' || k == KEY_UP)   set_dir(a, a->dir - 1);
        else if (k == 's' || k == 'S' || k == 'j' || k == 'J' || k == KEY_DOWN) set_dir(a, a->dir + 1);
        else if (k == ' ' || k == 'x' || k == 'X') set_dir(a, 0);
        else if (k == 't' || k == 'T' || k == KEY_ENTER) { set_dir(a, 0); begin_input(a, IN_CHAT); }
    }
}

/* ------------------------------------------------------------------ */
/* startup                                                            */
/* ------------------------------------------------------------------ */
static void parse_hostport(const char *in, char *host, size_t hn, int *port) {
    const char *c1;
    *port = PP_DEFAULT_PORT;
    if (in[0] == '[') {                               /* [v6addr]:port */
        const char *e = strchr(in, ']');
        size_t n = e ? (size_t)(e - in - 1) : strlen(in) - 1;
        if (n >= hn) n = hn - 1;
        memcpy(host, in + 1, n);
        host[n] = 0;
        if (e && e[1] == ':' && atoi(e + 2) > 0) *port = atoi(e + 2);
        return;
    }
    c1 = strchr(in, ':');
    if (c1 && !strchr(c1 + 1, ':')) {                 /* host:port (a bare v6 has >1 colon) */
        size_t n = (size_t)(c1 - in);
        if (n >= hn) n = hn - 1;
        memcpy(host, in, n);
        host[n] = 0;
        if (atoi(c1 + 1) > 0) *port = atoi(c1 + 1);
    } else {
        snprintf(host, hn, "%s", in);
    }
}

static void prompt(const char *q, const char *def, char *out, size_t n) {
    size_t l;
    printf("%s", q);
    fflush(stdout);
    if (!fgets(out, (int)n, stdin)) out[0] = 0;
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    pp_trim(out);
    if (!out[0] && def) snprintf(out, n, "%s", def);
}

static void pause_on_windows(void) {
#ifdef _WIN32
    fprintf(stderr, "\nPress Enter to close...");
    fflush(stderr);
    (void)getchar();
#endif
}

int client_main(int argc, char **argv) {
    static App a;
    char host[160] = "", srvarg[160] = "", namearg[64] = "", err[200];
    int port, i;
    sock_t s;
    const char *env;

    for (i = 0; i < argc; i++) {
        if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--server")) && i + 1 < argc) snprintf(srvarg, sizeof srvarg, "%s", argv[++i]);
        else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--name")) && i + 1 < argc) snprintf(namearg, sizeof namearg, "%s", argv[++i]);
        else {
            fprintf(stderr, "usage: pingpong [-s HOST[:PORT]] [-n NAME]\n"
                            "       pingpong server [-p PORT]\n");
            return !(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"));
        }
    }
    if (net_init() != 0) { fprintf(stderr, "network init failed\n"); pause_on_windows(); return 1; }

    if (!srvarg[0] && (env = getenv("PINGPONG_SERVER")) != NULL) snprintf(srvarg, sizeof srvarg, "%s", env);
    printf("PING PONG - online multiplayer\n\n");
    if (!srvarg[0]) prompt("Server address (host or host:port) [localhost]: ", "localhost", srvarg, sizeof srvarg);
    if (!namearg[0]) {
        const char *def = getenv("USER");
        if (!def) def = getenv("USERNAME");
        if (!def) def = "player";
        snprintf(namearg, sizeof namearg, "%s", def);
        {
            char q[128], d[64];
            snprintf(d, sizeof d, "%s", namearg);
            snprintf(q, sizeof q, "Your name [%s]: ", d);
            prompt(q, d, namearg, sizeof namearg);
        }
    }
    snprintf(a.myname, sizeof a.myname, "%s", namearg);
    for (i = 0; a.myname[i]; i++) if ((unsigned char)a.myname[i] < 32 || (unsigned char)a.myname[i] > 126) a.myname[i] = '?';
    pp_trim(a.myname);
    if (!a.myname[0]) snprintf(a.myname, sizeof a.myname, "player");

    parse_hostport(srvarg, host, sizeof host, &port);
    snprintf(a.server, sizeof a.server, "%s:%d", host, port);
    printf("Connecting to %s ...\n", a.server);
    s = net_connect(host, port, 6000, err, sizeof err);
    if (s == SOCK_BAD) { fprintf(stderr, "%s\n", err); pause_on_windows(); return 1; }
    a.s = s;
    a.mode = M_CONNECTING;
    send_hello(&a);

    if (term_init() != 0) {
        fprintf(stderr, "pingpong needs an interactive terminal with ANSI support (Windows 10+ / any Linux terminal).\n");
        net_close(s);
        pause_on_windows();
        return 1;
    }
    atexit(term_restore);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGHUP
    signal(SIGHUP, on_signal);
#endif

    a.need_clear = 1;
    while (!g_quit && !a.dead) {
        int sock_ready = 0;
        uint64_t t;
        int k, interval;

        term_wait(a.s, 8, &sock_ready);
        if (sock_ready) pump_read(&a);
        while ((k = term_read_key()) != KEY_NONE) {
            if (k == KEY_OTHER) continue;
            handle_key(&a, k);
        }
        app_flush(&a);

        t = now_ms();
        if (a.mode != M_CONNECTING && t - a.t_ping >= 2000) { a.t_ping = t; send_ping(&a); }
        if (a.mode == M_LOBBY && t - a.t_list >= 2000) { a.t_list = t; send_simple(&a, C_LIST); }
        interval = (a.mode == M_ROOM) ? 16 : 80;
        if (t - a.t_render >= (uint64_t)interval) { a.t_render = t; render(&a); }
    }
    term_restore();
    net_close(a.s);
    if (a.dead) {
        fprintf(stderr, "Disconnected: %s\n", a.dead_msg);
        pause_on_windows();
        return 1;
    }
    printf("Thanks for playing!\n");
    return 0;
}
