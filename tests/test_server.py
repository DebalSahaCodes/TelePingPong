#!/usr/bin/env python3
"""
End-to-end tests for the pingpong server. Spawns the real binary and talks the
wire protocol over TCP with scripted clients (no mocks).

usage: test_server.py [path-to-pingpong]
"""
import os
import socket
import struct
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./pingpong"

C_HELLO, C_LIST, C_CREATE, C_JOIN, C_LEAVE, C_INPUT, C_CHAT, C_PING = range(1, 9)
S_WELCOME, S_ROOMS, S_JOINED, S_ROOM_INFO, S_STATE, S_CHAT, S_NOTICE, S_ERROR, S_PONG, S_LEFT_ROOM = range(101, 111)
PH_WAITING, PH_COUNTDOWN, PH_PLAYING, PH_POINT, PH_GAMEOVER = range(5)
ROLE_LEFT, ROLE_RIGHT, ROLE_QUEUE = range(3)
ERR_VERSION, ERR_FULL, ERR_LIMIT, ERR_FLOOD, ERR_PROTO = 1, 2, 3, 4, 5


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Server:
    def __init__(self, *args):
        self.port = free_port()
        self.proc = subprocess.Popen([BIN, "server", "-p", str(self.port), *args],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                time.sleep(0.15)   # let the server reap the probe (it counts toward --max-per-ip)
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def stop(self):
        self.proc.terminate()
        try:
            out, _ = self.proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
        return out


def rd_str(b, o):
    n = b[o]
    return b[o + 1:o + 1 + n].decode("latin1"), o + 1 + n


class Client:
    def __init__(self, port, name=None, hello=True, version=1):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.closed = False
        self.name = name
        self.info = None        # latest ROOM_INFO
        self.state = None       # latest STATE
        self.rooms = None
        self.notices, self.chats, self.errors = [], [], []
        self.nstates = 0
        self.joined = 0
        self.left_room = 0
        self.welcome = None
        self.pongs = 0
        if hello:
            self.hello(name, version)

    def send(self, t, payload=b""):
        self.s.sendall(struct.pack(">HB", len(payload) + 1, t) + payload)

    def hello(self, name, version=1):
        nb = name.encode()
        self.send(C_HELLO, bytes([version, len(nb)]) + nb)

    def create(self, name):
        nb = name.encode()
        self.send(C_CREATE, bytes([len(nb)]) + nb)

    def join(self, rid):
        self.send(C_JOIN, struct.pack(">H", rid))

    def chat(self, text):
        b = text.encode("latin1")
        self.send(C_CHAT, bytes([len(b)]) + b)

    def inp(self, d):
        self.send(C_INPUT, bytes([d + 1]))

    def pump(self, timeout=0.05):
        if self.closed:
            return
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(65536)
            if not d:
                self.closed = True
                return
            self.buf += d
        except socket.timeout:
            pass
        except OSError:
            self.closed = True
            return
        while len(self.buf) >= 3:
            n = struct.unpack(">H", self.buf[:2])[0]
            if len(self.buf) < 2 + n:
                break
            t, p = self.buf[2], self.buf[3:2 + n]
            self.buf = self.buf[2 + n:]
            self.handle(t, p)

    def handle(self, t, p):
        if t == S_WELCOME:
            self.welcome = struct.unpack(">HHH", p[:6])
        elif t == S_ROOMS:
            n, o, rooms = p[0], 1, []
            for _ in range(n):
                rid, np_, nq, ph = struct.unpack(">HBBB", p[o:o + 5]); o += 5
                name, o = rd_str(p, o)
                host, o = rd_str(p, o)
                rooms.append(dict(id=rid, players=np_, queue=nq, phase=ph, name=name, host=host))
            self.rooms = rooms
        elif t == S_JOINED:
            self.joined += 1
        elif t == S_ROOM_INFO:
            role, pos = p[0], p[1]
            rn, o = rd_str(p, 2)
            left, o = rd_str(p, o)
            right, o = rd_str(p, o)
            nq = p[o]; o += 1
            q = []
            for _ in range(nq):
                nm, o = rd_str(p, o)
                q.append(nm)
            self.info = dict(role=role, pos=pos, room=rn, left=left, right=right, queue=q)
        elif t == S_STATE:
            ph, ms, sl, sr = struct.unpack(">BHBB", p[:5])
            bx, by, vx, vy, p0, p1 = struct.unpack(">6h", p[5:17])
            self.state = dict(phase=ph, ms=ms, score=(sl, sr), bx=bx / 100, by=by / 100,
                              vx=vx / 100, vy=vy / 100, pad=(p0 / 100, p1 / 100), dir=(p[17] - 1, p[18] - 1))
            self.nstates += 1
        elif t == S_CHAT:
            who, o = rd_str(p, 0)
            text, o = rd_str(p, o)
            self.chats.append((who, text))
        elif t == S_NOTICE:
            self.notices.append(rd_str(p, 0)[0])
        elif t == S_ERROR:
            self.errors.append((p[0], rd_str(p, 1)[0]))
        elif t == S_PONG:
            self.pongs += 1
        elif t == S_LEFT_ROOM:
            self.left_room += 1

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def wait_until(cond, clients, timeout=8.0, what=""):
    end = time.time() + timeout
    while time.time() < end:
        for c in clients:
            c.pump(0.01)
        if cond():
            return True
    raise AssertionError("timed out waiting for: " + what)


passed = failed = 0


def check(name, fn):
    global passed, failed
    t0 = time.time()
    try:
        fn()
        passed += 1
        print(f"  PASS  {name}  ({time.time() - t0:.1f}s)")
    except Exception as e:  # noqa
        failed += 1
        print(f"  FAIL  {name}: {type(e).__name__}: {e}")


# ----------------------------------------------------------------------------
def t_handshake_and_ping():
    srv = Server()
    try:
        a = Client(srv.port, "alice")
        wait_until(lambda: a.welcome, [a], what="WELCOME")
        a.send(C_PING, struct.pack(">I", 12345))
        wait_until(lambda: a.pongs == 1, [a], what="PONG")
        a.send(C_LIST)
        wait_until(lambda: a.rooms == [], [a], what="empty room list")
    finally:
        srv.stop()


def t_bad_inputs():
    srv = Server()
    try:
        # wrong protocol version
        a = Client(srv.port, "old", version=99)
        wait_until(lambda: a.errors, [a], what="version error")
        assert a.errors[0][0] == ERR_VERSION, a.errors
        wait_until(lambda: a.closed, [a], what="close after fatal error")

        # oversized frame length
        b = Client(srv.port, hello=False)
        b.s.sendall(struct.pack(">HB", 60000, 1))
        wait_until(lambda: b.errors, [b], what="protocol error")
        assert b.errors[0][0] == ERR_PROTO, b.errors

        # command before HELLO
        c = Client(srv.port, hello=False)
        c.send(C_LIST)
        wait_until(lambda: c.errors, [c], what="expected HELLO error")

        # random garbage must never crash the server
        for _ in range(20):
            g = Client(srv.port, hello=False)
            g.s.sendall(os.urandom(300))
            g.pump(0.05)
            g.close()
        d = Client(srv.port, "still-alive")
        wait_until(lambda: d.welcome, [d], what="server survives garbage")
    finally:
        out = srv.stop()
        assert "Segmentation" not in out


def t_flood_protection():
    srv = Server()
    try:
        a = Client(srv.port, "flooder")
        wait_until(lambda: a.welcome, [a])
        for _ in range(1500):
            try:
                a.send(C_PING, struct.pack(">I", 1))
            except OSError:
                break
        wait_until(lambda: any(e[0] == ERR_FLOOD for e in a.errors) or a.closed, [a], what="flood kick")
    finally:
        srv.stop()


def t_per_ip_limit():
    srv = Server("--max-per-ip", "3")
    try:
        cs = [Client(srv.port, f"p{i}") for i in range(3)]
        wait_until(lambda: all(c.welcome for c in cs), cs)
        extra = Client(srv.port, "extra")
        wait_until(lambda: extra.errors, [extra], what="per-IP rejection")
        assert extra.errors[0][0] == ERR_LIMIT, extra.errors
    finally:
        srv.stop()


def t_host_find_join_and_queue():
    srv = Server()
    try:
        host = Client(srv.port, "host")
        wait_until(lambda: host.welcome, [host])
        host.create("Friday night")
        wait_until(lambda: host.joined == 1 and host.info, [host], what="host joined own match")
        assert host.info["role"] == ROLE_LEFT and host.info["room"] == "Friday night"

        # a different client finds it through the lobby list
        p2 = Client(srv.port, "bob")
        wait_until(lambda: p2.welcome, [p2])
        p2.send(C_LIST)
        wait_until(lambda: p2.rooms, [p2], what="room visible in lobby")
        r = p2.rooms[0]
        assert (r["name"], r["host"], r["players"], r["queue"]) == ("Friday night", "host", 1, 0), r
        p2.join(r["id"])
        wait_until(lambda: p2.info and p2.info["role"] == ROLE_RIGHT, [p2, host], what="bob is right player")

        # 3rd, 4th, 5th players are queued in arrival order
        others = []
        for i, nm in enumerate(["carol", "dave", "erin"]):
            c = Client(srv.port, nm)
            wait_until(lambda: c.welcome, [c])
            c.join(r["id"])
            wait_until(lambda: c.info and c.info["role"] == ROLE_QUEUE and c.info["pos"] == i + 1,
                       [c, host, p2], what=f"{nm} queued at {i + 1}")
            others.append(c)
        allc = [host, p2] + others
        wait_until(lambda: host.info["queue"] == ["carol", "dave", "erin"], allc, what="queue order")
        assert host.info["left"] == "host" and host.info["right"] == "bob"

        # match starts and everybody (incl. queued spectators) gets state
        wait_until(lambda: all(c.nstates > 5 for c in allc), allc, what="snapshots to every member")
        p2.send(C_LIST)
        wait_until(lambda: p2.rooms is not None, [p2])
        # lobby view reflects 2 players + 3 waiting
        x = Client(srv.port, "lurker")
        wait_until(lambda: x.welcome, [x])
        x.send(C_LIST)
        wait_until(lambda: x.rooms, [x])
        assert x.rooms[0]["players"] == 2 and x.rooms[0]["queue"] == 3, x.rooms
    finally:
        srv.stop()


def t_gameplay_authoritative_and_rotation():
    # first-to-1 so a single missed ball ends the game quickly
    srv = Server("--win-score", "1")
    try:
        a, b, c, d = (Client(srv.port, n) for n in ("amy", "ben", "cal", "dee"))
        allc = [a, b, c, d]
        wait_until(lambda: all(x.welcome for x in allc), allc)
        a.create("rot")
        wait_until(lambda: a.info, allc)
        rid = None
        b.send(C_LIST)
        wait_until(lambda: b.rooms, allc)
        rid = b.rooms[0]["id"]
        # join one at a time: separate sockets give no cross-connection ordering guarantee
        for x in (b, c, d):
            x.join(rid)
            wait_until(lambda: x.info is not None, allc, what=f"{x.name} in match")
        wait_until(lambda: d.info and d.info["queue"] == ["cal", "dee"], allc, what="queue cal,dee")

        # state machine: countdown -> playing, ball moves, paddle input is honoured server-side
        wait_until(lambda: a.state and a.state["phase"] == PH_COUNTDOWN, allc, what="countdown")
        a.inp(-1)
        wait_until(lambda: a.state["phase"] == PH_PLAYING, allc, what="playing")
        wait_until(lambda: a.state["pad"][0] < 3.0, allc, what="left paddle moved up under input")
        assert a.state["dir"][0] == -1
        x0 = a.state["bx"]
        time.sleep(0.2)
        wait_until(lambda: a.state["bx"] != x0, allc, what="ball moving")

        # a paddle pinned in a corner will miss -> game over -> loser rotates to queue tail
        wait_until(lambda: a.state["phase"] == PH_GAMEOVER, allc, timeout=15, what="game over")
        sl, sr = a.state["score"]
        assert sl + sr == 1, a.state
        winner, loser = ("amy", "ben") if sl == 1 else ("ben", "amy")
        # after the GAMEOVER pause the next challenger steps in and the loser is queued last
        wait_until(lambda: a.info and a.info["queue"] and a.info["queue"][-1] == loser,
                   allc, timeout=8, what="loser moves to back of queue")
        i = a.info
        assert winner in (i["left"], i["right"]), i
        challenger = i["right"] if i["left"] == winner else i["left"]
        assert challenger == "cal", i
        assert i["queue"] == ["dee", loser], i
        assert any("back of the queue" in n for n in a.notices), a.notices
        # new match has fresh scores
        wait_until(lambda: a.state["phase"] in (PH_COUNTDOWN, PH_PLAYING) and a.state["score"] == (0, 0), allc,
                   what="scores reset")
    finally:
        srv.stop()


def t_forfeit_and_host_migration():
    srv = Server()
    try:
        a, b, c = (Client(srv.port, n) for n in ("hostie", "guest", "waiter"))
        allc = [a, b, c]
        wait_until(lambda: all(x.welcome for x in allc), allc)
        a.create("m")
        wait_until(lambda: a.info, allc)
        b.send(C_LIST)
        wait_until(lambda: b.rooms, allc)
        rid = b.rooms[0]["id"]
        b.join(rid)
        c.join(rid)
        wait_until(lambda: c.info and c.info["queue"] == ["waiter"], allc)
        wait_until(lambda: a.state and a.state["phase"] in (PH_COUNTDOWN, PH_PLAYING), allc)

        # player drops mid-match (hard disconnect): waiting player steps in, opponent wins by forfeit
        b.close()
        wait_until(lambda: c.info and c.info["role"] == ROLE_RIGHT and c.info["queue"] == [], [a, c],
                   what="queue head promoted after disconnect")
        assert any("forfeit" in n for n in a.notices), a.notices

        # host leaves politely -> room survives, host migrates; list still shows it
        a.send(C_LEAVE)
        wait_until(lambda: a.left_room == 1, [a, c])
        probe = Client(srv.port, "probe")
        wait_until(lambda: probe.welcome, [probe, c])
        probe.send(C_LIST)
        wait_until(lambda: probe.rooms, [probe, c])
        assert len(probe.rooms) == 1 and probe.rooms[0]["host"] == "waiter", probe.rooms

        # last member leaves -> match disappears
        c.send(C_LEAVE)
        wait_until(lambda: c.left_room == 1, [c])
        probe.send(C_LIST)
        probe.rooms = None
        probe.send(C_LIST)
        wait_until(lambda: probe.rooms == [], [probe], what="empty room removed")
    finally:
        srv.stop()


def t_chat_sanitised():
    srv = Server()
    try:
        a, b = Client(srv.port, "chatty"), Client(srv.port, "listener")
        wait_until(lambda: a.welcome and b.welcome, [a, b])
        a.create("c")
        wait_until(lambda: a.info, [a, b])
        b.send(C_LIST)
        wait_until(lambda: b.rooms, [a, b])
        b.join(b.rooms[0]["id"])
        wait_until(lambda: b.info, [a, b])
        a.chat("hi \x1b[2Jthere\x07")
        wait_until(lambda: b.chats, [a, b], what="chat delivered")
        who, text = b.chats[0]
        assert who == "chatty" and "\x1b" not in text and "\x07" not in text, repr(text)
        assert text == "hi ?[2Jthere?", repr(text)
    finally:
        srv.stop()


def t_slow_client_does_not_stall_others():
    srv = Server()
    try:
        host, fast, slow = (Client(srv.port, n) for n in ("h", "fast", "slow"))
        allc = [host, fast, slow]
        wait_until(lambda: all(x.welcome for x in allc), allc)
        host.create("s")
        wait_until(lambda: host.info, allc)
        host.send(C_LIST)
        wait_until(lambda: host.rooms, allc)
        rid = host.rooms[0]["id"]
        fast.join(rid)
        slow.join(rid)
        wait_until(lambda: slow.info, allc)
        slow.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        # 'slow' stops reading entirely; others must keep receiving ~30 snapshots/s
        n0, t0 = fast.nstates, time.time()
        while time.time() - t0 < 3.0:
            fast.pump(0.02)
            host.pump(0.0)
        rate = (fast.nstates - n0) / (time.time() - t0)
        assert rate > 20, f"snapshot rate to healthy client collapsed: {rate:.1f}/s"
        # and the server is still responsive to new connections
        late = Client(srv.port, "late")
        wait_until(lambda: late.welcome, [late], what="server responsive while a client is stalled")
    finally:
        srv.stop()


def t_name_uniqueness():
    srv = Server()
    try:
        a, b = Client(srv.port, "same"), Client(srv.port, "same")
        wait_until(lambda: a.welcome and b.welcome, [a, b])
        a.create("n")
        wait_until(lambda: a.info, [a, b])
        b.send(C_LIST)
        wait_until(lambda: b.rooms, [a, b])
        b.join(b.rooms[0]["id"])
        wait_until(lambda: b.info and b.info["right"] != "", [a, b])
        assert b.info["left"] == "same" and b.info["right"].startswith("same#"), b.info
    finally:
        srv.stop()


if __name__ == "__main__":
    print(f"testing {BIN}")
    for name, fn in [
        ("handshake + ping/pong + empty lobby", t_handshake_and_ping),
        ("rejects bad version / bad frames / garbage", t_bad_inputs),
        ("flood protection", t_flood_protection),
        ("per-IP connection limit", t_per_ip_limit),
        ("host, discover in lobby, join, 3-deep queue", t_host_find_join_and_queue),
        ("server-authoritative play + loser rotates to queue", t_gameplay_authoritative_and_rotation),
        ("forfeit on disconnect + host migration + cleanup", t_forfeit_and_host_migration),
        ("chat is sanitised (no terminal escapes)", t_chat_sanitised),
        ("stalled client can't stall the server", t_slow_client_does_not_stall_others),
        ("duplicate names are disambiguated", t_name_uniqueness),
    ]:
        check(name, fn)
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
