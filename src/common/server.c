/*
 * server.c - lobby + match server.
 *
 * One single-threaded select() loop handles every connection:
 *   - non-blocking sockets with per-client send queues (a stalled client can
 *     never block the game; droppable snapshots are skipped when it lags and
 *     it is disconnected if its backlog grows too large)
 *   - 60 Hz fixed-timestep physics, 30 Hz snapshots, fully server-authoritative
 *   - lobby: clients list / create / join matches
 *   - each match: 2 players + a FIFO queue; the loser of each game goes to the
 *     back of the queue and the next waiting player steps in
 */
#include "platform.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <signal.h>

#define MAX_CLIENTS     512
#define MAX_ROOMS       64
#define TICK_US         (1000000 / TICK_HZ)
#define TX_LIMIT        (256 * 1024)   /* hard backlog limit -> disconnect */
#define TX_DROP_STATE   (16 * 1024)    /* soft limit -> skip snapshots     */
#define HANDSHAKE_MS    10000
#define IDLE_MS         20000
#define CLOSE_GRACE_MS  3000

enum { ST_HANDSHAKE, ST_LOBBY, ST_ROOM };

typedef struct Room Room;

typedef struct Client {
    int      used, dead, close_after_flush;
    sock_t   sock;
    uint16_t id;
    int      state;
    char     name[PP_NAME_MAX + 1];
    char     ip[48];
    uint8_t  rx[2 + PP_MAX_FRAME];
    size_t   rxlen;
    uint8_t *tx;
    size_t   txlen, txoff, txcap;
    uint64_t t_conn, t_rx, t_close;
    Room    *room;
    double   tokens;              /* flood-control bucket */
    uint64_t t_tok;
    int      chat_n;
    uint64_t chat_t;
} Client;

struct Room {
    int      used;
    uint16_t id;
    char     name[PP_ROOM_NAME_MAX + 1];
    uint16_t host_id;
    char     host_name[PP_NAME_MAX + 1];
    Client  *side[2];             /* 0 = left player, 1 = right player */
    Client  *queue[PP_MAX_QUEUE];
    int      nq;
    int      phase;
    float    left;                /* seconds left in the current timed phase */
    int      score[2];
    float    bx, by, bvx, bvy;
    float    pad[2];
    int      dir[2];
    int      serve_to;
    unsigned snap_n;
};

static Client   g_cl[MAX_CLIENTS];
static Room     g_rooms[MAX_ROOMS];
static uint16_t g_next_id = 1, g_next_room = 1;
static int      g_win_score = DEFAULT_WIN_SCORE;
static int      g_max_per_ip = 8;
static int      g_max_rooms = MAX_ROOMS;
static uint64_t g_now_ms;
static volatile int g_run = 1;

static void on_signal(int s) { (void)s; g_run = 0; }

#if defined(__GNUC__)
static void slog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
static void slog(const char *fmt, ...) {
    char ts[16];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    va_list ap;
    if (tm) strftime(ts, sizeof ts, "%H:%M:%S", tm); else snprintf(ts, sizeof ts, "--:--:--");
    printf("[%s] ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void room_remove(Room *r, Client *c, const char *how);

/* ------------------------------------------------------------------ */
/* connection plumbing                                                */
/* ------------------------------------------------------------------ */
static void kill_client(Client *c, const char *why) {
    if (c->dead) return;
    c->dead = 1;
    if (why) slog("client #%u (%s) dropped: %s", c->id, c->ip, why);
}

static void tx_put(Client *c, const uint8_t *d, size_t n, int droppable) {
    size_t pend;
    if (!c->used || c->dead || c->close_after_flush) return;
    pend = c->txlen - c->txoff;
    if (droppable && pend > TX_DROP_STATE) return;
    if (pend + n > TX_LIMIT) { kill_client(c, "send backlog too large (slow connection)"); return; }
    if (c->txoff == c->txlen) c->txoff = c->txlen = 0;
    if (c->txlen + n > c->txcap) {
        if (c->txoff) {
            memmove(c->tx, c->tx + c->txoff, pend);
            c->txlen = pend;
            c->txoff = 0;
        }
        if (c->txlen + n > c->txcap) {
            size_t nc = c->txcap ? c->txcap : 2048;
            uint8_t *p;
            while (nc < c->txlen + n) nc *= 2;
            p = (uint8_t *)realloc(c->tx, nc);
            if (!p) { kill_client(c, "out of memory"); return; }
            c->tx = p;
            c->txcap = nc;
        }
    }
    memcpy(c->tx + c->txlen, d, n);
    c->txlen += n;
}

static void flush_client(Client *c) {
    while (c->txoff < c->txlen && !c->dead) {
        int n = (int)send(c->sock, (const char *)c->tx + c->txoff, (int)(c->txlen - c->txoff), MSG_NOSIGNAL);
        if (n > 0) { c->txoff += (size_t)n; continue; }
        if (n < 0 && net_would_block()) return;
        if (n < 0 && net_interrupted()) continue;
        kill_client(c, "send error");
        return;
    }
    if (c->txoff >= c->txlen) c->txoff = c->txlen = 0;
}

static void finish_frame(Client *c, Wr *w, int droppable) {
    wr_end(w);
    if (!w->err) tx_put(c, w->p, w->len, droppable);
}

static void send_error(Client *c, int code, const char *text) {
    uint8_t b[2 + PP_MAX_FRAME];
    Wr w;
    wr_init(&w, b, sizeof b);
    wr_begin(&w, S_ERROR);
    wr_u8(&w, (unsigned)code);
    wr_str(&w, text, 120);
    finish_frame(c, &w, 0);
    if (err_is_fatal(code)) {
        /* Flush the error, then linger: we keep draining (and discarding) what
         * the peer sends so close() doesn't turn into a RST that destroys the
         * error message before the peer has read it. */
        flush_client(c);
        if (!c->dead) { c->close_after_flush = 1; c->t_close = g_now_ms; }
        if (c->room) room_remove(c->room, c, "was disconnected");
    }
}

static void send_notice_to(Client *c, const char *text) {
    uint8_t b[2 + PP_MAX_FRAME];
    Wr w;
    wr_init(&w, b, sizeof b);
    wr_begin(&w, S_NOTICE);
    wr_str(&w, text, 120);
    finish_frame(c, &w, 0);
}

/* ------------------------------------------------------------------ */
/* rooms                                                              */
/* ------------------------------------------------------------------ */
static int room_members(Room *r, Client **out) {
    int n = 0, i;
    if (r->side[0]) out[n++] = r->side[0];
    if (r->side[1]) out[n++] = r->side[1];
    for (i = 0; i < r->nq; i++) out[n++] = r->queue[i];
    return n;
}

static int room_count(Room *r) { return (r->side[0] != NULL) + (r->side[1] != NULL) + r->nq; }

#if defined(__GNUC__)
static void room_notice(Room *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void room_notice(Room *r, const char *fmt, ...) {
    char text[160];
    Client *m[2 + PP_MAX_QUEUE];
    int n, i;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    n = room_members(r, m);
    for (i = 0; i < n; i++) send_notice_to(m[i], text);
}

static void send_room_info_to(Room *r, Client *c) {
    uint8_t b[2 + PP_MAX_FRAME];
    Wr w;
    int role = ROLE_QUEUE, pos = 0, i;
    if (r->side[0] == c) role = ROLE_LEFT;
    else if (r->side[1] == c) role = ROLE_RIGHT;
    else for (i = 0; i < r->nq; i++) if (r->queue[i] == c) pos = i + 1;
    wr_init(&w, b, sizeof b);
    wr_begin(&w, S_ROOM_INFO);
    wr_u8(&w, (unsigned)role);
    wr_u8(&w, (unsigned)pos);
    wr_str(&w, r->name, PP_ROOM_NAME_MAX);
    wr_str(&w, r->side[0] ? r->side[0]->name : "", PP_NAME_MAX);
    wr_str(&w, r->side[1] ? r->side[1]->name : "", PP_NAME_MAX);
    wr_u8(&w, (unsigned)r->nq);
    for (i = 0; i < r->nq; i++) wr_str(&w, r->queue[i]->name, PP_NAME_MAX);
    finish_frame(c, &w, 0);
}

static void room_send_info(Room *r) {
    Client *m[2 + PP_MAX_QUEUE];
    int n = room_members(r, m), i;
    for (i = 0; i < n; i++) send_room_info_to(r, m[i]);
}

static void room_send_state(Room *r) {
    uint8_t b[96];
    Wr w;
    Client *m[2 + PP_MAX_QUEUE];
    int n = room_members(r, m), i;
    unsigned ms = r->left > 0 ? (unsigned)(r->left * 1000.0f) : 0;
    if (ms > 65535) ms = 65535;
    wr_init(&w, b, sizeof b);
    wr_begin(&w, S_STATE);
    wr_u8(&w, (unsigned)r->phase);
    wr_u16(&w, ms);
    wr_u8(&w, (unsigned)r->score[0]);
    wr_u8(&w, (unsigned)r->score[1]);
    wr_i16(&w, (int)(r->bx * FP));
    wr_i16(&w, (int)(r->by * FP));
    wr_i16(&w, (int)(r->bvx * FP));
    wr_i16(&w, (int)(r->bvy * FP));
    wr_i16(&w, (int)(r->pad[0] * FP));
    wr_i16(&w, (int)(r->pad[1] * FP));
    wr_u8(&w, (unsigned)(r->dir[0] + 1));
    wr_u8(&w, (unsigned)(r->dir[1] + 1));
    wr_end(&w);
    if (w.err) return;
    for (i = 0; i < n; i++) tx_put(m[i], b, w.len, 1);   /* droppable */
}

static void room_reset_ball(Room *r) {
    r->bx = FIELD_W / 2; r->by = FIELD_H / 2; r->bvx = r->bvy = 0;
}

static void room_begin(Room *r) {
    r->score[0] = r->score[1] = 0;
    r->pad[0] = r->pad[1] = FIELD_H / 2;
    room_reset_ball(r);
    r->serve_to = (frand() < 0.5f) ? 0 : 1;
    if (r->side[0] && r->side[1]) {
        r->phase = PH_COUNTDOWN;
        r->left = 3.0f;
        room_notice(r, "Match starting: %s vs %s (first to %d)", r->side[0]->name, r->side[1]->name, g_win_score);
    } else {
        r->phase = PH_WAITING;
        r->left = 0;
    }
}

/* Move waiting players from the front of the queue into empty player slots. */
static void room_fill_slots(Room *r) {
    int s, i;
    for (s = 0; s < 2; s++) {
        if (!r->side[s] && r->nq > 0) {
            Client *c = r->queue[0];
            for (i = 1; i < r->nq; i++) r->queue[i - 1] = r->queue[i];
            r->nq--;
            r->side[s] = c;
            r->dir[s] = 0;
            room_notice(r, "%s steps up to play", c->name);
        }
    }
}

static void room_destroy(Room *r) {
    slog("match #%u \"%s\" closed", r->id, r->name);
    memset(r, 0, sizeof *r);
}

static void room_pick_host(Room *r) {
    Client *m[2 + PP_MAX_QUEUE];
    if (room_members(r, m) > 0) {
        r->host_id = m[0]->id;
        snprintf(r->host_name, sizeof r->host_name, "%s", m[0]->name);
        room_notice(r, "%s is now the host", m[0]->name);
    }
}

static void room_join(Room *r, Client *c) {
    uint8_t b[16];
    Wr w;
    c->room = r;
    c->state = ST_ROOM;
    wr_init(&w, b, sizeof b);
    wr_begin(&w, S_JOINED);
    wr_u16(&w, r->id);
    finish_frame(c, &w, 0);

    if (!r->side[0])      { r->side[0] = c; r->dir[0] = 0; }
    else if (!r->side[1]) { r->side[1] = c; r->dir[1] = 0; }
    else {
        r->queue[r->nq++] = c;
        room_notice(r, "%s joined the queue (position %d)", c->name, r->nq);
    }
    if (r->side[0] && r->side[1] && r->phase == PH_WAITING) room_begin(r);
    else if (r->side[0] == c || r->side[1] == c) room_notice(r, "%s joined", c->name);
    room_send_info(r);
    slog("%s joined match #%u \"%s\" (%d in match)", c->name, r->id, r->name, room_count(r));
}

static void room_remove(Room *r, Client *c, const char *how) {
    int s = -1, qi = -1, i;
    for (i = 0; i < 2; i++) if (r->side[i] == c) s = i;
    for (i = 0; i < r->nq; i++) if (r->queue[i] == c) qi = i;
    if (s < 0 && qi < 0) return;
    c->room = NULL;
    if (!c->dead) c->state = ST_LOBBY;

    if (qi >= 0) {
        for (i = qi + 1; i < r->nq; i++) r->queue[i - 1] = r->queue[i];
        r->nq--;
        room_notice(r, "%s left the queue", c->name);
    } else {
        Client *other = r->side[1 - s];
        int mid_match = other && (r->phase == PH_COUNTDOWN || r->phase == PH_PLAYING || r->phase == PH_POINT);
        r->side[s] = NULL;
        r->dir[s] = 0;
        room_notice(r, "%s %s", c->name, how);
        if (mid_match) room_notice(r, "%s wins by forfeit", other->name);
        room_fill_slots(r);
        room_begin(r);
    }
    slog("%s left match #%u \"%s\" (%s)", c->name, r->id, r->name, how);
    if (room_count(r) == 0) { room_destroy(r); return; }
    if (r->host_id == c->id) room_pick_host(r);
    room_send_info(r);
}

/* After a finished game: loser goes to the back of the queue, next in line plays. */
static void room_rotate(Room *r) {
    int w = (r->score[0] > r->score[1]) ? 0 : 1, l = 1 - w;
    if (r->nq > 0 && r->side[l]) {
        Client *loser = r->side[l], *next = r->queue[0];
        int i;
        for (i = 1; i < r->nq; i++) r->queue[i - 1] = r->queue[i];
        r->nq--;
        r->side[l] = next;
        r->dir[l] = 0;
        r->queue[r->nq++] = loser;
        room_notice(r, "%s goes to the back of the queue; %s steps up", loser->name, next->name);
    } else if (r->side[0] && r->side[1]) {
        room_notice(r, "Nobody is waiting - rematch!");
    }
    room_begin(r);
    room_send_info(r);
}

static void room_serve(Room *r) {
    float a = (frand() - 0.5f) * 0.8f;
    float dirx = (r->serve_to == 1) ? 1.0f : -1.0f;
    r->bx = FIELD_W / 2; r->by = FIELD_H / 2;
    r->bvx = dirx * SERVE_SPEED * cosf(a);
    r->bvy = SERVE_SPEED * sinf(a);
}

static void room_point(Room *r, int scorer) {
    r->score[scorer]++;
    room_reset_ball(r);
    r->serve_to = 1 - scorer;
    if (r->score[scorer] >= g_win_score) {
        r->phase = PH_GAMEOVER;
        r->left = 3.5f;
        if (r->side[scorer])
            room_notice(r, "%s wins %d-%d!", r->side[scorer]->name, r->score[scorer], r->score[1 - scorer]);
        slog("match #%u: game over %d-%d", r->id, r->score[0], r->score[1]);
    } else {
        r->phase = PH_POINT;
        r->left = 1.2f;
    }
}

static void paddle_hit(Room *r, int s, float iy) {
    float spd = sqrtf(r->bvx * r->bvx + r->bvy * r->bvy) * SPEED_UP;
    float off = (iy - r->pad[s]) / (PAD_H * 0.5f + BALL_R), ang;
    if (spd > MAX_SPEED) spd = MAX_SPEED;
    if (off > 1) off = 1;
    if (off < -1) off = -1;
    ang = off * MAX_ANGLE;
    r->bvx = (s == 0 ? 1.0f : -1.0f) * spd * cosf(ang);
    r->bvy = spd * sinf(ang);
}

static void room_ball_step(Room *r, float dt) {
    float bx = r->bx, by = r->by;
    float nx = bx + r->bvx * dt, ny = by + r->bvy * dt;
    const float rf = FIELD_W - PAD_FACE;
    const float reach = PAD_H * 0.5f + BALL_R;

    if (ny < 0)            { ny = -ny;                    r->bvy = -r->bvy; }
    else if (ny > FIELD_H) { ny = 2 * FIELD_H - ny;       r->bvy = -r->bvy; }

    if (r->bvx < 0 && bx >= PAD_FACE && nx < PAD_FACE) {
        float t = (bx - PAD_FACE) / (bx - nx), iy = by + (ny - by) * t;
        if (fabsf(iy - r->pad[0]) <= reach) { paddle_hit(r, 0, iy); nx = PAD_FACE + 0.01f; }
    } else if (r->bvx > 0 && bx <= rf && nx > rf) {
        float t = (rf - bx) / (nx - bx), iy = by + (ny - by) * t;
        if (fabsf(iy - r->pad[1]) <= reach) { paddle_hit(r, 1, iy); nx = rf - 0.01f; }
    }

    if (nx < 0)            { room_point(r, 1); return; }
    if (nx > FIELD_W)      { room_point(r, 0); return; }
    r->bx = nx;
    r->by = ny;
}

static void room_tick(Room *r, float dt) {
    int s;
    for (s = 0; s < 2; s++) {
        if (!r->side[s]) { r->dir[s] = 0; continue; }
        r->pad[s] += (float)r->dir[s] * PAD_SPEED * dt;
        if (r->pad[s] < PAD_H / 2) r->pad[s] = PAD_H / 2;
        if (r->pad[s] > FIELD_H - PAD_H / 2) r->pad[s] = FIELD_H - PAD_H / 2;
    }
    switch (r->phase) {
    case PH_WAITING: break;
    case PH_COUNTDOWN:
    case PH_POINT:
        r->left -= dt;
        if (r->left <= 0) { room_serve(r); r->phase = PH_PLAYING; r->left = 0; }
        break;
    case PH_PLAYING:
        room_ball_step(r, dt);
        break;
    case PH_GAMEOVER:
        r->left -= dt;
        if (r->left <= 0) room_rotate(r);
        break;
    }
    r->snap_n++;
    if (r->phase == PH_WAITING ? (r->snap_n % (TICK_HZ / 4) == 0) : (r->snap_n % SNAPSHOT_DIV == 0))
        room_send_state(r);
}

static Room *room_find(unsigned id) {
    int i;
    for (i = 0; i < MAX_ROOMS; i++) if (g_rooms[i].used && g_rooms[i].id == id) return &g_rooms[i];
    return NULL;
}

static Room *room_create(const char *name, Client *host) {
    int i, n = 0;
    for (i = 0; i < MAX_ROOMS; i++) if (g_rooms[i].used) n++;
    if (n >= g_max_rooms) return NULL;
    for (i = 0; i < MAX_ROOMS; i++) {
        if (!g_rooms[i].used) {
            Room *r = &g_rooms[i];
            memset(r, 0, sizeof *r);
            r->used = 1;
            r->id = g_next_room++;
            if (g_next_room == 0) g_next_room = 1;
            snprintf(r->name, sizeof r->name, "%s", name);
            r->host_id = host->id;
            snprintf(r->host_name, sizeof r->host_name, "%s", host->name);
            r->phase = PH_WAITING;
            room_reset_ball(r);
            r->pad[0] = r->pad[1] = FIELD_H / 2;
            slog("%s created match #%u \"%s\"", host->name, r->id, r->name);
            return r;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* protocol handlers                                                  */
/* ------------------------------------------------------------------ */
static int count_lobby_clients(void) {
    int i, n = 0;
    for (i = 0; i < MAX_CLIENTS; i++) if (g_cl[i].used && !g_cl[i].dead && g_cl[i].state != ST_HANDSHAKE) n++;
    return n;
}

static int count_rooms(void) {
    int i, n = 0;
    for (i = 0; i < MAX_ROOMS; i++) if (g_rooms[i].used) n++;
    return n;
}

static void send_rooms(Client *c) {
    uint8_t b[2 + PP_MAX_FRAME];
    Wr w;
    int i, n = 0;
    size_t count_pos;
    wr_init(&w, b, sizeof b);
    wr_begin(&w, S_ROOMS);
    count_pos = w.len;
    wr_u8(&w, 0);
    for (i = 0; i < MAX_ROOMS && n < PP_MAX_LIST; i++) {
        Room *r = &g_rooms[i];
        if (!r->used) continue;
        if (w.len + 4 + 1 + PP_ROOM_NAME_MAX + 1 + PP_NAME_MAX + 8 > sizeof b) break;
        wr_u16(&w, r->id);
        wr_u8(&w, (unsigned)((r->side[0] != NULL) + (r->side[1] != NULL)));
        wr_u8(&w, (unsigned)r->nq);
        wr_u8(&w, (unsigned)r->phase);
        wr_str(&w, r->name, PP_ROOM_NAME_MAX);
        wr_str(&w, r->host_name, PP_NAME_MAX);
        n++;
    }
    if (w.err) return;
    w.p[count_pos] = (uint8_t)n;
    finish_frame(c, &w, 0);
}

static void make_unique_name(Client *c) {
    int i, clash = 0;
    pp_trim(c->name);
    if (!c->name[0]) snprintf(c->name, sizeof c->name, "player%u", c->id);
    for (i = 0; i < MAX_CLIENTS; i++) {
        Client *o = &g_cl[i];
        if (o != c && o->used && !o->dead && o->state != ST_HANDSHAKE && strcmp(o->name, c->name) == 0) clash = 1;
    }
    if (clash) {                                   /* "bob" -> "bob#17" */
        char suf[8];
        size_t sl, bl = strlen(c->name);
        snprintf(suf, sizeof suf, "#%u", (unsigned)(c->id % 1000));
        sl = strlen(suf);
        if (bl + sl > PP_NAME_MAX) bl = PP_NAME_MAX - sl;
        memcpy(c->name + bl, suf, sl + 1);
    }
}

static void handle_frame(Client *c, int type, Rd *r) {
    if (c->state == ST_HANDSHAKE && type != C_HELLO && type != C_PING) {
        send_error(c, ERR_PROTO, "expected HELLO");
        return;
    }
    switch (type) {
    case C_HELLO: {
        unsigned ver;
        char nm[PP_NAME_MAX + 1];
        uint8_t b[32];
        Wr w;
        if (c->state != ST_HANDSHAKE) { send_error(c, ERR_PROTO, "duplicate HELLO"); return; }
        ver = rd_u8(r);
        rd_str(r, nm, sizeof nm);
        if (r->err) { send_error(c, ERR_PROTO, "malformed HELLO"); return; }
        if (ver != PP_VERSION) { send_error(c, ERR_VERSION, "protocol version mismatch - please update your client"); return; }
        snprintf(c->name, sizeof c->name, "%s", nm);
        make_unique_name(c);
        c->state = ST_LOBBY;
        wr_init(&w, b, sizeof b);
        wr_begin(&w, S_WELCOME);
        wr_u16(&w, c->id);
        wr_u16(&w, (unsigned)count_lobby_clients());
        wr_u16(&w, (unsigned)count_rooms());
        finish_frame(c, &w, 0);
        slog("client #%u (%s) is \"%s\"", c->id, c->ip, c->name);
        break;
    }
    case C_LIST:
        send_rooms(c);
        break;
    case C_CREATE: {
        char rn[PP_ROOM_NAME_MAX + 1];
        Room *rm;
        rd_str(r, rn, sizeof rn);
        if (r->err) { send_error(c, ERR_PROTO, "malformed CREATE"); return; }
        if (c->state != ST_LOBBY) { send_error(c, ERR_STATE, "leave your current match first"); return; }
        pp_trim(rn);
        if (!rn[0]) snprintf(rn, sizeof rn, "%s's match", c->name);
        rm = room_create(rn, c);
        if (!rm) { send_error(c, ERR_MAX_ROOMS, "the server has too many matches right now"); return; }
        room_join(rm, c);
        break;
    }
    case C_JOIN: {
        unsigned id = rd_u16(r);
        Room *rm;
        if (r->err) { send_error(c, ERR_PROTO, "malformed JOIN"); return; }
        if (c->state != ST_LOBBY) { send_error(c, ERR_STATE, "leave your current match first"); return; }
        rm = room_find(id);
        if (!rm) { send_error(c, ERR_NO_ROOM, "that match no longer exists"); return; }
        if (rm->nq >= PP_MAX_QUEUE) { send_error(c, ERR_ROOM_FULL, "that match's queue is full"); return; }
        room_join(rm, c);
        break;
    }
    case C_LEAVE: {
        uint8_t b[16];
        Wr w;
        if (c->state == ST_ROOM && c->room) room_remove(c->room, c, "left the match");
        wr_init(&w, b, sizeof b);
        wr_begin(&w, S_LEFT_ROOM);
        finish_frame(c, &w, 0);
        break;
    }
    case C_INPUT: {
        int d = (int)rd_u8(r) - 1;           /* wire: 0,1,2 -> -1,0,+1 */
        Room *rm = c->room;
        if (r->err || d < -1 || d > 1) { send_error(c, ERR_PROTO, "malformed INPUT"); return; }
        if (c->state != ST_ROOM || !rm) return;
        if (rm->side[0] == c) rm->dir[0] = d;
        else if (rm->side[1] == c) rm->dir[1] = d;
        break;
    }
    case C_CHAT: {
        char text[PP_CHAT_MAX + 1], out[2 + PP_MAX_FRAME];
        Client *m[2 + PP_MAX_QUEUE];
        Wr w;
        int n, i;
        rd_str(r, text, sizeof text);
        if (r->err) { send_error(c, ERR_PROTO, "malformed CHAT"); return; }
        if (c->state != ST_ROOM || !c->room) return;
        pp_trim(text);
        if (!text[0]) return;
        if (g_now_ms - c->chat_t > 5000) { c->chat_t = g_now_ms; c->chat_n = 0; }
        if (++c->chat_n > 5) { send_notice_to(c, "You're chatting too fast."); return; }
        wr_init(&w, (uint8_t *)out, sizeof out);
        wr_begin(&w, S_CHAT);
        wr_str(&w, c->name, PP_NAME_MAX);
        wr_str(&w, text, PP_CHAT_MAX);
        wr_end(&w);
        if (w.err) return;
        n = room_members(c->room, m);
        for (i = 0; i < n; i++) tx_put(m[i], w.p, w.len, 0);
        break;
    }
    case C_PING: {
        uint32_t ts = rd_u32(r);
        uint8_t b[16];
        Wr w;
        if (r->err) { send_error(c, ERR_PROTO, "malformed PING"); return; }
        wr_init(&w, b, sizeof b);
        wr_begin(&w, S_PONG);
        wr_u32(&w, ts);
        finish_frame(c, &w, 0);
        break;
    }
    default:
        send_error(c, ERR_PROTO, "unknown message type");
        break;
    }
}

static void process_rx(Client *c) {
    for (;;) {
        size_t flen, total;
        Rd r;
        if (c->dead || c->close_after_flush || c->rxlen < 2) return;
        flen = ((size_t)c->rx[0] << 8) | c->rx[1];
        if (flen == 0 || flen > PP_MAX_FRAME) { send_error(c, ERR_PROTO, "bad frame length"); return; }
        total = 2 + flen;
        if (c->rxlen < total) return;

        /* flood control: ~100 messages/s sustained, bursts up to 200 */
        c->tokens += (double)(g_now_ms - c->t_tok) * 0.1;
        c->t_tok = g_now_ms;
        if (c->tokens > 200) c->tokens = 200;
        if (--c->tokens < 0) { send_error(c, ERR_FLOOD, "sending too many messages"); return; }

        rd_init(&r, c->rx + 3, flen - 1);
        handle_frame(c, c->rx[2], &r);
        memmove(c->rx, c->rx + total, c->rxlen - total);
        c->rxlen -= total;
    }
}

static void read_client(Client *c) {
    int n;
    if (c->dead) return;
    if (c->close_after_flush) {                 /* lingering: discard input, wait for peer to close */
        char scratch[512];
        n = (int)recv(c->sock, scratch, sizeof scratch, 0);
        if (n == 0) kill_client(c, NULL);
        else if (n < 0 && !net_would_block() && !net_interrupted()) kill_client(c, NULL);
        return;
    }
    n = (int)recv(c->sock, (char *)c->rx + c->rxlen, (int)(sizeof c->rx - c->rxlen), 0);
    if (n > 0) {
        c->rxlen += (size_t)n;
        c->t_rx = g_now_ms;
        process_rx(c);
    } else if (n == 0) {
        kill_client(c, "closed by peer");
    } else if (!net_would_block() && !net_interrupted()) {
        char es[160];
        net_strerror(es, sizeof es);
        kill_client(c, es);
    }
}

static void reap_dead(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &g_cl[i];
        if (!c->used || !c->dead) continue;
        if (c->room) room_remove(c->room, c, "disconnected");
        net_close(c->sock);
        slog("client #%u \"%s\" disconnected (%d online)", c->id, c->name, count_lobby_clients());
        free(c->tx);
        memset(c, 0, sizeof *c);
    }
}

static void accept_clients(sock_t ls) {
    int k;
    for (k = 0; k < 32; k++) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        sock_t s = accept(ls, (struct sockaddr *)&ss, &sl);
        char ip[48] = "?", svc[8];
        Client *c = NULL;
        int i, same = 0;
        uint8_t b[160];
        Wr w;
        if (s == SOCK_BAD) break;
        getnameinfo((struct sockaddr *)&ss, sl, ip, sizeof ip, svc, sizeof svc, NI_NUMERICHOST | NI_NUMERICSERV);

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!g_cl[i].used) { if (!c) c = &g_cl[i]; }
            else if (!g_cl[i].dead && strcmp(g_cl[i].ip, ip) == 0) same++;
        }
        {
            const char *why = NULL;
            int code = 0;
#ifndef _WIN32
            if ((int)s >= FD_SETSIZE) { why = "server is full"; code = ERR_FULL; }
#endif
            if (!c)                    { why = "server is full"; code = ERR_FULL; }
            else if (same >= g_max_per_ip) { why = "too many connections from your address"; code = ERR_LIMIT; }
            if (why) {
                wr_init(&w, b, sizeof b);
                wr_begin(&w, S_ERROR);
                wr_u8(&w, (unsigned)code);
                wr_str(&w, why, 100);
                wr_end(&w);
                if (!w.err) send(s, (const char *)b, (int)w.len, MSG_NOSIGNAL);
                net_close(s);
                slog("rejected %s: %s", ip, why);
                continue;
            }
        }
        net_set_nonblock(s);
        net_set_nodelay(s);
        memset(c, 0, sizeof *c);
        c->used = 1;
        c->sock = s;
        c->id = g_next_id++;
        if (g_next_id == 0) g_next_id = 1;
        c->state = ST_HANDSHAKE;
        snprintf(c->ip, sizeof c->ip, "%s", ip);
        c->t_conn = c->t_rx = c->t_tok = g_now_ms;
        c->tokens = 200;
        slog("connection #%u from %s", c->id, ip);
    }
}

static void check_timeouts(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &g_cl[i];
        if (!c->used || c->dead) continue;
        if (c->close_after_flush) {
            if (g_now_ms - c->t_close > CLOSE_GRACE_MS) kill_client(c, NULL);
        } else if (c->state == ST_HANDSHAKE && g_now_ms - c->t_conn > HANDSHAKE_MS) {
            send_error(c, ERR_TIMEOUT, "handshake timed out");
        } else if (g_now_ms - c->t_rx > IDLE_MS) {
            send_error(c, ERR_TIMEOUT, "connection timed out");
        }
    }
}

/* ------------------------------------------------------------------ */
int server_main(int argc, char **argv) {
    int port = PP_DEFAULT_PORT, i;
    char err[200];
    sock_t ls;
    uint64_t next_tick, last_to = 0;

    for (i = 0; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")) && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--win-score") && i + 1 < argc) g_win_score = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-per-ip") && i + 1 < argc) g_max_per_ip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-matches") && i + 1 < argc) g_max_rooms = atoi(argv[++i]);
        else {
            fprintf(stderr,
                "usage: pingpong server [-p PORT] [--win-score N] [--max-matches N] [--max-per-ip N]\n");
            return !(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"));
        }
    }
    if (port < 1 || port > 65535 || g_win_score < 1 || g_win_score > 99 ||
        g_max_per_ip < 1 || g_max_rooms < 1 || g_max_rooms > MAX_ROOMS) {
        fprintf(stderr, "invalid option value\n");
        return 1;
    }

    if (net_init() != 0) { fprintf(stderr, "network init failed\n"); return 1; }
    ls = net_listen(port, err, sizeof err);
    if (ls == SOCK_BAD) { fprintf(stderr, "%s\n", err); return 1; }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    srand((unsigned)time(NULL));

    slog("pingpong server (protocol v%d) listening on TCP port %d - first to %d wins", PP_VERSION, port, g_win_score);
    g_now_ms = now_ms();
    next_tick = now_us() + TICK_US;

    while (g_run) {
        fd_set rf, wf;
        sock_t maxfd = ls;
        struct timeval tv;
        int64_t wait;
        uint64_t nowu;
        int n, ticks = 0;

        FD_ZERO(&rf); FD_ZERO(&wf);
        FD_SET(ls, &rf);
        for (i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &g_cl[i];
            if (!c->used || c->dead) continue;
            FD_SET(c->sock, &rf);
            if (c->txoff < c->txlen) FD_SET(c->sock, &wf);
            if (c->sock > maxfd) maxfd = c->sock;
        }
        wait = (int64_t)next_tick - (int64_t)now_us();
        if (wait < 0) wait = 0;
        if (wait > 50000) wait = 50000;
        tv.tv_sec = 0;
        tv.tv_usec = (long)wait;
        n = select((int)maxfd + 1, &rf, &wf, NULL, &tv);
        g_now_ms = now_ms();
        if (n < 0 && !net_interrupted()) sleep_ms(1);

        if (n > 0) {
            if (FD_ISSET(ls, &rf)) accept_clients(ls);
            for (i = 0; i < MAX_CLIENTS; i++) {
                Client *c = &g_cl[i];
                if (!c->used || c->dead) continue;
                if (FD_ISSET(c->sock, &rf)) read_client(c);
                if (!c->dead && FD_ISSET(c->sock, &wf)) flush_client(c);
            }
        }

        nowu = now_us();
        while (nowu >= next_tick && ticks < 5) {
            int r;
            for (r = 0; r < MAX_ROOMS; r++) if (g_rooms[r].used) room_tick(&g_rooms[r], 1.0f / TICK_HZ);
            next_tick += TICK_US;
            ticks++;
        }
        if (nowu >= next_tick) next_tick = nowu + TICK_US;   /* fell behind: resync */

        for (i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &g_cl[i];
            if (c->used && !c->dead && c->txoff < c->txlen) flush_client(c);
        }
        if (g_now_ms - last_to > 250) { last_to = g_now_ms; check_timeouts(); }
        reap_dead();
    }

    slog("shutting down");
    for (i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &g_cl[i];
        if (c->used && !c->dead) { send_error(c, ERR_SHUTDOWN, "server is shutting down"); flush_client(c); }
        if (c->used) { net_close(c->sock); free(c->tx); }
    }
    net_close(ls);
    return 0;
}
