# Tele Ping Pong - online multiplayer, written in C

Two players on different networks play ping pong in a terminal. Anyone can host a
match, anyone can find and join it, and extra players wait in a queue.

One small program does everything (`pingpong` / `pingpong.exe`):

```
pingpong server      run the lobby + match server   (needs to be reachable by every player)
pingpong             play: browse / host / join matches
```

## How players on different networks connect

Two home networks can't normally reach each other (NAT, firewalls), so the game
uses a **central server** that every player connects *out* to. Players never need
port forwarding; only the machine running the server does.

```
 Alice (home network A) --\
                           \                  +--------------------------+
 Bob   (home network B) ----->  TCP :7777  -->|  pingpong server         |
                           /                  |   - lobby (list/host/join)|
 Carol (cafe wifi)     --/                    |   - runs each match      |
 Dave  (mobile hotspot) -/                    |   - queue per match      |
                                              +--------------------------+
```

* **You need one server reachable by everybody** - a cheap VPS, or a PC with TCP
  port 7777 forwarded on your router (see "Running a server").
* The server is **authoritative**: it simulates the game (60 Hz) and streams
  snapshots (30 Hz). Clients only send paddle input, so latency never makes the
  two players' games diverge and nobody can cheat by editing their client.

## Playing

1. Start the client: `pingpong` (Windows: double-click `start-client.bat`).
   It asks for the **server address** (`host` or `host:port`, default port 7777)
   and your **name**. Skip the questions with `pingpong -s my.server.com -n Alice`
   (or set `PINGPONG_SERVER=host:port`).
2. **Lobby** - open matches are listed and refresh automatically.
   `H` host a new match, `Up/Down` + `Enter` (or the number `1`-`9`) join, `R` refresh, `Q` quit.
3. **Hosting** creates a match and puts you on the left paddle; it shows up in
   everybody's lobby immediately. The game starts as soon as a second player joins.
4. **More than two players?** Everyone after the first two joins a **queue** and
   watches the game. First to 7 wins; the **winner stays on, the loser goes to the
   back of the queue**, and the next person in line steps up. With an empty queue
   the same two players get a rematch.
5. If a player leaves or drops mid-game, their opponent wins by forfeit and the
   first person in the queue takes the free seat. If the host leaves, the match
   gets a new host; it closes when the last person leaves.

| In a match | |
|---|---|
| `W` / `Up` | move up (press once to start; opposite key once = stop, twice = reverse) |
| `S` / `Down` | move down |
| `Space` / `X` | stop the paddle |
| `T` or `Enter` | chat with everyone in the match (including the queue) |
| `Q` / `Esc` | leave the match, back to the lobby |
| `Ctrl+C` | quit |

Terminals can't report key *releases*, so the paddle keeps moving in the chosen
direction until you stop or reverse it. Needs a terminal of at least 62x24;
Windows 10 or newer (Windows Terminal or the classic console), any Linux terminal.

## Install

* **Debian / Ubuntu / Raspberry Pi OS:** `sudo apt install ./pingpong-net_1.0.0_amd64.deb`
  (`_arm64.deb` for 64-bit Raspberry Pi / ARM servers). Installs `/usr/bin/pingpong`
  and a systemd unit. The binary is statically linked, so it works on any release.
  Prefer no install? Extract `pingpong-1.0.0-linux-amd64.tar.gz` and run `./pingpong`.
* **Windows:** unzip `pingpong-1.0.0-windows-x64.zip`, double-click `start-client.bat`.
  The `.exe` is not code-signed, so SmartScreen may say "unrecognized app": choose
  *More info > Run anyway*.

## Running a server

```
pingpong server                      # TCP port 7777
pingpong server -p 9000 --win-score 11 --max-matches 20 --max-per-ip 4
```

Make TCP port 7777 reachable from the internet:

* **VPS / cloud:** allow inbound TCP 7777 in the provider's firewall / security
  group, and in the OS (`sudo ufw allow 7777/tcp`). Players use the VPS address.
* **Debian package + systemd:** `sudo systemctl enable --now pingpong-server`,
  logs with `journalctl -u pingpong-server -f`. The unit runs as a throw-away
  user inside a strict sandbox.
* **Home PC (Windows: double-click `start-server.bat`):** allow it through Windows
  Firewall when asked, then forward TCP 7777 on your router to this PC and give
  friends your public IP (or a dynamic-DNS name). On systems with IPv6 the server
  listens on IPv6 as well as IPv4 (one dual-stack socket); otherwise IPv4 only.
  If your ISP uses carrier-grade NAT (you have no public IPv4 address), port
  forwarding can't work; use a VPS or IPv6.

Limits (all enforced server-side): 512 connections, 64 matches, 12 queued players
per match, 8 connections per IP, 100 messages/s per client, 10 s to say HELLO,
20 s of silence = disconnect. A client that stops reading is dropped instead of
slowing anyone else down. Names and chat are reduced to printable ASCII, so a
hostile player can't inject terminal escape codes into other people's screens.

**Not included:** encryption or accounts. Traffic is plain TCP, and anyone who can
reach the port can play. Don't put anything private in names or chat.

## Wire protocol (for the curious)

TCP, `TCP_NODELAY`. Each message is `[u16 length][u8 type][payload]`, integers
big-endian, strings `[u8 length][bytes]`. Full definition and comments: `src/proto.h`.

| client -> server | server -> client |
|---|---|
| `HELLO` version, name | `WELCOME` id, players online, matches |
| `LIST` | `ROOMS` list of matches (id, players, queue, phase, name, host) |
| `CREATE` name | `JOINED`, `ROOM_INFO` (your role/queue position, names, queue) |
| `JOIN` id | `STATE` 30 Hz snapshot: phase, score, ball pos+velocity, paddles |
| `LEAVE` | `NOTICE`, `CHAT`, `ERROR` (fatal ones close the connection) |
| `INPUT` direction -1/0/+1 | `PONG`, `LEFT_ROOM` |
| `CHAT`, `PING` | |

Networking details worth knowing: snapshots are *droppable* (skipped when a
client's send queue backs up, so a slow link gets fresh data, not a growing
backlog), the client extrapolates ball and paddles between snapshots, and the
client sends a ping every 2 s to show latency and keep NAT entries alive.

## Building from source

```
make                 # native build -> ./pingpong
make test            # server tests + WAN-latency test + pseudo-terminal client test (pip install pyte)
make dist            # everything in dist/: Windows zip, static Linux amd64+arm64, .deb packages
```

`make dist` needs `musl-tools`, `gcc-mingw-w64-x86-64`, `zip`, `dpkg-dev`, and
`pip install ziglang` (for the arm64 build). Plain C11 with no libraries beyond the OS.

Source layout - files are grouped by platform:

```
src/common/    all platforms: main.c  server.c  client.c  net_common.c  proto.h  platform.h  platform_internal.h
src/linux/     Linux / POSIX: platform_linux.c   platform_linux.h     (sockets, clock, termios terminal)
src/windows/   Windows:       platform_windows.c platform_windows.h   (Winsock, QPC clock, console VT mode)
```
