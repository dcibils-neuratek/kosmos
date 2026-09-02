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

**MMIO only through `mmio_read32` / `mmio_write32`.** They carry the barriers inside. No loose `volatile`.

**No precompiled Lua bytecode.** Source only. The bytecode loader verifies nothing and gives arbitrary execution.

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

C is what touches hardware or defines the isolation boundary. Lua is everything else.

The test: **if a bug there can corrupt another process, it is C. If it can only kill its own process, it is Lua.**

**C:** the kernel, drivers, kits, and any server on the frame or the packet
path. Plus the Lua interpreter, the `lua_State` allocator, syscall bindings,
the minimal libc and the table serialiser.

**Lua:** apps, programs, libraries, and the policy servers - the namespace,
init and supervision, the `/app` registry - which decide things and move
almost no bytes.

**The argument for C is jitter, not speed.** Structure-shaped code in Lua
costs about 2%, measured, which is nothing. What decides a server is
`gc_pause_max`: about 1.25 ms, arriving when the collector chooses. A frame
is 16 ms. No amount of optimising the Lua removes that, and responsiveness is
a promise about the worst case rather than the average.

**The argument for Lua is hot reload.** M5's definition of done was replacing
the console server's code while the shell was mid-conversation with it, and
`layout.md` records that there is no dynamic linking - so a C server cannot
be reloaded at all. Moving one to C gives that up rather than trading it,
which is why a server that moves no bytes stays here.

**The window manager is the open case, and it is a measurement rather than an
opinion.** Its pixel work is already C; its Lua half is layout, focus, damage
and event routing. Whether that is five per cent of a frame or fifty is not
known, because nothing measures a frame yet. Profile before rewriting thirty
thousand lines.

`docs/glossary.md` defines what a server, a kit, a library, a program, an app
and a tool each are, and the distinction that does the most work: **a kit is
code you run, a server is someone you ask.**

**Do not push things down to C "because it is faster" without a profile that justifies it.** Every time something is pushed to C, hot reload is lost, and hot reload is the reason for the entire design.

**But notice what that cost is made of, because it is not always there.** The price of C is losing hot reload. A finished algorithm has nothing to reload: JPEG is not going to change, and neither is DEFLATE, or the syntax of a PDF content stream. For those the cost is zero and the speed is free, so they belong in C and the profile is a formality.

The test, then, is two questions rather than one: **is it a loop over bytes, and would you ever want to reload it?** A window manager's layout policy is reloaded constantly. A Huffman decoder never is.

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

**At the end of a session, update `docs/state.md`.** Without that, the next session starts from zero.

**When a design decision is taken, propagate it to the documents in the same session.** A decision that lives only in chat history is lost. The order: the row in the decision log in `README.md`, the explanation in the matching section of `docs/design.md` (or `ui.md` / `gfx.md` / `hal.md` depending on the topic), and the scope adjustment in `docs/roadmap.md` if it changes what has to be built. If a decision contradicts something already written, correct the old text instead of adding an exception next to it.

---

## Layout

```
boot/        assembly entry, linker script
arch/        aarch64/
hal/         qemu-virt/  (pi5/ and pi1/ arrive at milestone 2)
kernel/      mmu, sched, ipc, caps, exceptions
assets/      vendored data: fonts/ (BDF + its licence), converted at build time
user/bin/    programs, in Lua. Carried in the image and served at /bin
lua/         upstream/ + patches/
runtime/     minimal libc, bindings, serializer
servers/     Lua
apps/        Lua
lib/         Lua: ui, gfx
tests/       guest-side tests (C until M2, Lua after that)
bench/       benchmarks and baselines.json
tools/       host-side test runner, scripts
docs/
```
