# Architecture

The diagram every operating systems course starts with looks like this:

```
    +---------------------------+---------------------------+
    |     command-line tools    |     GUI applications      |
    |   +-------------------+   |   +-------------------+   |
    |   |      shells       |   |   |       GUIs        |   |
    |   |  +-------------------------------------------+|   |
    |   |  |                                           ||   |
    |   |  |                 KERNEL                    ||   |
    |   |  |   scheduler  memory  files  network  tty  ||   |
    |   |  |   +-----------------------------------+   ||   |
    |   |  |   |             drivers               |   ||   |
    |   |  |   |             HARDWARE              |   ||   |
    |   |  |   +-----------------------------------+   ||   |
    |   |  +-------------------------------------------+|   |
    |   +-------------------+   |   +-------------------+   |
    +---------------------------+---------------------------+
```

It says something true about Unix and something false about Kosmos. The
kernel there is the *widest* box: everything that matters is inside it, and
the shell and the applications sit on top of it because there is nowhere else
to sit. A filesystem is kernel code. A terminal is kernel code. Drawing on the
screen is kernel code.

Kosmos is drawn the other way round. The kernel is the *narrowest* box, and
almost everything the picture above puts inside it is a process beside the
shell rather than underneath it.

---

## 1. The picture Kosmos actually draws

```
  EL0  ..................................................................
       .  Every box on this floor is a process. Each has an address     .
       .  space of its own, a capability table of its own, and no way   .
       .  to name anything that is not in that table.                   .
       .                                                                .
       .   programs, in Lua, from /bin                                  .
       .   +--------+--------+--------+--------+--------+--------+      .
       .   |  htop  |  cat   |   ls   | monitor| hello  |benchmark      .
       .   +--------+--------+--------+--------+--------+--------+      .
       .                                                                .
       .   +--------+  the window manager. Applications send it lists   .
       .   |   wm   |  of drawing commands; it owns every pixel they    .
       .   +--------+  ask for, which is why a hung one still has a     .
       .      ^   ^    window that moves.                               .
       .      |   |                                                     .
       .   +------+ +--------+                                          .
       .   |hello-| | stuck  |  started by wm, each handed /dev/wm and   .
       .   | win  | |        |  nothing else it did not already have     .
       .   +------+ +--------+                                          .
       .        ^                                                       .
       .        | spawned, each in a space of its own                    .
       .   +---------+                                                  .
       .   |  shell  |          servers, in C                            .
       .   +---------+     +---------+ +---------+ +---------+ +------+ .
       .        ^          | console | |  /data  | |  /bin   | | /dev | .
       .        |          +---------+ +---------+ +---------+ +------+ .
       .        |               ^           ^           ^          ^    .
       .        +-------- IPC --+-----------+-----------+----------+    .
       .                                                                .
       .   +--------+       init starts all of the above and holds the  .
       .   |  init  |       capabilities nobody else is allowed         .
       .   +--------+                                                   .
       ..................................................................
                                   |
                        18 syscalls |  the whole interface
                                   v
### What is inside one of those boxes

Every box on that floor is drawn as though it were Lua, and none of them
are only Lua. A process is Lua source *and* the C that runs it, in one
address space, at EL0:

```
   any one of the boxes above
   +--------------------------------------------------------+
   |  the program, and the libraries it loaded    Lua source |
   |  ui.lua, gfx.lua, kfs.lua, theme.lua         Lua source |
   |  - - - - - - - - - - - - - - - - - - - - - - - - - - -  |
   |  the Lua interpreter                                  C |
   |  gfx.c        fill, blit, blend, glyphs, discs        C |
   |  stb_truetype outlines                                C |
   |  serialize.c  a table to bytes and back               C |
   |  libc         memcpy, malloc, snprintf, setjmp        C |
   +--------------------------------------------------------+
```

**That C is not kernel code and is not privileged.** It is compiled into
the user image, which is a different binary from the kernel, and it runs
with the process's own page tables at EL0. A bug in `gfx.c` can corrupt
the process it is in and nothing else - the same blast radius as a bug in
the Lua above it, only faster.

Which is exactly why the language rule permits it. The rule asks what a bug
there could reach, not what language it is written in: the pixel loop is C
because Lua cannot write two million pixels in time, and it is *allowed* to
be C because it cannot reach past its own address space.

The one qualification worth keeping: a surface shared with the compositor
is shared memory, so a bad write there can scribble on pixels another
process is reading. It cannot reach anything else, because the mapping is
the only thing it was handed.

**A decoder is the same shape.** When there is audio, the code that turns a
compressed file into samples is C in the player's own address space, beside
the font rasteriser. What has to live lower down is the *driver* - talking
to the device needs MMIO and interrupts, which userland cannot reach yet -
and that is a limitation rather than a principle. See `hal.md`.

---

  EL1  +----------------------------------------------------------------+
       |  kernel: threads, address spaces, IPC, capabilities             |
       |                                                                 |
       |  arch/aarch64   page tables, exception vector, context switch   |
       |  hal/qemu-virt  UART, GIC, timer, framebuffer, keyboard         |
       +----------------------------------------------------------------+
                                   |
                                   v
       +----------------------------------------------------------------+
       |  hardware                                                       |
       +----------------------------------------------------------------+
```

Three differences from the first picture, and they are the whole design.

**The kernel does not know what a file is.** There is no `open` in the list of
eighteen syscalls, because there is nothing for it to open. `/data` is a
process. `/bin` is a process. When `cat` reads a file it sends a message to
another program and waits for the answer, exactly as it would over a network.

**The shell is not above the servers.** In the first picture the shell sits on
the kernel and the kernel provides the filesystem, so there is an order:
applications, then shells, then services. Here the shell and the filesystem
are both processes at EL0 sending each other messages. Neither is underneath
the other. The shell can do nothing `cat` cannot; it simply happens to have
been handed more capabilities.

**There is no global namespace.** A process cannot walk a tree to find
something, because there is no tree to walk. It has a table of capabilities,
by index, and a mount table mapping names onto them. A program that was not
given `/dev` does not get "permission denied" - the path does not exist. See
`design.md` §6.

---

## 2. The layers, from the bottom

### Hardware

QEMU's `virt` machine, today and only today. A PL011 UART at `0x09000000`, a
GICv3, the ARM generic timer, a `ramfb` framebuffer negotiated through fw_cfg,
and a virtio-input keyboard. Real hardware arrives at M2, blocked on cables.

### `arch/aarch64/` - "which CPU are you"

Page tables, the exception vector, the context switch, the barriers. This is
not abstracted across architectures; a second architecture would reimplement
it rather than parameterise it. About 2,000 lines.

### `hal/qemu-virt/` - "which peripheral do you have"

The UART, the interrupt controller, the timer, the framebuffer, the keyboard.
Common interface, one implementation per board. The interface is eleven
functions and is deliberately not larger: the right shape for a HAL only
becomes visible once there is a second real target, and writing it now with
one target produces the shape of QEMU with generic names. About 1,500 lines.

**This is the layer that is still in the wrong place, and knowingly so.** In
the design these drivers are userland processes like everything else. Today
they are linked into the kernel, because the kernel needs a console before
there is a userland to provide one, and because the boot screen has to be
drawn before init exists to draw it. `hal.md` says where the line goes when
they move out.

### `kernel/` - threads, address spaces, IPC, capabilities

And nothing else. No files, no network, no graphics beyond the boot screen, no
Lua since M5.

With `arch/`, `hal/` and `boot/` the whole kernel is 5150 lines of code against
a budget of 10,000 — and 11096 lines in the file, because it is more than half
comments. The budget counts the first number, and `make size` prints both:
what it exists to catch is something creeping *in*, not somebody explaining
what is already there.

There is **no allocator**. Every kernel object lives in a statically declared
pool with a fixed size:

| pool            | size |
| --------------- | ---- |
| threads         |   48 |
| processes       |   32 |
| endpoints       |   96 |
| address spaces  |   32 |

Running out is then an error at a known limit rather than a failure at an
unknown one - and every one of those numbers is reported by `ps`, because a
limit nothing counts is a limit nobody can find. That lesson was learned
twice here, both times painfully.

### The syscall boundary - eighteen calls

```
   0 exit         6 reply          12 sysinfo
   1 write        7 getchar        13 map
   2 yield        8 spawn          14 unmap
   3 endpoint     9 wait           15 setname
   4 call        10 ticks          16 proctable
   5 receive     11 screen         17 endpoint_destroy
```

Four of them - `endpoint`, `call`, `receive`, `reply` - are the entire
mechanism by which every service in this system is reached. The rest is
process lifetime, time, memory, and the two concessions the boot console
forces (`write` and `getchar`).

Every one of them that names something takes a **capability index**: a
number meaningful only inside the calling process's own table. There is no
global identifier for anything, anywhere, on purpose. See `design.md` §5.

### `runtime/` and `user/lib/` - what a Lua process is made of

A freestanding libc, the Lua interpreter, the syscall bindings, the table
serialiser that turns a Lua value into a message, and the graphics primitives.
This is C, and it runs at EL0 inside each process. A bug here kills that
process and no other, which is exactly the test for what may be written in C:
*if a bug can corrupt another process it is C's job to prevent, and if it can
only kill its own process it may be Lua.*

The graphics primitives are the standing exception to "Lua unless proven
otherwise": a pixel loop is never written in Lua. Lua decides *what* is drawn
and *where*; the loop over the pixels happens inside a surface, in C, and
nothing in Lua ever computes a pixel offset - the pitch is not `width * 4`
and pretending it is produces diagonal lines. `gfx.md` §19.

### `user/init/init.lua` and `user/servers/` - init and every server

One binary, many roles. The image carries a single userland ELF and the role
number it is spawned with decides what it becomes.

**Where that number is answered moved in September 2026.** `user/init/main.c`
dispatches the server roles *before* the Lua interpreter is opened, so those
processes have no collector at all rather than a promise not to allocate:
`/dev/audio`, `/dev`, `/bin`, `/lib`, `/app`, `/dev/console` and `/data` are
each one file in `user/servers/`, speaking a struct declared in
`user/include/`.

What is left in `init.lua` is init itself, the shell, the runner that hosts
one program, `diskfs`, and the namespace - which is a *kit* rather than a
server, run in the caller's own process with no endpoint and no thread.

### `user/bin/` - programs

`htop`, `cat`, `ls`, `monitor`, `hello`, `benchmark`, `spin`. Lua source,
carried inside the image because there is no disk until M8, served by `/bin`,
and run by typing the name.

---

## 3. What actually happens when you type `cat /bin/ls.lua`

Follow one command all the way down and back. Every arrow is a real boundary.

```
   you press a key
     |
     v
   virtio-input raises an interrupt        hardware
     |
     v
   GIC -> the exception vector -> the keyboard driver       EL1
     |
     v
   the byte joins the kernel's input ring
     |
     v
   the shell returns from SYS_GETCHAR (7)                   the boundary
     |
     v
   the shell has a line. "cat" is not a Lua name,           EL0, shell
   so it asks /bin whether /bin/cat.lua exists -
   asks, and does not read it
     |
     v
   SYS_ENDPOINT (3): a private channel for one exchange
   SYS_SPAWN (8): a new process, a new address space,
   and exactly five capabilities handed to it
     |
     v
   the runner receives the *path*, not the source,          EL0, runner
   builds a namespace from the capabilities it was
   given, and fetches the program through it
     |
     v
   cat.lua runs. fs.read("/bin/ls.lua") is an IPC           EL0, cat
   call to /bin, which answers in 1400-byte chunks
   because a message is 2048 bytes
     |
     v
   printing is another IPC call, to the console server      EL0, console
     |
     v
   the console server calls SYS_WRITE (1)                   the boundary
     |
     v
   the kernel console puts each byte on the UART and        EL1
   blits its glyph into the framebuffer
     |
     v
   pixels                                                   hardware
```

Two things are worth noticing about that trace.

**The shell never read the file.** It asked whether it existed and then handed
the *name* to a process that could fetch it. The bytes cross the boundary
once. That was not the first design - the first one sent the source in the
spawn message and broke the moment a program grew past 2,048 bytes.

**Nothing in that chain has ambient authority.** `cat` can read `/bin/ls.lua`
because the shell chose to hand it the `/bin` capability. Had it not, the path
would not exist for `cat`. There is no configuration that grants this and no
check that denies it; the capability either is in the table or is not.

---

## 4. Where the lines are drawn, and why

| line | rule |
| ---- | ---- |
| `arch/` vs `hal/` | "which CPU" vs "which peripheral". Not the same question, and blurring them is how a HAL ends up with the shape of one board. |
| kernel vs userland | Threads, address spaces, IPC, capabilities. If it is not one of those four it does not go in. |
| C vs Lua | If a bug there can corrupt another process, C. If it can only kill its own process, Lua. |
| syscall vs IPC | A syscall is for something only the kernel can do. Everything else is a message to a process. |
| inside a process vs the system | A libc inside an app is fine and necessary. A POSIX personality - `fork`, signals, global descriptors, a tree reachable without a namespace - is forbidden. `design.md` §17. |

The last one is the one that costs something. It means ports get patched
rather than accommodated, and it is the deliberate price of the rest.

---

## 5. What this buys, concretely

**A driver bug is a dead process, not a dead machine.** When `/bin` died
during development - it tried to reply with a message larger than 2,048 bytes
- the shell printed an error and carried on. Nothing else noticed.

**A hung application does not hang the screen.** `wm hello-win,stuck` puts
two applications on screen, one of which stops replying for ever. Its window
still shows what it drew and still moves, because its pixels were never in
its address space and nothing in the compositor ever waits for it. That is
this milestone's definition of done and there is a display check for it.

**A server can be replaced while the system runs.** That is level-1 hot
reload, and it is the reason the userland is Lua at all. Every time something
is pushed down into C to make it faster, that is what is being spent.

**The boundaries are testable from outside.** A pitch bug in the graphics
primitives passes 103 of 103 tests run inside the guest, because every read
and every write agree with each other. It is only visible to an observer on
the other side of the framebuffer - which is why the test suite has a phase
that boots QEMU and inspects the picture it scans out.

---

## 6. What is not built yet

Honest list, so this document does not describe an aspiration as though it
were a fact.

- **Drivers are still in the kernel.** See §2. This is the largest gap
  between the diagram in `design.md` and the diagram at the top of this file.
- **The console and the window manager share one framebuffer**, so printing
  a line scrolls a window's pixels, and the window manager cannot usefully
  run detached because it and the shell's line editor would both be draining
  one keyboard. Both go away when the shell is a window.
- **A window has no view tree and no widgets.** An application draws by
  sending a flat list of commands. The view tree, follow modes and the
  widget set are the rest of M6.
- **A process cannot be ended from outside.** Control-C works by asking:
  a program that polls can be stopped and one that does not cannot -
  `/bin/spin.lua` is the standing counterexample. `process_exit` panics if
  it is not the running process, and a real kill has to unlink the target
  from three IPC queues and settle what happens to whoever holds a reply
  handle for it.
- **There is no filesystem on a disk.** `/data` is RAM and `/bin` is compiled
  into the image. M8.
- **Single core.** The code is written SMP-ready - per-CPU runqueue,
  `TPIDR_EL1`, no loose mutable globals - but nothing has ever run on a
  second core. It is listed under M6 as "if it fits", which is the honest
  status: with one core the bugs are deterministic, and with four they appear
  once every thousand boots and are debugged over a serial line.

---

See also: `design.md` for why each of these decisions was taken, `hal.md` for
the target and driver boundary, `gfx.md` for the path pixels take, and
`roadmap.md` for what comes next.
