#!/usr/bin/env python3
"""
Drives the real client binary through pseudo-terminals (80x24) against a real
server, and inspects the rendered screen with a terminal emulator (pyte).

usage: test_client_pty.py [path-to-pingpong]
"""
import fcntl
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time

import pyte

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./pingpong")
COLS, ROWS = 80, 24


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Term:
    """A client process attached to a pty plus a virtual screen."""

    def __init__(self, port, name):
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(BIN, [BIN, "-s", f"127.0.0.1:{port}", "-n", name])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def pump(self, t=0.1):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.02)
            if r:
                try:
                    d = os.read(self.fd, 65536)
                except OSError:
                    return
                if not d:
                    return
                self.stream.feed(d)

    def text(self):
        return "\n".join(self.screen.display)

    def send(self, s):
        os.write(self.fd, s.encode() if isinstance(s, str) else s)

    def wait_text(self, needle, others=(), t=6.0):
        end = time.time() + t
        while time.time() < end:
            for o in others:
                o.pump(0.01)
            self.pump(0.05)
            if needle in self.text():
                return True
        raise AssertionError(f"never saw {needle!r}; screen was:\n{self.text()}")

    def alive(self):
        if getattr(self, "exited", False):
            return False
        try:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
        except ChildProcessError:
            pid = self.pid
        if pid != 0:
            self.exited = True
        return pid == 0

    def stop(self):
        try:
            os.kill(self.pid, signal.SIGINT)
            time.sleep(0.1)
            os.kill(self.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


def main():
    port = free_port()
    srv = subprocess.Popen([BIN, "server", "-p", str(port), "--win-score", "3"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.4)
    terms = []
    ok = True
    try:
        a = Term(port, "alice")
        terms.append(a)
        a.wait_text("lobby")
        a.wait_text("No matches yet")
        print("  PASS  client connects, shows empty lobby")

        # --- host a match from the UI ---
        a.send("h")
        a.wait_text("Name your match")
        a.send("Friday Night\r")
        a.wait_text("Friday Night")
        a.wait_text("you: LEFT paddle")
        a.wait_text("Waiting for an opponent")
        print("  PASS  hosting a match creates it and waits for an opponent")

        # --- second client discovers and joins it from the lobby list ---
        b = Term(port, "bob")
        terms.append(b)
        b.wait_text("Friday Night", [a])
        b.wait_text("1/2", [a])
        b.send("1")
        b.wait_text("you: RIGHT paddle", [a])
        a.wait_text("Get ready", [b])
        print("  PASS  lobby lists the match; joining starts the countdown for both players")

        # --- third and fourth clients get queued ---
        c = Term(port, "carol")
        terms.append(c)
        c.wait_text("Friday Night", [a, b])
        c.send("\r")                         # Enter joins the highlighted match
        c.wait_text("waiting, #1 in queue", [a, b])
        d = Term(port, "dave")
        terms.append(d)
        d.wait_text("Friday Night", [a, b, c])
        d.send("1")
        d.wait_text("waiting, #2 in queue", [a, b, c])
        a.wait_text("Queue (next up first): 1.carol  2.dave", [b, c, d])
        print("  PASS  3rd/4th players wait in a FIFO queue (shown to everyone)")

        # --- gameplay screen sanity: field renders, paddle moves with keys ---
        a.wait_text("alice (you)", [b, c, d])
        for t in (a, b, c, d):
            t.pump(0.3)
        scr = a.screen.display
        field_rows = [r for r in scr[2:18]]
        assert all(r.startswith("|") and r[61] == "|" for r in field_rows), "field borders misaligned:\n" + a.text()
        a.wait_text("Get ready", [b, c, d]) if "Point!" not in a.text() and "O" not in "".join(field_rows) else None
        # wait for the ball ('O') to be in play
        end = time.time() + 8
        while time.time() < end and "O" not in "".join(a.screen.display[2:18]):
            for t in terms:
                t.pump(0.05)
        assert "O" in "".join(a.screen.display[2:18]), "ball never rendered:\n" + a.text()
        print("  PASS  field renders inside 80x24 with ball and borders aligned")

        def paddle_rows(term, col):
            return [i for i, r in enumerate(term.screen.display[2:18]) if r[col] == "#"]

        a.send("s")                          # nudge down -> paddle starts moving down
        for t in terms:
            t.pump(0.05)
        before = paddle_rows(a, 2)   # screen col 0 is the border, paddle is at field x=1
        for _ in range(10):
            for t in terms:
                t.pump(0.05)
        after = paddle_rows(a, 2)
        assert before and after, (before, after, a.text())
        assert max(after) > max(before), f"paddle did not move down: {before} -> {after}"
        print("  PASS  keyboard input moves the paddle (round trip through the server)")

        # --- chat ---
        a.send("t")
        a.send("hello team\r")
        c.wait_text("alice: hello team", [a, b, d])
        print("  PASS  in-match chat reaches queued players")

        # --- play until a game finishes; loser must rotate to the queue ---
        a.send(" ")                          # stop paddle; nobody tries -> someone scores to 3
        end = time.time() + 120
        seen = False
        while time.time() < end:
            for t in terms:
                t.pump(0.05)
            if "back of the queue" in c.text():
                seen = True
                break
        assert seen, "no queue rotation seen:\n" + c.text()
        c.wait_text("you: ", [a, b, d])
        print("  PASS  after a game the loser goes to the back of the queue, next player steps up")

        # --- leaving returns to the lobby; Ctrl-C restores the terminal ---
        d.send("q")
        d.wait_text("lobby", [a, b, c])
        print("  PASS  leaving a match returns to the lobby")
        os.kill(d.pid, signal.SIGINT)
        end = time.time() + 3
        while time.time() < end and d.alive():
            d.pump(0.05)
        assert not d.alive(), "client did not exit on SIGINT"
        print("  PASS  client exits cleanly on Ctrl-C")

        # --- server going away is reported, not hung ---
        srv.terminate()
        srv.wait(timeout=3)
        for t in (a, b, c):
            end = time.time() + 5
            while time.time() < end and t.alive():
                t.pump(0.05)
            assert not t.alive(), "client still running after server shutdown"
        print("  PASS  clients notice server shutdown and exit")
    except AssertionError as e:
        ok = False
        print("  FAIL ", e)
    finally:
        for t in terms:
            t.stop()
        if srv.poll() is None:
            srv.terminate()
        srv.communicate(timeout=3)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    print(f"pty-testing {BIN}")
    main()
