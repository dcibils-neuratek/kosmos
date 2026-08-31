# Kosmos

Microkernel OS with a Lua userland. AArch64.

A personal learning project. There are no users, no compatibility to maintain, no deadline. **Correctness and simplicity always win over delivery speed.**

**Active target today: QEMU `virt` aarch64, and nothing else.** Real hardware (Pi 5, Pi 1) arrives at milestone 2, once the serial cables are here. Do not write Pi code yet, but do respect the `arch/` vs `hal/` separation from now on.

- The layers and how a command crosses them: `docs/architecture.md`
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

`-mgeneral-regs-only` is there because the kernel does not save FP/SIMD registers on a context switch. If something in the kernel needs a float, it is badly designed.

---

## Hard rules

These are not preferences. If a proposal violates one, the proposal is wrong.

**No dynamic allocator in the kernel.** Everything lives in statically declared fixed-size pools: an array of threads, of address spaces, of endpoints. There is no `malloc` or equivalent.

**The kernel does not know what a file is.** Threads, address spaces, IPC, capabilities. Nothing else. No networking, no graphics, no filesystem, and no Lua inside it from milestone 4 onward.

**The kernel stays small, and 10k lines of code is the smoke alarm — not the rule.** The rules are the two above it: the kernel knows about threads, address spaces, IPC and capabilities and nothing else, and it has no allocator. Those are what must hold. The line count is a symptom worth watching, and `make size` reports it (code only; this codebase is more than half comments on purpose, and a budget that counted them would ask for worse code in order to satisfy itself).

If the number goes up because something crept in that does not belong, the answer is to take that thing out — and it would have been wrong at any size. If it goes up because what legitimately belongs there needed more code, that is not a problem to solve. **Do not move a thing out of the kernel to satisfy a number.** Move it out because it does not belong.

**Compatibility inside a process yes, at system level never.** A libc that lives in the app's address space and whose I/O functions resolve against that process's namespace is fine and necessary. What is forbidden is a POSIX personality: `fork`, `exec`, `signal`, `pipe`, `socket`, `select`, `ioctl`, `unistd.h`, global file descriptors, or any path tree reachable without a namespace. If a port asks for one of those, patch the port. Detail: `errno` is per-process, never global. See `docs/design.md` §17.

**MMIO only through `mmio_read32` / `mmio_write32`.** They carry the barriers inside. No loose `volatile`.

**No precompiled Lua bytecode.** Source only. The bytecode loader verifies nothing and gives arbitrary execution.

**Capabilities by index, never global IDs.** A syscall takes an index into the process's table. If a design needs to name something globally, the design is wrong.

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

**C:** the whole kernel, the Lua interpreter, the `lua_State` allocator, syscall bindings, the minimal libc, the table serializer for IPC. And only when measurement justifies it: the blitter, the font rasterizer, framebuffer drivers.

**Lua:** the namespace server, filesystem servers, the entire app server including compositing, the shell, the REPL, init, supervision, every app, the whole UI kit.

**Do not push things down to C "because it is faster" without a profile that justifies it.** Every time something is pushed to C, hot reload is lost, and hot reload is the reason for the entire design.

The legitimate exception is pixel loops: never in Lua. Lua decides what gets drawn and where, the loop happens inside a surface, in C.

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
