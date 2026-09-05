# Roadmap

Thirteen milestones. Each has a verifiable **definition of done**: something that either runs or does not, with no room for interpretation.

The rule: a milestone does not start until the previous one meets its criterion. The temptation to pull things forward "because they are cheap" is the main way projects like this scatter and die.

---

## The numbers are names, not an order

A milestone here is a *name for a body of work*, and the number is part of
the name. M8 is "own filesystem" whatever else happens; it does not become
M9 because something overtook it.

They were written in the order they seemed likely to happen and that order
has already changed twice - interrupt-driven input arrived inside M6 because
an idle desktop sitting at ninety per cent made it urgent, and the
filesystem is being deferred past three smaller pieces that unblock more.
Renumbering to match would break every cross-reference in six documents to
buy nothing.

**The order being worked in now**, which is not the numbering:

1. **The Terminal** (M6's list). Small, and it removes two structural
   problems rather than adding a feature: the kernel console and the
   compositor share one framebuffer, and the desktop cannot run detached
   because it and the shell's line editor drain one keyboard. Both go away
   when the shell is a window.
2. **Shared-memory surfaces** (M7's list, `gfx.md` 19.4). What every
   graphics-heavy thing after it depends on, and the honest answer to
   "how would a video player work".
3. **The 3D soft engine** (M9). Fun, visible, and the right way to decide
   what belongs in C: write it in Lua, measure, and move only the part that
   is slow.
4. **M8, the filesystem.** The biggest, and the one that benefits most from
   not being rushed.

**Revised, September 2026.** The three above M8 are done, and what came
next was chosen by hand rather than from this list: a PDF reader, the
scheduler bands, and a frame profiler. The order now is:

5. **M13, the desktop.** Menus, resizing, scrollbars, columns, a tree, and
   Tracker. Decided ahead of Doom deliberately - the UI has to look like an
   operating system before the machine is asked to prove it is a fast one.
6. **M10, Doom.** After the desktop, not before it.

### Later, and not yet scheduled

**The shell needs to be one.** It has `cd`, `pwd`, `ls`, `cat`, aliases and
Lua evaluation, and the Terminal now has the builtins that have to live in
the process that owns the working directory. What it does not have is the
rest of what makes a shell useful: pipes, redirection, globbing, history, a
prompt that says something, `cp`, `mv`, `rm`, `find`, `grep`. None of it is
hard and all of it is worth doing once the filesystem underneath it is
finished, because most of those commands are questions about a filesystem.

**`list` should be able to answer with attributes.** `ls` costs one round
trip per entry to ask for a size and a kind, which is the protocol's shape
rather than the program's fault. A `list` that can return what it already
knows would turn a listing of thirty files from thirty-one messages into
one.



**Process memory is committed up front, and mostly unused.** Every process
gets 2 MB of heap and 64 KB of stack allocated and zeroed at creation
whether it uses them or not, plus a private 577 KB copy of the image. With
fourteen processes that is about 37 MB of the 512 MB, and most of the heap
is empty.

Two separate things to fix, and the cheaper one first:

- **The image is copied per process and need not be.** Its code pages are
  already mapped read-only and executable - W^X is enforced and the image
  header says how many bytes are read-only - so every process could map one
  shared copy. That is 145 pages once instead of 145 pages fourteen times,
  and it does not touch share-nothing, which is about mutable state.
- **The heap is committed rather than grown.** Growing it on demand means
  faulting a page in when it is touched, which means a fault handler that
  allocates - and that is a real change to the kernel rather than a tidy-up.

Neither is urgent and neither should be done to make a number smaller. The
signal to watch is `pages_free` in `sysmon`; when process count starts
pushing it, the shared image is the first thing to reach for because the
groundwork is already there.

---

## Overview

| # | Milestone | Difficulty | Notes |
|---|---|---|---|
| 0 | Boot under QEMU | low | Known ground, plenty of material |
| 1 | MMU, exceptions, timer | medium | |
| 2 | Lua in the kernel + second target | medium | First real hardware |
| 3 | Microkernel | high | The heart of the system |
| 4 | Lua to userspace | **the highest** | This is where the design gets tested |
| 5 | Namespaces and servers | medium | Original design starts here |
| 6 | Graphics and app server | high | It becomes a desktop |
| 7 | Attributes, live queries, replicants | medium | The BeOS part |
| 8 | Own filesystem | medium | |
| 9 | Software 3D demo | low | Earned reward |
| 10 | Doom | medium | The verifiable goal |
| 11 | GPIO, USB, networking | high | Optional, by interest |
| 12 | SSH client | medium | Tests the Plan 9 networking model |

Milestones 0 through 3 are documented territory. Milestone 4 is where the project can stall for months. From 5 onward it is original design, and that is the part worth doing.

---

## M0 — Boot under QEMU

Get a character out to the console. It sounds trivial and it is the step that stops the most people.

**What gets built**

- Linker script: where `.text` starts, where `.bss` goes, where the stack goes
- `boot/start.S`: check the core ID and park anything that is not core 0, read `CurrentEL` and drop to EL1 if it is higher, set up the stack, zero `.bss`, jump to C
- `hal/qemu-virt/uart.c`: PL011, four registers
- `kernel/main.c`: a `puts` and a `while(1)`
- Makefile with `make qemu`, `make debug`, `make test`
- **The test harness, now and not later:** a host-side runner that launches QEMU, reads the serial, parses TAP and exits with a code. Guest exit via semihosting (`-semihosting-config enable=on,target=native`, `HLT #0xF000` with `SYS_EXIT`). Fifty lines, and it is what gives every subsequent milestone a safety net. See `testing.md` §18.1

**Definition of done:** `make qemu` prints "Kosmos" in the terminal, and `make test` exits 0.

**Traps:** a misaligned linker script makes `.bss` collide with the stack. The entry exception level is not fixed: plain `-M virt` starts at EL1, `virtualization=on` at EL2, `secure=on` at EL3, and the Pi firmware hands off at EL2 — read `CurrentEL` rather than assuming either way. On hardware every core starts, and if you do not park the secondaries you see the output four times interleaved (under QEMU this only shows up with `-smp 4`, since the default is one core). And `sp` at reset is 0, so a C function called before the stack is set faults in its own prologue and hangs with no output whatsoever.

---

## M1 — MMU, exceptions, timer

**What gets built**

- Physical page allocator: a bitmap over available RAM
- AArch64 long-descriptor page tables, 4KB, identity map of the kernel
- Turn the MMU on and survive the jump
- Exception vector: the 16-entry table
- A sync exception handler that prints ESR, ELR and FAR
- GICv3: distributor and redistributor, enable one IRQ. **`-M virt` defaults to GICv2; v3 needs `gic-version=3`.** See `hal.md`
- ARM generic timer, ticking at 100Hz — **250 Hz since September 2026**, chosen by the sound device: a 5.8 ms audio period cannot be fed on a 10 ms clock, and `kernel/kernel.h` carries the measurement
- Stack guard page
- **Expected-exception support in the handler:** a flag plus `setjmp` so a test can cause a fault on purpose, record it and continue. It has to be anticipated when the vector is written; retrofitting it later is worse. See `testing.md` §18.2

**Definition of done:** the timer prints one tick per second, and a deliberate `*(volatile int*)0 = 1` prints a readable data abort with the causing address instead of hanging.

That second point matters more than the first. **An exception handler that tells you what happened and where is the tool you will use for the entire rest of the project.** Time invested here pays back tenfold.

---

## M2 — Lua in the kernel + second target

Two things together because the second target verifies the earlier work.

**What gets built**

- Minimal freestanding libc: `memcpy`, `memset`, `memmove`, `strlen`, `strcmp`, `strcpy`, `strchr`, `setjmp`/`longjmp`, a minimal `snprintf`, the `math` functions Lua asks for. Scope and the POSIX line in `design.md` §17
- Upstream Lua 5.4 compiled freestanding, with the allocator pointing at the page allocator
- A REPL over UART
- `hal/pi5/`, or `arch/armv6/` **and** `hal/pi1/` — which is not the same size of job, and `hal.md` says why: the Pi 1 is ARMv6 and 32-bit, so it is a second *architecture*, not a second board. Writing it as "the pi1 HAL" here was wrong and made the cheaper-sounding option the bigger one
- Adjust the HAL interface with two real implementations in front of you

**Definition of done:** a `>` prompt over serial where `2+2` returns `4`, under QEMU **and** on real hardware.

**Why here:** the motivational payoff of seeing your own prompt on a physical board is enormous, and it arrives exactly when the novelty of bring-up has worn off. Also, the HAL takes its correct shape now, with two targets, not before.

**Traps:** Lua's `longjmp` needs your own `setjmp`, and getting it wrong is a bug that only shows up when something raises an error. Lua asks for more libc functions than you expect; you discover them one at a time, through link errors.

---

## M3 — Microkernel

**What gets built**

- Address spaces: create, destroy, map pages
- Threads with their context, and a context switch in assembly
- Round-robin scheduler with a per-CPU runqueue (even with a single CPU)
- Synchronous IPC: rendezvous, send/receive/reply
- A per-process capability table, indexed
- Syscalls exposed as Lua functions

**Definition of done:** two kernel threads passing a message back and forth 100,000 times, with the cycle count per round trip printed at the end.

That number is the system's performance baseline and you will look at it for the rest of the project. It goes in `bench/baselines.json` with its commit and its tolerance. Under QEMU it is measured with `-icount`, which is deterministic; on hardware with `PMCCNTR_EL0`. See `testing.md` §18.3.

The benchmark suite also starts here: context switch, syscall entry and exit, page fault latency, interrupt latency.

**Traps:** a botched context switch corrupts registers in a way that surfaces five functions later. When an endpoint is destroyed, everyone blocked waiting on it has to be woken with an error, or they hang forever.

---

## M4 — Lua to userspace

**The hardest milestone in the project.** It touches everything at once.

**What gets built**

- One `lua_State` per process, with a bounded heap (~2MB for servers)
- The process running at EL0
- Syscall bindings as Lua functions, validating capabilities
- The Lua table serializer for IPC
- Removing Lua from the kernel (done: the kernel has no interpreter, and the Lua tests and benchmarks run at EL0)
- Deciding which Lua libraries exist inside a process (part of the security model, not configuration)
- Loading Lua code from a ramdisk embedded in the image

**Definition of done:** two processes at EL0, with separate address spaces, exchanging a Lua table over IPC. And one doing `*(nil)` dies without taking the system with it.

**Benchmarks added:** serializing a typical message, allocating and freeing a table, the overhead of a syscall from Lua versus the same one from C, and **the maximum GC pause** (the maximum, not the average: it is the number that will decide whether there is stutter and it will be the project's recurring problem).

**Why it hurts:** up to here Lua lived comfortably at EL1 with access to everything. Now there is a privilege boundary in the middle, and everything that used to work has to be rebuilt as a message.

---

## M5 — Namespaces and servers

The original design starts here. From this point there is far less reference material and far more of your own judgment.

**What gets built**

- Namespace server: mapping paths to capabilities, per process
- The protocol: `list`, `read`, `write`, `getattr`, `setattr`
- Lua coroutines as the servers' concurrency layer
- Console server
- ramfs
- Hot reload level 1: `load()` of new code while preserving state — *done, and removed again in September 2026; see `design.md` §10*
- Init and basic supervision
- The Lua shell as a REPL against the system

**Definition of done:** from the shell, mount the same ramfs server at two different paths in two different processes, and have each see only its own. And reload a server's code while a client is connected, without the client noticing.

Both were met. The first is still a permanent test. **The second no longer is:** every server is C, there is no dynamic linking, and reload was removed deliberately along with its test — the one case in this project of a milestone's definition of done being withdrawn rather than kept. The reasoning is in `design.md` §10, and it is recorded rather than quietly dropped because a milestone that silently loses its test is a milestone nobody can check.

---

## M6 — Graphics and app server

**What gets built**

- ramfb under QEMU (**done**), then virtio-gpu, and the mailbox on the Pi. ramfb first because `hal_fb_init` is "ask the firmware for a linear framebuffer" and that is ramfb and the Pi mailbox both — virtio-gpu is the one that needs an explicit flush, so it is the target that earns the interface a `hal_fb_flush`, with two implementations in front of it instead of one. Everything above the HAL is identical either way. What ramfb does not give: dirty rectangles and a vblank, because QEMU rescans the whole buffer on its own schedule
- Backbuffer in cached RAM, dirty-rectangle blit to the framebuffer
- An 8x16 bitmap font first, `stb_truetype` afterwards
- Alpha-blending blitter in C
- App server in Lua: windows, BeOS-style tab decoration, stacking, focus
- Input on a highest-priority thread
- Damage tracking, vblank synchronization
- UI kit: view tree, follow modes, basic widgets (see `ui.md`)
- SMP, if it fits
- First apps: Terminal, Inspector, Widget gallery
- The Terminal includes ANSI/VT100 emulation from the start. It is a prerequisite for M12 and it is far cheaper to do now than to retrofit

**Definition of done:** drag a window with a hung app inside it, and have the window keep moving smoothly.

That is the BeOS test and the one that determines whether the system feels good or not.

**Met, literally.** `wm hello-win,stuck` puts two applications on screen,
one of which never replies again, and the hung one's window is dragged by
its title bar with the mouse. There is a display check that does exactly
that, driving a virtio tablet through QMP.

**Still open in M6:** **interrupt-driven input** (see below), row and column
containers, a scroll widget, the Terminal with its VT100 emulation,
virtio-gpu as the second `hal_fb_*` implementation, the compositor
benchmarks (blitter throughput, frame compose, damage versus full redraw,
input latency), and SMP.

**Interrupt-driven input is the one that matters most now.** The keyboard
and the tablet are polled, so the window manager runs a loop that is always
runnable, so the machine never idles and the processor meter reads ninety
per cent on an empty desktop. It is not a measurement error - the core
really is busy. Applications already block, because the compositor parks
their event polls; the compositor cannot, until asking for input is
something a thread can block on. That means wiring the virtio-mmio
interrupt through the GIC and adding a wait that a thread can sleep in.

The Terminal is the one that matters most, and not only as an application:
the kernel console and the window manager currently draw on one framebuffer,
so printing a line scrolls a window's pixels, and the window manager cannot
usefully run detached because it and the shell's line editor would be
draining one keyboard. Both disappear when the shell is a window.

**Also:** `gfx.surface` as a userdata over flat bytes, the C primitive set (`fill`, `blit`, `blend`, `span`, `get`/`set`), and surface lifetime management (explicit `free`, a `__gc` net, telling the GC the real size). See `gfx.md`. Pixels do not go into Lua tables, and that rule has no exception.

**Benchmarks added:** blitter throughput in MB/s, time to compose a typical frame, damage tracking overhead versus full redraw, and **input latency measured from event to pixel** (a timestamp on the event and another on the flip). Everything is recorded by maximum and p99, never by average. See `testing.md` §18.5.

**Traps:** an uncached framebuffer is 10-50x slower; drawing straight into it is the mistake that kills performance. The pitch is almost never `width * 4`. On the Pi the channel order is usually BGRA.

---

## M7 — Attributes, live queries, replicants

The BeOS part, and what almost nobody replicated.

**What gets built**

- Typed attributes on namespace nodes
- Per-attribute indexes in the filesystem server
- Live queries: register a predicate and receive a message when the result changes
- Workspaces and stack-and-tile
- Replicants: a view serialized as source + state + `needs`
- **Entity files:** nodes with no content, only attributes. It is the model of BeOS People files: a named entity with email, phone and web, no data inside. It enables dragging an entity into an app and having the right thing happen for that type. See `beos.md` §17.2
- **Shared-memory surfaces:** double buffering, `commit` with mandatory damage, memory mapped inner shareable from the kernel. It is what makes Paint real. See `gfx.md` §19.4 and §19.5
- **A benchmark of query time versus file count, and it has to come out flat.** If time grows with the number of files, the index is not working and the whole premise of the filesystem collapses. It is the most important number in this milestone
- **Scripting architecture:** each app publishes its hooks as nodes in its own namespace (`/app/paint/color`). `ui.window` publishes the window properties by default and the app adds its own declaratively. Consequence: every app is manipulable from the REPL without its author having done anything. See `beos.md` §17.2
- Apps: Tracker, Deskbar, Text editor, Clock, Log viewer, Paint, Preferences

**Definition of done:** an open query over a directory, and another process writes a file that matches the predicate, and the view updates on its own without polling. And drag the desktop clock into a Tracker window, and have it keep working with the capabilities it declared. And from the REPL, change Paint's brush color with an `fs.write` into its namespace, without Paint having any scripting code.

**All three are met**, with the dragging replaced by the same transfer done
without a pointer:

1. **The live query.** `watch kind=note &`, then `attr /data/x kind=note`
   from the prompt, and the watcher reports. It is blocked in one call the
   whole time - no timer, no repeated question - because the filesystem
   parks the reply until the answer changes. A server that called back into
   a client could be blocked by that client.
2. **The replicant.** `wm clock,adopt`: one application publishes a view
   as source, state and a `needs` list; another adopts it and runs it, with
   only what it declared. It is handed over through /data rather than
   dragged, because dragging is what a pointer is for.
3. **The scripting.** `setprop /app/gallery/title=...` renames a running
   window, and `gallery.lua` contains no scripting code - it called
   `ui.window`.

**Still open in M7:** workspaces, stack and tile, entity files as a named
idea (attributes already do the work), shared-memory surfaces, and the
applications - Tracker proper, Deskbar, a log viewer, Paint, Preferences.
The query benchmark exists and comes out flat; `qbench` measures a control
at every size, without which it reported a slope that was in the round trip
and not in the index.

---

## M8 — Own filesystem

**Now the next milestone**, pulled forward, because things that should not
be in the kernel image keep ending up there. The attempt to avoid that with
QEMU's fw_cfg is written up under "what was tried" below.



FAT32 is enough to boot and it is horrible: no attributes, no journaling.

**What is already built, and is not this milestone.** The protocol -
`list`, `read`, `write`, `getattr`, `setattr` - is done and proven, the
namespace and mount table are done, and **attributes, the index over them
and live queries all work**: `attr`, `find` and `watch`, with a watcher
parked in a blocked call until the answer changes. `/data` is a real
server answering the real protocol. What is missing is that it is in
memory.

So this milestone adds exactly one thing: **persistence**. That is worth
stating plainly, because it means the on-disk format already has a
specification - it has to hold the node model the ramfs proved, and
nothing more.

**What gets built**

- An own format with native typed attributes
- **Name, size and modification date always indexed**, for every file, without anyone declaring them. It is what BFS did and it is why a query by name was fast regardless of how many files there were. Other attributes get indexed when declared
- Journaling, with writes batched into the journal before going to their final location. Beyond protecting the structure, **it improves performance**: disks are good at writing large blocks, and writing 100K costs almost the same as writing 1K
- Format and fsck tools

**Three decisions taken before any of it was written.**

*The block driver goes in the kernel HAL*, beside virtio-input, and the
filesystem server reads blocks through a syscall. A driver in userland is
the claim this project makes and it is the right long-term answer, but it
needs MMIO mapped into a process and interrupts delivered to one, neither
of which exists - and building both here would make this milestone about
driver infrastructure instead of about the filesystem. It moves out when
there is a second driver to shape the interface, which is what `hal.md`
already says about writing an interface against one implementation.

*The indices are rebuilt at mount, not stored on disk.* BFS kept them as
on-disk B+trees because it had hundreds of thousands of files. This has
hundreds. A journaled B+tree with split and merge is the largest and most
error-prone component in the whole milestone, and dropping it costs a scan
of the attributes at mount into the same `index[attribute][value] -> paths`
the ramfs already builds. It is the DR8 warning below applied one level
down: the trap is building the general machine before there is a load that
needs it. When mount time hurts, that is the moment to persist them, with a
measurement saying so.

*Files first, the journal second.* `/data` surviving a reboot without a
journal is a real checkpoint and a shorter road to one. The format is laid
out with the journal's space reserved from the start so nothing has to move
later.

**How a large file is delivered, which the two walls below made urgent.** A
`read` that returns bytes cannot carry one, and the mechanism that can now
exists: `sys.memory` makes a region, the capability travels in the reply,
and the client maps it. Same move as a shared surface - the file's pages
exist once and both processes see them. Small reads stay strings; this is
for the case that used to fail. The ceiling is `MEMOBJ_MAX`, sixteen
regions, so it is a handful of large files at once rather than an arbitrary
number.

**What does NOT get built:** a relational database underneath the filesystem. BeOS tried it through DR8 and pulled it at DR9 because maintaining it was hideously complex and cost too much performance. They lost very little functionality replacing it with a filesystem *shaped like* a database. It is the most useful warning that project left behind. See `beos.md` §17.1

**What was tried, and why it is not the answer.** QEMU's fw_cfg can hand
files to a guest at boot - `-fw_cfg name=opt/kosmos/x.png,file=x.png` - and
it worked for small ones. Two walls, and both say the same thing:

1. **fw_cfg cannot seek.** Selecting an item resets its offset, so reading
   the two hundredth page means re-reading the first hundred and ninety
   nine. A megabyte in four-kilobyte pages is about a hundred megabytes of
   DMA.
2. **A megabyte will not fit through a Lua string.** `fs.read` accumulates
   one and a process's heap is 2 MB by design, so a 936 KB picture gives
   `error: not enough memory`.

The second is the interesting one, and the filesystem has to answer it: a
`read` that returns bytes cannot be how a large file is delivered. Mapping
pages is what `gfx.surface` and `sys.share` already do, and it is what the
filesystem's read of a large file should do too.

fw_cfg is a *configuration* channel and behaved exactly like one being made
to carry files. It was removed rather than left half-working.

**What it needs first:** a block device. virtio-blk, which is the same shape
as the virtio-input work - a virtqueue, a feature negotiation, an interrupt.

**Definition of done:** cut power to the Pi during a write and have the filesystem mount clean on the way back.

---

## M9 — Software 3D demo

An earned reward, and the best end-to-end stress test before Doom.

**What gets built**

- Rotozoomer (~200 lines)
- Tunnel with a lookup table (~200 lines)
- Cube with a z-buffer: projection, backface culling, triangle rasterization, flat shading (~500 lines of C)
- All in 16.16 fixed point, no floats
- Parameters in Lua, pixel loop in C

**Definition of done:** a steady 60fps, without a single stutter for a minute. And changing the rotation speed from the REPL with the demo running.

If there is a hiccup every two seconds, it is the Lua GC. Zero allocations in the loop, pre-allocated tables, `collectgarbage("step", n)` after the flip.

---

## M10 — Doom

**What gets built**

- `doomgeneric`: five functions (`DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`, `DG_GetKey`)
- Extended libc: `malloc`/`free` over the process heap, `printf`, `abort`, `qsort`, `fopen`/`fread`/`fwrite`/`fclose`
- **The rule that holds the POSIX line:** every I/O function is a call into the process's namespace and nothing else. No fallback to a global tree. Never `fork`, `exec`, `signal`, `pipe`, `socket`, `select`, `ioctl`. See `design.md` §17
- `errno` per process, in the state struct, not global (with coroutines a global does not work)
- ~~Lazy FP save in the context switch~~ - **done, Sep 2026**, ahead of the rest of M10. `context_switch` 9.812 -> 6.875 ticks and `ipc_roundtrip` 36.251 -> 30.376. See `arch/aarch64/fp.c`
- Loading the 4MB WAD from the filesystem

**Definition of done:** Doom at 35fps in an app server window, on real hardware.

**Why it matters:** Doom is the first userland process that is not Lua. It proves the message boundary is a real boundary and not a language convention. If Doom runs as a normal citizen, with its own namespace and declared capabilities, Kosmos is an OS and not a Lua runtime with pretensions.

---

## M13 — The desktop

The point at which Kosmos stops looking like a system that can draw windows
and starts looking like one you would use.

**Why this comes before Doom.** Doom proves the machine is fast. It proves
nothing about whether the machine is *a desktop*, and a system with no menus,
no scrollbars and windows that cannot be resized is not one however many
frames a second it manages. The order is deliberate: furniture first, then
the thing that shows off.

**The inspiration is QNX Photon, and specifically not its shading.** What
makes a Photon screenshot read as a real operating system is not the grey
bevels, it is that every window has the full set of furniture - a menu bar,
a toolbar, scrollbars, a status bar; lists with columns you can sort; a tree
with expand arrows; a splitter between panes. That is behaviour and
completeness, which `ui.md` 16.8b says to copy. The shading is decided
fresh, and 16.8b now says dimensional: raised, sunken, grooved.

**What gets built, in the order that unlocks the most**

1. **Resize, and the window controls.** Nothing reads as a desktop while
   windows are a fixed size - `ui.lua` currently says so in a comment, that
   `width` is "an honest answer while windows cannot be resized". Splitters
   and panes are meaningless without it, so it is first. Minimise and
   maximise come with it, and the frame grows a grip.

2. **The overlay layer, and then menus.** The hard part is not drawing a
   menu, it is that a dropdown must appear *above other windows* and outside
   its own window's frame. That is a compositing and input-routing question
   the window manager owns, and it is the same mechanism a context menu, a
   combo box and a tooltip all need - so it is designed once, deliberately,
   before any of them.

   The constraint that decides the design: **a hung application must not be
   able to wedge the desktop.** That is M6's definition of done and it is
   not negotiable for a menu either.

3. **Scrollbars.** Lists and text views already scroll and never say so;
   `procs` grew a hand-rolled scroll offset the day this milestone was
   written. A real widget, sunken trough and raised thumb.

4. **The columned list, and the tree.** A list with headers you can click to
   sort, and a tree with expand/collapse. These two together are what a file
   manager is made of, which is why they are one item.

5. **The toolbar and the status bar.** Cheap once the rest exists, and they
   are most of what makes a window look finished.

6. **Tracker.** A real file manager, which is the app that proves the rest:
   a tree of places on the left, a columned listing on the right, a splitter
   between them, a toolbar above and a status bar below. `deskbar.lua`
   already explains that Tracker is the file manager and that Kosmos does
   not have one yet. This is that.

**Definition of done:** open Tracker, resize its window with the mouse, drag
the splitter, sort the listing by clicking a column header, scroll it with
the scrollbar, and open a file from a menu - with a second application open
behind it whose menu drops over Tracker's window and dismisses correctly.
And `make frames` shows a composing pass no worse than it is today.

That last clause is the one with teeth. A bevel is more pixels per widget
than a flat rectangle and composing is already 83% of a busy pass, so this
milestone is the first that could plausibly make the desktop slower. It has
a profiler now; it has no excuse.

---

## M11 — Drivers, by interest

No fixed order. Picked up as the appetite appears.

- **GPIO, I2C, SPI.** On the Pi 5 this requires an RP1 driver over PCIe, which is serious work. On the Pi 1 it is direct.
- **USB: XHCI + HID.** A real keyboard. Tedious but bounded and well documented.
- **Networking. Done as far as ping, and not with lwIP.** `hal/qemu-virt/net.c` is virtio-net, `user/servers/net.c` is Ethernet, ARP, IPv4 and ICMP in about seven hundred lines, and `ping 8.8.8.8` answers. lwIP was the plan and was not taken: it is forty thousand lines whose socket API is what `design.md` §17 forbids at system level, and what ping needs is small enough to write and understand. That decision is worth revisiting *at TCP*, which is where the porting cost would buy something - and where the answer may still be no, for the same reason.

  **The line about "a virtio-net driver at EL0 like every other driver" was wrong about the present** and is corrected here rather than quietly: no driver is at EL0. They are all in the HAL, and `architecture.md` says why - a driver in userland needs MMIO mapped into a process, interrupts delivered to one, and DMA memory a process can hand a device, and none of the three exists. That is a milestone about driver infrastructure and it is not this one.

  Still ahead: TCP, then connections exposed as namespace nodes, Plan 9 style. The property worth testing is unchanged - mount another machine's `/net` into your local namespace, and have a process use that computer's network without knowing.

- **Audio: virtio-sound.** See below; it moved out of the out-of-scope list and it is worth saying why rather than quietly editing the line.

**Out of scope, no discussion:** WiFi, Bluetooth, GPU-accelerated 3D, and any system-level POSIX personality.

**Audio used to be on that list, as "audio with decent latency", and the qualifier is what changed.** What the line was protecting was a *quality bar* rather than the existence of a driver: everything else on it is a subsystem where the hard part is the standard rather than the code, and for audio the hard part is bounded jitter. That concern is real and specific here - `gc_pause_max` is about 1.25 ms, and a mixer that misses a refill deadline does not degrade, it clicks.

But the line was drawn in the wrong place. A virtio-sound driver is a bounded, documented driver on a transport this system already speaks - `input.c` and `blk.c` already do the queue setup, the feature negotiation and the interrupt path - which is exactly what "drivers, by interest" means. Refusing it was refusing the cheap half because the expensive half is expensive.

So: **audio is in scope, and *guaranteed* latency is not.** There is no promise about worst-case jitter and there will not be one until something bounds the collector. What there is instead is a number: `make bench` reports the worst refill it saw, and a click budget that is written down rather than hoped for. A best-effort mixer that says how often it was late is honest; one that claims to be real-time would not be.

---

## M11a — Sound

Four stages, each testable on its own, and the first two are the ones that
carry no risk to the promise above.

1. **`hal/qemu-virt/snd.c`** - virtio-sound over virtio-mmio. Negotiate,
   claim the output stream, set the format, push periods. Testable with
   QEMU's `wav` audio backend, which writes a file the host can check: no
   speakers, and no ears required to know it worked.
2. **`/dev/audio`** - a server that owns the device and takes PCM from
   whoever holds a capability to it. The same shape as the screen: one
   owner, everybody else asks.
3. **The mixer**, in C, because it is a loop over samples. This is where the
   latency question gets answered with a measurement rather than an opinion,
   and it is the stage that has to report its own worst case.
4. **Doom's `DG_sound_module`.** doomgeneric already has the hook behind
   `FEATURE_SOUND` and ships `i_sdlsound.c` to read. The WAD's `DS*` lumps
   are 11 kHz 8-bit mono, so the work is resample-and-sum into stage 3.

**The definition of done is a number, not a noise.** The permanent test is
not "sound came out" - it is that the refill deadline was met a stated
fraction of the time, measured under `make stress`, because that is the
claim being made and the only one worth defending.

---

## M12 — SSH client

Depends on M11's networking and M6's terminal emulator.

**Why it is worth it:** it is the proof that the Plan 9 networking model works. An SSH client on POSIX opens a socket, calls `select`, manages descriptors and fights with `termios`. On Kosmos:

```lua
local c = fs.read("/net/tcp/clone")
fs.write("/net/tcp/" .. c .. "/ctl", "connect 10.0.0.5!22")
fs.write("/net/tcp/" .. c .. "/data", handshake)
```

No sockets, no `select`, no descriptors. The same protocol as reading a file. And the coroutine gives you channel multiplexing without a state machine.

**What gets built**

- Port **monocypher**: 2,000 lines, public domain, no dependencies, freestanding by design. It brings Curve25519, Ed25519, ChaCha20-Poly1305 and SHA-256. **Do not write your own cryptography.**
- Expose the primitives to Lua as a C library. In pure Lua it would be unusable.
- The SSH protocol in Lua: transport layer, authentication, channels. ~1,500 lines of state machine and parsing, which is where Lua is good.
- **Modern algorithms only.** Curve25519, Ed25519, ChaCha20-Poly1305 and nothing else. If the server does not support them, there is no connection. That removes half the protocol's complexity, which is twenty years of legacy negotiation.

**Definition of done:** an interactive session against a real OpenSSH server, with the terminal responding properly.

**And the criterion that matters more:** mount another machine's `/net` into the local namespace and have the SSH client go out over that machine's network without a line of code changing. That is the Plan 9 property no other system has.

**Natural extension:** mount a remote directory over SSH at `/mnt/server`, and have apps read it like any other path, without knowing there is a network in the middle. That is worth more than the SSH client itself.

---

## Recorded goals

Things asked for that are not milestones of their own. Written down so they
shape what gets built rather than arriving as a surprise late.

| Goal | Where it lands | What it needs first |
|---|---|---|
| **A text editor good enough to write Lua in** | M7's app list, and started early - `/bin/edit.lua` | Nothing. It is the first thing that makes the machine able to change itself without a rebuild, which is the point of the whole design |
| **Lightweight games in Lua** | M9's neighbourhood | The UI kit's input path and a surface a program can draw into fast. The blitter is there; what is missing is a frame loop with a known cost |
| **An HTTP server for personal pages** | M11 drivers, then a stack | A network driver at EL0 like every other driver, then TCP/IP in Lua with the packet loop in C. See §"drivers are processes" in architecture.md - a network card is exactly the case that argues for it |
| **A markdown viewer**, for manuals and tutorials in the system itself | M7's app list | A text view that can hold a paragraph and a few styles. The parser is Lua and small; the part that does not exist is a view that wraps text |
| **Telnet to a remote host**, and then SSH | M11 driver, then a stack, then M12 | Three pieces and they are separable: a virtio-net driver at EL0 like every other driver; TCP/IP; and a client. The stack is the interesting one - the C libraries that exist assume a socket API, a global namespace of ports and `errno`, and this system has none of those. What fits is a stack *as a server*: a process holding the driver's capability, handing out a connection as an endpoint, so "a socket" is a capability like everything else and a program that was not given one cannot open one. Telnet first because it is a line protocol and proves the whole path; SSH is the same path plus cryptography |
| **A software 3D demo** - a teapot in a window | M9 | Nothing about the design forbids it, and it is worth saying why, because it looks like it does. The rule is that *pixel loops* live in C and that no line of Lua computes a pixel offset - not that a program may not produce an image. A triangle rasteriser is exactly the kind of primitive `gfx` is meant to grow: Lua decides where the vertices are, C fills the spans, and the result is a surface the app sends to the compositor like any other drawing. What is missing is the rasteriser, not permission. `jonasgeiler/3d-soft-engine-lua` is a good shape to start from - matrices, projection and culling in Lua, which is where they belong; the part of it that plots pixels is the part that becomes a `gfx` primitive, and the honest way to decide which is to write it in Lua first and measure |

The HTTP server is worth stating as a target rather than a wish, because it
decides an argument that would otherwise be had abstractly: a network stack
is the first thing in this system with a hard latency budget that is not the
display's, and it is the second consumer of the driver model. One driver
proves nothing about a driver interface.

---

## What to do when you get stuck

Every milestone has a point where something does not work and you do not know why. It is normal and it is half the learning.

The order that works:

1. Instrument over UART until you find the last line that executed
2. Check memory barriers and cache maintenance
3. Verify the MMIO address against the datasheet, not against what you remember
4. Compare with QEMU if the bug is on hardware, or the other way around
5. Reduce to the smallest thing that reproduces the problem

And if a milestone stretches past what is tolerable, **the way out is to cut scope, not to abandon**. M6 without SMP is still M6.
