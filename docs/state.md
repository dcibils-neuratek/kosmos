# State

**Update at the end of every session.** This file is what keeps you from starting over each time.

Last updated: 2026-09-05

---

## Where this left off

**`httpd` serves eight at once.** Eight simultaneous requests for a 106 KB
file all come back complete in half a second of wall clock - about what one
of them costs on its own - with a ninth client deliberately stuck half way
through its request and going nowhere.

There are no threads to do that with. There is no thread syscall at all:
`SYS_SPAWN` makes a *process*, and a process has one `lua_State`, so two
threads inside one would want a lock around the interpreter and would take
turns anyway. What it has instead is nginx's shape - **one process, an event
loop, and a coroutine per connection** - and the coroutine is why `serve`
still reads as a straight line: read the request, find the file, write the
answer, with a `yield` wherever it used to wait.

**That needed the `select` this system had wanted six separate times** and
had worked around with a timer every time. `fs.poll` is it, and the shape it
arrived in is the interesting part: **two masks, not one.** A caller waiting
to read wants to hear that bytes arrived; a caller waiting to write wants to
hear that room appeared. One mask has to guess, and the version that guessed
deadlocked - it reported a connection writable only when there was room
*and* something still queued, so a client that acknowledged the whole ring
in one go left the server waiting for news that could no longer arrive.
Eight requests hung and the server logged none of them.

**Two bugs underneath it, and both had been there a while:**

- **The console's `interrupted` span the console server for ever.** Its
  drain loop read through `next_byte`, which empties the *stash* before it
  asks the hardware - so it took a byte off the stash, put it back, and took
  it again. One character typed ahead and the console never answered anybody
  again, which looks exactly like whichever program had asked having hung.
  Nothing found it for months because every caller was shaped so it could not
  happen: a status bar asks between screens with the keyboard drained, and
  the old `httpd` asked once per request, *after* `accept` had blocked. An
  event loop asking ten times a second hit it on the first pass.
- **`respond` allocated the file again on every pass.** It built `head ..
  body` and then wrote `text:sub(at)` - the whole remainder - each time
  round, so a 106 KB file allocated 106 KB, then 90, then 74, per connection.
  Six at once ran the heap out and two conversations died with "not enough
  memory" while four were served. It writes a ring's worth at a time now and
  streams the body with `fs.chunks`, so the file is never held.

**And `fs.write` no longer raises on the disk.** Chasing the concurrency
bug turned up a probe of mine that wrote 150 KB and read back 16 KB, which I
put down to `/data`'s ceiling - and `/data` was innocent: it returns `false,
"/data is full"` and always did, and I had not looked at the return value.
The disk was not. `fs.write` above about two kilobytes reached `sys.call`
and came back as `value does not fit in a message` - an *exception* out of
the serialiser, from a call whose failures are otherwise values.

The namespace splits a long write for `/data` and cannot for the disk, whose
`write` takes no offset and hands the whole body to `kfs.store`. So the same
line worked on one mount, failed with a sentence on another, and threw on a
third - which is exactly the difference a namespace exists to hide. It sends
a big string through a region now, which is the route `write_from` and
`files.copy` already took and that diskfs implemented for this reason. Three
checks in `run_interchange.py`, one of them taking the file back out of the
image byte for byte.

**And that uncovered a capability leak that had been there all along.**
Sending a value through a region hands the server a capability, and diskfs
kept every one: thirty-two is what a thread gets, so the thirty-second large
write failed with `that is not a region this process can map` - which reads
like a bad pointer rather than like a table that is full. The read side had
paid that debt since a PDF read in 256-byte windows found it on its
fifteenth read; the write side never had, and nothing noticed because
`files.copy` was its only caller. `fs.write` sending large values that way
made it an ordinary path, and a loop reaches thirty-two in a second.

That is the third resource bug of this exact shape - capability slots gone on
the sixteenth read, a region per font per size, a process table full at round
twenty-two - which is why the check for it is a loop of forty writes rather
than one write, and why `make stress` exists.

Two ceilings are unchanged and both are deliberate: `/data` still holds 16 KB
a file, and diskfs still refuses more than a megabyte because it assembles
the bytes in its own heap. A big *table* still fails, because a region
carries bytes and sending a packed table through one would break the promise
that you get back the table you wrote.

**And `accept` gained a deadline**, which makes every park in the stack
bounded - the pings, the waits, the pollers and now this. `poll` saying
somebody arrived and `accept` reaching the stack are two moments, and a
reset in between would otherwise wedge an event loop for good.

Two things worth knowing that came out of the hunt rather than out of the
code. The first two "bugs" I chased were not bugs: a stale `build/kosmos.elf`
and a `wait_for` that matched the *echo* of the command it had just typed.
And a server cannot print - `sys_write` is gated on `owns_console` - so
instrumenting one means opening that gate in the kernel for the length of the
session. Both are worth remembering before the next silent hang.

---

**Kosmos serves web pages.** `httpd 80 /home` and this Mac fetches a 152 KB
PNG from it, byte for byte identical to the file on the guest's disk.

That needed the half of TCP left out on purpose - LISTEN, SYN_RECEIVED, and
a way to hand a caller a connection it did not ask for - which `roadmap.md`
said an HTTP server would be the argument to settle. It was cheaper than the
note predicting it suggested.

**Three bugs on the way and none of them said anything was wrong**, which is
the reason to write them down:

- `send_ip`'s buffer was `NET_PAYLOAD_MAX + ICMP_HEADER`, 156 bytes, because
  ICMP was its only caller. TCP could send a handshake and nothing larger.
  The capture showed a FIN with `len=0` - a server that answered nothing.
- **A FIN is a wish, not an act.** It takes a sequence number, so sending it
  while the ring still held bytes numbered it as if they did not exist. The
  image arrived as 141 KB, the server logged a complete file, and the client
  saw a clean close.
- A client had no way to wait for *outgoing* space. `wait` only woke for
  incoming bytes, so a client that filled the ring parked until the
  connection died - the first truncation, at exactly 16 KB, which is the
  ring. An ACK that frees space wakes the waiter now, and `wait` honours the
  deadline it had been recording and ignoring.

**Two apps.** `network` shows the card and edits the addresses, applies them
live, saves to `/home/.network` for the next boot, and can ping the gateway
to say whether any of it worked. `webserver` starts and stops `httpd` and
shows its requests arriving.

The manager is not the server and that is forced: `accept` blocks and a
window that blocked would stop drawing. They talk through `/data` - the
server writes its state and last forty lines, the manager reads them on a
tick - which is a file rather than a message because the server has no idea
anybody is watching.

**SSH's primitives are written and checked**: SHA-256, HMAC-SHA256,
ChaCha20, Poly1305 and X25519, each against the vectors in its own
specification. Two bugs came out of that which running would never have
shown - a nonsense shift in `fe_from_bytes`, and one apparent mismatch that
was *the test* reusing RFC 8439 2.3.2's nonce for 2.4.2. What is left is
Ed25519 verification, then the binary packet protocol, key exchange,
userauth and channels.

---

**Kosmos is on the internet.** `ping 8.8.8.8` answers, in about 22 ms
through QEMU's NAT.

Four pieces, each with its own test:

`hal/qemu-virt/virtio.c` is **the transport, once**. It was three copies -
`blk.c`, `input.c`, `snd.c` - of a handshake whose *order* is the protocol,
and `qemu-virt.h` recorded the reason not to share it: "splitting the
transport out before there are two would be inventing an interface against
a single caller". That expired two devices ago and the card is the fourth.
What is shared is the conversation; what is not is the ring, because block
chains three descriptors and waits, input hands the device empty buffers,
and sound has four queues with different jobs. A generic ring over those
would be the same mistake one layer up.

`hal/qemu-virt/net.c` is **virtio-net**: two queues running in opposite
directions, a twelve-byte header the wire never sees, and `NET_F_MAC`
asked for and checked rather than assumed. `run_network.py` reads QEMU's
own pcap, because nothing inside the guest can establish that a frame left
it - `sys.net_send` returning true is a statement about a virtqueue.

`user/servers/net.c` is **Ethernet, ARP, IPv4 and ICMP**, in C because it is
a server on the packet path. Fragments and IP options are refused rather
than half-handled. `SPAWN_NET` is the disk's grant pointed outwards - whoever
can send a raw frame can claim any address and read every frame that arrives
- so one process holds the card and `/net` is what everything else asks.
A card is a device; a stack is someone you ask.

`/kits/network` and `ping`. **No capability leaves the namespace**: a program
says `fs.ping("/net", ...)`, the namespace resolves and hands the kit the
capability, exactly as `fs.wait_input` does for the console. There is
deliberately no `fs.capability`.

**And TCP.** `fetch 1.1.1.1 80 /` pulls a page off Cloudflare;
`fetch 10.0.2.2 <port> /` pulls one off this Mac, which is what the test
drives because slirp maps the host at 10.0.2.2 and nothing leaves the
machine.

A connection's bytes never travel in a message: `tcpring.h` is two
single-producer rings in a region both sides hold, and that was written down
before a byte moved rather than after somebody found a message worked for
the first ten kilobytes.

Three trades, all deliberate. **One segment in flight**, retransmitted on a
doubling timer - a queue and a sliding window buy throughput on a long fat
link and nothing on a line protocol, and it is one timer to get right
instead of four. **Out-of-order segments are dropped**, which costs a
retransmission and saves a reassembly buffer with a policy about overlapping
pieces, the well-known way to get a stack wrong. **The receive window is the
ring's free space**, not a number the stack invented - a window is a promise,
and a client that stops reading really does slow the sender down.

No LISTEN: this end connects out, which is telnet and SSH. Accepting needs
the other half of the diagram and a way to hand a caller a connection it did
not ask for, which is where the HTTP server's argument belongs.

**Not good, and written down rather than left unexamined:** the local port is
a counter. A predictable source port is one an off-path attacker can guess,
and there is no randomness here to do better with.

`telnet` is written and connects; it is half-verified, because driving it
needs a keyboard. Its loop polls `fs.keys` because the console has one reader
and there is no way to wait on two things at once - **the third time that
missing `select` has come up**, after live queries and the stack's own loop.
It is the thing to build when something needs it a fourth time.

Three things that went wrong on the way, all the same shape: **a question
asked in the wrong place.** `net_mtu` landed inside the `hal_snd_present()`
branch, so a machine with a card and no speaker reported no network.
`sys.net()` raised for a process that was not granted the card, when from
inside a program "no card" and "not mine" are the same fact - it returns nil
now, like `sys.screen()`. And the shell has to *hold* the card to pass it on,
which `init.lua` has now got wrong four times; the no-card boot is a
permanent check rather than a comment.

---

**Queries were returning the wrong paths for the whole disk, and no test
looked.** One disk is mounted three times - `/system`, `/user`, `/home`,
each naming a subtree of itself - so the namespace mapped `/home/doc.pdf`
onto `/home/doc.pdf` in the server and then put the mount prefix back on the
way out: `/home/home/doc.pdf`. And the server answered a question asked
about `/home` with everything on the disk, `/system` included.

Both survived because **every query test used `/data`**, the one mount with
no root, so the two paths through that code had never both been walked.
`qbench` measures how fast a query is and `latency.lua` how quickly a watch
wakes; neither would notice the answers being wrong. M7's definition of done
was a live query and `make test` had never checked one.

`tools/run_queries.py` now does, on both kinds of mount, and it checks that
a returned path can be *read back* - a doubled prefix is still a string and
still looks like an answer. Verified by putting the bug back and watching it
fail.

**Kosmos runs under `hvf`**, natively on this Mac's cores: `make fast`.
`gfxbench` reads 4x on a fill, 5x on a blit and **14x on a circle drawn in
Lua** - the interpreter is branch-heavy, which is what TCG is worst at, and
the interface is Lua. It needed a kernel fix. `mmio_write32` was a volatile
store and GCC chose a post-indexed addressing mode for it, which is correct
on hardware and cannot be emulated by any hypervisor: ARM sets ISV=0 in the
syndrome for a load or store with writeback, so nothing in the trap says
which register or what width. The MMIO accessors are one hand-written
instruction each now, which is what Linux's `__raw_writel` does and for the
same reason. `-cpu host` is required, so speed and Pi-5 fidelity are two
targets rather than one flag, and `make bench` stays on TCG because
`-icount` does not exist without it.

**Tracker has the chrome from a Finder window**: back and forward, a search
box top right, the count bottom right. A plain word filters what is on
screen as you type; `name:value` is a query, run on Enter, whose answer is a
folder of files from all over the volume. A query result is *refreshed
twice a second rather than pushed*, because the window is already blocked in
the desktop's poll and there is no way to wait on two things at once - what
is missing is a select, or a second thread.

There is a select now, and it does not help here: `fs.poll` waits on network
connections, and what this wants is to wait on the desktop's input and a file
watch together. The two waits live in different servers, so a select over
both would have to be something the *namespace* offers rather than something
`/net` does. Worth knowing before anyone reads the paragraph above and
assumes the problem went away.

Also: **the Places pane never worked.** Two `local show` declarations, and
the tree's `on_select` closed over the one that is never assigned, so
clicking a directory in it called a nil value.

**Nothing writes an attribute yet**, so queries find nothing until you set
one with `attr`. A Get Info panel that shows and edits them is what makes
the feature real and is the next thing Tracker wants.

---

**A process holds 2224 KB where it held 7232**, and the two things that made
the difference were both consequences of there being one userland image that
every process is a copy of.

`ramfs` kept its store in `.bss` - `static struct node nodes[128]`, 2.1 MB -
so the window manager, Tracker, the shell and twelve other processes each
carried a copy of a server's private storage. It asks for the pages at
startup now, and the cost shows against `ramfs` in the process list where it
belongs.

Then the read-only half of the image - code, fonts, icons, the Lua source of
every program, 2.8 MB - stopped being copied at all. It is mapped where it
lies, one set of physical pages for the machine, read-only and executable at
EL0. Permissions live in the mapping rather than in the page, so nothing
about isolation changes; `design.md` §4.1.1 has the argument and the two
constraints it introduced (an image must be page-aligned, and its declared
read-only half must be bytes the image actually has).

`el0: separate address spaces` had to be rewritten and is a better test for
it. It checked that two processes' code was a *different* physical page,
which was a proxy for isolation and is now deliberately false. It is
`el0: code shared, writable not`, and it asserts the sharing rather than
tolerating it - a test that merely stopped looking would pass if the sharing
quietly stopped happening.

**A whole desktop with a 3D cube turning holds 47 MB where it held 148.**
The heap is now 92% of what a process privately holds, so that is where to
look next - and the answer is starting smaller and growing, not making 2 MB
bigger, because §5.2 chose that number for the collector.

**Drag and drop crosses windows.** The desktop carries a `kind` and an
opaque string it never reads, because it is the only process that knows what
is under the pointer - a press grabs, so the destination never sees one. The
reply is a one-shot right given to the window that was handed the drop and
taken back after, so that "tell the source" is not a way to post an event to
any window whose handle you can guess.

`kfs.rename` takes a path as well as a name, so a move between directories
is two directory entries being edited inside one journal transaction rather
than four megabytes read and written. The test checks the inode number, the
start block and the free count, because "the name changed" is true of a copy
too.

Found on the way: **`files.copy` had not existed since `488f981`** - the
comment block documenting it survived the icons rewrite and the function did
not, so Tracker's Paste had been calling a nil value. And **Photo could only
ever show pictures compiled into the kernel image**, which is why opening one
from Tracker said "no picture called /home/lucas-k.png"; a leading slash now
means a file, decided in the compositor, with no new message and no change to
`ui.image`'s callers.

---

**Six servers are C now, and the console was the interesting one.** The
order was audio, devices, binfs, libfs, appfs, console - each lived with
before the next was started, which is what `CLAUDE.md` asks for and what
found the problems below.

Each speaks a **declared shape** rather than a Lua table: `audioproto.h`,
`devproto.h`, `binproto.h`, `appproto.h`, `conproto.h`. `main.c` dispatches
their role numbers before the interpreter is opened, so a server has no
collector in the process at all rather than a promise not to allocate.

**The console needed a kit, and that is the finding worth carrying
forward.** It is the one protocol with *two* implementations: a terminal
window mounts itself as its child's `/dev/console`, so an application
answers the console ABI as well as the server does, and the runner that
mounts it cannot tell which it got - which is the capability discipline
working correctly. So the layout is compiled once into `use("/kits/console")`
and both sides go through it, rather than a format string in `init.lua` and
a second copy in `terminal.lua`.

`read` also stopped blocking inside a handler. The Lua console pumped its own
mailbox while a line was half-typed, which worked and cost a re-entrant
server and a `sys.yield` spin - there is no UART interrupt to park on. The C
one records who asked and answers from the loop.

**`ramfs` went, and hot reload went with it.** Decided rather than
discovered: ramfs is 247 lines of paths and table lookups, nothing's timing
depends on it, and by `CLAUDE.md`'s own rule it was the weakest candidate of
the seven. It went so the system would be one thing rather than six servers
in C and one in Lua for a feature's sake, at a price known in advance -
`ROLE_RELOAD` deleted and `help("demos")`'s watchable reload with it.
`design.md` §10 is the record, and the honest word is *removed*.

Three things that conversion found, none visible by reading: `/data` had
always stored **Lua values, not bytes** - `help("fs")` promises you get back
the table you wrote - so the namespace packs and the server holds bytes with
a flag saying what they are; the two M4/M5 test clients mount it and had to
learn the protocol; and a replicant publishes a table holding its own source,
several messages long, so packed values page as well.

**`diskfs` is the only Lua server left**, because `kfs.lua` runs on the host
as well as the guest, which is what lets `make test` check the journal's
power-loss window without booting a machine.

**Audio: control by message, data by shared memory.** The period path is an
SPSC ring in shared memory (`audioring.h`) and the message says only which
slot is live. It replaced a 1024-byte period travelling *as* a message
payload 172 times a second, which manufactured 340 KB/s of garbage inside a
5.8 ms deadline. `TICK_HZ` is 250 because of it, and three constants that
silently changed meaning with the tick have been fixed.

**TinyGL is vendored**, with eight demos as eight applications under
`demos/GLDemos`, and a GL Kit at `use("/kits/gl")`. The context is 10 bytes
a pixel and `gl_kosmos.c` refuses one it cannot afford, because
`ostgl_create_context` asserts and an assert here is a panic.

**The chrome is not flat.** `theme.chrome` derives both ends of a gradient
from the one colour a palette names; window tabs and menu bars use it.

**MP3 plays.** minimp3 is vendored and is the first thing brought in here
that needed no patches at all - no libm, no allocation, sixteen-bit output by
default, and NEON is safe because `fp.S` saves the whole `q0`-`q31` file
rather than the callee-saved half. `use("/kits/mp3")` is the door; `music`
takes `.mp3` beside `.wav` and `mp3info` reports the headroom.

**61x real time on QEMU**, and the number that matters is that one 256-frame
period costs 0.095 ms to decode against a 5.8 ms deadline. That is a QEMU
number and worth what `CLAUDE.md` says QEMU numbers are worth, but sixty
times over is margin rather than a fit.

**The window manager's Lua half is about a ninth of a busy pass**, measured
with `frames` on two windows: composing 84.7% (already C), application
requests 10.2%, polls 3.5%, everything else ~1.3%. `CLAUDE.md` carried
"profile before rewriting it" as an open question for months; the answer is
no, and it is recorded there.

**What the same profile found instead**: 4.2 KB of garbage a pass, 3.63 KB of
it in `wait_input` - the call the desktop makes every pass whether or not
anything happened - against 0.02 KB for composing. It was the marshalling: a
1036-byte request string and a 1400-byte reply string, sixty times a second,
because the console had moved to a declared struct. `con.wait` does that
exchange in C into a reused table now. **0.6 KB a pass, one collection
instead of four, worst collecting pass 5.75 ms -> 1.40 ms.**

`frames` reports a KB/pass column per stage, which is what made any of this
visible.

## Next, in order

1. **SSH**, in layers with a test each: the binary packet protocol, then
   Curve25519 key exchange, then ChaCha20-Poly1305, then userauth, then
   channels. This is the one place in the project where a bug is *silent*
   rather than loud - a stack that gets a sequence number wrong stops
   working, and a cipher that gets a nonce wrong keeps working and is not
   secure - so "it connected" is not evidence and every layer needs test
   vectors from the specification.
2. **Get Info, showing and editing attributes.** The query engine works and
   nothing writes an attribute, so the search box finds nothing until you
   use `attr` at a prompt. This is what makes M7 visible.
3. **A preferences app for file types.** `filetypes.by_extension` is
   compiled into the image; it wants to be a file in `/home` that an
   application edits, the way `.appearance` already is.
4. **A name in the index.** A query is over attributes, so `name:*.png`
   cannot be one and the search box filters locally instead. BeOS indexed
   `name` precisely so that it could be a query rather than a walk.
3. **Doom's `DG_sound_module`** - the hook is there behind `FEATURE_SOUND`
   and `i_sdlsound.c` is the model. Doom is silent.
2. **An equaliser in the Mixer**, which is the first thing that will want
   the ring to carry something other than what was written to it.
3. **Seeking in `music`** - the bar is drawn and cannot be dragged. For MP3
   it means finding a frame boundary rather than a byte offset, which is
   what `mp3.decoder():reset()` exists for.
4. **The remaining 0.6 KB a pass** is `application requests` (0.42) and
   `answering polls` (0.15) - the desktop serving its own applications over
   the table protocol. Smaller than what was just removed, and the same
   shape if it ever matters.

## Still open

- **3-4 audio underruns per 2.3 s**, and six structural changes did not
  move it: the ring, the priority band, an 8x buffer, the interrupt, the C
  rewrite, and measuring during play rather than after. 194 interrupts per
  400 periods means the device services about two periods per raise, which
  is QEMU's model rather than ours. **The next measurement wants real
  hardware**, which is M2.
- **`procs` at the shell prints `/dev speaks a fixed protocol; there is no
  send to it`.** Pre-existing and verified against an unmodified tree, so
  it is not from the server conversions. `wm procs` is fine; it is the
  console path that is wrong.

## Two things found while adding a kind column, and not yet fixed

**Every launched program is handed the screen.** Both launchers say
`may_pass_screen() and SPAWN_SCREEN or 0`, so any program the shell or the
desktop starts gets `process_grant_screen` - which maps the framebuffer into
its address space *and* promotes it to `SCHED_PRIO_DISPLAY`. Two
consequences, neither intended. Any program can draw over the desktop
without going near the window manager, which is ambient authority in a
system whose first principle is that what you were not handed you cannot
reach. And the compositor's band means nothing when everything is in it -
`process_grant_screen`'s own comment says "whoever was handed the screen is
the one drawing it", which was true when only the desktop was handed it.

Confirmed on the running machine rather than only in the source: a kind
column that tested `owns & SCREEN` labelled `procs` a server.

**Fixing it uncovers something worse, which is why it is not fixed yet.**
Granting the screen only to programs that declare `kosmos: needs screen`
works, and the four programs that draw (`wm`, `deskbar`, `monitor`, `edit`)
now carry the declaration. But with ordinary programs at NORMAL instead of
DISPLAY, **a thread that spins on `sys.yield()` instead of blocking is
starved outright while the desktop runs.** `say 3 hello` never reaches its
own deadline - and its deadline is wall-clock, off `sys.ticks()`, so even one
per cent of a core would finish it. Instrumented, the loop advances only when
it makes an IPC call: a thread that *blocks* is woken and runs, a thread that
only yields is not. The display harness caught it.

So the scheduler answers for that first, and the screen change is one line
here once yielding at NORMAL is fair. This was invisible until now because
every program was promoted into the compositor's band - nothing had ever run
at NORMAL.

**TinyGL is vendored and the GL Kit works (Sep 2026).** Bellard's software
rasteriser, MIT, byte for byte in `runtime/upstream/tinygl/`. It compiled
freestanding on the first attempt with no errors - it wants `malloc`,
`memcpy`, `assert` and seven functions out of `math.h` - so it is compiled
with `-w -Wno-error` as Doom is. `/kits/gl` is the door; gears renders at
41 fps in a 388x400 window.

All eight demos are upstream C, renamed apart on the compile line because
each defines a function called `draw`. `mech` needs one extra `-D`: it calls
its per-frame function `display`, GLUT's name rather than `ui.h`'s.

**All eight are their own application now, under Demos > GLDemos**, and the
Deskbar nests one level for it: `kosmos: section demos/GLDemos`. The teapot
runs at 34 fps, gears at 41.

**What actually stopped them was a heap, not a channel.** A GL context costs
ten bytes a pixel - a colour buffer and a conversion buffer at four each, a
depth buffer at two - out of a two-megabyte heap Lua is already living in.
360x330 wants 1.2 MB and runs; 460x380 wants 1.7 MB and killed the process
with nothing said, because TinyGL *asserts* rather than returning when an
allocation fails, and an assert here is a panic.

So the kit refuses a context it cannot afford and says what would fit:

    a 460x380 context wants 1707 KB and this process may spend 1536;
    about 157286 pixels fit

The budget is three quarters of the heap, and that fraction is measured
rather than chosen - 388x400 wants 1516 KB and runs, 460x380 wants 1707 and
dies. Half the heap was tried first and would have refused the size that
demonstrably worked, which is the other way to be wrong about a limit and
the more annoying one, because it looks like caution.

**The audio server is C, and its protocol is a struct (Sep 2026).** 482
lines of Lua became 462 of C plus a 108-line header. `main.c` dispatches
role 16 before the interpreter is opened, so the process serving
`/dev/audio` has no `lua_State` at all - which is the difference between
promising not to allocate and being unable to.

`sys.call_raw` and `fs.raw` carry bytes rather than a serialised table; the
namespace still resolves the path and is otherwise not involved. The layout
is written twice, in C and as a `string.pack` format, which is the one place
`serialize.h`'s one-implementation rule is deliberately bent - a
`_Static_assert`, a load-time assert and a server length check stand where
the shared implementation used to.

**It did not change the underrun count**, which was predicted beforehand: 4
against 3, noise. Six structural changes have now been tried against that
number - the shared ring, a priority band, an eight-times deeper buffer, the
device interrupt, the C rewrite, and measuring during play instead of after
- and none of them moved it. The server meets a 5.8 ms deadline with the
whole buffer as margin (worst turn 5.4 ms, machine noise floor 0.26 ms), and
194 interrupts for 400 periods says the device completes about two periods
per raise. **The remaining question is a hardware question**, and `CLAUDE.md`
already says QEMU detects regressions rather than saying whether something is
fast.

What the C server bought is not clicks: it is that the audio path can no
longer acquire a collector by accident, and that a client can no longer send
a shape the server has to think about.

**Sleeping is `sys.sleep`; yielding in a loop is a spin (fixed, Sep 2026).**
The audio path had two of them - the server between refills, the client on a
full queue - and playing a tone cost 63% of the machine for the client and
26% for the server, against 8% for Doom, which renders a frame and then
actually waits. `SYS_SLEEP` now exposes `thread_sleep_until`, which the
kernel has had since M6 behind `SYS_WAIT_INPUT`'s console check, and the
same tone costs under 1%.

The comment in `process.c` that refused to promote the audio server to the
display band said a spinning server is the wrong shape and no band fixes it.
That was right, and it is now moot: the server sleeps, and priority
inheritance (`thread_inherit`) already gives it the caller's urgency for as
long as a client is waiting on it, which is the only time it needs any.

**The tick is 250 Hz, and the sound device chose it.** Four measurements,
delivering 580 ms of audio: spinning server 502 ms, sleeping server at
100 Hz 922 ms, sleeping server at 100 Hz with the queue doubled to 46 ms
966 ms, sleeping server at 250 Hz 520 ms. The third is the one that settles
it - **buffering more does not help, because the shortfall is in how often
the queue is topped up rather than how much it holds.** A 5.8 ms period
cannot be serviced on a 10 ms clock. `audiolag` is the instrument: if its
mean wait is a whole tick, the pipeline is running at the tick rate instead
of the device rate.

Anything measured in ticks changed meaning with it, and **the blinking
cursor is how that got found** - it went five times a second and looked
frantic, which was `CURSOR_TICKS = 25` with a comment reading "at 100 Hz".
Behind it were two that mattered more and were silent: both schedulers set
their quantum to a bare `10`, so a tenth of a second quietly became a
twenty-fifth. All three now derive from `TICK_HZ`, and `wm`'s input timeout
derives from `sysinfo.tick_hz` instead of the literal `1` it was when a tick
happened to be the interval it wanted.

The lesson is the one this system keeps relearning: **a duration written as
a count of ticks is one fact stored twice.** Nothing failed, no test caught
any of the three, and the only reason the quantum change was noticed at all
is that a cursor next to it was visible.

**Waiting is `sys.receive` with a deadline, not `sys.sleep` (Sep 2026).**
The sleep was right about cost and wrong about shape: **a server that sleeps
on a timer is deaf.** The audio server could not answer a client until the
timer got round to it, so every `play` cost a tick - and the Music window,
which hands over a dozen periods a pass, spent 45 ms on twelve round trips
that should be microseconds and played at two thirds speed with the
processor almost idle. *Slow and idle at the same time* is the signature:
it means waiting on the wrong thing.

`ipc_receive` now takes a timeout in scheduler ticks, so a server waits for
a message **or** a deadline, whichever comes first. Both halves already
existed - a thread blocked on an endpoint is `THREAD_BLOCKED`, and
`thread_wake_sleepers` wakes any blocked thread whose `wake_at` has arrived
- and had never been put together.

**Writing the test found a real race.** Unlinking the timed-out receiver
inside `ipc_receive` after it woke leaves a window: the timer makes the
thread *ready* and it does not run until the scheduler reaches it, so a
sender arriving in between was handed a receiver that had already given up -
the message lost, the sender blocked for ever. `ipc_timed_out` now unlinks
from the timer, before the thread becomes runnable. A control test with the
same scaffolding and no timeout in it passed throughout, which is what said
the fault was in the kernel rather than in the test.

**One test was written for this and is not in the tree**, which is worth
being explicit about rather than quiet. `ipc: a timed-out receiver leaves no
trace` - time out a receiver, then have a second thread call and check the
message reaches a fresh receiver - fails, and the same sequence without the
timeout passes. It may be a residual fault in the timeout path or a
thread-pool artifact of the kernel test harness; it was not isolated. What
is known: the path carries thousands of round trips a second under Music
with nothing lost, and `ipc: a receive with a deadline gives up` passes.
**Not a closed question.**

**The virtio-sound interrupt is still the right answer** and is still not
used. Polling at 250 Hz costs almost nothing and meets the deadline, but it
is a rate this system chose rather than one the device asked for. The driver
has the queue set up for it.

**An intermittent panic in `prio_pick_next`.** Seen twice today, both at
`sched_prio.c:165` reading `far 0x2b0` - `head[level]` was NULL while the
`occupied` bitmask said that level had somebody in it. Once in the benchmark
image and once in a screenshot run; both times the run before and the run
after passed with identical code, so it is timing-dependent. Not
root-caused. The queue invariant has only three writers (`prio_init`,
`prio_enqueue`, `prio_pick_next`) and they look locally consistent, so the
next place to look is a thread whose effective band changes while it is
queued - `thread_inherit` and `thread_disinherit` write `sched.effective`,
which is what `level_of` reads, and neither re-queues.

---

## A frame is measured now, and what it said

`make frames`. `wm` keeps seven stage counters, `/bin/frames.lua` reads them
over the window manager's own protocol, and `tools/run_frames.py` drives an
idle desktop, one with a plasma animating, and a window being dragged.

This existed because nothing measured a frame. All five gated benchmarks are
the kernel - IPC, context switch, page fault, allocation - in a system whose
stated aim is a desktop that stays responsive on a Pi 5, and every argument
about rewriting the window manager in C was therefore an argument about a
number nobody had.

**Under animation, composing is 83% of a busy pass, and composing is already
C.** The Lua half - event routing, focus, damage bookkeeping, layout - is
the other seventeen per cent, and most of that is IPC rather than
computation. Rewriting this process in C attacks the seventeen.

**The worst pass has never been a garbage collection.** The worst collecting
pass came to 45-69% of the worst pass overall, in every run. GC jitter is
the whole stated reason for moving a server to C, and here it is not what
the worst case is made of. That is the measurement the C question was
waiting on, and it says no for this process.

**What it found on its first run was in the compositor.** A dragged window
composed half the pixels of an animating one and took four fifths of the
time, which is a cost that does not scale with the damage - and it was
`back:fill(fx, fy, fw, fh, tab)` filling the whole frame of every window a
damage rectangle touched. Ten pixels of damage on a 360x264 window cost
95,040 of them. Clipped, plus the title and the desktop stamp only when the
rectangle reaches them: 4.24 -> 4.09 ms a composing pass, 8.84 -> 8.08 ms
worst. Small on the animating case on purpose - a plasma commits its whole
window, so that is the load the fix helps least. Small damage was the case
paying a whole window frame.

**Not gated, and deliberately.** These are QEMU numbers and `CLAUDE.md` is
clear about what those are worth: the shape survives the emulator, the
milliseconds wait for a Pi.

**What is still weak in it.** The dragging scenario composes about nineteen
frames in eight seconds, which means the synthetic drag is not reliably
landing on the title bar - the numbers from that row should not be trusted
until it is calibrated. And nothing yet decomposes *inside* compose: the
split between what the C primitives cost and what asking for them costs is
still inferred from three points rather than measured.

---

## Current milestone

**No milestone is the current one, and that is the honest answer.** M6, M7,
M8 and M9 have all met their definitions of done; M2's remaining half is a
cable. What is being built now is chosen by hand — see **Concrete next
step** at the bottom: a PDF reader, then sound, then Doom.

**The PDF reader reads.** Not draws: `pdftext` puts a page of The Odyssey on
the console in about 380 ms, through the object layer, the C scanner and the
`/ToUnicode` tables. What is missing before it is a *viewer* is glyph
rendering - `gfx` rasterises by codepoint and a CID font gives glyph
*indices*, so `stbtt_GetGlyphBitmap` and loading a font from the document's
own bytes are the next C additions - and then the window, which is
`reader`'s text view with a different thing behind it.

**M6 — Graphics and the app server. Done.** Its definition of done is met and
tested: `wm hello-win,stuck` drags a window with a hung application inside it
and the window keeps moving. There is a framebuffer, a surface type, a
blitter, bitmap and outline fonts, a compositor with a backbuffer and damage
tracking, a mouse, a UI kit, a Deskbar, and a Terminal.

The Terminal closed the last structural problem on that list. The window
manager could not usefully be run detached while it and the shell's line
editor were both draining one keyboard; once the shell is a window there is
one reader. The framebuffer half had already gone the same way — a process
that owns the screen takes it, and the console stops drawing.

**Still ahead of M6, and not blocking anything:** virtio-gpu and the
`hal_fb_flush` it will earn the HAL. ramfb gives no dirty rectangles and no
vblank, so damage tracking saves the drawing but not the scanout. Under
emulation neither is the bottleneck.

**M5 — Namespaces and servers. Done.** Its definition of done is met, both halves, and the last item on the list — taking Lua out of the kernel — is done as well.

**M4 — Lua to userspace.** Its definition of done is met. Two of its listed pieces are not built; see below.

**M3 — Microkernel. Done.**

**M2 — Lua in the kernel + second target**, whose remaining half is the second target and is blocked on cables.

Definition of done: a `>` prompt over serial where `2+2` returns `4`, under QEMU **and** on real hardware.

**The QEMU half is done.** The prompt runs Lua 5.4.8 with coroutines, closures, the string and math libraries, and errors caught by `pcall`.

**The hardware half is blocked on cables** and is the only thing left in M2.

**M0 and M1 are closed.**

## Active target

QEMU `virt` aarch64, and nothing else. Real hardware arrives at M2.

## Recently done

- **Doom.** See above. Seven bugs on the way and the two the *system* owned
  are the ones to remember: `snprintf` ignored precision on integers, so
  `%.3d` of 33 gave "33" and Doom's HUD font lumps came out one character
  short - a missing-file error caused by a formatting bug. And C output from
  a windowed process vanished entirely, because `SYS_WRITE` is refused
  unless the process owns the console; forty thousand lines of Doom started
  up, failed and said nothing. A refused write now spills into a ring that
  something with a namespace drains.

- **`exit()` can land somewhere.** It panicked, and the comment was right -
  there is nothing to exit *to*. That is exactly what a vendored port
  breaks: `I_Error` prints and calls `exit`, and with a panic on the end the
  process died with the explanation still in a buffer. `kosmos_exit_arm()`
  is a `setjmp` a caller may arm.

- **`fopen` works for files a process already holds.** `kosmos_provide(name,
  bytes, len)` and then `fopen` finds it. Not a global tree: one process
  saying what a name means to it, which is what CLAUDE.md's "a libc whose
  I/O resolves against that process's namespace" comes to when there is no
  tree to resolve against.

- **Keys are two streams now.** `key` is characters, as always. `rawkey` is
  transitions - keycode and up/down - posted only to the focused window,
  gated on owning the console because a process that can watch every key is
  a keylogger. A character cannot say a key is *held*, which is why holding
  a direction in Doom was a step per key-repeat.

- **A clock.** PL031 at 0x9010000, read out of the device tree rather than
  remembered. `/dev/clock`, `lib/clock.lua`, and a Date & Time panel. The
  offset is an *offset*, not a timezone: there is no tzdata, so summer time
  is set by hand twice a year and the panel says so.

- **Restart and Shut Down**, in the Deskbar. PSCI over `hvc`, again from the
  device tree. Gated on `owns_procctl`, because turning the machine off is
  ending every process at once.

- **The desktop is Tracker with the frame taken off.** `backdrop = true`
  puts a window at the bottom of the stack undecorated; `strip = "top"` is
  its opposite and takes room away rather than sitting over things.

- **Real file icons.** The Tango Icon Library, public domain, at the 32x32
  size it was drawn for, decoded by `gfx.png` and composited by
  `surface:blend`. Nothing converts them.

- **Startup items**, in `/bin/startup`, read by the Deskbar - deliberately
  not on the boot path, because init's argument about a machine that cannot
  reach a prompt still stands.

- **One bug shape, five times in one session**, and it is worth naming
  because it will happen again: *two copies of one fact that agree until
  they do not*. The font role and the list selection. The bar's height and
  the height it was granted. `USER_HEAP_PAGES` in the kernel and
  `USER_HEAP_SIZE` in userland. Appearance's hardcoded layout against a font
  size the user picks. And `struct sysinfo` never being zeroed, which is the
  same thing wearing a security bug's clothes - any field the kernel does
  not write is kernel stack handed to a process.

- **A review pass, and two live races it found.** Reading `memobj.c` rather
  than remembering writing it: `memobj_unref` cleared `in_use` *before*
  walking the index to free the pages, so another `sys.memory` could claim
  the descriptor and start rewriting `index[]` mid-walk - pages freed twice,
  and pages belonging to the new region freed under it. `memobj_create`
  mirrored it, writing `pages` and the index into a slot it had not claimed.
  The old contiguous code had a narrow version of both; turning a region
  into a page list widened the window enormously. The slot is claimed first
  and released last now.

- **The text-extraction path is gone, with everything that served it.**
  `pdfpage.text`, `decode`, `char`, `tounicode`, the `/ToUnicode` parser and
  `user/bin/pdftext.lua`. It was the first idea - read a PDF as text and lay
  it out in the system font - and the renderer superseded it.

  It was not merely dead. `pdfpage.font` read and inflated the `/ToUnicode`
  stream **on every page render, per font**, to build a table nothing used
  any more. A page went 152 ms to 143 ms by deleting it.

  Also removed: `pdfpage.prepare`, which was written for an allocation
  ordering that no longer applies and was never called; and `multiply`,
  superseded by `multiply_into` the day the interpreter stopped allocating a
  matrix per glyph. `pdfpage.lua` is 30 KB down to 22 KB.

- **A comment that had become false.** `pdfpage.lua` explained its buffer
  sizes in terms of `pmm_alloc_contiguous` - true when it was written and
  wrong since regions became lists of pages. The sizes are right for their
  own reason and now say so.


- **A server runs at the priority of whoever is waiting for it.** Priority
  inheritance, from QNX, and it fits a synchronous rendezvous exactly: the
  kernel already knows who is blocked on whom, because that is what
  `ipc_call` is. `thread_inherit` on delivery in both directions,
  `thread_disinherit` in `ipc_reply`, and the scheduler queues on the
  *effective* band rather than the given one.

  This is the answer to the thing that failed two days ago. Promoting the
  console server to the input band starved the machine, because it is also
  the path every `print` takes - at the top band it outranks everything it
  serves. Inheritance means the question never arises: it sits at NORMAL and
  *becomes* urgent for exactly as long as something urgent is waiting on it.
  The note in `process.c` that recorded the failure now records the answer.

  Cleared on reply rather than unwound. A coroutine server handling two
  requests at once would need a stack of borrowed bands to be exact, and the
  error either way lasts one request - erring downward, which is the safe
  direction: a server that stays high starves the machine, one that drops
  early is briefly slower.

  There is a test, and it is the kind that would fail silently without care:
  a server created at LOW, a caller at INPUT, and the server reads its own
  effective band from *inside* the handler and again after replying.

- **The benchmark "deadlock" was a stale image, and three conclusions drawn
  from it were wrong.** Worth recording in full, because the failure was in
  the method rather than the code.

  The bench image is `build/bench/kosmos.elf`, built by `make BENCH=1
  build/bench/kosmos.elf`. Plain `make` builds `build/kosmos.elf`, a
  different target. Booting the second while believing it was the first gave
  a `thread_block: every thread is blocked` panic from an image several
  commits old - and on the strength of it: the benchmarks were declared
  hung, priority inheritance was blamed, inheritance was reverted, the panic
  "persisted", and inheritance was therefore declared innocent. Every one of
  those was reasoning about a binary nobody had rebuilt.

  Rebuilt properly, HEAD runs all five benchmarks under `-icount` and there
  is no deadlock anywhere.

  The harness timeout is still raised to thirty minutes, which is right for
  its own reason: the image has grown by the glyph rasteriser, the PDF
  scanner and the inflate kit, and `-icount` is several times slower.

- **What the benchmarks actually say about priority inheritance:**
  `context_switch` 8.375 -> 9.125 (+9.0%) and `ipc_roundtrip` 36.438 ->
  41.439 (+13.7%). Not recorded as a new baseline, because whether that is
  worth paying is a decision rather than a measurement. The cost is
  `thread_effective_priority` - a max of two fields - being called on every
  enqueue, every pick and every wake, plus the inherit and disinherit on the
  IPC path. It is optimisable: the effective band could be stored on the
  thread and recomputed only when either input changes, which would take
  most of it back.


- **A PDF renders as it was typeset.** `wm pdfview:/home/odyssey.pdf` draws
  the page in the document's own Times New Roman, at the positions its
  producer chose, from the font programs carried inside the file. 685 glyphs
  in 152 ms, and scrolling is a blit out of a surface that already holds the
  page - the interpreter does not run again and no glyph is rasterised
  twice.

  `user/lib/docfont.c` is the C half: a font loaded from bytes in the
  document, rasterised **by glyph index** rather than codepoint, cached per
  face per size, and a `draw` that takes a whole page as a flat array so a
  page is two or three crossings instead of two thousand. `pdfview` is a
  direct window (`gfx.md` 19.4), which is what lets it own its pixels.

- **A PDF's fonts have no `cmap`, and that is correct.** A CID-keyed subset
  is addressed by glyph index, so a character map means nothing and the
  producer drops it - the font here has eleven tables and `cmap` is not
  among them. `stbtt_InitFont` refuses a font without one, *and* refuses one
  whose cmap carries no encoding record it recognises: the last thing it
  does is `if (info->index_map == 0) return 0`. Rather than touch vendored
  code, `ensure_cmap` writes a 22-byte table into our own copy of the font
  and repoints the unused `post` entry at it.

- **Sixteen capabilities a thread was a number from when the userland was a
  shell.** A graphical application holds its console, its `/dev/wm`
  endpoint, the filesystem, its window's region, a read buffer, the buffers
  a page decodes through, and a region per embedded font. It ran out
  mid-page, and the failure arrived as `NO_ROOM` - which reads as "out of
  memory" and sent two evenings at the allocator. There were 117,000 free
  pages at the time.

  Three things came out of that and all three stay: the limit is 32,
  `SYS_ERR_NO_CAPS` is its own error rather than folded into `NO_ROOM`, and
  **`sysinfo` reports the region pool**, which it never did - `memobj_in_use`
  and `memobj_total` had existed since regions did and nothing had ever
  called them, so "could not allocate a region" was the same sentence
  whether the machine was out of memory or out of descriptors.

  **Sixty-four was tried first and panicked the benchmark image.** A slot is
  32 bytes, so that was 73 KB more `.bss`, and this kernel has a documented
  constraint about exactly that: the thread stacks and their guard pages
  have to stay inside the first 2 MB of RAM, the only part mapped a page at
  a time. `make test` passed and the benchmarks did not, which is the
  argument for having a third build.

- **Errors from a syscall have words now.** `ipc_error` knew five codes and
  answered "unknown error" for the rest, including every `SYS_ERR_*`. Half
  of the debugging above was reading that phrase.


- **A region is a list of pages, not a run of them.** `memobj.h` used to
  explain why they were contiguous and named the cost in its own words: "a
  large region can fail to allocate on a fragmented machine even when there
  is enough memory. That is real." It became real, so the premise changed.

  A page holds 512 pointers, so a region's pages are indexed by up to eight
  index pages taken from the allocator itself. The objection the old comment
  raised - a quarter of a megabyte of `.bss` for lists that are usually
  empty - is answered rather than ignored: the index is allocated per region
  that exists, and the descriptor grows from 40 bytes to 96, which is 24 KB
  of `.bss` instead of 10.

  What is given up: mapping walks an index instead of adding to a base, and
  a region can no longer be handed to a device expecting one physical run.
  Nothing does - DMA here uses kernel buffers, identity mapped and
  contiguous by construction.

  156 checks and the benchmarks are unmoved by it.


- **The scheduler can be changed while the machine is running.** `wm
  scheduler`: which policy, how long a turn lasts, what the timer rate is
  and how many bands there are - all read from the kernel, and the first two
  changeable from the window. There is a button that starts a busy thread,
  because an idle machine schedules identically whatever you pick and the
  app would otherwise be a display of numbers that never move.

  `SYS_SCHED_INFO` and `SYS_SCHED_SET`, and `sched_switch_to` underneath
  them. **Swapping policies had to drain, not reset.** `sched_use` calls
  `init`, which empties the queues - correct at boot, where nothing is in
  them, and a way to lose every runnable thread on the machine at any other
  moment. The threads are not in a list the kernel keeps; they are in
  whatever structure the policy chose, and `pick_next` is the only handle on
  them. So they are pulled out one at a time and handed to the new policy
  before it is installed, with interrupts masked across the exchange.

  **Quantum and policy are anyone's to change; priority is not.** Tuning the
  machine you are sitting at is not reaching into another process, and there
  is nobody to defend a single-user system from. Setting a *priority* is
  different: bands are handed out by capability precisely so nothing can
  promote itself, and a syscall for it would undo that in one line.

- **The screen owner runs in the display band; the console owner does not
  run in the input band.** The first is committed and works. The second was
  tried, because it looks like the same argument - the console owner is the
  one process allowed to read input, so it is what every keystroke waits on
  - and it starves the machine: that process is also the *output* path, so
  at the top band it outranks everything it is serving. `thread: three
  threads interleave` failed within a minute.

  The fix is one of two things this policy does not have: a boost that lasts
  only across the wake, or priority inheritance across IPC, which is QNX's
  answer and the better idea. Written up in `process.c` where the promotion
  would go.

- **A struct in `syscall.h` outside the assembler guard.** `user/hello.S`
  includes that header for the syscall numbers, and a struct there is a
  syntax error per line. It only broke the *test* image, which is the only
  build that assembles that file - so the ordinary build was clean and the
  suite caught it.


- **Priorities, and a wake that is acted on.** `kernel/sched_prio.c`: five
  named bands - idle, low, normal, display, input - with round robin inside
  each, and a thread that becomes ready while outranking the running one
  takes the CPU at the next exception instead of waiting out a quantum.
  `design.md` and `ui.md` had both called input-at-highest-priority
  non-negotiable since before there was a scheduler that could express it,
  and `sched_rr.c` said in its own first comment that there was "nothing to
  prioritise". Both are now true at the same time.

  From QNX, which is a microkernel of the same shape and a real-time system
  - and real-time means bounded, not fast. Taken: strict priority with
  immediate preemption, and a quantum that can be changed. Not taken: 256
  levels, and hard guarantees. Starvation is real under strict priority and
  is accepted deliberately; the reasoning is in the file.

  **The cost, measured rather than assumed.** `context_switch` 6.875 ->
  8.375 and `ipc_roundtrip` 30.376 -> 36.438. Read against two days ago
  rather than against yesterday: the switch is **-14.6%** net and IPC is
  flat, because lazy FP save bought the priority queue rather than the
  priority queue being free.

  The first version scanned the eight levels to find the highest occupied
  one, and cost twice that - almost everything runs at NORMAL, so almost
  every pick walked five empty bands first. An occupancy bitmask and `clz`
  make it one instruction, which is what QNX and Linux both keep. The
  benchmark caught it the same afternoon.

- **A test was quietly disabling the feature it sat above.** The policy-seam
  test swaps in a deliberately terrible LIFO scheduler and then restores the
  default - by *name*. The name it restored was `sched_round_robin`, which
  stopped being the default the moment `sched_priority` arrived, so every
  test after it ran under round robin. Both new scheduler tests reported the
  priority policy broken when what was broken was that one line.

  A test that changes global state and puts back what it *thinks* was there
  is a test that can disable a feature for everything after it, and report
  the feature as the failure.

- **And one of those tests was wrong in the other direction**: it put the
  low-priority thread at LOW while the test thread itself ran at NORMAL, so
  strict priority starved it exactly as designed and the test measured
  starvation rather than ordering. Correct behaviour, badly built test.


- **FP and SIMD are saved lazily, and it is the first piece of M10.**
  `context_switch` **9.812 -> 6.875 ticks, -29.9%**, and `ipc_roundtrip`
  **36.251 -> 30.376, -16.2%**, because a round trip is two switches and was
  paying for the whole register file twice.

  The switch does not save FP any more. It disarms it - `CPACR_EL1.FPEN` to
  0b00 - and the first floating-point instruction the incoming thread
  executes traps into `fp_fault`, which writes the previous owner's
  registers into its context, reads this thread's back, and arms FP again. A
  thread that uses FP pays one fault per time slice. A thread that does not
  pays nothing, and most do not: the kernel is built `-mgeneral-regs-only`
  and cannot emit an FP instruction, so every kernel thread is in the second
  group.

  **Trapping EL0 alone was tried first and three tests said no**, which is
  the argument for having had them. Kernel threads run at EL1, so their
  registers would have been neither saved by the switch nor faulted in by
  anything - which is exactly the bug the eager save was added to fix. And
  `longjmp` writes `d8`-`d15` from its buffer, so a kernel thread returning
  through one would overwrite whatever EL0 thread owned those registers.
  Arming both levels is simpler than either.

  `exception` went **7.250 -> 7.562, +4.3%**, and that is the price rather
  than a regression to chase: every exception now begins by asking whether
  it is an FP trap, because that has to be settled before the
  fault-expectation machinery looks at the frame. One comparison on every
  exception against the whole register file on every switch.

  Two permanent tests. The first was rewritten rather than added: it used to
  assert `CPACR.FPEN == 0b11`, which tested the old *mechanism* - FP enabled
  once at boot and never moved - and says nothing now. It asserts the
  property instead, that FP works at EL1, plus that the lazy path is what
  made it work. The second drives the mechanism directly: disarmed and
  unowned after a reset, armed and owned after one instruction.

  **That second test was wrong on its first attempt in an instructive way.**
  It called `thread_yield` to force a switch, and a yield with nothing else
  runnable does not switch at all - so the setup silently did nothing and
  the test failed for a reason unrelated to what it was checking. A test
  whose setup can quietly not happen is a test that will one day pass for
  the wrong reason.

- **doomgeneric is vendored and the WAD is on the disk.** 95 `.c` files,
  73,095 lines, unmodified under `runtime/upstream/doom/` with its licence,
  the same rule `lua/upstream/` and `stb/` follow. Its platform layer is six
  functions - `DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`,
  `DG_GetKey`, `DG_SetWindowTitle` - and `pixel_t` is `uint32_t`, so a frame
  is a blit into an XRGB8888 framebuffer rather than a conversion. Nothing
  is built yet: it is not in the Makefile.

  `doom1.wad` is 4,196,020 bytes of shareware and is **not** in the tree. It
  lives on `build/play.img` beside `odyssey.pdf`, which is what a filesystem
  is for.


- **A PDF is readable in a window.** `wm pdfview:/home/odyssey.pdf` opens
  The Odyssey and turns its pages, 142 ms a page. Text, wrapped by the
  window in the system font - not the page as it was typeset, which needs
  glyphs rasterised by index out of the font inside the document and is the
  next piece.

  What stood between it and working was memory, and finding it took three
  wrong answers. The scanner returns a batch of tokens as two Lua tables,
  and at 1024 entries those did not fit beside everything else a windowed
  program holds - a window starts around 640 KB because the UI kit is
  loaded, against 330 KB for a console one, which is exactly why `pdftext`
  worked all along and `pdfview` did not.

  Two things fixed it rather than one, and the second is the one that
  matters: the batch is 256 now, and the page list holds an object number
  per page instead of a table with a reference and four inherited
  attributes in it. That was 500 tables for a 254-page book and about
  310 KB still held after opening; the inherited attributes are found by
  walking `/Parent` when a page is asked for. Before it, 512 worked and
  1024 did not. After it, 2048 works. The batch sits at 256 well inside
  that.

- **A check that cannot see a failure reports a pass, twice in one
  evening.** The first was `grep -c ... || echo 0`, which prints `0` twice
  when it matches nothing, so every run compared unequal and every batch
  size looked broken. The second was worse: `pdfview` showed its error *in
  the window* and printed nothing, so grepping the serial log said every
  batch size worked - including the ones a screenshot plainly showed
  failing. Errors go to the serial line as well as the window now, which is
  the actual fix; the lesson is the one this project keeps paying for, that
  a test exercising a different path from the user is a test that agrees
  with you.


- **A PDF reads on the machine, and the language line moved with the
  measurement.** `/lib/pdf.lua` is the object layer - cross-reference table,
  indirect objects, the page tree - and it never holds the document: it asks
  its source for a window at a time, and parsing all 1127 objects of a 1.6 MB
  book reads 5.9% of the file. The window is 256 bytes because that was
  measured against 128, 512, 2048 and 8192, and the table is in the file.

  `/lib/pdfpage.lua` interprets the content stream and `pdfinfo`, `pdftext`
  and `pdfbench` are the programs over it. The Odyssey's 254 pages, its
  fonts, and a page of Homer as text, all from `/home/odyssey.pdf` on a real
  disk.

- **The scanner is C and the profile is why.** A page cost 1.1 seconds, of
  which 538 ms was the tokenizer - 144 microseconds a token, which is
  `string.sub` allocating a one-character string per byte. In C the same
  3,657 tokens take **4.7 ms**. The page is now 380 ms and the remainder is
  the interpreter's matrix arithmetic, which is the next thing to look at and
  has not been.

  `pdfbench` prints the profile phase by phase and stays, so the day someone
  wonders whether the C is still earning its keep the answer is one command.

- **Kits.** C libraries reached as `use("/kits/compress")`, through the
  namespace like any other library. `inflate` and the PDF scanner sat in
  `sys` for an evening and did not belong: `sys` is the syscall boundary, and
  a decompressor is not a syscall. `kits` lists them.

- **A capability can be given back, which it could not before.**
  `SYS_CAP_DROP`. A thread has sixteen slots and nothing ever released one -
  `ipc_caps_release` ran when a thread died and that was all - so the
  filesystem server, handed a buffer per request, filled its table and
  refused every request after the sixteenth for the life of the machine. The
  endpoint pool had this same bug once and was fixed; memory never got the
  matching half. Found by a PDF read in 256-byte windows, on the fifteenth
  read.

- **And releasing has to mean losing the mapping.** `SYS_SHARE_UNMAP`, because
  `SYS_UNMAP` is bounded to the window `SYS_MAP` hands out and *frees* what it
  unmaps - correct there, a double free here, since a shared region's pages
  belong to the memobj. Without it `sys.release` gave up the right to name a
  region while keeping the ability to read and write it.

- **The bug behind the bug, and it cost an evening.** `region_of` in
  `sys_user.c` caches mappings keyed by capability *index*. That was safe for
  exactly as long as an index was never reused, which was until `sys.release`
  existed. Afterwards a server dropped slot 1, the next region arrived at
  slot 1, and the cache handed back the *previous* region's address - so the
  server wrote 1811 bytes into somebody else's pages, read them back
  correctly, and reported success, while the process that owned the buffer
  saw zeroes.

  What found it was making the server read back its own write: `server_sent
  789c, server_readback 789c, client 0000` says the two capabilities are not
  the same object, and nothing else does. Three hypotheses were tested and
  discarded first - the install dedupe, generation on release, and a false
  byte count - which is worth recording because each was plausible and none
  was it.

- **An install dedupe was tried and removed.** Handing back an existing index
  for a region a thread already held, without taking a reference. Wrong in
  company: `SYS_MEM_CREATE` unrefs after installing on the stated grounds
  that install took a reference, so when the dedupe fired there the count
  went to zero and freed the region its caller had just made.

- **Decimals are parsed exactly.** The C scanner multiplied a running scale
  by 0.1 per digit, so `-2.25` came out a few units in the last place from
  what Lua's own `tonumber` gives: it prints identically and compares
  unequal. Now it is an integer mantissa divided once by a power of ten. A
  content stream is mostly fractions like `.23999999`, six before every `cm`,
  so this was a rendering bug and not only a test one. The test found it on
  its first run.

- **`BOOT` with a space in it never worked.** `make qemu BOOT="wm blocks"` is
  in the Makefile's own comment as an example; the shell split it and QEMU
  took the second word for a filename. Quoted now.

- **Three permanent tests**, one per thing above: forty regions made,
  mapped, written, read back and released; a Flate stream produced elsewhere,
  both through a string and region to region; and the scanner against a
  content stream with a negative number, a leading-dot fraction, a hex
  string and an escaped bracket in it.


- **The disk can be written from this Mac.** `tools/kfs.lua` runs the
  filesystem on the host over the image file: `create`, `ls`, `put`,
  `get`, `rm`. It is the answer to the one real cost of not using FAT32 -
  a Mac cannot mount the image, but it can write it. Both directions are
  tested by `run_interchange.py` in `make test`: a file written here is
  read inside the machine, and a file written inside is read back here.
  One implementation of the format, not two, which is what makes it
  trustworthy.

- **Subtree mounts, and the layout is real.** `ns.mount(prefix, cap,
  root)` maps a name onto part of a server, so one disk appears as
  `/system`, `/user` and `/home` - which is what `layout.md` describes and
  what a single filesystem could not do before. `mkfs` makes those three
  directories, because a formatted disk should be a Kosmos disk: without
  it `save notes.txt` failed on a fresh drive, `/home` being a mount point
  with nothing behind it.

- **Next, decided:** a PDF reader for plain documents - no forms, no
  encryption - then sound, then Doom. The PDF viewer is more tractable
  than it looks: `puff` already does Flate, `stb_truetype` already
  rasterises glyphs, and the blitter already exists. What is left is the
  object model and the content-stream interpreter, which is structure
  parsing and belongs in Lua.


- **A file can be larger than a message.** `read` takes `into` and `write`
  takes `from` - a capability to shared pages the caller owns - which is
  `read(fd, buf, n)` with the buffer named by a capability instead of a
  pointer, because a server at EL0 cannot dereference the caller's
  pointer. 200 KB out and back, in one extent, verified across a reboot.
  Before it, nothing above about two kilobytes could be written at all.
  `kfs.read_range` is the `pread` underneath, so a window is read without
  the rest of the file. `MEMOBJ_MAX` is 256, so a tiled image can hold a
  region per tile.

- **`docs/layout.md`**: what lives where, in the tree and at runtime. A
  BeOS-shaped runtime layout - `/system` for what ships, `/user` for what
  is installed, `/home` for what you made - with the caveat that it is a
  convention init hands out rather than a tree anything can walk. It also
  records two constraints any "servers in C" plan runs into: every process
  is currently the same ELF entered at a different role, and there is no
  dynamic linking, so C system libraries are a build-time fact rather than
  files.


- **A directory bigger than a message lists.** `list` answers in pieces the
  way `read` already did - offset in, `more` out. Before it, `ls` on a
  directory of two hundred files said "the answer does not fit in a
  message" and showed nothing, which the power test had been quietly
  hiding behind an `or {}`. Found because that harness printed "128 files
  survived, 0 names" and the two numbers disagreed.


- **The journal works, and M8's definition of done is met.** ext3's shape:
  write the blocks into the journal, write one commit block, apply them,
  clear the header. The ordering is the whole guarantee. `make powertest`
  kills the machine five times mid-write and finds every directory entry
  readable and no file half-written; `tools/test_kfs.lua` tests replay by
  *choosing* the instant, because a SIGKILL cannot aim at the few
  milliseconds that matter. Each of the four rules was deleted in turn to
  check its test fails without it - and one did not, which is how a hole in
  the uncommitted-journal case was found.

- **A host Lua**, `build/host/lua`, so pure-logic libraries are tested
  without booting. `kfs.lua` is the case that asked for it.

- **The language line, measured rather than argued.** The journal's
  structure in Lua costs about 2%; its checksum, a byte loop in Lua, cost
  30%. Moved to C it recovers about half - the rest is unexplained and
  worth chasing. `design.md` 6 now carries the whole reasoning: where the C
  actually runs (EL0, in the process, not the kernel), what a C server
  would cost in marshalling and hot reload, and the experiment that would
  settle it - `kfs.store` written both ways. The prediction on record is
  fifteen percent.

- **Next, by decision rather than by roadmap:** audio - a virtio-snd driver
  in the HAL, WAV first to prove the path, then a vendored MP3 decoder as
  userland C beside the font rasteriser - and a music player. Then Doom,
  then real hardware. `roadmap.md` is a guide and not a rule; it still says
  audio is out of scope, and it is out of date rather than right.


- **The serialised format is little-endian on purpose now.** It used to be
  a `memcpy` of the native bytes, which was invisible because every target
  so far agrees. It matters for the disk before anything else: attribute
  blocks are `sys.pack` output written into a block, and `kfs.lua` is
  explicitly little-endian everywhere else, so that was the one part of the
  on-disk format that depended on the machine that wrote it. A disk
  outlives a boot and can be carried to another machine. The test checks
  the actual bytes, because a round trip proves only that the two halves
  agree with each other - two matching native-endian halves round-trip
  perfectly and produce a format nothing else can read.


- **The index is rebuilt at mount, and queries work on the disk.** A scan
  of the tree reads every attribute block into `index[attribute][value] ->
  paths`, the same shape the ramfs builds. Nothing about it is written
  down: derived state that is also stored is state that can disagree with
  itself, and on a filesystem that disagreement is a query returning a file
  that is not there. `name` is indexed for every file without anybody
  declaring it, which is BFS's rule. `find` now asks every mount that can
  answer instead of the one it used to name.

- **The arrow keys are not broken.** The display harness's arrow phase
  fails often and it has been recorded as a bug twice. The input path was
  instrumented end to end and is correct at every stage: the window manager
  forwards all three bytes, the kit decodes them, and the selection moves
  sixteen pixels a press - 264, 280, 296, 312, 328, measured with the
  harness's own probe. What fails is the harness, on a busy host, at
  whichever phase it happens to be running. `testing.md` 18.11 has the two
  mistakes made while working that out, including a control run that shared
  the variable it was controlling for.


- **Kosmos is MIT.** `LICENSE` at the root, and every file this project
  writes carries a one-line notice above its description. Vendored code is
  untouched and its licences are named in `LICENSE`. `assets2c.py` now
  repeats each vendored file's licence in the generated C and warns when
  one has none - which found two fonts and an image that have none.

- **Attributes on the disk.** One block per file, pointed at by the inode
  field that had been reserved and zero since the format was laid down, and
  serialised with the same `sys.pack` a message uses. `kind`, `size`,
  `mtime` and the extent count are read from the inode and refused by
  `setattr`: a stored copy of a fact is a copy that can disagree. `kind` is
  the exception and it took a failing test to see why - structural for a
  directory, free for a file, because a People file's kind is `person`.
  Nineteen checks in `make disktest` now, across two boots.

- **A benchmark for comparing machines.** `score` prints it, `sysbench`
  draws it, and both are the same engine in `/lib/bench.lua`. Twenty-two
  measurements in six groups, two minutes, one number. Fixed time rather
  than fixed work, so a Pi 1 takes the same two minutes and reports smaller
  numbers. `testing.md` 18.10 has the three mistakes that had to be fixed
  before any of it meant anything.

- **Pac-Man is gone and Tetris is `blocks`**, titled Falling Blocks. Names
  somebody else owns are not worth the trouble on a public repository.


- **Outline fonts, with three roles.** `stb_truetype` vendored unmodified,
  TrueType and CFF, so `.otf` works as well as `.ttf`. The build embeds
  whatever is in `assets/fonts/`, so adding a face is dropping a file there.
  Three independent roles - `ui`, `text`, `mono` - because a terminal's face
  has to be fixed-width whatever the other two are, and one setting for all
  three could only be right for one of them. Appearance picks a face and a
  size per role. The check that says they are really independent: measuring
  `iii` against `WWW` gives 21/75 for a proportional face and 18/18 for the
  monospace one. See `gfx.md` §19.12.

- **A machine with no display boots again.** It had not, and silently: init
  asked for the screen grant whether or not there was a screen, the kernel
  refuses a grant it cannot give, and the refused spawn was the shell - so
  `make serial` reached stage 12 and then a prompt that never came. The
  same mistake was in `run_program`, so even with the shell fixed nothing
  would start. Both fixed by asking for the grant only when the machine can
  give it, which is what the disk already did four lines higher.
  `tools/run_headless.py` now runs as part of `make test`, and both sites
  were re-broken one at a time to prove it catches each. See `testing.md`
  §18.9.


- **The disk is real, and what is written to it survives.** `mkfs --yes`
  formats, and a machine that has never seen the disk before finds a
  filesystem on it. virtio-blk in the HAL, three syscalls behind the
  strongest grant in the system, a disk server that is the only process
  holding it, and `kfs.lua` - the format, borrowed from ext2's skeleton with
  BFS's semantics and extents instead of indirect blocks. `make disktest`
  boots the machine twice against one image, because the question cannot be
  asked inside one boot.

- **A software 3D renderer.** `wm cube3d`: a solid, shaded, rotating cube at
  about 42 fps, drawn into a surface shared with the compositor. The split
  is the point - `surface:triangle` is the only new C, because it runs once
  per pixel; the matrices, the projection, the back-face test and the depth
  sort are all Lua in `/lib/g3d.lua`, because they run once per vertex. A
  cube is 12 triangles and tens of thousands of pixels, which is why that is
  not a close call.

- **Preemption preserves the whole FP register file.** The switch saved
  `d8`-`d15`, which is right for a thread that *called* it and wrong for one
  interrupted between two instructions. Silent wrong numbers, never a crash.
  A software renderer would have been the first thing to hit it constantly.

- **Shared-memory surfaces.** `sys.memory(pages)` makes a region, the
  capability travels in an ordinary message, and both sides map the same
  pages. An application draws straight into the surface the compositor
  composites from, so a frame costs one `commit` instead of a message per
  drawing command: `plasma` runs at 42-60 fps. The rules still hold - the
  pixels are behind `gfx.wrap`, so nothing in Lua ever holds one or computes
  its offset.

- **The Terminal serves its children without sleeping on them.** A window
  that is somebody's console cannot poll once a second: every child `write`
  blocks in `sys.call` until the window next wakes, so `ls` arrived at one
  line per second. `ui` has a `poll_wait` a window can lower; the Terminal
  sets it to one tick.

- **`make release` builds one image per resolution.** `builds/` carries a
  suffixed ELF for each of 1024x768, 1280x800 and 1920x1080, and
  `run-kosmos.sh -r WxH` picks one (`-r list` says which exist). Verified by
  booting all three and reading the geometry they report, which is how the
  first version was caught building the same image three times.

- **A Terminal.** `wm terminal`: type a program's name and it runs *in the
  window*, with its output going there instead of to the machine's console.
  It is a console server - it speaks the same `write` and `read` that
  `/dev/console` speaks and hands itself to its children under that name -
  which is the design working rather than a trick played on it. No program
  knows or can ask what is behind `/dev/console`.

- **PNG.** `gfx.png(bytes)` decodes into a surface, using `puff` - zlib's own
  reference inflate - vendored under `runtime/upstream/`. `ui.image` and the
  `photo` app draw one. The picture is *named*, not carried: an application
  sends `{op="image", asset=...}` and the compositor decodes it, because a
  decoded image is megabytes and a message is two kilobytes.
- **The compositor clips each window to the damage rectangle.** It redrew
  every window in full for every rectangle, so dragging got heavier with
  every window opened. That was the jerkiness.
- **The fine-mapped low region follows the image size** instead of being a
  hardcoded 2 MB. Adding a megabyte to the image used to panic in
  `mmu_init` about a stack guard, which is nowhere near the cause.

## Known bad

**One display phase fails about one run in three, and I do not know why.**
`check_widgets` clicks the gallery's list and presses Down through QEMU's
input plumbing; the selection has to move a row. It passes standalone every
time, and it passed two full runs out of the last four.

**Sharpened 2026-08-31, and it is wider than this one phase.** Six runs that
day failed three times, at *three different* phases: Control-C out of
`plasma`, the Down arrow twice, and Control-C out of the window manager.
Every one of them is the same sentence - an input the harness sent did not
arrive - and they are not one mechanism: the arrow goes through the monitor
socket and virtio-input, and Control-C goes down the serial line. So
whatever this is, it is not the monitor backlog on its own, and naming it
"the arrow key phase" was wrong. It is *input delivered while the guest is
busy*, by either road.

One thing changed that day which is worth holding against it rather than
forgetting: the context switch got 36% slower when it started saving the
whole FP register file, and IPC 17%. That would not create a race, but it
would change the odds of one that was already there. Nothing has been
measured either way, and repetition is not the way to measure it - the way
is to timestamp the send and the guest's receipt and find which side loses
it.

What has been ruled out, so nobody repeats it:

- **Not the arrow keys themselves.** They were genuinely broken - see below
  - and that is fixed and confirmed by hand. The phase failed before and
  after, so it is something else.
- **Not latency.** Twenty-five seconds of waiting does not help.
- **Not the shared monitor socket**, though that was worth fixing: `sendkey`
  and `screendump` share one connection and neither read their replies, so
  a backlog built up. Draining it made no difference to the failure rate.

What has not been ruled out: something in the preceding phases leaving the
console or the desktop in a state this one does not expect. Every phase runs
against the same boot.

**`make test` (109 tests) is unaffected and passes every run.** The display
harness should be treated as informative rather than as a gate until this is
understood, and the honest way to understand it is probably to make each
phase boot its own guest.

**(fixed) The arrow keys did nothing on a real keyboard.** This was written
up here as an intermittent test, which it was not: it was a real bug, and
the person using the system found it in ten seconds by pressing Down.

An arrow over a serial line is three bytes - escape, '[', a letter - because
that is what a terminal sends. On a keyboard it is a single keycode with no
character at all, and the driver's keymap has one byte per code, so
`keymap_plain[108]` was zero and the key produced nothing. Arrows worked
over the cable and did nothing in the window.

It hid because **every automated check typed over the serial line**, which
is the one path that already worked. The driver now turns those keys into
the sequence a terminal would have sent, so everything above it sees one
input language rather than two, and the widget check presses real keys
through QEMU's input plumbing.

The lesson is the one this project keeps relearning: a test that exercises a
different path from the user is a test that agrees with you.

**The display harness is flaky.** Two different phases have failed on two
consecutive full runs and both pass on their own. It is the harness, not the
system: applications now block for up to a second between events, so how
long a keystroke takes to show up depends on where in that second it landed,
and several phases still sleep a fixed time and then look. `check_widgets`
was already converted to wait-for-the-result and the rest have not been.

**Nothing should be pushed until that is fixed.** A suite that fails
differently each run tells you nothing, and the first thing it will hide is
a real regression.

**A file cannot be more than about a megabyte.** `fs.read` accumulates into
a Lua string and a process's heap is 2 MB by design (`design.md` 5.2, to
keep collections short). Reading a 936 KB PNG through `/share` gave
`error: not enough memory`. This is what killed the fw_cfg experiment and it
is the constraint the real filesystem has to answer - probably by mapping
pages rather than by returning strings.

- **Three apps.** `procs`, BeOS's ProcessController - every process with a
  bar beside it, busiest first. `about`, the About box, with the machine
  down the left and what the system is down the right. `sysmon`, the
  meters. All in `/bin`, all in the Deskbar.
- **A wrapping text view** in the kit, with a few styles and scrolling.
  Written for the About box; it is also most of what a markdown viewer
  needs.
- **The Deskbar sizes itself** to the number of applications. It was two
  fixed lists of seven rows, chosen when there were five.
- **A runner is named after what it runs.** They were all called "run", so
  `ps` and the process app showed a column of identical names.

- **A process can be ended from outside.** `SYS_KILL`, for a parent, which
  is the authority `wait` already implies. It marks and unblocks; the
  process dies at its own next entry into the kernel, which is at most one
  timer period away even for a program that has stopped making syscalls.
  Windows have a BeOS close box: the application is asked, and ended a
  second later if it never listened.
- **A Deskbar**, a graphical monitor, a version stamp on the desktop.
- **Applications block instead of polling.** The window manager parks a
  `poll` until there is an event or the caller's deadline - the same parked
  reply a live query uses.

**An idle desktop now idles: one per cent, down from ninety-six.** The
virtio input devices raise interrupts, `SYS_WAIT_INPUT` lets the one process
allowed to read input sleep until there is some, and the interrupt cuts that
sleep short - so a key is still noticed at interrupt speed. Applications
were already blocking, because the compositor parks their event polls.
There is a display check that reads the meter.

- **The Deskbar.** `wm` on its own starts a desktop with a panel top right:
  every window on screen, and every program that declared itself an
  application. Click one to start it; click a running one to raise it.
  It asks the window manager what is on screen rather than asking `/app`
  what registered, because a program that opens a window directly has one
  and no registration.
- **The cursor is composited** rather than drawn on the screen after the
  fact, and a window's drawing is damaged only once it is complete. Both
  were flicker: the first showed as the cursor blinking on every click, the
  second as a window seen half-redrawn.
- **`make FB=1920x1080` now rebuilds.** It did not: make compares
  timestamps and knows nothing about the command line, so it said "Nothing
  to be done" and ran the old image at the old size.

- **Clicks reach the widgets.** A press on a title bar is the window
  manager's; a press anywhere else is forwarded to the application in that
  window's coordinates, and the kit routes it to the view under it. Click to
  focus, click a list row, tick a checkbox, put the caret in a field. A
  button fires on the release and only if the pointer is still on it.

- **Graphical mode.** A process that owns the screen takes it from the
  kernel console with `sys.screen_take(true)`, and until it gives it back
  the console writes to the serial line only. `wm` and `edit` both do it.
  Nothing is silenced - the cable has everything, which is where a machine
  running a window manager is debugged from - and a panic takes the screen
  back regardless of who holds it.

- **A mouse.** `hal/qemu-virt/input.c` drives two virtio-input devices - a
  keyboard and a tablet - told apart by asking each whether it has absolute
  axes, since both answer to the same device id. The window manager draws a
  cursor, raises a window on click, and drags one by its title bar. M6's
  definition of done is now literal rather than done with arrow keys.

- **Replicants** - M7's second definition of done, minus the dragging.
  `wm clock,adopt`: one application publishes a view as source, state and
  a `needs` list, and another, which has never heard of clocks, adopts it
  and runs it. Both tick, with different state. The replicant reports from
  *inside* its own environment what it could reach, which is the only
  honest place to ask.

- **The scripting architecture** - M7's third definition of done. An
  application registers with `/app` and answers for its own properties
  because it called `ui.window`, not because it has any scripting code:
  `apps`, `apps gallery`, and `setprop /app/gallery/title=...` renames a
  running window. The registry hands out capabilities and never forwards, so
  one slow application cannot hold everyone else's door.

- **The UI kit.** `lib/ui.lua`: a view tree with nested coordinates and real
  clipping, follow modes, and widgets - label, button, checkbox, field,
  list. `wm gallery` shows all of them. Drawing produces commands, never
  pixels, so a view's list can be resent without re-running its handler.
- **`/lib` and `use()`.** A library is a file in the process's namespace,
  loaded into the caller's own environment. No package path, no search, no
  global module table: a program that was not given /lib has none.
- **The window manager reserves one key**, Control-W, and it introduces a
  command. It used to take Tab and the arrows, which the gallery showed to
  be untenable in one screenshot.

- **`edit`, a screen editor.** The machine can write and run its own Lua
  without a rebuild. `edit /data/x.lua`, Control-S, Control-Q, then
  `run /data/x.lua`.

- **The scheduler was costing every yield a timer period.** The idle loop
  slept with runnable threads in the queue. A yield went from 10 ms to
  0.04 ms and an IPC round trip from 20 ms to 0.8 ms - about a thousandfold,
  system-wide, on something no benchmark could see. See `testing.md`.
- **The window manager.** `wm hello-win,stuck`: two applications, one hung,
  and the hung one's window still moves. That is M6's definition of done.
  Backbuffer, damage tracking, BeOS tabs, stacking, focus.
- **Attributes, an index over them, and live queries.** `attr`, `find`,
  `watch`. A watcher blocks in one call and the filesystem parks the reply
  until the answer changes - no timer, no repeated question. `qbench` shows
  the query cost flat against sixteen times the nodes.
- **Control-C**, and the non-blocking receive a single-threaded server needed
  in order to answer it.

- **`/bin` is a real program directory.** `htop`, `cat`, `ls`, `monitor`,
  `hello`, `benchmark`, `spin`. Typing a name that is not already a Lua name
  runs the program; a trailing `&` detaches it.
- **`monitor` redraws on its own clock**, once a second, from a detached
  process. Every earlier version did not, and none of them could be told
  apart over serial - so there is now a display check for it.
- **The Lua is checked at build time.** `tools/luacheck.c` parses every file
  and `tools/luaglobals.py` compares the globals each one reads against the
  environment it will run in. That second one exists because a lost `local`
  has killed a server four separate times, and it catches exactly that.
- **`docs/architecture.md`**, the layer diagram and one command traced from
  the keypress to the pixels.

## Working

`make qemu`, `make test`, `make bench`, `make bench-record`, `make debug`, `make disasm`, `make size`, `make clean`.

109 tests, five benchmarks, and 56 display checks. A 340 KB image, of which 232 KB is the userland carried inside it and 20 KB is the kernel's own machine code. Plus 3.2 MB of framebuffer, which is `.bss`-like and costs the file nothing.

`make qemu` opens a window and keeps the shell on the terminal. `make serial` is the old serial-only behaviour, for when there is no screen to open.

**The prompt is a process.** What you type is read by the console server, sent to the shell over IPC, evaluated in the shell's own `lua_State`, and printed back the same way.

```
Kosmos shell. A process, talking to servers.
Try: fs.list("/data")   fs.read("/data/sensor")   2+2

kosmos> 2+2
4
kosmos> fs.write("/data/sensor", { celsius = 47.2, unit = "C" })
true	nil
kosmos> fs.read("/data/sensor").celsius
47.2
kosmos> fs.list("/data")[1]
sensor
kosmos> sys.write("direct")
-102
kosmos> fs.read("/nowhere")
nil	no such path: /nowhere
```

`sys.write` returning −102 is the point: the shell **cannot** print directly. Only the console server holds the serial port, and everything else has to ask it.

## M6 — where it stands

- [x] A framebuffer under QEMU, via ramfb: 1024x768, XRGB8888
- [x] `hal_fb_init` in the HAL, and a boot splash that proves the display at every boot
- [x] `make screenshot`: boot, screendump through QEMU's monitor, check the picture
- [x] `gfx.surface` as a userdata over flat bytes, and the C primitive set
- [x] Explicit `free`, a `__gc` net, and telling the GC the real size
- [x] The screen reachable from a process, as a surface
- [x] A backbuffer and damage tracking
- [x] An 8x16 bitmap font
- [x] A narrated boot with a progress bar, on the screen and on the serial line
- [x] The shell visible on the screen, and `help` at the prompt
- [x] The app server in Lua: windows, decoration, stacking, focus
- [x] Input beyond the serial line — **virtio-input over virtio-mmio.** `virt` has 32 virtio-mmio transports at 0xa000000, stride 0x200, SPI 16 upward, and `virtio-keyboard-device` attaches to that bus. mmio rather than PCI is the whole point: no ECAM walk and no capability parsing, so it is a few fixed registers and one virtqueue — and the same transport then gives `virtio-gpu-device`, which is where real dirty-rectangle flush and vblank come from. The keyboard pays for the GPU.
- [x] **Definition of done: drag a window with a hung app inside it, and have the window keep moving smoothly** — `wm hello-win,stuck`

**ramfb, not virtio-gpu, and the order is deliberate.** `hal_fb_init` is "ask the firmware for a linear framebuffer, and let it say where the pixels are", which is exactly what QEMU's ramfb and the Pi's mailbox both do. virtio-gpu is the odd one out — it needs an explicit `RESOURCE_FLUSH` after drawing — so it is the target that will earn the interface a `hal_fb_flush`, with two implementations in front of it rather than one. That is `hal.md`'s own argument applied to the display. It also cost about a hundred lines against the eight hundred that PCI enumeration plus virtqueues plus the virtio-gpu command set would have cost before a single pixel appeared, and everything above the HAL is identical either way.

**What ramfb does not give:** dirty rectangles and a vblank. QEMU rescans the whole buffer on its own schedule, so damage tracking in the compositor still saves the drawing but cannot save the scanout. Under emulation neither is the bottleneck. virtio-gpu is where both come back.

**The stride is padded on purpose: 4160 bytes, not 4096.** ramfb lets the guest choose, so it could be the tidy value, and that is the reason not to. A framebuffer whose pitch equals `width * 4` lets every address calculation in the system be written wrong and still work — for months, until the first real board, where the firmware picks whatever alignment it likes and every one of them shears at once. Two tests assert the padding, so that removing it as an oddity fails loudly.

**Two halves of the display are tested, and neither can prove the other.** `make test` proves what the kernel wrote into its own memory: that the framebuffer exists, is page aligned, is writable to the last row, and that the padded stride really moves rows. It cannot prove a pixel ever reached a screen — a wrong fourcc, a wrong stride in the ramfb config or a wrong address would leave all six passing and the display black. `make screenshot` asks QEMU instead, through the monitor, on the far side of everything this kernel controls. Both were made to fail on purpose before being trusted.

**Lua draws, and no line of it computes a pixel offset.** `gfx.surface{w=,h=}` is a userdata over flat bytes with `fill`, `span`, `blit`, `blend`, `get` and `set`; every pixel loop is in `user/lib/gfx.c` and every primitive clips rather than raising, because a window half off the edge of the screen is the normal case. `gfx.screen()` is the framebuffer as a surface, for the one process that was handed it.

**The boot log says what each stage is *for*, in one sentence.** Twelve stages: a line saying why the stage exists, then the facts it found. A log that prints "physical memory" and a number teaches nothing to somebody who does not already know why an operating system needs a page allocator before it can build a page table.

The first version of this used four to six lines a stage and was worse, not better — it filled the screen twice over and read like a lecture. One sentence and the numbers is the shape: it fits in 44 lines, which is one under what the screen holds. The longer explanations live in the source, which is where somebody who wants them is already looking.

**Backspace drew a box on the screen and worked perfectly over serial.** The console server's line editing was right all along — it sends `\b \b`, which serial terminals have understood since teletypes. The kernel's screen sink had never been told: `\b` is not printable, so it fell through to the glyph blitter, landed outside `0x20..0x7e`, and came out as the font's "no such glyph" box. Every correction while typing left a row of them. The erasing is still the space's job, not the backspace's; doing it in both places would delete the character before the one being deleted.

**The processor identifies itself**, in `arch/aarch64/cpu.c` — which is the most literal possible reading of what `arch/` is for. MIDR, the cache geometry, the physical address range and the ISA feature bits, all out of registers the architecture requires every AArch64 core to implement, so it needs no board knowledge and works on the first boot of new hardware. Part numbers from `arch/arm64/include/asm/cputype.h`, field positions from `arch/arm64/tools/sysreg` — Linux's machine-readable register description, not memory. **The raw registers are printed beside the decode**, everywhere, because a table of part numbers goes stale the moment a part ships that is not in it and a reader who can see `MIDR_EL1` can look it up.

**`/dev` is a server**, reached through the namespace over the same `list`/`read` protocol the filesystem answers. `fs.read("/dev/cpu")` is not a special case anywhere; it is an ordinary request sent somewhere else. `SYS_SYSINFO` hands back **raw** ID registers and pool counts and decodes nothing — the tables that turn `0x410fd083` into "Cortex-A72" live in Lua, so a processor the kernel has never heard of is described properly without the kernel changing.

**The device server does not list `console`, and that is the point.** The machine has one, but `/dev/console` is mounted to the console *server* — longest prefix wins — so a read of that path means "give me a line of input". The first version listed it anyway; the `devices` command dutifully read every name it was given, and the console server answered by swallowing the next thing typed at the prompt. Listing a name you do not answer for is a lie, and that is what it costs.

**A command can be a Lua program.** `alias` points one word at another; `def` compiles a line of Lua and gives it a name, argument string arriving as `...`. It is compiled at definition time, into the same environment as the prompt, so a syntax error is reported when you write it and it reaches exactly what you reach. That is the shape the idea deserved: an alias that is only a second name for a command is a convenience, and a command that is a program is a way to extend the system from inside it.

**Programs launch programs, and the shell has stopped being where they live.**

    kosmos> ls /bin
      benchmark.lua  cat.lua  hello.lua  htop.lua  spin.lua

`benchmark` and `cat` were shell commands and are programs now. `benchmark` launches `spin` — a program in /bin starting another program in /bin, with no special case anywhere: `run` is a function the runner hands its program, and it can pass on no more capabilities than it holds itself.

**`detach` is the difference between `run` and `benchmark`.** Detached, the child answers as soon as it has been told what to run and gets on with it, so "start four of these" does not mean "run four of these one at a time". Undetached, the answer comes when the program is finished, which is what a command line wants.

**The endpoint leak is closed.** `SYS_ENDPOINT_DESTROY` exposes what the kernel could already do and nothing could ask for: every program run consumed one of ninety-six for ever. No permission check beyond the capability itself — the index resolves against the caller's own table, so a process can only destroy one it was given.

**The working directory travels with a program.** It is the shell's idea and no server knows about it, so it goes in the request rather than being asked for. `cd /data` then `cat notes` works, and `cat` is a program that has never heard of the shell.

**A slice-based edit silently deleted `run_program` for the third time.** The first cost nine test functions, the second cost `tests_run()` and looked like a hang, and this one left the shell calling a function that no longer existed — which killed the shell, silently, because the shell prints by asking the console server. It is written down here three times now; the rule is narrow anchors, and I keep not following it.

That failure did surface a real bug worth keeping: the bare-word program path was not wrapped in `pcall` the way the command path is, so a program that failed to *start* took the shell down. A program that fails while *running* was always isolated — it is its own process — but starting one happens in the shell.

**There is a `/bin`, and programs run in processes of their own.**

    kosmos> ls /bin
      hello.lua        704 bytes
      htop.lua        3890 bytes
    kosmos> htop

`htop` is a Lua program in `user/bin/`, carried in the image because there is no disk until M8, served by a read-only `/bin` server, and run by a `runner` process that gets the capabilities the shell chose to hand it. It shows itself in its own process table.

**This is what `exec` looks like with no ambient authority.** No path search, no inherited environment, no global tree: a program reaches exactly what it was given. A bare word runs a program only if it does not already name something in Lua — the same rule the command dispatcher uses — so nothing installed in `/bin` can shadow the language.

**The shell sends a name, not the source.** The first version sent the program in the message and could not: a program is several kilobytes and `MSG_BYTES` is 2048. Sending the name is better than making it fit — the shell no longer reads a program in order to start one, and the bytes cross the boundary once instead of twice.

**Reads can now span messages.** `MSG_BYTES` stayed at 2048 rather than growing, because `struct thread` embeds a message — every thread would pay — and `sys_call` keeps one on a 16 KB exception stack. A server holding something large answers with `more = true` and honours `offset`; one that does not ignores the field, as every existing server does.

**And `serve` no longer dies when a reply will not fit.** `sys.reply` raises on a value that does not serialise, and that call is *outside* the coroutine that isolates a handler — so the first time `/bin` was asked for a program bigger than a message, the program store died and the client saw only that its request never came back. The failure now reaches whoever asked.

**The pools were raised, and doing it found a limit nothing could see.** Threads 16 to 48, processes 8 to 32, endpoints 32 to 96. Raising the first two changed nothing: spawning still failed at eleven processes, with every pool the system could report showing plenty free — 16 of 32 processes, 17 of 48 threads, 469 MB.

The real ceiling was **`ADDRSPACE_MAX` in `arch/aarch64/mmu.c`**, a third pool nothing counted and no report mentioned. A limit nothing counts is a limit nobody can find. It is 32 now, `as_count()`/`as_total()` exist, `SYS_SYSINFO` reports them and `ps` prints them; the same spawn loop reaches 27. And because `arch/` must not include a kernel header to learn `PROCESS_MAX`, the two are tied together by a test that creates that many address spaces and fails if any is refused.

Costs, measured: 134 KB of `.bss` for all three pools, up from 44 KB. A process is ~2.3 MB of RAM when it exists, dominated by its 2 MB heap — the same 2 MB that stops a full-screen surface fitting. Both get solved by the same change.

**Known gap found while testing this:** processes spawned from the shell are never reaped, because the shell never calls `sys.wait`. init reaps its own children; nobody reaps the shell's. They hold their slots as zombies until reboot.

**A blinking cursor**, driven from the timer tick — the one thing on the screen that has to change without anybody printing. Every path that writes a cell hides it first, so the block is never left sitting on top of a character somebody just printed, and it follows `cx`/`cy` rather than remembering where it was, so scrolling does not leave a second cursor behind.

**The namespace has a root now, and it is the one thing no server can answer.** `ls /` used to say "no such path" while `/data` and `/dev` both plainly existed — because nothing is mounted at `/`, and a path with no server behind it does not resolve. A server knows what it holds; only the namespace knows what has been *attached* to it and where, and that table lives in the process.

So `ns.list` returns whatever the server said **plus** whatever is mounted below the path, both being true: `/dev` holds `cpu` because the device server says so, and holds `console` because something else was attached there. A path with no server but with mounts under it — which is exactly what `/` is — is a directory made entirely of mount points.

Worth being clear about what this is not: there is a filesystem, and it is `servers`-in-memory. The ramfs at `/data` is a real server answering the real protocol; what M8 adds is persistence, attributes and queries, not the idea.

**`ls`, `cd`, `pwd`, `cat`, and a working directory that lives in the shell.** Not in the kernel and not in a server: a server is always told a whole path and knows nothing about where anybody thinks they are, which is what keeps `fs.read` the same operation for every caller. And "is this a directory" is answered by asking whether whoever serves it will list it — the only definition that means anything across three different servers.

**A leading slash always means a command**, and that came out of a question worth recording: aliases and Lua names can collide. `/ps` is unambiguous; a bare `ps` is treated as a command only when it does not also name something in Lua. So `type` gives you Lua's function and `/type` runs the alias you gave that name. Refusing to guess is the point — a shell where `type` sometimes means a command and sometimes means the function is a shell you cannot write anything in.

**CPU usage is the difference between two readings, never one.** The kernel charges every timer tick to the idle thread or to everything else, and both counters only rise. A single reading says what fraction of *all time since boot* was busy, which after a minute at a prompt is a number that never moves again. `ps` says so on the first call rather than printing a meaningless 0%.

Sampling at the tick rather than accumulating real time per thread is deliberate: accumulating would mean reading the counter twice on every context switch — a cost on the hottest path in the kernel to answer a question nobody asks more than once a second.

**The shell takes commands as well as Lua**, with aliases. A line is a command when its first word names one *and the rest has no Lua punctuation in it* — so `help` and `help gfx` are commands while `help("gfx")` stays an expression. Both work, which matters because the parentheses are simultaneously what people forget and what they reach for.

**A status bar, in the rows the kernel console reserves.** Two writers on one framebuffer with no compositor, which is only honest because the regions cannot overlap by construction — and is exactly the arrangement a compositor exists to stop needing.

**There is a keyboard.** virtio-input over virtio-mmio, in `hal/qemu-virt/keyboard.c`. mmio rather than pci is what makes it three hundred lines instead of nine hundred: `virt` has 32 fixed windows at 0xa000000, stride 0x200, and the whole of discovery is reading two registers thirty-two times. No PCI bus, no ECAM walk, no capability list.

**It is not a new HAL call.** A keyboard is a source of characters and `hal_getchar` is where characters come from, so the board answers from whichever of its sources has one. The console server, the shell and every process reading a line are unchanged by the keyboard existing — which is the property that says the HAL boundary was drawn in the right place.

**QEMU's virtio-mmio defaults to the legacy interface**, and this cost a debugging round. The device was found in slot 31 with the right magic and the right device id, reporting version 1 — the legacy layout, reached through `QUEUE_PFN` and `GUEST_PAGE_SIZE`, which modern structures read as garbage. The driver refused it, correctly, and the boot said "none". `-global virtio-mmio.force-legacy=false` is what Linux passes too, and it is now on every QEMU line here including both test runners. Worth knowing: the failure looked like "the device is not there" and was actually "the device is speaking the other dialect".

**Polled, not interrupt-driven**, like the UART. The console server already yields between polls, so a key in the used ring is found on the same schedule a character in the UART is. `roadmap.md`'s input-on-a-highest-priority-thread is when the interrupt starts to matter.

**And the next device is nearly free.** Everything above the last two functions in that file is the virtio transport, and `virtio-gpu-device` is the same transport with a different id — which is where a real dirty-rectangle flush and a vblank come from. It is not split into its own file yet, because there is one device and splitting it now would be inventing an interface against a single caller.

**The boot narrates itself, on the screen and on the wire.** Ten numbered stages, each with the facts that make it worth watching, and a progress bar in rows the text never scrolls through. `CLAUDE.md` says the kernel has no graphics, and it still does not have a graphics *subsystem* — what `console.c` gained is forty lines that put a glyph in a framebuffer, and the reason is `panic()`: it writes through the same console, so a panic now reaches a screen. On a board with no serial cable, a panic that prints into the void and a machine that does not work are the same thing.

`BOOT_STAGES` is a constant and the `boot_stage` calls are scattered through `kmain`, so there is a test that they still agree — a bar that stops at four fifths reads as "something hung" rather than as "somebody added a stage", and the screenshot check catches the same drift from outside.

**Keystrokes typed during boot are still lost.** Nothing polls either input source until the console server starts, and there is no input buffer. The keyboard did not change this — its ring holds sixteen events and the boot produces none, but nothing reads them until userland is up. Harmless with a person at the keyboard; it cost twenty minutes when a test harness typed too early.

**The pre-display boot log is replayed onto the screen.** The display cannot be the first thing up — it needs the MMU on first, or the framebuffer is Device memory and clearing three megabytes of it is the 10-50× penalty `gfx.md` §19.5 warns about. So the first stages happen before there is anywhere to draw them. An earlier comment argued against buffering them on the grounds that such a buffer could overflow during a panic; that was simply wrong, since nothing writes to it once the screen is attached. Two kilobytes, written before there is a second thread, replayed once.

**`help` at the prompt**, with `help("fs")`, `help("gfx")`, `help("sys")` and `help("demos")`. A table with `__tostring` and `__call`, so the bare word works as well as the call — the parentheses are the thing every newcomer forgets.

**init says which child died, by name.** It used to record the exit code into a local and drop it, on the reasoning that the console server might be the thing that just died. True, and still no reason to say nothing: a process that dies takes its own error message with it, because it prints by asking the console server and a dead process asks nothing. init is the only one left who knows.

**A slice-based rewrite of `kmain` silently deleted `tests_run()` and `bench_run()`.** The symptom was not a failure. It was `make test` appearing to hang — the image booted perfectly into a shell while the host runner sat waiting for a TAP plan that was never coming, which reads as "the boot got slow" and sent me looking at the console's scrolling for half an hour. **Second time an edit by slice has quietly dropped lines from the middle of something**; the first cost nine test functions. Narrow anchors, always.

**There is text.** Spleen 8x16, BSD-2-Clause, vendored unmodified under `assets/fonts/` beside its licence and converted by `tools/bdf2c.py` into 96 glyphs of sixteen bytes — one byte per row, MSB leftmost, which is the VGA ROM layout and is what makes drawing a glyph a shift and a test rather than a lookup. `s:text(x, y, string, colour [, background])` returns the next x, so laying out a line needs no arithmetic about pixels in Lua.

Two choices worth recording. **Linux's `font_8x16.c` was rejected**: it is the obvious thing to reach for, it is the same VGA font, and it is GPL — it would have made the kernel image GPL. And **the generated file prints each byte beside the row it draws**, because a shifted bit, a reversed row order or an off-by-one in the range are all obvious read pairwise and all subtle on a screen. Five of those were introduced deliberately and all five failed the tests.

The tests compare rendered pixels against patterns written out by hand rather than against the array that drew them — comparing the output to its own input would pass just as happily with the bits reversed.

**A pitch bug is invisible from inside the process, and that is the most useful thing found this session.** If `row_of` steps by `width * 4` instead of by the pitch, every read agrees with every write — the surface just has an unused gap at the end of each row — and *the whole suite passes, 103 of 103*. It was tried, not reasoned about. The only observer who disagrees is on the far side of the framebuffer, where the stride is 4160 and a row written 4096 bytes along lands sixteen pixels left.

So the check for the rule this whole module exists to enforce cannot live in the guest. `make screenshot` grew a second phase: it waits for the shell prompt, types a `gfx.screen()` drawing of vertical bars, screendumps, and requires them to still be vertical. With the bug in place it reports the bar "found at x=184..189" instead of 200 — the drift, exactly.

**Surfaces come from the process heap, so a full-screen one does not fit.** 1024x768 is 3.2 MB against a 2 MB heap, and `gfx.surface` says so rather than failing obscurely. That is a real limit and it has to be solved inside M6, not at M7: the app server's backbuffer is full-screen by definition. It needs pages from the kernel rather than from the Lua heap, which is the same mechanism M7's shared surfaces want.

**The `serialize` benchmark moved twice this session, and neither time was the serialiser.** +2.2% when `gfx` started being opened in every process, and +0.9% again when the font array joined it — 1364.9 to 1412.5, with `serialize.c` untouched throughout. Every process now opens the `gfx` library, and the library table, the surface metatable and its methods are more objects on a 2 MB heap, so the collector paces differently through the measured loop. Measured rather than guessed: the same tree with the `luaL_requiref` for `gfx` commented out gives 1358.2, back inside the old range.

Worth knowing as a property of the metric — **`serialize` is sensitive to what else is in the process**, not only to the serialiser, and it drifts every time anything joins the user image. If it moves and nothing in `serialize.c` or `sys_user.c` did, look at what was added. Making it independent of its process — a fixed GC pause setting and a forced collection before the loop — is a known improvement and is not done; it is written down here rather than left to be rediscovered the third time this happens. `gc_pause_max` shifted by the same cause and only +0.09%, because a long collector step is dominated by the 3000-object heap the benchmark builds rather than by a handful of library tables.

**init says why it could not start something.** Its spawns used to be `if not x then sys.exit(1) end`, and the system died at boot in total silence — kernel output looking perfectly healthy, then nothing. That is exactly what happened the first time `SPAWN_SCREEN` was refused, and it cost a debugging cycle to find. init holds the console precisely so it can speak, and the moment it most needs to is when it cannot build the system.

**The framebuffer is in its own linker section, after the stacks.** Three megabytes in `.bss` would sit before them and push both guard pages past the first 2 MB of RAM — the only part mapped a page at a time — and a guard page inside a 2 MB block cannot be punched out, so `mmu_init` would panic. `NOLOAD`, so the image file carries none of it; inside `__image_end`, so the page allocator counts the pages as the kernel's and never hands them out. There is a test for that last part, because the symptom otherwise is garbage on screen rather than anything that looks like an allocator bug.

## M5 — where it stands

- [x] The protocol: `list`, `read`, `write`, `getattr`, `setattr`, over typed records
- [x] Per-process namespaces: a mount table in the process, so what is not mounted does not exist
- [x] Lua coroutines as the servers' concurrency layer
- [x] Console server, owning the device
- [x] ramfs
- [x] Capability transfer over IPC, so userland can do its own mounting
- [x] The Lua shell as a REPL against the system
- [x] **Definition of done, both halves: the same server mounted at two paths in two processes, each seeing only its own; and the console server's code replaced while the shell was talking to it, without the shell noticing**
- [x] Hot reload level 1: `load()` of new code while preserving state and clients
- [x] Init and supervision in userland, on a spawn syscall
- [x] Removing Lua from the kernel

**The kernel starts init and nothing else.** init spawns the console server, the ramfs and the shell, passing on the capabilities it was given, and then waits. It cannot promote a child beyond itself: a spawn resolves every capability against the parent's own table, and passing on the console is refused unless the parent holds it.

**Supervision is noticing, not restarting.** init waits and learns which child ended with what code. Restarting one is hot reload level 2, and `design.md` §10 deliberately leaves that until there is state worth recovering — the server would come back empty, and deciding where its state should have lived is the actual design question.

**There is no Lua in the kernel.** Not in the boot path, not in the image, not in the source list. `CLAUDE.md` has said since the start that the kernel has none from M4 onward, and until this session that was simply untrue: the interpreter was 163,648 bytes of `.text` against 28,556 for the entire rest of the kernel, reachable from nothing, kept alive only because fifty test assertions drove the kernel through it.

Those assertions live in `user/tests/luatest.lua` now, one role per test, and `tests/tests.c` starts a process and turns its exit code into a TAP line — so the plan, the numbering and the names all stayed where they were. The two benchmarks that opened a `lua_State` moved the same way, into `user/tests/luabench.lua`.

What left with it: `malloc`, `math`, `snprintf`, `strtod`, `stdio` and `-lm`, every one of which was in the kernel for Lua and for nothing else. And the 2 MB heap `kmain` allocated for a `lua_State` it opened itself.

**Kernel machine code went from 204,800 bytes of `.text` to 20,480.** The whole image is 348,168 bytes against 569,364, and 237,579 of what is left is the userland image carried inside it — payload, not kernel.

Two fixture blobs went with it. `user/hello.S` and `user/faulty.S` are what the tests run at EL0 to check that a process exits and that a null dereference kills only it; nothing outside the suite referred to them, and they were 4 KB of the shipping image that no code path could reach.

**What stays despite being unreachable in the shipping image:** `fault_expect_begin`/`_end` in `arch/aarch64/trap.c`, and `setjmp.S` under it. Only the tests and the benchmarks call them. Compiling them out would mean the trap handler that ships is not the trap handler that was tested, and that is a worse trade than a couple of hundred bytes and one predictable branch on a path that is already an exception.

**Hot reload works because state and behaviour are separate things.** A server is a `state` table plus a factory that turns it into handlers, and `serve` takes the factory rather than the handlers. Anything captured in a closure built at startup is lost on reload; anything in `state` survives, because the new handlers are handed the same table. That is the whole mechanism, and getting it wrong is silent: the server keeps working and quietly forgets.

**A reload that does not compile is refused with the old code still serving.** Load, run, and install, in that order, each checked. A server that half-reloads is worse than one that refuses.

**Device access is a boolean, not a capability.** `process_grant_console` sets a flag, and the flag is checked by `sys.write` and `sys.getchar`. It enforces the property that matters — exactly one process owns the console and everything else asks it — but a device should be named the way everything else is, by a capability the process holds. That needs a capability that names a device rather than an endpoint.

## M4 — where it stands

- [x] One `lua_State` per process, with a bounded heap (2 MB, mapped by the kernel and not growable)
- [x] The process running at EL0, in its own address space
- [x] Syscall bindings validating capabilities, and every pointer checked before it is touched
- [x] The Lua table serialiser for IPC
- [x] Deciding which Lua libraries exist inside a process
- [x] Loading Lua code from an image embedded in the kernel
- [x] **Definition of done: two processes at EL0, separate address spaces, exchanging a Lua table. And `*(nil)` kills only the process.**
- [x] Removing Lua from the kernel — done at M5, see above
- [ ] Benchmarks: allocating and freeing a table; a syscall from Lua versus the same one from C

**Two things are deliberately temporary, and both are recorded where they are written:**

*The kernel is in TTBR0, alongside every process.* `state.md` previously said M4 would move it to TTBR1. It does not, because that is a large refactor of boot, the linker script and every kernel pointer, and it is not what buys isolation. Isolation is the AP bits: every kernel mapping is `AP=00`, EL1 read/write and no EL0 access, with PXN and UXN set. A process faulting on the kernel image gets a **permission** fault, not a translation fault — the page is in its own tables and still untouchable. TTBR1 becomes worth doing when a process needs the whole low half.

*The reply token is a raw kernel pointer.* `sys.receive` hands EL0 the address of a `struct thread`. It is safe only because `ipc_reply` checks the target is really waiting, and a value that is safe only because of what the callee checks is one audit away from not being safe. At M5 it becomes a capability index like everything else.

## M3 — done

- [x] Threads with their context, and a context switch in assembly
- [x] Round-robin scheduler with a per-CPU runqueue, behind a pluggable `struct scheduler`
- [x] Synchronous IPC: rendezvous, call, receive, reply
- [x] A per-thread capability table, indexed, with generation numbers
- [x] A separate exception stack, so a stack overflow is readable rather than a double fault
- [x] **Definition of done: 100,000 round trips, cost per round trip printed and baselined**
- [x] Address spaces: create, destroy, map pages
- [x] Syscalls exposed as Lua functions, as `sys`
- [x] Preemption, in the vector's epilogue

**One thing is deliberately temporary.** Every address space contains the kernel, because there is no TTBR1 split yet: the kernel is identity mapped through TTBR0 like everything else, so a space without it would fault on the instruction after the switch. A new space copies the kernel's top level and shares the tables below it, which is why user mappings are confined to virtual addresses at or above 2 GB and anything lower is refused rather than allowed to quietly edit the kernel's own map.

At M4 that is replaced by the real arrangement: the kernel in TTBR1 at the top, TTBR0 belonging entirely to the process, and a space containing no kernel at all.

## Decisions taken while implementing (this session)

**2026-08-31 — The disk goes to one process, and everything else asks it.** Raw sectors are every file on the machine whatever any namespace says, which makes `SPAWN_DISK` a stronger grant than the screen - the screen can only draw. So the kernel hands it to init, init hands it to exactly one child, and `mkfs` and `diskinfo` are ordinary programs that reach `/disk/super` and `/disk/format` through their namespace and cannot touch a sector.

Doing it the other way was tried first and refused itself: granting the runner `SPAWN_DISK` would have given it to every program, which is ambient authority with extra steps. The version that works is the shape `/dev`, `/bin` and `/lib` already have.

Block *contents* deliberately do not cross that boundary. A message is 2048 bytes and a block is 4096.

**2026-08-31 — The format is borrowed, not invented.** ext2's skeleton without its block groups, which exist to keep inodes near data on a spinning disk and buy nothing on an SD card. ext3's journal, whose space is reserved from the start so turning it on does not move the data blocks. BFS's attribute semantics. Extents instead of indirect blocks. Written with `string.pack`, so the format string beside the field names is the specification - and bounds-checked, which a C struct read off a disk is not. A corrupt superblock is exactly the hostile input a filesystem has to survive, and `<I4I4I8` also cannot acquire padding on the way to the Pi.

**2026-08-31 — The superblock is written last, and that is the ordering rather than a detail.** A format interrupted before it leaves a disk that says it is not a filesystem, which is true. Interrupted after, it would leave one that claims to be and is not.

**2026-08-31 — `readline` cannot be used to wait for this system's prompt.** `kosmos>` has no trailing newline, so `select` reports the pipe ready and the read then blocks for ever waiting for a line ending that only arrives when something else is printed. It cost an hour in a new harness, and `run_screenshot.py` had already solved it by reading raw bytes - which is worth knowing before writing the third one.

**2026-08-31 — A preempted thread needs the whole FP register file, and the switch is the place to save it.** `context_switch` saved `d8`-`d15`: the callee-saved set, and the correct answer for a thread that called it, because the compiler has already spilled the rest. A preempted thread called nothing. Two kernel threads holding a known value in `d0` across a spin longer than a quantum lost it on **14 preemptions out of 14**; after the fix, 0 out of 14.

Saved in the switch rather than on the exception path, because the kernel's own C is `-mgeneral-regs-only` and cannot touch an FP register - so by the time the switch runs the values are still exactly what the interrupted thread left, and a switch is far rarer than an exception. Full 128-bit `q` registers plus `fpcr`/`fpsr`: userland is not built `-mgeneral-regs-only`, so saving only what Lua's arithmetic obviously needs would be the same bug one layer down.

It costs `context_switch` 7.187 -> 9.812 and `ipc_roundtrip` 31.001 -> 36.251, both recorded rather than hidden. Lazy FP save is on the roadmap at M10 to get it back, with the caveat that it wins on switches between threads that do not touch FP and nearly every thread here runs Lua.

**The first version of that test passed with the bug.** Its spin was shorter than a 100 ms quantum, so it was never actually preempted. It now asserts the preemptions happened, and the same lesson as the arrow keys applies: a test that does not exercise the path is a test that agrees with you.

**2026-08-31 — Face winding is computed, not written by hand.** The first `g3d.cube` had four of its six faces wound the wrong way. On screen that is a cube showing *four* faces at once, which a cube cannot do - and the version where all six are backwards shows three, rotates, shades correctly, and is inside out. Neither is visible in a screenshot.

So `g3d.orient` derives it: for each triangle the right-handed normal either points toward the centre of the mesh or away, and those are exactly the two windings. The faces list only has to name the right corners. Exact for a convex mesh centred on the origin, which is stated where it is defined, because a teapot is neither.

The test that pins it is not the display check. A display check sees three faces whether the near ones or the far ones are drawn; the Lua test puts the cube in a known pose and asserts *which* face is in the middle of the picture, then turns it half a turn and asserts the opposite one is. Reverting the cull sign makes it report the far face's colour by name.

**2026-08-31 — A shared region is mapped in its own address window, because a range is what says who frees the pages.** Shared regions went into the window `SYS_MAP` hands out. A process frees everything still mapped in that window when it exits, on the grounds that it allocated it - so two processes mapping one region freed its pages twice, and the second one panicked the machine with `pmm_free_page: double free`. Opening `plasma` and pressing Control-C was enough.

The panic was the lucky half. The quiet half is that `SYS_UNMAP` would have let a process hand a region's pages back to the allocator while the other process was still drawing into them: a use-after-free with no symptom at all until the pages were handed to somebody else.

The fix is a second window, `USER_SHARE_VA`, and not a per-page owner flag. The two pieces of code that free pages by walking a range are now bounded by the window whose pages they own, and the check is where the addresses come from rather than a bit they carry. Both windows have a ceiling now as well: neither pointer reuses an address, so without one the `SYS_MAP` pointer would eventually walk into the shared window and bring the same bug back by a longer road.

Shared pages are also not charged against the mapper's budget. They are charged to whoever created the region; charging them again to everybody who maps it would mean a compositor and an application sharing one surface pay for it twice.

There is a permanent test, and the page count is checked from C rather than from inside either process - the number only becomes true once both have been reaped, and neither of them is around to look. It fails by panicking when the fix is reverted, which is how it was confirmed to test anything.

**2026-08-31 — A capability slot with a kind costs one tick per round trip, and that is the price of the design.** `ipc_roundtrip` went 30.000 to 31.001. A slot used to hold an endpoint and nothing else, so `resolve` went straight to the pointer; it holds either an endpoint or a region now, so every resolve loads the tag first and a round trip resolves twice. `context_switch` and `exception` did not move at all across the same change, which is what says the cost is in the lookup and not in the path around it. The alternative - a second table with a second index space - is the global-name design this kernel does not have. Baseline raised, with the reason in it.

`serialize` moved 1412.5 to 1450.6 in the same change, and its baseline note had already predicted the shape of it: the benchmark is sensitive to what else is in the process, and this added four bindings to every one of them.

## Concrete next step

**A PDF reader, then sound, then Doom.** Decided by hand rather than taken
from `roadmap.md`, which still schedules M9's 3D demo next and still says
audio is out of scope. The roadmap is a guide; where it disagrees with this
paragraph it is out of date rather than right.

**The PDF reader is more tractable than it looks, and that is why it is
first.** Three of its four hard parts are already in the tree: `puff` does
Flate, `stb_truetype` rasterises glyphs, and the blitter draws them. What is
left is the cross-reference table, the object model and the content-stream
interpreter — structure parsing over bytes that arrive from somewhere else,
which is the Lua side of the line `design.md` §6 draws. Plain documents
only: no forms, no encryption, no JavaScript.

Two things it will lean on that arrived in the last session. `read` takes
`into`, a capability to pages the caller owns, so a document larger than a
2 KB message crosses at all; and `kfs.read_range` is the `pread` underneath
it, so a page is read without the rest of the file. And `tools/kfs.lua`
means a real PDF can be put on a disk image from this Mac before any of it
is written — which is the right way to start, because a synthetic document
fails in none of the ways a real one does.

**Then sound**: a virtio-snd driver in the HAL, WAV first to prove the whole
path from device to speaker, then a vendored MP3 decoder as userland C
beside the font rasteriser — the same shape, and for the same reason. A
music player after it.

**Then Doom**, and then real hardware.

### Open, and not part of that

**The display harness is unreliable, and it is a gate on pushing.** About one
run in three fails at one of several phases, and the common sentence is that
an input the harness sent did not arrive — by the monitor socket or by the
serial line, so it is not one mechanism. `make test`'s 109 tests are
unaffected and pass every run. See **Known bad** above; the honest fix is
probably a guest per phase, and the honest measurement is a timestamp at each
end rather than more repetitions.

**M2 cannot be closed without a cable**, and its remaining half is the point
of the milestone: the HAL takes its real shape once there are two
implementations to compare, and `hal.md` is explicit that writing that
interface against one target produces the shape of QEMU wearing generic
names. Nothing is gained by guessing at it now. When a cable arrives:
`hal/pi1/` or `hal/pi5/`.

**Two benchmarks `roadmap.md` asked for at M4 are still not built:**
allocating and freeing a table, and the overhead of a syscall from Lua versus
the same one from C. The second is the number that says what the EL0 boundary
costs per crossing, and `sys.ticks()` makes it measurable from inside a
process.

**`list` still costs a round trip per entry** to ask for a size and a kind.
A `list` that answered with attributes would turn a listing of thirty files
from thirty-one messages into one. It is the protocol's shape, not `ls`'s
fault.

**The book is at chapter 2 of the outline in `book/OUTLINE.md`.** Chapters
are written after the thing they describe works, which is the rule that keeps
them describing what happened rather than what was intended.

## Decisions taken while implementing

Decisions that came out of writing code go here. Format: date, what was decided, why.

Design decisions (as opposed to implementation ones) go in the decision log in `README.md` and are propagated to `design.md` and `roadmap.md` in the same session.

**2026-08-30 — The kernel contains no Lua, and the tests for Lua run at EL0.**

`CLAUDE.md` had said since the first commit that the kernel has no Lua in it from M4 onward. It was false for two milestones: the interpreter was 163,648 bytes of `.text` against 28,556 for everything else in the kernel, reachable from no code path, kept alive by fifty test assertions that drove the kernel through it.

The tests were the whole of the reason, so the tests moved. `user/tests/luatest.lua` holds them, one role per test; `tests/tests.c` starts a process in a role and turns its exit code into a TAP line, which keeps the plan, the numbering and the names on the C side where they were. Same names, same count, one boundary further out.

What this bought is not speed — it was dead code, and the three benchmarks that touch no Lua are unchanged to three decimals. It bought the complexity budget back. `CLAUDE.md` allows 10k lines of kernel; the kernel's own source is 5,610, and Lua was another ~30,000 lines of C at EL1, where any bug in it is a kernel bug.

**Kernel machine code: `.text` 204,800 bytes to 20,480. Exactly ten times smaller.** Measured by building the previous commit in a worktree rather than remembered; an earlier note in this session said 192,204, which was the sum of symbol sizes out of the map and missed alignment.

**2026-08-30 — What left with Lua: `malloc`, `math`, `snprintf`, `strtod`, `stdio`, and `-lm`.** Every one of them was in the kernel for Lua and for nothing else — nothing in `kernel/`, `arch/` or `hal/` allocates or touches a float, and `-mgeneral-regs-only` has been turning the second into a compile error all along. Only `string.c` and `setjmp.S` are left, the latter because `trap.c` builds its fault-expectation mechanism on it. The test image links the rest back for its own unit tests of them, and brings up a 256 KB heap of its own to do it.

**2026-08-30 — A monotonic counter is a syscall; the wall clock stays a capability.** `SYS_TICKS` reads `CNTPCT_EL0`. `design.md` §4.4 makes `/dev/clock` something a process is handed or is not, and that does not change: what a program wants is a date, and a date comes from a server. But the server has to read the counter from somewhere, and a tick is the kind of thing that genuinely cannot be a message — it has to be sampled where the code being timed runs, or the sample measures the sampling.

It also retired a weakness that was written down and not fixed: a process could not read the counter at all, so Lua's string-hash seed at EL0 came off a stack address, and two processes from the same image start at the same address and so got the same seed.

**2026-08-30 — A benchmark harness that waits for a process must block, not spin.** The two Lua benchmarks are processes now, and the kernel side blocks in `ipc_receive` rather than yielding in a loop until the process exits. A spin would leave the harness thread runnable for the whole measurement, so every timer tick would switch into it and charge its work to the number being measured.

**And the reply has to be a value.** Replying with a zero-length message is not replying with nothing — it is replying with something that is not a serialisable Lua value, so the caller raises, the process dies, and the harness waits for ever on a sender that no longer exists. That is a hang rather than a failure, and it is what the first version of this did. The reply is now the request, echoed back.

**2026-08-30 — Recording a baseline no longer throws away its note.** `run_bench.py --record` carried `tol` across and rebuilt everything else, which silently dropped the `note` field — the part that says what a number means and why, and the part that is expensive to work out twice. Found while recording the baselines this change moved.

**2026-08-30 — Two of the five benchmarks changed what they measure, and the numbers are not comparable across the change.**

`context_switch`, `exception` and `ipc_roundtrip` are identical to three decimals, which is the evidence that the kernel itself did not change.

`gc_pause_max` moved 77,860 to 78,130, +0.3%. A collector step costs the same at EL0 as it did at EL1, which is the direct answer to whether the move cost performance: the interpreter's own work does not care what privilege level it runs at. Interrupts are now live inside the measured window, because a process cannot mask them — and for a pause metric that is more honest rather than less, since a pause the user feels includes the tick that landed in it.

`serialize` moved 1,037.6 to 1,364.9, +31%. Not a slowdown of the serialiser: `sys.pack` returns a Lua string where the C version wrote into a `struct message` and never touched the heap. Measured rather than assumed — an allocate-and-copy of a string that size through one Lua-to-C call costs 164 ticks against a 325-tick increase, so the allocation is about half of it and the rest is the second crossing and the copy back into a message on the way in. What it now measures is what a caller out here actually pays.

**2026-08-30 — The toolchain is the official ARM GNU 14.2.Rel1 `aarch64-none-elf`, unpacked under `~/toolchains`.** The Homebrew recipe `setup.md` used to recommend (`aarch64-unknown-linux-gnu`) targets Linux and brings glibc and Linux start files, which is what `-ffreestanding -nostdlib -nostartfiles` exists to avoid. It also produces differently named binaries. `setup.md` corrected in the same session. Homebrew is not installed on this machine and MacPorts has no `aarch64-elf-gcc` port, so the ARM tarball was the only path that lands on the documented binary names.

**2026-08-30 — A capability travels out of band, never inside the serialised bytes.** An index means something only in the table it came from, so the kernel translates it on delivery and what arrives is the receiver's own index. Stored as index plus one, so that zero — which is what `{0}` gives — means none.

**2026-08-30 — Anything that is "one per running thing" is stored on the running thing.** `current_process` and the address space were both global-shaped and both broke the moment there were two processes. The same shape appears again wherever a global is "enough for now".

**2026-08-30 — A thread is runnable the instant it exists, and preemption makes that instant real.** Anything created and then configured on the next line has already run. `thread_create_suspended` exists for that, and the race was found three separate times before the habit stuck: once in processes, once in `sys.spawn`, and once in tests.

**2026-08-30 — Isolation is the AP bits, not the address space layout.** Every kernel mapping is `AP=00` with PXN and UXN, so a process cannot touch kernel memory whether or not the kernel is mapped in its space. A process reading the kernel image gets a permission fault at level 3, not a translation fault, which is the difference stated as plainly as it can be.

**2026-08-30 — The address space follows the thread, and the process pointer lives on the thread.** Both were global-shaped and both broke the moment there were two processes: whichever ran last owned TTBR0 and owned `current_process`, so one process ran with another's memory underneath it and its syscalls checked pointers against the wrong address space. Anything that is "one per running thing" has to be stored on the running thing.

**2026-08-30 — A process is built before it is startable.** `process_create` leaves it suspended and `process_start` makes it runnable, because a runnable process runs: one created and granted its capabilities on the next line had already exited by then. Removing a race by construction beats masking interrupts around it.

**2026-08-30 — Copy the length, never the buffer.** A round trip moves a message five times, and copying all 512 bytes regardless of use made it thirty-six times slower. The benchmark caught it on the first run, which is the entire argument for having had one since M3.

**2026-08-30 — Preemption switches in the vector's epilogue, never in C.** A context switch moves `SP_EL1`, and everything after the switch reads the frame at `sp`, so that frame has to belong to the thread about to be resumed. Splitting the decision (`thread_tick`, in the handler, asking the policy) from the act (`thread_preempt_if_needed`, in the epilogue) is what lets the decision stay a C function the policy owns.

**A consequence that cost an instruction abort at an address that was never code:** `context_switch` had assumed `SPSel` was 0. True for a thread that yields, false for one arriving from the epilogue where taking the exception already set it to 1. `SPSel` is now part of the saved context.

**2026-08-30 — Every address space contains the kernel, and that is temporary.** There is no TTBR1 split, so a space without the kernel would fault on the instruction after the switch. A space copies the kernel's top level and shares the tables below, so user mappings are confined to 2 GB and above and anything lower is refused. M4 replaces this with the kernel in TTBR1.

**2026-08-30 — `sys` is a preview of the interface, not the interface.** At M4 these become real syscalls across a privilege boundary, and at M5 the inspection half disappears into `/proc`, read through the namespace protocol like every other resource. `design.md` §9.5 is emphatic that there must not be a second way to reach it, so `sys.threads` and `sys.memory` are scheduled for deletion rather than for extension.

**2026-08-30 — Every spawned Lua thread gets its own `lua_State`.** Not a design choice so much as the only thing that works: a `lua_State` is not reentrant, and two kernel threads inside one would corrupt it. `design.md` §2's share-nothing userland arrives early because the alternative does not run. They currently share one `malloc` heap; per-state heap limits are M4's problem.

**2026-08-30 — The scheduling policy is behind an interface, not wired into the thread code.** `thread.c` owns the mechanism and `struct scheduler` owns which runnable thread runs next, so a different algorithm is a new file. Per-thread policy state is embedded in `struct thread` rather than reached through a pointer, because there is no allocator to hand a policy its own storage; the fields are named for what algorithms need generally rather than for round robin. A test installs a deliberately terrible LIFO policy and asserts both exact orderings, which is the only proof the seam is real.

**2026-08-30 — Kernel threads run on SP_EL0 and take exceptions on SP_EL1.** Taking an exception always sets `SPSel`, so the hardware hands the handler a different stack with no code to switch it. That is what makes a stack overflow readable rather than a double fault, and it makes a double fault name itself: a fault in ordinary code lands in the first quarter of the vector table and one inside the handler lands in the second.

**A consequence that cost time:** `SP_EL1` cannot be named from EL1, because its system-register encoding is an EL2 one. `mrs x10, sp_el1` there is not a trap, it is an undefined instruction, and it arrives as EC 0x00 "unknown reason" explaining nothing. Reaching it means making it the current stack pointer briefly with `SPSel`, which in turn means the context switch has to mask interrupts.

**2026-08-30 — A thread is on exactly one IPC queue at a time.** All three of an endpoint's queues thread through the same link field, so a thread on two of them silently truncates one. Written as a warning in a comment and then done anyway; the symptom appeared three tests away from the cause.

**2026-08-30 — Dead threads release their slot, and their stacks go with it.** The stacks are already allocated with their guard pages already unmapped, which is the state a new thread wants. A recycled slot has its capability table cleared: inheriting the dead thread's capabilities would let a new thread reach endpoints it was never handed, and everything would appear to work.

**2026-08-30 — FP and SIMD are enabled at EL1, and the kernel's own C still is not allowed to use them.** `CPACR_EL1.FPEN` resets to trapping every FP access at EL0 and EL1 alike. Two things need them: `setjmp`/`longjmp` save `d8`–`d15` because AAPCS64 makes them callee-saved, and Lua's numbers are doubles. Neither is optional, so the trap has to go. `-mgeneral-regs-only` stays on every C file, so the kernel still cannot emit an FP instruction by accident; the flag restricts code generation, not what hand-written assembly may save.

Found the hard way: the first `setjmp` panicked with EC 0x07, whose name ("unhandled exception class") says nothing about floating point. There is now a test that executes an FP instruction, not merely one that reads `CPACR`.

**The consequence to carry into M3:** the context switch will not save FP state. While Lua is on a single thread that is fine. The moment it is not, this needs lazy FP save, which `roadmap.md` schedules at M10 for Doom and which will be needed sooner.

**2026-08-30 — The libc lives in `runtime/` and the kernel shares it.** At M2 there is one address space and one image, so a second copy in `kernel/` would be a duplicate symbol rather than a boundary. `kernel/string.c` is gone. The split happens at M4, when Lua moves to EL0 and `runtime/` becomes what the design calls it: the libc inside a process, whose I/O resolves against that process's namespace and nowhere else.

**2026-08-30 — QEMU `virt` defaults to a GICv2; Kosmos drives a GICv3 and passes `gic-version=3`.** Read out of QEMU's own device tree rather than assumed, after the same documents turned out to be wrong about the entry exception level. The flag is on the QEMU line in the Makefile and in `tools/run_tests.py`, and the two have to stay in agreement. `hal.md` corrected, including the claim about the Pi 5's GIC, which is now marked as an open question instead of an assumption.

**2026-08-30 — The timer rearms from the previous deadline (CVAL), never from the current time (TVAL).** TVAL sets the comparator to "now plus interval", where "now" is when the handler runs, so every period absorbs the cost of taking the interrupt and the error accumulates. Under QEMU that cost is about 195,000 counter ticks, roughly 3 ms against a 10 ms period, and a nominal 100 Hz ran at 73: eight ticks in eleven seconds of wall clock. Measured, not reasoned about. There is a test that fails if it is ever changed back, and the "ticks advance" test passes with the bug, which is why the second one exists.

**2026-08-30 — A null dereference cannot be written in C.** GCC is entitled to assume undefined behaviour never happens, so it emits the store and then treats the rest of the function as unreachable, appending a `brk`. The store faults, the handler steps ELR past it, and execution lands on the `brk`: a second fault with the expectation already spent, and a panic. Every deliberate fault in the tests goes through an inline-assembly store.

**2026-08-30 — Expected faults recover by stepping ELR, not by `setjmp`.** There is no `setjmp` until the libc arrives at M2, and every A64 instruction is four bytes, so the arithmetic is exact. Only synchronous exceptions are recoverable this way: an IRQ did not come from the instruction at ELR.

**2026-08-30 — On AArch64, `SYS_EXIT` takes a pointer, not a status.** `x1` holds the address of a two-field block: the reason code (`ADP_Stopped_ApplicationExit`, `0x20026`) and then the exit status. Passing the status directly in `x1` is the AArch32 form; it assembles, it runs, and QEMU exits 0 no matter what the guest meant. A harness that always reports success is worse than no harness, so the failure paths were exercised rather than assumed: a failing test, a hanging test, a missing banner, a build error, and QEMU's exit code on its own. All five produce a non-zero exit.

**2026-08-30 — The test image is a separate build under `build/test/`.** Same sources plus `tests/`, with `-DKOSMOS_TEST`. Two directories rather than one so the normal image never carries test code and the two cannot share a stale object file.

**2026-08-30 — `README.md` and `CLAUDE.md` moved from `docs/` to the repository root.** Their links were written relative to the root (`docs/design.md`), so from inside `docs/` every one of them resolved to `docs/docs/...` and was broken. `CLAUDE.md` also has to be at the root for Claude Code to load it automatically.

## Known bugs

**Characters typed before the prompt appears are lost.** The PL011's receive FIFO is sixteen bytes and nothing drains it until the REPL starts, so anything pasted into the terminal during boot overflows it silently. A person typing at a live prompt never sees this; it showed up feeding the REPL from a pipe. The fix is an interrupt-driven receive path with a ring buffer, which is worth doing when there is a real terminal at M6 and not before.

**An exception taken while the stack is exhausted is a double fault.** The handler builds its frame on the stack that just overflowed, so it faults again inside the vector and the kernel hangs with no output. The guard page turns a silent overflow into a readable abort, which is the improvement; it does not survive one. The fix is a separate exception stack, and it belongs with the thread work at M3.

**A minimal `kmain` with no stack faults silently.** Found while smoke-testing the toolchain. On QEMU `virt` the reset value of `sp` is 0, so any function prologue touching the stack writes to unmapped memory, takes an exception with no vector installed, and hangs with no output. It is not a bug in the system, it is the reason `boot/start.S` sets `sp` before branching to C. Recorded because the failure mode is a silent hang, which is indistinguishable from twenty other causes.

## Hardware pending

- [ ] 3-pin JST-SH debug UART cable (Pi 5) — blocks M2 on that target
- [ ] 3.3V USB-serial adapter (Pi 1) — cheaper and arrives sooner

---

## Overall progress

**The numbers are names, not an order** (`roadmap.md`), so this table is read
down the status column rather than across it. Work has repeatedly jumped
ahead of the numbering: M6, M7 and M8 all met their definitions of done while
M2's second half sat waiting for a cable.

| # | Milestone | Status |
|---|---|---|
| 0 | Boot under QEMU | **done** |
| 1 | MMU, exceptions, timer | **done** |
| 2 | Lua in the kernel + second target | **half done** — QEMU yes, hardware blocked on cables |
| 3 | Microkernel | **done** |
| 4 | Lua to userspace | **done** — two benchmarks on its list unbuilt |
| 5 | Namespaces and servers | **done** — both halves, plus Lua out of the kernel |
| 6 | Graphics and app server | **done** — window manager, compositor, mouse, UI kit, Terminal |
| 7 | Attributes, live queries, replicants | **done** — all three definitions of done |
| 8 | Own filesystem | **done** — kfs on virtio-blk, journalled, host tooling |
| 9 | Software 3D demo | **done ahead of order** — `cube3d`, `g3d.lua` |
| 10 | Doom | after the PDF reader and sound |
| 11 | Drivers (GPIO, USB, network) | |
| 12 | SSH client | |

**"Done" here means the milestone's own definition of done is met and has a
permanent test**, not that nothing more will ever be added to it. M6 is the
clearest case: the window drag with a hung app inside it works and is
checked, and virtio-gpu and the `hal_fb_flush` it will earn are still ahead
of it.
