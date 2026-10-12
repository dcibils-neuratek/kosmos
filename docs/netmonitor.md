# Network Monitor

What goes in and out of this machine, now and over the last minute, as
Monitor shows the processors. Diego, 12 October 2026: "i realizsed we need
network monitor app as well to see network input and output". Drawn the same
day in `docs/netmonitor.html` and agreed: "mockup is perfect! go ahead and
build it".

This document is the feature set and the architecture; `netmonitor.html` is
the window and `netmonitor-architecture.png` the picture of what it stands
on.

## What a person can do

- **See the network's card**: its maker and model, how fast its link is,
  this machine's address.
- **See how fast it is going now**, in and out, in blue and orange, the same
  two colours everywhere in the window.
- **See the last minute** as a graph: what came in above a centre line, what
  went out below it, on one scale fitted to the busiest second and said at
  its edge; the peak each way above it.
- **See the totals since the machine started**: bytes received and sent,
  packets each way, and how many were dropped - by the card or by the stack.
- **See every open connection**: the program that holds it, where it goes -
  a name when this machine knows one, an address and port otherwise - its
  state, and how fast it is going each way now; the busiest first.
- **Choose the pace** - every half second, second or two - and **bytes or
  bits** a second, from the dots' menu, as Monitor's.

## What it is made of

**The application is the thinnest layer** (`CLAUDE.md`): it asks and draws.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window, the card, the list, the menu | the UI kit | as it is |
| **The graph of a minute** | **`ui.history`**, a kit piece: Monitor's graph moved into the kit, a column a sample, two series stacked or mirrored | **moved**, so Monitor and Network Monitor draw with one copy |
| **The device's card** - its picture, maker and model, a line of facts, and the two rates now | **`ui.meter_card`**, a kit piece | **new** - Monitor's processor card, Network Monitor's card, Disk Monitor's drive |
| **The two colours** - in or read, out or written | **the theme's `io_in` and `io_out`**, blue and orange | **new**, one pair for every monitor |
| **The counters**: bytes and packets each way, dropped | **the network server** (`net.c`), counted where a frame comes in and where it goes out, in total and on each connection | **new** |
| **The connections**: each one's program, where it goes, its state, its bytes | **the network server**, `NET_OP_STATS`, a declared shape in `netproto.h`, a page of connections a request | **new** |
| **Which program opened a connection** | **the kernel**: `SYS_SENDER`, who sent the message a server last received - as QNX's `_msg_info` says it | as it is: it arrived with Notifications, and the network server asks it on `NET_OP_CONNECT`, `NET_OP_LISTEN` and `NET_OP_UDP_BIND` |
| A program's name from its process id | `sys.processes()`, as Processes reads it | as it is |
| The card's maker, model and link speed | the driver, through `NET_OP_INFO` as Network's window already shows | as it is |

**Asked for, not sent** (`CLAUDE.md`, control by message): Network Monitor
asks the server once a pace - one request, a reply of numbers - and works out
the rates as the difference between two readings, as Monitor works out a
processor's share. The server keeps nothing for it beyond the running totals
it keeps anyway, and nothing is sent to it unasked. No bytes of traffic go
anywhere near it.

**`SYS_SENDER` and what it gives away.** A server receives a token for each
caller - what it replies to - and nothing more about it; `SYS_SENDER` says
whose process the message it last received came from, which the
notification server already asks (`syscall.h`). A process id is already in
`sys.processes()` for anybody to read, so saying which process asked names
nothing that was hidden, and a server learns only about its own callers. The
network server asks once, when a connection is opened, and keeps the id with
the connection. (This said `SYS_SENDER` was new; it was written before
anybody looked, and it had been there since Notifications' first step.)

**C or Lua.** Counting is C, in the network server, on every frame - a
handful of additions, nothing that allocates. The window is Lua: a request a
second, a subtraction per counter and a list of at most a few dozen rows.

## The busiest path

**A frame**, in or out: the server adds its length to the total and to its
connection's count, and counts the packet. Two or three additions on a path
that already copies the frame; nothing measurable.

**The window**, once a pace: one `NET_OP_STATS` for the totals and the first
page of connections, `sys.processes()` for the names, the differences taken,
a column added to the graph, and a paint. At a second a pace that is nothing
next to Monitor's own sampling.

## Steps

1. **N1 - the counters and the request**: the network server counts, in
   total and per connection; `NET_OP_STATS` answers them, into a region the
   caller owns as `NET_OP_POLL`'s list is; `SYS_SENDER` names a connection's
   process. A guest test: bytes moved over a connection to a
   peer on the Mac, counted within a frame's worth each way, and the
   connection named with the test's own process.
   **Done in 0.11.167** (`testing.md` 18.560), with `netstat` printing
   what the server counts; a byte TCP sends again is counted once.
2. **N2 - the meters in the kit**: `ui.history` (Monitor's graph moved into
   the kit) and `ui.meter_card` (its processor card, made general), with the
   theme's `io_in` and `io_out`; Monitor drawn with them and its suite
   unchanged. Diego, 12 October: "can we make those cpu, disk, network meters
   part of the uikit so another app can make use of it?", and "we should
   stick to a single color schemem for all reading/writing in all monitoring
   apps".
   **Done in 0.11.168** (`testing.md` 18.561).
3. **N3 - the window**: as drawn, the pace and bytes or bits in the menu; its
   suite reading the rates while a transfer runs.
   **Done in 0.11.169** (`testing.md` 18.562); what is left is in
   `roadmap.md` under Network Monitor.

Only x86-64 runs the tests for now (`CLAUDE.md`).
