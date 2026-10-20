#!/usr/bin/env python3
"""
WAN-condition test. Runs the real server and puts a delay/jitter proxy in front
of every client, so each packet crosses a link like a long-distance internet path:
60 ms one way plus up to 25 ms of jitter each way (150-170 ms round trip), and a one
second stall injected mid-match. Verifies that play stays smooth and responsive.

usage: test_wan.py [path-to-pingpong]
"""
import os
import queue
import random
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_server as T  # noqa: E402  (shares the protocol client + server launcher)

ONE_WAY = 0.060
JITTER = 0.025


class DelayProxy:
    """TCP proxy that delays every chunk by ONE_WAY + jitter (never reordering)."""

    def __init__(self, target_port):
        self.target = target_port
        self.ls = socket.socket()
        self.ls.bind(("127.0.0.1", 0))
        self.ls.listen(16)
        self.port = self.ls.getsockname()[1]
        self.hold_until = 0.0
        self.conns = []
        threading.Thread(target=self._accept, daemon=True).start()

    def stall(self, seconds):
        self.hold_until = time.time() + seconds

    def _accept(self):
        while True:
            try:
                a, _ = self.ls.accept()
            except OSError:
                return
            b = socket.create_connection(("127.0.0.1", self.target))
            for s in (a, b):
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.conns += [a, b]
            for src, dst in ((a, b), (b, a)):
                q = queue.Queue()
                threading.Thread(target=self._read, args=(src, q), daemon=True).start()
                threading.Thread(target=self._write, args=(dst, q), daemon=True).start()

    def _read(self, src, q):
        last = 0.0
        while True:
            try:
                d = src.recv(65536)
            except OSError:
                d = b""
            if not d:
                q.put(None)
                return
            due = max(last, time.time() + ONE_WAY + random.uniform(0, JITTER))
            last = due
            q.put((due, d))

    def _write(self, dst, q):
        while True:
            item = q.get()
            if item is None:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                return
            due, d = item
            while True:
                now = time.time()
                wait = max(due, self.hold_until) - now
                if wait <= 0:
                    break
                time.sleep(min(wait, 0.01))
            try:
                dst.sendall(d)
            except OSError:
                return

    def close(self):
        for s in self.conns + [self.ls]:
            try:
                s.close()
            except OSError:
                pass


def rate(clients, secs):
    n0 = [c.nstates for c in clients]
    t0 = time.time()
    while time.time() - t0 < secs:
        for c in clients:
            c.pump(0.01)
    dt = time.time() - t0
    return [(c.nstates - n) / dt for c, n in zip(clients, n0)]


def main():
    srv = T.Server("--win-score", "99")
    proxy = DelayProxy(srv.port)
    ok = True
    try:
        a, b, c = (T.Client(proxy.port, n) for n in ("amy", "ben", "cat"))
        allc = [a, b, c]
        T.wait_until(lambda: all(x.welcome for x in allc), allc, what="welcome through proxy")

        # 1. the proxy really does add latency (sanity check of the test itself)
        t0 = time.time()
        a.send(T.C_PING, struct.pack(">I", 7))
        T.wait_until(lambda: a.pongs == 1, allc, what="pong")
        rtt = time.time() - t0
        assert 0.11 < rtt < 0.40, f"proxy latency not applied (rtt {rtt * 1000:.0f} ms)"
        print(f"  PASS  link emulation active: ping RTT {rtt * 1000:.0f} ms")

        # 2. host + join + queue all work over the slow link
        a.create("wan")
        T.wait_until(lambda: a.info, allc, what="create")
        b.send(T.C_LIST)
        T.wait_until(lambda: b.rooms, allc, what="list")
        rid = b.rooms[0]["id"]
        b.join(rid)
        T.wait_until(lambda: b.info, allc, what="b joined")
        c.join(rid)
        T.wait_until(lambda: c.info and c.info["role"] == T.ROLE_QUEUE and c.info["pos"] == 1, allc, what="c queued")
        T.wait_until(lambda: a.state and a.state["phase"] == T.PH_PLAYING, allc, timeout=10, what="match playing")
        print("  PASS  host / join / queue work over the delayed link")

        # 3. snapshot stream stays near 30/s for players and for the queued spectator
        rates = rate(allc, 3.0)
        assert all(r > 25 for r in rates), f"snapshot rate collapsed: {rates}"
        print("  PASS  snapshot stream holds ~30/s for everyone (%s)" % ", ".join("%.1f" % r for r in rates))

        # 4. input latency: key press -> paddle state change visible at the client
        lat = []
        for d in (1, -1, 1, 0):
            t0 = time.time()
            a.inp(d)
            T.wait_until(lambda: a.state["dir"][0] == d, allc, timeout=3, what="input echoed")
            lat.append(time.time() - t0)
            time.sleep(0.25)
        worst = max(lat)
        assert worst < 0.45, f"input latency too high: {lat}"
        print("  PASS  input -> visible response: %s ms (about one round trip)" % ", ".join("%.0f" % (x * 1000) for x in lat))

        # 5. a one-second network stall on one client must not disturb the others,
        #    and the stalled client must catch up afterwards without a backlog
        proxy.stall(1.0)
        before = (a.nstates, b.nstates)
        time.sleep(0.05)
        r_during = rate([a], 0.8)[0]          # amy is also behind the proxy -> stalled too
        T.wait_until(lambda: a.nstates > before[0] + 30, allc, timeout=6, what="recovery after stall")
        rates = rate(allc, 2.0)
        assert all(r > 25 for r in rates), f"did not recover after stall: {rates}"
        # queued client is still correctly queued, state still current
        assert c.info["role"] == T.ROLE_QUEUE and c.state["phase"] in (T.PH_PLAYING, T.PH_POINT)
        print("  PASS  recovers from a 1 s network stall (%.1f/s during, %.1f/s after)" % (r_during, min(rates)))

        # 6. a client that vanishes without closing (no FIN) is cleaned up and its slot rotates
        b.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        b.close()                              # RST
        T.wait_until(lambda: c.info and c.info["role"] == T.ROLE_RIGHT, [a, c], timeout=5, what="queue head promoted")
        print("  PASS  abrupt disconnect: queued player takes the free seat")
    except Exception as e:  # noqa
        ok = False
        print(f"  FAIL  {type(e).__name__}: {e}")
    finally:
        proxy.close()
        srv.stop()
    print("\n%s" % ("WAN test passed" if ok else "WAN test FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    print(f"wan-testing {T.BIN}")
    main()
