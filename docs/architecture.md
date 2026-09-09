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
       .   |hello-| | stuck  |  started by wm, each handed /app/wm and   .
       .   | win  | |        |  nothing else it did not already have     .
       .   +------+ +--------+                                          .
       .        ^                                                       .
       .        | spawned, each in a space of its own                    .
       .   +---------+                                                  .
       .   |  shell  |          servers, in C                            .
       .   +---------+     +---------+ +---------+ +---------+ +------+ .
       .        ^          | console | |  /ramfs  | |  /bin   | | /dev | .
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
eighteen syscalls, because there is nothing for it to open. `/ramfs` is a
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
`/dev/audio`, `/dev`, `/bin`, `/lib`, `/app`, `/dev/console` and `/ramfs` are
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

## 5. Time: three clocks, and which one a number means

**This is here because getting it wrong has cost this system two working
subsystems in two days** - the window manager on 6 September and the
resolver on the 7th, in unrelated code, the second one *after* the first
was found and fixed. Both times the bug was invisible in the same way: a
number crossed a process boundary and its unit did not cross with it.

### The three

| | what it is | the question it answers | who uses it |
|---|---|---|---|
| **the wall clock** | `/dev/clock`, from the board's RTC, in seconds since 1970 | *"What is the date?"* | 4 files |
| **the counter** | `sys.ticks()` / `kosmos_ticks()` - CNTPCT_EL0 on ARM, the TSC on x86 | *"How long did that take?"* | ~114 sites |
| **the scheduler tick** | a count of timer interrupts, at `TICK_HZ` = 250 | *"Wake me later"* | every timeout |

The first is a capability and is barely used: the Deskbar's clock, Tracker,
`machine`, and `clock.lua`. It is not a duration and nothing measures with
it. `design.md` §4.4 says why a date is a capability rather than a syscall.

The other two are what get confused, because **both are called "ticks" and
both are plain integers**.

### Why there are two of them at all

They need different hardware, and that is the whole reason:

- **Reading the time must be cheap** - one instruction, no interrupt, no
  lock. A free-running counter does that. `mrs cntpct_el0` and `rdtsc` are
  the same idea on both boards.
- **Being woken later requires an interrupt to happen.** A counter does not
  interrupt. Something has to be programmed to fire.

So two *mechanisms* are genuinely necessary. **Two *units* are not**, and
having them is an inheritance from hardware that could only be told
"interrupt me every N" rather than "interrupt me at time T". Unix called
its version `jiffies`; Linux carried the same split until hrtimers and
NO_HZ removed it. Both this kernel's timers can already be given an
absolute deadline. `hal/qemu-virt/timer.c` already writes `CNTP_CVAL_EL0`,
which *is* a comparator, and then adds an interval to it on every tick to
fake periodicity - so the one-shot mechanism is not merely available, it is
what is already running.

### The rates, and why you cannot memorise the ratio

```
                        counter        scheduler tick     ratio
QEMU virt, TCG          62.5 MHz       250 Hz             250,000
QEMU virt, hvf          24 MHz         250 Hz              96,000
QEMU q35, x86-64        ~1 GHz         250 Hz           ~4,000,000
```

**The same board gives two different answers** depending on whether it is
run under `make qemu` or `make fast`. There is no constant to keep in your
head, which is why every correct piece of counter arithmetic in this tree
reads `counter_hz` from `/dev/cpu` three lines above the sum - where the
unit is visible - and why the two that were wrong were the two that did
not.

### The rule

> **Every timeout is scheduler ticks. Every timestamp and every measured
> duration is the counter. A number that crosses a process boundary says
> which in its name.**

`sys.sleep(n)`, `fs.wait_input(n)`, `sys.receive(..., n)` and every server
protocol's `wait_ticks` are scheduler ticks. `sys.ticks()`, a ping's round
trip, a frame time and a benchmark are the counter.

### Where the two meet, which is now two functions

**In the kernel: `thread_deadline_in`.** Every deadline a thread carries -
`wake_at` - is a *counter* value, and this is the only place that turns a
caller's scheduler ticks into one. `SYS_SLEEP`, `SYS_WAIT_INPUT` and a
receive with a timeout all go through it.

They used to say `hal_ticks() + n` instead, which was wrong in two ways.
`hal_ticks` counts interrupts **actually taken**, and `hal_ticks_missed`
exists precisely because they are not always taken - so a machine under
load ran every sleep long by however many it had missed, which is a clock
that stretches exactly when something is already going wrong. And a count
of interrupts cannot be handed to a comparator, so the periodic tick could
never have been removed while deadlines were expressed in it.

**In the network stack: `in_counter` in `user/servers/net.c`**, for the
same reason and with the same shape.

### What went wrong, twice

**The window manager, 0.9.1.** A poll carried a field called `wait`.
Eight call sites wrote `wait = 1` meaning one scheduler tick; the window
manager added it to `sys.ticks()`. Every animating window asked to be woken
in sixteen nanoseconds - which is to say immediately - so the cube ran
*faster while the mouse was moving*, because pointer events cut the
manager's sleep short and it passed more often. Fixed by naming the field
`wait_ticks` and building the message in one place.

**The resolver, 0.9.6.** `struct net_request` had one `ticks` field and
four operations read it. Three converted; `NET_OP_RESOLVE` added it to the
counter raw. A five-second lookup became twenty microseconds, so **the
resolver answered whichever query beat the next sweep and timed out the
rest** - it had never worked twice in one boot. Its only caller was the
browser, which passed `counter_hz`, wrong in the same direction by the same
factor, so the pair was self-consistent and nothing could contradict it.
Writing `host` - thirty lines, a second caller - broke it immediately.

**Two lessons, and the second is the cheaper one.** A convention is not a
defence: the second bug happened after the first was fixed, in a field that
predated the fix. And **an operation with one caller is untested no matter
what the suite says**, because the caller and the reader can agree on
something wrong.

### A deadline is not a reason, and that cost x86 every timeout it had

Found while checking that step one had actually worked, by timing a sleep
from outside the guest rather than trusting a green suite - `ping` sleeps
exactly one second between echoes, so five echoes is four seconds of
sleeping and nothing else.

```
AArch64   4 sleeps of 250 ticks: expected 4.0s, took 4.29s
x86-64    4 sleeps of 250 ticks: expected 4.0s, took 0.16s
```

The deadline was right - the kernel computed `want=999,292,000` counter
ticks for one second at a counter it had correctly measured at 998.9 MHz -
and the thread was woken after `got=3,683,000`, which is one scheduler
tick. Something was waking it 270 times too early.

**`thread_wake_sleepers_now` woke every thread that had a `wake_at`.** Its
job is to wake threads when the interrupt that just arrived is the thing
they were waiting for - a key. But `wake_at` says *when* a thread would
like to be woken and nothing about *what* it is waiting on, and a
`sys.sleep` carries one while waiting for nothing at all.

**Why only x86.** `input_arrived` is set by an input interrupt and cleared
in exactly one place - `SYS_WAIT_INPUT`, and only for the process that owns
the console. At a bare prompt nothing calls it, so on that board the flag
latches true and every timer interrupt runs the path. Measured at an idle
prompt:

```
AArch64      thread_wake_sleepers_now fired     0 times in 5s
x86-64                                       1200 times in 5s
```

AArch64 escaped it by never setting the flag, which is luck rather than
design - the code is identical on both boards.

So every timeout in the system on x86 was four milliseconds: `sys.sleep`,
`fs.wait_input`, an IPC receive with a deadline, every server wait. It looks
like a fast machine, which is why it survived.

The fix is a `wake_on_input` flag set only by `SYS_WAIT_INPUT`, and it is
the distinction the name was always making and the code was not.

**Two tests, and the first one does not catch it.** "A sleep lasts as long
as it asked" is a property nothing had ever checked on either board - worth
having, and it *passed with the bug deliberately put back*, because the
trigger is an input interrupt and the test image never has one. The second
calls `thread_wake_sleepers_now` directly, which is exactly what a key
does, and fails the moment the distinction is removed.

### Where this is going

Step one is done and is what this section describes: deadlines are counter
values, and there is one conversion.

What follows removes the second unit rather than managing it. A one-shot
timer programmed to the next deadline - which both boards' hardware
supports, and which x86 wants the LAPIC timer for anyway, because the 8254
PIT is one device and SMP needs a per-CPU one. Then the boundary unit
becomes **nanoseconds**, converted once inside the kernel where
`counter_hz` is authoritative, and userland stops needing to know the rate
at all.

Two things fall out of it. Wakeups stop being quantised to 4 ms - which
matters for a system whose first commitment is *bounded* latency, and which
runs audio on a 5.8 ms period. And an idle machine stops taking 250
interrupts a second to discover it has nothing to do.

---

## 6. What this buys, concretely

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

## 7. What is not built yet

Honest list, so this document does not describe an aspiration as though it
were a fact.

**This section was wrong for a long time, and that is worth saying at the
top of it.** Every bullet below used to be joined by five more that had
quietly become false - a window with no view tree and no widgets, no
filesystem on a disk, no way to end a process from outside, a console
sharing the framebuffer with the window manager, and a claim that the code
was written SMP-ready. All five were true when written. None had been true
for months, and nothing noticed, because *code has `make test` and prose has
nobody* - which is the same lesson the 0.9.0 review found four times over.

- **Drivers are still in the kernel.** See §2. This remains the largest gap
  between the diagram in `design.md` and the diagram at the top of this
  file: `hal/virtio/` and `hal/qemu-virt/` link into the kernel binary, so
  a driver bug is a kernel bug. Everything else that was going to move out
  of the kernel has.

- **Four processors, and the placement policy is not switched on.**
  `make qemu` boots four. Each one installs its own exception vector, wakes
  its own GIC redistributor, arms its own generic timer, claims its own
  `struct percpu` through `TPIDR_EL1`, adopts its own idle thread and runs
  its own runqueue with its own lock. `thread_create_on(cpu, ...)` puts a
  thread on any of them and it runs there.

  The kernel has locks now - the pools, every endpoint, every runqueue, the
  console - and one rule: **every lock masks interrupts**, because the
  things worth locking are reached from a syscall and from an interrupt
  handler alike. A thread has a home and does not migrate, which is what
  lets IPC release its endpoint before the context switch rather than
  handing it to the next thread.

  What is still missing is no longer "one line and one afternoon", which is
  what this said while the drivers were unlocked. They were locked in
  0.9.20, and `make SMPWORK=4 qemu` places threads on all four processors -
  **which now use them**: six compute-bound processes read 100% on every
  core, where three of them used to go idle within a second.

  Both causes were in the preemption path rather than in placement.
  `thread_tick` returned before `policy->tick` on every core but zero, so
  only core zero preempted on a quantum; and `thread_wake` compared against
  the *waking* core's `current` and flagged the *waking* core, so a
  cross-core wake never preempted the target.

  What is left: the display harness fails at its editor phase under
  `SMPWORK=4`, which is why placement is still off by default; and a panic
  protocol - a core that panics has to *stop* the others rather than queue
  behind them. `docs/smp.md` is the map, and carries the measurement.

  This bullet has been wrong twice in opposite directions. It said "nothing
  has ever run on a second core, and there is no per-CPU struct"; and before
  that `CLAUDE.md` claimed from the repository's first commit that the code
  was written SMP-ready with a per-CPU pointer and a per-CPU runqueue.
  Neither was checked by anything, which is the whole argument for auditing
  the state files against the code.

  The x86-64 port paid for part of that in advance without meaning to. Two
  pieces of state there are per-CPU rather than per-thread - the TSS
  holding the stack a ring-3 entry lands on, and whoever owns the
  floating-point registers - and the boundary stopped being theoretical
  when a global holding the interrupted stack pointer turned out to be
  per-*thread* and returned a process onto another process's stack. One
  core was enough to prove it wrong.

- **Every message blocks.** `SYS_CALL`, `SYS_RECEIVE` and `SYS_REPLY` are
  the whole IPC surface, so a request to a server deschedules the caller
  until the reply comes back - by construction, with no way to say
  otherwise. Half a browser frame is an application waiting on a `commit`
  whose handler swaps an index and records a rectangle. A non-blocking send
  is a change to the IPC model rather than an optimisation: a syscall, a
  fixed-size queue in the endpoint struct because there is no allocator, a
  decision about what a full queue does, and backpressure.

- **An address is four numbers.** There is no resolver, so the browser
  reaches `188.184.67.127/` and not a name. UDP exists in the stack only as
  far as DNS will need it, and there are no sockets, because nothing else
  has asked for any.

- **The screen is one framebuffer with no flush.** `hal_fb_init` asks the
  firmware for a linear framebuffer and lets it choose where the pixels
  live, which is the one operation QEMU's ramfb and the Pi's mailbox both
  perform. virtio-gpu does not fit it - it needs an explicit flush after
  drawing - and that is precisely what will grow the interface a
  `hal_fb_flush`, with two implementations behind it rather than one.

- **The terminal cannot be selected from.** The clipboard reaches
  `ui.editor` and `ui.field`; the terminal draws its own scrollback through
  `ui.view`, so it has no anchor and no cursor. The honest fix is lifting
  that machinery out of the editor rather than writing it twice.

- **`tests/tests.c` has run on one board.** 127 checks, the largest single
  test asset here, and no x86-64 image target builds it - so the suite that
  most directly exercises the kernel has never run on the second
  architecture. Everything else in `make test` runs on both.

---

See also: `design.md` for why each of these decisions was taken, `hal.md` for
the target and driver boundary, `gfx.md` for the path pixels take, and
`roadmap.md` for what comes next.
