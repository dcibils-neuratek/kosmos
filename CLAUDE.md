# Kosmos

Microkernel OS with a Lua userland. AArch64.

**Two names, and they are not interchangeable.** *Kosmos* is the operating
system: the servers, the namespace, the desktop, the applications, the
userland. *Nebula* is the microkernel it runs on - threads, address spaces,
IPC, capabilities, and nothing else. Almost everything in these documents
that says "the kernel" means Nebula, and almost everything that says "the
system" means Kosmos.

**Versions are `major.minor.revision`, in the `VERSION` file.** A revision
per push, a minor per milestone, and a major when we decide something was
big enough to be one. `make bump`, `make bump-minor`, `make bump-major`.

A personal learning project. There are no users, no compatibility to maintain, no deadline. **Correctness and simplicity always win over delivery speed.**

---

## What this is aiming at

**A blazing fast, responsive graphical operating system.** Not a shell that
grew a window manager - that is where it started and it is no longer what it
is. The bet is BeOS's, updated: a desktop that feels instant on hardware that
exists now, with QNX's answers where BeOS did not have them.

Four things, and they are the order of preference when two of them disagree:

- **Fast.** A microkernel with fast servers, and a UI with no slow paths in
  it. Where something is slow, measure it and move the byte loops to C; where
  it is fast enough, leave it in Lua.
- **Responsive.** Bounded, not merely quick on average. Input reaches the
  thing that draws without waiting behind whatever else is running.
- **Secure.** Capabilities, no global names, no ambient authority. What you
  were not handed, you cannot reach.
- **Scalable.** It has to still be true when the machine is busy, which is
  what `make stress` is for.

**The target is a Raspberry Pi 5**, and it is chosen to be hard: a fast UI on
it is a real result rather than a QEMU number.

**We know an OS can be built here. The remaining question is whether it can
be a fast one.** That is what to optimise for now - not more features, and
not another subsystem, but the speed and the feel of the ones that exist.

**Active target today: QEMU `virt` aarch64, and nothing else.** Real hardware (Pi 5, Pi 1) arrives at milestone 2, once the serial cables are here. Do not write Pi code yet, but do respect the `arch/` vs `hal/` separation from now on.

- The layers and how a command crosses them: `docs/architecture.md`
- What lives where, in the tree and at runtime: `docs/layout.md`
- Design and the reasoning behind every decision: `docs/design.md`
- Current state and next step: `docs/state.md` — **read it before proposing anything**
- Milestones: `docs/roadmap.md`
- Targets and HAL: `docs/hal.md`
- UI kit and window manager: `docs/ui.md`
- The path pixels take: `docs/gfx.md`
- Measurement and regressions: `docs/testing.md`
- BeOS lineage: `docs/beos.md`
- Toolchain and build: `docs/setup.md`
- Glossary: `docs/glossary.md`

---

## Licence and the header every file carries

**Kosmos is MIT, and the terms are in `LICENSE`.**

**Every file this project writes opens with one line saying so**, above its
descriptive comment and below nothing:

```
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.     (Lua)
/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */  (C)
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.     (Python, sh)
```

One line, not a fifteen-line block. The full text lives in `LICENSE` and
repeating it in three hundred files would be three hundred copies to keep
in step.

**Vendored files do not get it.** `lua/upstream/`, `runtime/upstream/` and
`assets/` carry their authors' notices exactly as shipped, and adding a
line to them would be modifying them - which is the one thing the rule
about vendored code forbids. Their licences are named in `LICENSE`.

**`-- kosmos: application` may sit anywhere in the opening comment block**,
not only on the first line. It used to have to be first, and the copyright
line above it turned every application in `/bin` into a console program
with an empty Deskbar to show for it.

---

## Language

**Everything in this repository is written in English.** Documents, source code, comments, identifiers, commit messages, test names, log output, error strings. No exceptions, including in Lua code and in strings printed over the UART.

---

## Build

```
make qemu        # build and run under QEMU virt, in a window
make FB=1920x1080 qemu   # the same, at that display size
make serial      # the same, serial only, no window
make test        # run the suite under QEMU, exit code 0 or 1
make screenshot  # boot, screendump, and check the picture QEMU scans out
make bench       # the benchmarks, under -icount
make debug       # QEMU with a gdbserver on :1234
make clean
```

Toolchain: `aarch64-none-elf-gcc`, `qemu-system-aarch64`.

Mandatory flags, not to be changed without discussion:

```
-std=c11 -ffreestanding -nostdlib -nostartfiles
-Wall -Wextra -Werror -fno-common -fno-strict-aliasing
-mgeneral-regs-only
```

`-mgeneral-regs-only` is there because **the kernel may not touch an FP or SIMD register at all**, and the flag turns that from a promise into a compile error. If something in the kernel needs a float, it is badly designed.

That rule is what makes lazy FP save possible. The registers are not saved by the context switch: the switch disarms them (`CPACR_EL1.FPEN`) and the first floating-point instruction after it traps, at which point `arch/aarch64/fp.c` writes the previous owner's registers into its context and reads the new owner's back. A thread that never touches FP - which is every kernel thread, precisely because of this flag - never faults and never pays. `context_switch` is 29.9% faster for it and `ipc_roundtrip` 16.2%, against one extra comparison on every exception.

The two exceptions at EL1 are `setjmp` and `longjmp`, which save and restore `d8`-`d15` because AAPCS64 makes them callee-saved. They trap and are served like anything else, which is why the trap is armed for both privilege levels rather than EL0 alone.

---

## Principles

**This is a learning project, and these are principles rather than laws.**
They are the positions this system has taken and the reasons it took them,
and they are load-bearing: most of what makes Kosmos interesting follows
from them, and the temptation to break one is usually the moment before
learning why it was there.

So the bar for departing from one is not "never". It is: say which principle
is in the way, say what it is costing, and decide it deliberately. What is
not allowed is drift — quietly working around a principle, or violating one
without noticing.

And a principle that turns out to be wrong gets *changed*, here, with the
reasoning recorded. That has already happened more than once. What must not
happen is a principle that is still written down and no longer true.

Practically, for me: do not answer a proposal by citing one of these as
though it settled the matter. Say what the principle is protecting and
whether that applies here. If it does not, say so.

**No dynamic allocator in the kernel.** Everything lives in statically declared fixed-size pools: an array of threads, of address spaces, of endpoints. There is no `malloc` or equivalent.

**The kernel does not know what a file is.** Threads, address spaces, IPC, capabilities. Nothing else. No networking, no graphics, no filesystem, and no Lua inside it from milestone 4 onward.

**The kernel stays small, and 10k lines of code is the smoke alarm — not the rule.** The rules are the two above it: the kernel knows about threads, address spaces, IPC and capabilities and nothing else, and it has no allocator. Those are what must hold. The line count is a symptom worth watching, and `make size` reports it (code only; this codebase is more than half comments on purpose, and a budget that counted them would ask for worse code in order to satisfy itself).

If the number goes up because something crept in that does not belong, the answer is to take that thing out — and it would have been wrong at any size. If it goes up because what legitimately belongs there needed more code, that is not a problem to solve. **Do not move a thing out of the kernel to satisfy a number.** Move it out because it does not belong.

**Compatibility inside a process yes, at system level never.** A libc that lives in the app's address space and whose I/O functions resolve against that process's namespace is fine and necessary. What is forbidden is a POSIX personality: `fork`, `exec`, `signal`, `pipe`, `socket`, `select`, `ioctl`, `unistd.h`, global file descriptors, or any path tree reachable without a namespace. If a port asks for one of those, patch the port. Detail: `errno` is per-process, never global. See `docs/design.md` §17.

**Control by message, data by shared memory.** A message says *what to do*;
a region holds *what to do it to*. **A stream of data never travels as a
message payload** - not audio periods, not video frames, not packets, not
disk blocks.

The test is the rate: **if it recurs because the hardware says so - a frame,
a period, a packet, a block - the bytes live in a region and the message
carries only where and how much.** One-shot payloads are fine and always
were: a `getattr` reply, a keystroke, a line of text. Those happen because
somebody asked, not because a clock came round.

This was learned the expensive way and the evidence is in the tree twice
over. The window manager takes a *shared surface* from an application and
the message says "buffer 2 is live" - which is why `cube3d` holds 60 frames
a second. The audio server took a 1024-byte period *as a message payload*
172 times a second, which meant a Lua string allocated on each side of every
one: 340 KB a second of garbage manufactured inside a 5.8 ms deadline, with
`gc_pause_max` at 1.25 ms. It played, and it jittered, and no amount of
scheduling fixed it because the collector was in the audio path by
construction.

It happened because `MSG_BYTES` is 2048 and a period is 1024. **It fit.**
Fitting is not a reason - it is the trap, and the rule exists so that the
next subsystem does not walk into it.

This is the same law BeOS and QNX both arrived at. BeOS's media server is a
matchmaker: once two nodes are connected the buffers live in a shared
`BBufferGroup` and what passes between them is a buffer id and a timestamp,
with the server out of the loop entirely. QNX's `io-audio` puts samples in
shared memory and sends only control. Neither of them is a system that could
afford to be sentimental about it.

What the rule costs, and it is real: a region is harder to reason about than
a message. Messages are copied and therefore safe; a region is two processes
looking at the same bytes. So the discipline that comes with it is
**single-producer, single-consumer rings with indices** - one side only
writes, one side only reads, nobody takes a lock, and the indices are the
only thing both touch. Anything that wants more than that wants a message.

**MMIO only through `mmio_read32` / `mmio_write32`.** They carry the barriers inside. No loose `volatile`.

**No precompiled Lua bytecode.** Source only. The bytecode loader verifies nothing and gives arbitrary execution.

**A server receives exactly what it expects, not whatever somebody put in a
table.** It is a system component: the thing on the other side of that
message has to stay correct when the caller is wrong, out of date, or
hostile, and it does not get to assume otherwise.

So a boundary is an agreement rather than a conversation. Inside a program a
Lua table is exactly right - adding a field costs nothing and nobody has to
be told. Crossing into the system, what crosses is a *declared shape*: fixed
fields, fixed sizes, in a header both sides compile against.
`user/include/audioproto.h` is the first of them.

**This is the same argument as capabilities, one layer up.** A capability
means you cannot *name* what you were not handed. A struct means you cannot
*say* what the protocol has no field for. Both replace a server checking
what arrived with a shape that could not have arrived wrong, which is the
difference between a system that validates and one that is correct by
construction.

A table lets a caller send any shape at all - an extra key, a wrong type, a
nested table, a megabyte of string - and every one of those is something the
server must think about. A 48-byte struct means most of them cannot be
expressed, so the wire refuses them and the server never has to.

What it costs is real and is not hidden: adding a field means editing a
header and rebuilding both sides, and an error is a number with the sentence
composed by whoever shows it to a person. **Only `/dev/audio` speaks this way
today**; the rest still take tables and are being moved one at a time, each
lived with before the next is started.

**Capabilities by index, never global IDs.** A syscall takes an index into the process's table. If a design needs to name something globally, the design is wrong.

**Responsiveness is a design goal, not a later optimisation.** Kosmos is a
reimagining of BeOS for machines that exist now - the same bet that a
desktop should feel instant - with what has been learned since, and it is
free to take good ideas from anywhere because it owes compatibility to
nothing. It is not POSIX and never will be, so it inherits none of POSIX's
scheduling vocabulary and none of its constraints.

The scheduler is where that shows first. **Strict priority bands with
immediate preemption**, borrowed from QNX, which is a microkernel of the
same shape and a real-time system - and real-time does not mean fast, it
means *bounded*: the highest-priority ready thread starts within a known
worst case, every time. Two of its ideas are worth having here and cost
almost nothing:

- A thread that becomes ready and outranks the running one takes the CPU at
  the next exception, not when a quantum expires. Waking used to only
  enqueue, so an input event could sit behind a compute-bound thread for a
  hundred milliseconds.
- The quantum is a variable that can be changed and measured, not a constant
  compiled in and never questioned.

Not borrowed: 256 priority levels, where five say everything this system has
to say, and hard guarantees, which would mean bounding every kernel
operation. Kosmos wants a desktop that feels alive, not an airbag that fires
in time.

**Single-core until milestone 6.** But the code is written SMP-ready from now: no loose mutable globals, `TPIDR_EL1` as the pointer to the per-CPU struct, a per-CPU runqueue even with a single CPU.

**No hardware addresses outside `hal/`.** Not one.

**Upstream Lua is not modified.** It lives in `lua/upstream/` exactly as shipped. Freestanding changes go in `lua/patches/` and are applied during the build.

**Vendored data is not modified either, and carries its licence.** `assets/fonts/` holds the BDF the font's author ships, byte for byte, next to that font's licence text; `tools/bdf2c.py` converts it during the build. The rule is the same one `lua/upstream/` follows: what is in the tree should be what the author released, and everything done to it should be a build step somebody can read. Anything vendored records its licence beside it, and the generated file repeats the notice.

---

## app or program

- **program**: console-based. Prints, reads lines, run by name at the prompt.
- **app**: graphical. Opens a window, driven with the pointer, listed in the
  Deskbar, and marked `-- kosmos: application` on its first line.

Graphical is the intended way to use this system. "Let us build an app"
means a window, every time.

---

## Language split

**C is what runs on behalf of another process. Lua is what runs for a
person.**

- **C:** the kernel, `arch/`, `hal/`, drivers, **servers**, kits. Plus the
  Lua interpreter, the `lua_State` allocator, the syscall bindings, the
  minimal libc and the table serialiser.
- **Lua:** applications, programs, and the libraries they use.

The line is a *layer*, and that is the point of it. It used to be a
judgement - "any server on the frame or the packet path" - and a judgement
has to be made again at every new subsystem by somebody who remembers to
make it.

**Nobody remembered, and the audio server is what it cost.** It sits on the
period path, which is the frame path with a different clock, and the rule
named two paths so a third went unnoticed. Written in Lua, it then produced
in one day: a spin, because `return true` was wrong in a way nothing could
catch; two Lua strings allocated per period inside a 5.8 ms deadline, which
is what the shared-ring refactor exists to undo; no way to print a
diagnostic, because a server is spawned with one capability; and no way to
read the counter frequency, because it has no namespace to read `/dev/cpu`
from. Every one of those is a consequence of being a Lua process rather than
of anything the audio server was trying to do.

**A rule that requires you to recognise a third case will miss the fourth.**
So the layer decides, and the reason travels with it: if something else's
correctness or timing depends on you, you do not get a garbage collector.

**The namespace is not one of them, and calling it a server was an error
this file made for a long time.** `glossary.md` is exact - *a kit is code you
run; a server is someone you ask* - and `new_namespace` is run, in the
caller's own process, five separate times. It has no endpoint and no thread.
Nobody asks it anything. It is a **kit**, it is Lua, and it is already on the
right side of the line; moving it to C would be moving it to the wrong one.

Worth recording rather than quietly corrected, because the mislabel did
work: it appeared in this file as "the policy servers - the namespace, init
and supervision, the `/app` registry", and an argument for converting four
thousand lines was built on top of it before anybody looked at what
`new_namespace` actually is. A name in a document is a claim, and this one
went unchecked for months.

**What the move does cost, because it is not free.** The servers that hold
policy rather than a deadline - `binfs`, `appfs`, `devices` - are paths,
names and tables, and that is precisely where C buys the least and risks the
most. A Lua server cannot have a buffer overflow; the blast radius
is identical either way, since both are EL0 processes behind an address
space, but one of those bugs is a stack trace and the other is an evening.
That argument was right when it was written and it has not stopped being
right. What changed is that it was being used to justify a *judgement* about
which servers, and the judgement is what failed.

Two things make the move survivable that did not hold before: messages are
fixed-size and the serialiser is already C, so string handling here has a
bounded shape rather than an open one; and hot reload has since been removed
outright, so the old objection to a C server is gone.

**They have all moved, one at a time, and the order was the point.** Audio
first, because it is on a deadline and it is what proved the point; then the
small ones - `devices`, `appfs` - where the protocol convention got its
second try while it was still cheap to find it wrong; then `binfs` and
`libfs`; then `console`, which needed a kit because a terminal implements the
same ABI; then `ramfs`, which cost hot reload.

**Living with each before starting the next is what found the bugs**, and
every one of them was a thing no amount of reading would have shown: a reply
that dropped the pointer's range, a share that did not carry its protocol,
five servers that had quietly stopped naming themselves, and a `/data` that
had always stored Lua values rather than bytes.

**`diskfs` is the one left, and the one to leave alone**, for a reason that
has nothing to do with its size. Its core is `kfs.lua`, which runs on the host *and* the
guest, and that is what lets `make test` check the filesystem format and the
journal's power-loss window without booting a machine - at an exact instant
a SIGKILL aimed at a running QEMU hits only by luck. Rewriting it in C
throws that away. It may still be right one day; it is not a consequence of
"servers are C".

**The argument for C is jitter, not speed.** Structure-shaped code in Lua
costs about 2%, measured, which is nothing. What decides a server is
`gc_pause_max`: about 1.25 ms, arriving when the collector chooses. A frame
is 16 ms. No amount of optimising the Lua removes that, and responsiveness is
a promise about the worst case rather than the average.

**Hot reload is gone, and "gone" is the word.** It was real - M5 proved it,
with a server's code replaced while a client was mid-conversation with it -
and it was first demoted below speed, on the grounds that a C server cannot
be reloaded because there is no dynamic linking.

Then the disagreement stopped happening, because the servers ran out. Seven
moved to C and `ramfs` was the last one that could be reloaded. It was
converted in September 2026 knowing the price: `ROLE_RELOAD` deleted, and
`help("demos")`'s watchable reload with it.

**Do not describe this as a tiebreaker any more.** `fs.reload` does not
exist, `serve` has no reload branch, and nothing in the running system can
have its code replaced. `design.md` §10 is the record. What survives is the
shape - `serve` still takes a factory, so state and behaviour are separate -
and level 2, where a supervisor restarts a server that *died*, which never
depended on the language anything was written in.

**What keeps a policy server in Lua, then, is the shape of its bug.** A Lua
server cannot have a buffer overflow. The blast radius is identical either
way - both are EL0 processes behind an address space, and neither can touch
another - but one of those bugs is a stack trace and the other is an evening.
So C is for a server whose hot loop is *small and bounded*: a scanner, a
blitter, a packet path. Four thousand lines of policy rewritten in C would be
string handling and table lookups, which is where overflows live and where C
buys nothing.

**The window manager is the open case, and it is a measurement rather than an
opinion.** Its pixel work is already C; its Lua half is layout, focus, damage
and event routing - 1,422 lines of it. Whether that is five per cent of a
frame or fifty is now measurable rather than arguable: `frames` starts the
window manager's own stage counters and prints where a pass went. Profile
before rewriting it.

`docs/glossary.md` defines what a server, a kit, a library, a program, an app
and a tool each are, and the distinction that does the most work: **a kit is
code you run, a server is someone you ask.**

**Do not push things down to C "because it is faster" without a profile that justifies it.** Without a number you cannot tell whether you bought anything, and structure-shaped Lua costs about 2% - 2% of nothing is nothing.

**The question that decides is: is it a loop over bytes, or does it sit where a collector pause would be felt?** If neither, C buys about 2% and the answer is no.

Reload used to be the other half of this, and is not any more - there is nothing left to reload. What replaced it as the cost of C is the shape of the bug: a Lua module cannot have a buffer overflow, and 2% is not worth an evening with a debugger. That argument still says yes to a scanner, a blitter and a Huffman decoder, and no to four thousand lines of paths and table lookups.

The legitimate exception is pixel loops: never in Lua. Lua decides what gets drawn and where, the loop happens inside a surface, in C.

**Those C libraries are kits**, and they are reached through the namespace: `use("/kits/pdf")` gets a table the runtime built, exactly as `use("/lib/ui.lua")` gets one a Lua file returned. The caller writes the same line either way, because which language something is written in is not a fact its user should have to know - and a library whose hot loop later moves into C should not change a single call site.

The name is BeOS's and so is the idea (Interface Kit, Storage Kit, Media Kit, Translation Kit). Reaching them through the namespace rather than as globals keeps the rule everything else obeys: **what you were not given, you do not have.** `kits` at the prompt lists them.

Measured, on a page of a real PDF: the scanner in Lua took 538 ms and in C takes 4.7 ms, for identical output. That is the shape of the argument - not that C is faster, but that a scanner is the kind of thing where 110x is available and nothing is given up to take it.

---

## `arch/` vs `hal/`

A distinction that matters and is easy to blur:

- **`arch/`** is "which CPU are you". Page tables, the exception vector, context switch, barriers. It is not abstracted across architectures, it is reimplemented.
- **`hal/`** is "which peripheral do you have". UART, timer, interrupt controller, framebuffer. Common interface, one implementation per board.

The HAL interface as it actually stands. Every entry arrived with the milestone that needed it and none of it was written ahead of a caller:

```c
void          hal_early_init(void);

void          hal_putchar(char c);          /* M0 */
int           hal_getchar(void);            /* M5, non-blocking */
void          hal_ram_range(struct memrange *out);

void          hal_irq_init(void);           /* M1 */
void          hal_irq_handle(void);         /* called from the IRQ vector */

void          hal_timer_init(unsigned hz);  /* M1 */
unsigned long hal_ticks(void);
unsigned long hal_ticks_missed(void);       /* deadlines that came and went */

bool          hal_fb_init(struct fb *out);  /* M6; false when there is no screen */
bool          hal_keyboard_init(void);      /* M6; false when there is none  */
bool          hal_pointer_init(void);       /* M6; false when there is none  */
bool          hal_pointer_poll(struct pointer_state *out);
```

There is deliberately no `hal_keyboard_getchar`. A keyboard is a source of characters and `hal_getchar` is where characters come from, so the board answers from whichever of its sources has one. Nothing above the HAL changes because a keyboard exists.

The pointer *does* get its own pair, and the difference is the point: a character can come from any of several sources and be the same character, so merging them costs nothing. A position cannot. It has exactly one source, and merging two would mean choosing between them - a choice that does not exist until there is a board with two pointing devices.

`hal_pointer_poll` reports in the **device's own units, with the range beside them**, and does not scale to the screen. Same division as `sysinfo` and its raw ID registers: this layer says what the hardware said. Only the window manager knows how big the screen is, so only the window manager can scale.

`hal/hal.h` is the authority. If this list and that file disagree, that file is right and this one is stale — say so.

**Do not expand the HAL speculatively.** The right interface appears once there is a second real target, at milestone 2. Writing it now with a single target produces the shape of QEMU with generic names.

`hal_fb_init` is deliberately "ask the firmware for a linear framebuffer, and let it choose where the pixels live", because that is the one operation QEMU's ramfb and the Pi's mailbox both perform. virtio-gpu does not fit it — it needs an explicit flush after drawing — and that is precisely why adding virtio-gpu is what will grow the interface a `hal_fb_flush`, with two implementations in front of it rather than one.

---

## How to work here

**One milestone at a time.** See `docs/state.md`. Do not propose or implement things from future milestones even when they look obvious or cheap.

**Small, verifiable scope.** "The exception vector with its 16-entry table and the sync handler" is good scope. "Implement the microkernel" is not.

**Verify hardware addresses and offsets against the datasheet, not against memory.** If you do not have the datum, say so instead of inventing a plausible offset. This is the area where code that looks right and does not work is indistinguishable from code that does, until it runs.

**Memory barriers: state which one and why.** Do not sprinkle `dsb sy` just in case.

**Before writing new code, read what already exists.** The codebase is small and readable on purpose.

**When something does not work, instrument over UART first.** A well-placed `printf` beats a hypothesis.

**Every closed milestone leaves a permanent test.** The definition of done becomes a test that is never deleted. `make test` must pass before the next milestone starts. See `docs/testing.md`.

**Pixels never go inside a Lua table.** A surface is a userdata over flat bytes. A Lua array holding 2M pixels makes the GC walk 2M slots per cycle and the system falls apart. See `gfx.md` §19.1.

**Nothing in Lua computes a pixel offset.** The pitch is almost never `width * 4`. All address arithmetic happens inside the C primitives. See `gfx.md` §19.3.

**QEMU numbers are not performance numbers.** They are for detecting regressions (with `-icount`, which is deterministic), not for knowing whether something is fast. The PMU is not faithfully emulated under QEMU. Do not optimize against QEMU.

**A binary that leaves this machine has been used for a while first.**
`make test` says the parts work and `make screenshot` says the machine works
*once*. Neither notices a pool that fills on the fiftieth try - and every
resource bug this system has had was that shape: capability slots gone on the
sixteenth read, a region per font per size, a process table full at round
twenty-two. So `make release` runs `make stress`, which uses the machine hard
and then asks `sysinfo` whether it gave everything back.

Committing a working revision needs none of that. **Publishing a binary
does**, because that is the one somebody runs on another computer without
watching it.

**Before a minor or major version, review the code before adding to it.** Not
a skim: read the files that changed since the last one, as they now are
rather than as they were meant to be. The question is what has no reason to
exist any more - code kept from an idea the system has since replaced,
comments that were true when written and are not now, functions nothing
calls, work done every frame for a result nobody reads.

This is a policy because the alternative was tried. Complexity accumulates by
addition: each piece was reasonable when it arrived, and nothing removes it
when the idea behind it changes. One review pass found two live races in
`memobj.c`, eight kilobytes of a superseded text path in `pdfpage.lua`, a
comment explaining a constraint that no longer existed, and an application
whose event loop called a method that was never there - so it had been dying
on its first pass for a day while appearing to work. None of that was found
by writing more code.

**A version number is the moment to do it**, because it is the only moment
that arrives on its own.

**A push carries a picture. `make prepush`** runs the suites, the display
harness and `make shot`, which puts a 1920x1080 screenshot of the desktop -
Tracker, the widget gallery, Processes, Monitor and the cube, tiled - into
`docs/screenshots/` under the date and the revision.

It is a make target rather than a habit because it is the step that would be
forgotten: nothing fails without it, and a series of these with gaps in it
is worth much less than one without. A commit message says what changed; a
screenshot says what it became, and this is a system whose whole point is
something you look at.

**At the end of a session, update `docs/state.md`.** Without that, the next session starts from zero.

**When a design decision is taken, propagate it to the documents in the same session.** A decision that lives only in chat history is lost. The order: the row in the decision log in `README.md`, the explanation in the matching section of `docs/design.md` (or `ui.md` / `gfx.md` / `hal.md` depending on the topic), and the scope adjustment in `docs/roadmap.md` if it changes what has to be built. If a decision contradicts something already written, correct the old text instead of adding an exception next to it.

---

## Layout

```
boot/           assembly entry, linker script
arch/           aarch64/
hal/            qemu-virt/  (pi5/ and pi1/ arrive at milestone 2)
kernel/         mmu, sched, ipc, caps, exceptions
assets/         vendored data: fonts/ (BDF + its licence), icons/, images/
lua/            upstream/ + kosmos/
runtime/        minimal libc, bindings, serializer, and upstream/
user/           everything at EL0:
  init/           init, the roles, and the namespace kit
  servers/        the servers, in C, one file each
  include/        the protocol headers both sides compile against
  lib/            libraries: Lua, and C kits reached the same way
  bin/            programs and applications, in Lua. Carried in the image
                  and served at /bin
  tests/          the Lua suite
tests/          guest-side tests, in C
bench/          benchmarks and baselines.json
tools/          host-side test runner, scripts
docs/
```

**There is no top-level `servers/`, `apps/` or `lib/`.** There were, holding
nothing but a `.gitkeep` each, and both this file and `README.md` documented
them as the real layout for months - `servers/ Lua: namespace, fs, console,
appserver`, which by the end was wrong in three ways at once: those servers
are C, they live in `user/servers/`, and the namespace is a kit rather than a
server. Removed in the review before 0.8.

