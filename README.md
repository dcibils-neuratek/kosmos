# Kosmos

A microkernel desktop operating system with a userland written entirely in Lua.

A personal learning project. No users to serve, no compatibility to maintain, no deadline. That is precisely what makes it possible to take decisions a commercial OS cannot take.

Status: **M6, graphics.** A framebuffer, a shell on the screen, and programs
in `/bin`. See [docs/state.md](docs/state.md), and
[docs/architecture.md](docs/architecture.md) for how the layers fit together.

---

## The idea in one paragraph

Kosmos takes the microkernel from QNX, per-process namespaces from Plan 9, attributes and live queries from BeOS, the live image from Lisp Machines, and capabilities from seL4, and puts them on top of a userland written entirely in Lua.

The thesis that makes this a system rather than a collage: **the protocol between servers is the data model of the userland language.** IPC messages are Lua tables. Namespace nodes serve Lua tables. A server is a coroutine that receives a table and returns a table. No marshalling, no IDL, no two worlds.

---

## Why it exists

Three reasons, in order.

**Learning by building.** Writing a kernel is the only way to actually understand what an OS does. Everything else is reading about it.

**Testing ideas that exist but never appear together.** Every piece of Kosmos worked in some real system. None of them had all of it at once, almost always because of commercial constraints that do not apply here.

**Having a first-hand answer to a concrete question:** how much does a system simplify if every resource speaks one protocol and every process sees only what you mounted for it?

On Linux, reading a file, reading a sensor, listing processes, watching for changes and reading metadata are five different APIs. On Kosmos it is one. That is the entire argument.

---

## What it is NOT

**It is not a daily driver, and chasing that would kill the project.** A daily driver needs a browser, and a modern browser is 30 million lines that assume POSIX, threads, a JIT, a GPU and a full network stack. Porting Chromium is more work than the whole system. That was the wall Haiku hit, with 20 years and dozens of contributors.

**It is not a BeOS clone.** BeOS was monolithic, with drivers and the filesystem inside the kernel, and no hot reload. Kosmos takes its concurrency model, its live queries and its design sensibility. The architecture is QNX.

**It does not pursue system-level compatibility.** Computational libraries (SQLite, zlib, monocypher, Doom) can be ported with a libc shim inside the process, subject to its capabilities. What does not fit is a POSIX personality: `fork`, signals, BSD sockets, or a global tree. The moment that exists, the namespace design becomes decorative and the experiment loses its result.

---

## The verifiable goal

**Doom running at 35fps in an app server window, on a Raspberry Pi, next to a REPL where you redefine the window manager while you play.**

Doom is the best OS design benchmark there is: it needs no GPU and no floats on the hot path, but it does need a framebuffer with a fast blit, low-latency input, precise timing, real file reads and app-managed memory. If it runs smooth, the system is sound. If it stutters, there is a real and locatable problem.

---

## The seven principles

If something collides with one of these, the feature gets cut, not the principle.

1. **The kernel does not know what a file is.** Threads, address spaces, IPC, capabilities. Nothing else.
2. **What a process has not mounted does not exist.** Not permission denied — no such path.
3. **One protocol for every resource.** Sensor, process, file, window, network connection.
4. **Share-nothing userland.** One `lua_State` per process. Concurrency complexity stays confined to the kernel.
5. **Isolation comes from the hardware.** An EL0 address space plus capabilities. Lua does not need to be sandboxed.
6. **The system is modified while running.** A server reloads its code without losing state or clients.
7. **Compatibility inside a process, never at system level.** A libc inside an app is necessary and fine. A POSIX personality (`fork`, signals, a global tree, ambient authority) is forbidden. The line is drawn in [design.md](docs/design.md) section 17.

---

## Document map

| Document | What it answers |
|---|---|
| [docs/architecture.md](docs/architecture.md) | **The layers, and one command traced through all of them.** The place to start. |
| [docs/design.md](docs/design.md) | What Kosmos is and **why every decision is the way it is**. The central document. |
| [docs/ui.md](docs/ui.md) | UI kit and window manager. BeOS lineage with the corrections the design allows. |
| [docs/beos.md](docs/beos.md) | The BeOS lineage: what is inherited, what is corrected, and where Kosmos departs on purpose. |
| [docs/gfx.md](docs/gfx.md) | The path pixels take from Lua to the framebuffer, for apps that produce images. |
| [docs/testing.md](docs/testing.md) | How each layer is measured and how regressions are caught milestone by milestone. |
| [docs/hal.md](docs/hal.md) | How `arch/` is separated from `hal/`, the targets, and the trap in each piece of hardware. |
| [docs/roadmap.md](docs/roadmap.md) | The 13 milestones, each with a definition of done. |
| [docs/setup.md](docs/setup.md) | Toolchain, build, how to debug without a debugger. |
| [docs/glossary.md](docs/glossary.md) | The terms. Written to be re-read six months from now. |
| [docs/state.md](docs/state.md) | Where you are today. **Updated at the end of every session.** |
| [CLAUDE.md](CLAUDE.md) | Operating rules for working with Claude Code. |

Reading order if you come back after months: `state.md`, then `design.md`, then whichever one matches the current stage.

---

## Getting started

```
make qemu
```

Toolchain and prerequisites in [docs/setup.md](docs/setup.md).

---

## Layout

```
boot/        assembly entry, linker script
arch/        aarch64/ — what depends on the CPU
hal/         qemu-virt/, pi5/, pi1/ — what depends on the board
kernel/      EL1: mmu, sched, ipc, caps, exceptions
lua/         upstream/ untouched + patches/ for freestanding
runtime/     minimal libc, syscall bindings, serializer
servers/     Lua: namespace, fs, console, appserver
apps/        Lua
lib/         Lua: ui, gfx, and other shared libraries
tests/       guest-side tests (C until M2, Lua after that)
bench/       benchmarks and baselines.json
tools/       host-side test runner, scripts
docs/
```

---

## The criterion for every future decision

**Does this test an idea in the design, or does it just close a feature gap against Linux?**

A filesystem with attributes tests an idea. A Chromium port closes a gap. The first is why the project started. The second is why projects get abandoned.

---

## Decision log

Design decisions taken outside the documents get recorded here before being propagated to `design.md` and `roadmap.md`.

| Date | Decision | Where it landed |
|---|---|---|
| Aug 2026 | Vanilla Lua 5.4 as the userland language, LuaJIT rejected | design.md §5 |
| Aug 2026 | Freestanding C11 for the kernel, Rust rejected | design.md §6 |
| Aug 2026 | Command-based drawing model (B), shared memory only as an exception | design.md §7.4 |
| Aug 2026 | UI kit with follow modes, no constraint solver | ui.md §16.4 |
| Aug 2026 | Target order: QEMU → Pi 1 → Pi 5 → Jetson → G4. Mac Pro 2013 rejected (no serial) | hal.md |
| Aug 2026 | Doom as the project's verifiable goal | roadmap.md M10 |
| Aug 2026 | The POSIX line: a libc inside the process yes, a POSIX personality no | design.md §17 |
| Aug 2026 | A server pumps while it waits: `ipc_receive` takes a non-blocking flag, rather than a second thread inside a server | design.md §7, architecture.md §2 |
| Aug 2026 | Who gets Control-C is decided by who is reading the keyboard: foreground program, else the line editor. Background is left alone | architecture.md §3 |
| Aug 2026 | Interruption is cooperative and stays that way until a process can be ended from outside | roadmap.md M6 |
| Aug 2026 | A program hands capabilities to its children under names: `run(path, args, detach, shares)` | ui.md §16.7, architecture.md §2 |
| Aug 2026 | The idle thread yields when anything is runnable and sleeps only when nothing is. Every yield used to cost a timer period | testing.md, kernel/main.c |
| Aug 2026 | Latency between operations is measured separately from operations. `bench/` is structurally blind to it | testing.md |
| Aug 2026 | A live query is a parked reply, not a callback: a server that calls a client can be blocked by one | design.md §7, roadmap.md M7 |
| Aug 2026 | The UI kit takes BeOS's structure and not its skin: copy a decision about behaviour, decide a decision about shading fresh | ui.md §16.8b |
| Aug 2026 | An editor early, before the UI kit: nothing else lets the machine change itself without a rebuild | roadmap.md, recorded goals |
| Aug 2026 | The window manager reserves one key and it introduces a command, rather than reserving Tab and the arrows | ui.md §16.7 |
| Aug 2026 | A library is a file in the namespace, loaded into the caller's environment. No package path, no module table | ui.md, init.lua `use` |
| Aug 2026 | An app is scriptable because it used `ui.window`, not because it wrote scripting code | roadmap.md M7, ui.md §16.7 |
| Aug 2026 | The `/app` registry hands out capabilities and never forwards: a forwarding registry is one process any app could stop | init.lua, architecture.md §2 |
| Aug 2026 | A directory whose children are looked up on demand (`mount_registry`), so a namespace can hold things that come and go | init.lua `resolve` |
| Aug 2026 | A replicant is Lua source + state + a `needs` list, restricted by the language rather than by the kernel, and it says so | ui.md §16.8 |
| Aug 2026 | An absolute pointing device, not a relative one: no acceleration curve to agree on with the host, so the guest cursor cannot drift from the real one | hal.md |
| Aug 2026 | The pointer's position leaves the HAL in the device's own units with its range beside it. Only the window manager knows the screen size, so only it scales | hal.md, CLAUDE.md |
| Aug 2026 | A process that owns the screen takes it from the kernel console, which falls back to the serial line. A panic takes it back | kernel/console.c, architecture.md |
| Aug 2026 | A press grabs, at both layers: the window it landed in, and the view inside that window, until the release | ui.md §16.7 |
| Aug 2026 | Hover is not forwarded to applications; movement is, but only while a button is held | ui.md §16.7 |
| Aug 2026 | The cursor is part of the composite, not drawn over it afterwards | ui.md §16.7 |
| Aug 2026 | A window's damage waits until its drawing is complete, so a frame is never seen half-redrawn | ui.md §16.7 |
| Aug 2026 | A program declares it draws with `-- kosmos: application`; the store reports it. The launcher does not guess by reading source | init.lua binfs |
| Aug 2026 | The Deskbar lists the desktop's windows, not the `/app` registry: a window with no registration is still a window | deskbar.lua |
| Aug 2026 | A kill marks and unblocks; the process dies at its own next entry into the kernel, because teardown ends with the thread that performs it | process.c, trap.c |
| Aug 2026 | Only a parent may end a child - the authority `wait` already implies, and no new one | syscall.c |
| Aug 2026 | A close box asks first and ends second: an application that listens leaves tidily, one that does not is ended a second later | wm.lua |
| Aug 2026 | A label with no width given measures itself before every paint, not once when it was made | ui.md, ui.lua |
| Aug 2026 | Input interrupts wake a sleeper; they decode nothing. The queue is read in a thread, in its own time | hal/qemu-virt/input.c |
| Aug 2026 | A sleep is against the scheduler's clock, not the counter: only one of them interrupts | syscall.c, thread.c |
| Aug 2026 | "app" is graphical and "program" is console, and they are not interchangeable | glossary.md |
| Aug 2026 | SSH client as M12, with monocypher ported | roadmap.md M12 |
| Aug 2026 | Scripting architecture: every app exposes its hooks as nodes in its own namespace (from BeOS hooks) | beos.md §17.2, roadmap.md M7 |
| Aug 2026 | Name, size and modification date are always indexed; everything else when declared (from BFS) | beos.md §17.2, roadmap.md M8 |
| Aug 2026 | Entity files: nodes with no content, only attributes (from BeOS People) | beos.md §17.2, roadmap.md M7 |
| Aug 2026 | The filesystem is not a relational database. BeOS tried it through DR8 and pulled it for complexity and performance | beos.md §17.1, roadmap.md M8 |
| Aug 2026 | The kernel contains no Lua. Its tests and benchmarks moved to EL0 | state.md, CLAUDE.md |
| Aug 2026 | A monotonic counter is a syscall (SYS_TICKS); the wall clock stays a capability | design.md §4.4, syscall.h |
| Aug 2026 | ramfb before virtio-gpu. `hal_fb_init` is "ask the firmware for a linear framebuffer", which is ramfb and the Pi mailbox both; virtio-gpu comes second and is what grows a `hal_fb_flush` | hal.md, roadmap.md M6 |
| Aug 2026 | The QEMU framebuffer's stride is padded on purpose, so code that assumes `width * 4` breaks here rather than on real hardware | gfx.md §19.3, hal/qemu-virt/fb.c |
| Aug 2026 | A pitch bug is invisible from inside the process: reads and writes stay consistent with each other. The test for it lives outside the guest, in `make screenshot` | gfx.md §19.3, tools/run_screenshot.py |
| Aug 2026 | The screen is a boolean on the process, like the console, and init hands it on. Today to the shell; at the app server, to that instead | state.md, kernel/process.h |
| Aug 2026 | Spleen 8x16 as the bitmap font, BSD-2-Clause, vendored unmodified under `assets/fonts/` and converted at build time. Linux's `font_8x16.c` rejected: it is GPL and would infect the image | gfx.md, assets/fonts/ |
| Aug 2026 | The kernel console can write to the screen. Not a graphics subsystem: forty lines of glyph blitting, so that `panic()` reaches a display on a board with no serial cable | kernel/console.c, CLAUDE.md |
| Aug 2026 | The boot narrates itself in ten numbered stages with a progress bar, because what happens between the reset vector and a prompt is most of what there is to learn here | kernel/boot.h, state.md |
| Aug 2026 | Keyboard input is virtio-input over virtio-**mmio**, not virtio-pci: no bus to enumerate and no capability list to parse, and the same transport gives virtio-gpu next | hal/qemu-virt/keyboard.c |
| Aug 2026 | A keyboard is not a new HAL call. It is another source for `hal_getchar`, so nothing above the HAL changes because one exists | CLAUDE.md, hal/hal.h |
| Aug 2026 | `SYS_SYSINFO` reports raw ID registers and pool counts; the kernel decodes none of it. The tables that name a processor live in Lua | kernel/syscall.h, user/init/init.lua |
| Aug 2026 | Hardware is inventoried at `/dev`, a server reached through the namespace, not a shell built-in | user/init/init.lua |
| Aug 2026 | The shell dispatches bare-word commands before falling through to Lua, and supports aliases. A leading `/` always means a command; a bare word does not when it collides with a Lua name | user/init/init.lua |
| Aug 2026 | CPU usage is sampled at the timer tick as idle-vs-busy, and reported as the difference between two readings | kernel/thread.c |
| Aug 2026 | `def` compiles a line of Lua into a named command; the shell is extensible from inside itself | user/init/init.lua |
| Aug 2026 | The working directory is the shell's idea alone. Servers are always told whole paths | user/init/init.lua |
| Aug 2026 | A listing is the server's entries plus whatever is mounted below the path. Only the namespace knows the second half, which is what makes `/` a directory | user/init/init.lua |
| Aug 2026 | Every fixed pool must be countable and reported. `ADDRSPACE_MAX` was the real limit on processes for a while and no report mentioned it | arch/aarch64/mmu.c, kernel/syscall.h |
| Aug 2026 | Programs live in `/bin`, served read-only from the image, and run in a process of their own with the capabilities the shell chose. No path search, no inherited environment | user/bin/, user/init/init.lua |
| Aug 2026 | A read may span messages (`more`/`offset`) rather than growing `MSG_BYTES`: every thread embeds a message, so raising it is paid for by all of them | user/init/init.lua |
| Aug 2026 | A program can launch a program: the runner hands its child a `run`, so nothing needs to know role numbers and no one can pass on more than they hold | user/init/init.lua |
| Aug 2026 | The project is renamed Kosmos (previously Komo) | all docs |
| Aug 2026 | Pixels never live in Lua tables. A surface is a userdata over flat bytes; tables carry intent | gfx.md §19.1 |
| Aug 2026 | No line of Lua computes a pixel offset. Pitch and format live in the handle, the arithmetic happens only in C | gfx.md §19.3 |
| Aug 2026 | Presentation via double buffering and an explicit `commit` with mandatory damage. No locks | gfx.md §19.4 |
| Aug 2026 | Shared memory mapped inner shareable, no manual `DC` per frame | gfx.md §19.5 |
| Aug 2026 | Surfaces need an explicit `free` plus a `__gc` safety net, and the GC must be told the real size | gfx.md §19.6 |
| Aug 2026 | A shared region maps in its own address window, so no exit path frees pages it does not own | gfx.md §19.8 |
| Aug 2026 | A capability slot carries a kind; one index space for endpoints and memory both | design.md §4.4, state.md |
| Aug 2026 | The context switch saves the whole FP register file; a preempted thread has live values everywhere | state.md, bench/baselines.json |
| Aug 2026 | Face winding is derived from the geometry, never written out by hand | g3d.lua, state.md |
| Aug 2026 | M8: virtio-blk in the kernel HAL, moving to userland when a second driver shapes the interface | roadmap.md M8 |
| Aug 2026 | M8: attribute indices are rebuilt at mount, not stored on disk | design.md §8.3, roadmap.md M8 |
| Aug 2026 | M8: a large file is delivered as mapped pages, never as a string | design.md §8.4 |
| Aug 2026 | The 10k kernel figure is a smoke alarm, not a rule; what the kernel may *contain* is the rule | CLAUDE.md, design.md §2 |
| Aug 2026 | These are principles, not laws: departing from one is a decision to take out loud, never a drift | CLAUDE.md |
| Aug 2026 | Window decoration is one colour across the whole frame; the BeOS title-width tab is departed from | ui.md |
| Aug 2026 | Two palettes, switched live; the palette table is mutated in place so every widget follows | ui.md §16.9 |
| Aug 2026 | A widget colour may be *named*, and a name is resolved on every draw, never captured | ui.md §16.9 |
| Aug 2026 | Test harness from M0: host-side runner, TAP over serial, exit code via semihosting | testing.md §18.1, roadmap.md M0 |
| Aug 2026 | QEMU with `-icount` for regressions (deterministic), PMU on hardware for budget. They measure different things | testing.md §18.3 |
| Aug 2026 | Anything with a frame budget is measured by max and p99, never by average | testing.md §18.5 |
| Aug 2026 | Every milestone's definition of done becomes a permanent test | testing.md §18.7 |

---

## Language

**Everything in this repository is written in English.** Documents, source code, comments, commit messages, identifiers, test names, log output. No exceptions.
