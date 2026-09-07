# Kosmos

A microkernel desktop operating system with a userland written entirely in Lua.

A personal learning project. No users to serve, no compatibility to maintain, no deadline. That is precisely what makes it possible to take decisions a commercial OS cannot take.

**A desktop, a filesystem with a journal, a TCP/IP stack and a web
browser**, on a microkernel that knows about threads, address spaces, IPC
and capabilities and nothing else. [docs/roadmap.md](docs/roadmap.md) is
what is built and what is wanted; [docs/state.md](docs/state.md) is where it
actually is this week, and [docs/architecture.md](docs/architecture.md) is
how the layers fit together.

There are no milestones. There were thirteen, and the numbers went on being
quoted long after they stopped describing anything - this line said "M6,
graphics" while the machine had a browser in it.

---

There is a page about it at
[dcibils-neuratek.github.io/kosmos](https://dcibils-neuratek.github.io/kosmos/),
which is [`docs/index.html`](docs/index.html) - GitHub Pages will serve the
repository root or `/docs` and nothing else, so the site lives beside the
design documents rather than in a folder of its own.

## Run it

One command. Nothing to clone, nothing to build, no toolchain:

```sh
sh -c "$(curl -fsSL https://raw.githubusercontent.com/dcibils-neuratek/kosmos/main/get-and-run-kosmos.sh)" -- -b "wm"
```

You need QEMU, and it is the only thing this cannot fetch for you:
`brew install qemu` on macOS, `apt install qemu-system-arm` on Debian or
Ubuntu. Everything else - the newest image, the runner, a disk to keep files
on - it works out and downloads.

`-r 1280x720` asks for the smaller build, because the framebuffer size is
compiled in and a different resolution is a different image. On **Windows**,
use WSL and the same command, or run QEMU natively with the flags
[the website](https://dcibils-neuratek.github.io/kosmos/) spells out - the
shell script will not run there. Only macOS is actually tested.

That boots straight to the desktop. Some other things to try once it is up,
or to pass instead of `wm`:

```sh
-b "wm browser"      the browser, on a page inside the image
-b "wm tracker"      the file manager
-b "wm cube3d"       a spinning cube, in software, at 50 fps
-b "wm doom"         Doom, once there is a WAD at /home/doom1.wad
-serial              no window at all, just the shell on this terminal
```

**Nothing asks you to know a file name.** A release is called something like
`kosmos-0.8.34-8c8c1f5-1920x1080-full.elf` and the version and commit in it
change every time, so the published list is asked for and the newest worked
out. `--list` shows what there is.

`sh -c "$(curl ...)"` rather than `curl | sh`, and the difference is not
style: a pipe *is* the script's stdin, and the last thing it does is hand
over to QEMU with `-serial mon:stdio` - so the guest's console would read
from an exhausted pipe and see end-of-file the moment it started.

Keeping a copy of the script works too and is shorter to type afterwards.
It will tell you when it differs from the published one.

---

## The idea in one paragraph

**BeOS is the reference**, and not one influence among several: the desktop is drawn from it, and so is the standard it is held to. The tab as wide as its title, the Deskbar, replicants, typed attributes with live queries over them, and responsiveness as a design constraint rather than an optimisation.

What is taken from elsewhere is what BeOS did not have an answer for: the microkernel from QNX, per-process namespaces from Plan 9, capabilities from seL4, the live image from Lisp Machines. All of it on top of a userland written in Lua.

What holds it together is one rule about what travels between things: **the language's data model where the shape is the caller's to choose, a declared struct where the shape is agreed.**

So IPC messages are Lua tables almost everywhere — the shell, applications, the UI kit, scripting, the window manager — and that is what gives the system one mentality from the prompt upward. At the boundary into a system server they are structs declared in a header both sides compile against, for two reasons found by building it: a server has to stay correct when its caller is wrong, and a garbage collector cannot sit where something else's timing depends on it. `docs/design.md` §1 has the argument and what it costs.

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

**It is not a BeOS clone.** BeOS was monolithic, with drivers and the filesystem inside the kernel. Kosmos takes its concurrency model, its live queries and its design sensibility. The architecture is QNX.

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
6. **The system is modified while running.** ~~A server reloads its code without losing state or clients.~~ **Withdrawn September 2026**: every server is C, there is no dynamic linking, and hot reload was removed with the last Lua one. See `docs/design.md` §10.
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
| [docs/testing.md](docs/testing.md) | How each layer is measured and how regressions are caught. |
| [docs/hal.md](docs/hal.md) | How `arch/` is separated from `hal/`, the targets, and the trap in each piece of hardware. |
| [docs/roadmap.md](docs/roadmap.md) | What is built, and the wishlist. |
| [docs/setup.md](docs/setup.md) | Toolchain, build, how to debug without a debugger. |
| [docs/glossary.md](docs/glossary.md) | The terms. Written to be re-read six months from now. |
| [docs/state.md](docs/state.md) | Where you are today. **Updated at the end of every session.** |
| [CLAUDE.md](CLAUDE.md) | Operating rules for working with Claude Code. |

Reading order if you come back after months: `state.md`, then `design.md`, then whichever one matches the current stage.

---

## Building it

[Running it](#run-it) needs none of this. To build:

```
make qemu
```

That is the whole system at 1920x1080 - browser, Doom, network, every demo.
`FULL=0` gives a leaner image. Toolchain and prerequisites in
[docs/setup.md](docs/setup.md).

---

## Layout

```
boot/           assembly entry, linker script
arch/           aarch64/ — what depends on the CPU
hal/            qemu-virt/ — what depends on the board (pi5/, pi1/ at M2)
kernel/         EL1: mmu, sched, ipc, caps, exceptions
lua/            upstream/ untouched + kosmos/ for freestanding
runtime/        minimal libc, and upstream/ for what is vendored
assets/         vendored data: fonts, icons, images, with their licences
user/           everything at EL0:
  init/           init, the roles, and the namespace kit
  servers/        the servers, in C: audio, devices, binfs, appfs,
                  console, ramfs
  include/        the protocol headers both sides compile against
  lib/            libraries: Lua (ui, theme, pdf) and C kits (gfx, gl,
                  console, png)
  bin/            programs and applications, in Lua
  tests/          the Lua suite
tests/          guest-side tests, in C
bench/          benchmarks and baselines.json
tools/          host-side test runner, scripts
docs/
book/
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
| Aug 2026 | The microkernel is **Nebula**; Kosmos is the operating system on top of it | CLAUDE.md |
| Aug 2026 | Versions are major.minor.revision: a revision per push, a minor per milestone | VERSION, tools/bump.py |
| Aug 2026 | Action buttons go in a bar across the top of a window's content, never below it | ui.md §16.10 |
| Aug 2026 | A Lua/C crossing costs ~2000 pixels; a primitive writes that much or is batched | gfx.md §19.11 |
| Aug 2026 | Test harness from M0: host-side runner, TAP over serial, exit code via semihosting | testing.md §18.1, roadmap.md M0 |
| Aug 2026 | QEMU with `-icount` for regressions (deterministic), PMU on hardware for budget. They measure different things | testing.md §18.3 |
| Aug 2026 | Anything with a frame budget is measured by max and p99, never by average | testing.md §18.5 |
| Aug 2026 | Every milestone's definition of done becomes a permanent test | testing.md §18.7 |
| Aug 2026 | Outline fonts via `stb_truetype`, vendored unmodified; the bitmap font stays the default | gfx.md §19.12 |
| Aug 2026 | Three font roles - titlebar, text, monospace - because a terminal's must be fixed-width whatever the others are | gfx.md §19.12 |
| Aug 2026 | Fonts are embedded, wallpapers are not: the image is copied per process | gfx.md §19.12 |
| Aug 2026 | A font server is for containing the parser, not for saving memory; shared image pages are what save memory | gfx.md §19.13 |
| Aug 2026 | A grant is asked for only when the machine can give it: a refused spawn is a server that does not start | design.md §17, testing.md |
| Sep 2026 | Kosmos is MIT, and every file we write carries a one-line notice | LICENSE, CLAUDE.md |
| Sep 2026 | Attributes live in one block per file, pointed at by the inode; kind is structural only for directories | kfs.lua, gfx.md |
| Sep 2026 | A cross-machine benchmark measures for fixed time, never fixed work | testing.md §18.10 |
| Sep 2026 | The benchmark engine is a library; `score` prints it and `sysbench` draws it | testing.md §18.10 |
| Sep 2026 | The serialised format is little-endian by decision, not by accident | serialize.c, testing.md §18.11 |
| Sep 2026 | Journalling, ext3-style: journal, commit block, apply, clear. The order is the guarantee | kfs.lua, testing.md §18.12 |
| Sep 2026 | A host Lua, so pure-logic libraries are unit-tested without booting | Makefile, tools/test_kfs.lua |
| Sep 2026 | The language line is structure vs loops-over-bytes, not platform vs application | design.md §6 |
| Sep 2026 | Audio: a virtio-snd driver, WAV first, then a vendored MP3 decoder in userland C | design.md §6, state.md |
| Sep 2026 | Large files cross as `read(fd, buf, n)`: the buffer is a shared region named by a capability | design.md §8.4, layout.md |
| Sep 2026 | A BeOS-shaped layout: `/system` ships, `/user` is installed, `/home` is yours | layout.md |
| Sep 2026 | A finished algorithm goes in C: hot reload is the cost of C, and there is nothing to reload in a codec | CLAUDE.md, design.md §6 |
| Sep 2026 | **Kits**: C libraries reached as `use("/kits/pdf")`, through the namespace like any library | CLAUDE.md, design.md §6 |
| Sep 2026 | Where speed and hot reload disagree, speed wins. What keeps policy servers in Lua is that C buys ~2% there and can overflow a buffer | CLAUDE.md, design.md §10 |
| Sep 2026 | **The look is dimensional on purpose.** Raised, sunken, grooved - a bevel is not decoration, it is a sentence about what a thing does. Reverses "structure not skin" | ui.md §16.8b |
| Sep 2026 | `SYS_CAP_DROP`: a capability can be given back. Sixteen a thread, and nothing released one | design.md §4.3, kernel/ipc.c |
| Sep 2026 | `SYS_SHARE_UNMAP`: releasing a region means losing the mapping, not only the name | kernel/syscall.c |
| Sep 2026 | A PDF is read through a window and never held: the object layer is Lua, the scanner is C | design.md §6, pdf.lua |
| Sep 2026 | FP and SIMD are saved lazily: the switch disarms them, the first instruction that wants them faults | arch/aarch64/fp.c, CLAUDE.md |
| Sep 2026 | The FP trap is armed at both privilege levels, not EL0 alone, because kernel threads have FP state too | arch/aarch64/fp.c |
| Sep 2026 | Strict priority bands with round robin inside each, from QNX. Five named levels, not 256 | sched_prio.c, CLAUDE.md |
| Sep 2026 | A wake preempts a lower band at the next exception, rather than waiting out a quantum | sched.h, thread.c |
| Sep 2026 | The quantum is a variable, because the interesting thing about one is what changes when you change it | sched_prio.c |
| Sep 2026 | Responsiveness is a design goal: Kosmos owes compatibility to nothing and may borrow freely | CLAUDE.md |
| Sep 2026 | The scheduler is swappable while the machine runs; the queues are drained, not dropped | thread.c, scheduler.lua |
| Sep 2026 | Quantum and policy are anyone's to change; priority is not, because bands come from capability | syscall.c |
| Sep 2026 | A shared region is a list of pages, not a contiguous run; the index lives in allocated pages | memobj.h |
| Sep 2026 | A thread holds 32 capabilities, not 16: a graphical application needs more than a shell did | ipc.h |
| Sep 2026 | A full capability table is its own error, not "out of memory" | syscall.h |
| Sep 2026 | A document's own fonts are rasterised by glyph index, cached per face and size, drawn a page per call | docfont.c |
| Sep 2026 | Priority inheritance across IPC: a server runs at the band of whoever is waiting on it | ipc.c, thread.c |
| Sep 2026 | The aim is a fast, responsive graphical OS - BeOS's bet with QNX's answers, targeting a Pi 5 | CLAUDE.md |
| Sep 2026 | A released binary must survive `make stress`; a committed revision need not | CLAUDE.md, Makefile |
| Sep 2026 | Leaks are asserted on `sysinfo` counters, and memory is judged by whether it *stopped* falling | stress.lua |
| Sep 2026 | A server is not promoted by capability; it borrows urgency from its caller and gives it back | process.c |
| Sep 2026 | Audio is in scope; *guaranteed* latency is not. The mixer reports its worst refill rather than promising one | roadmap.md M11a |
| Sep 2026 | Waiting is `sys.sleep`, never `sys.yield` in a loop: yielding is a spin, and two of them cost 89% of the machine to play a tone | syscall.c |
| Sep 2026 | The tick is 250 Hz, chosen by the sound device rather than by convention: a 5.8 ms period cannot be fed on a 10 ms clock, and buffering more does not fix it | kernel.h |
| Sep 2026 | A server waits with `receive`-plus-deadline, never a sleep: a server that sleeps on a timer answers nobody while it sleeps | ipc.c |
| Sep 2026 | **Control by message, data by shared memory.** A stream never travels as a message payload; if it recurs at the hardware's rate, the bytes go in a region | CLAUDE.md |
| Sep 2026 | The language follows the *layer*, not a judgement: C runs on behalf of another process (kernel, drivers, servers, kits), Lua runs for a person (apps, programs) | CLAUDE.md |
| Sep 2026 | A boundary is an agreement: what crosses into the system is a declared struct, not a table. A capability is what you cannot *name*; a struct is what you cannot *say* | audioproto.h |
| Sep 2026 | The namespace is a **kit**, not a server: it is run in the caller's own process, has no endpoint and no thread. This file called it a server for months and an argument was built on the name | glossary.md |
| Sep 2026 | Servers speak `conproto.h` through a **kit** where a protocol has two implementations. `/dev/console` is the only one: a terminal mounts itself as its child's console, so an application answers the same ABI | con_kosmos.c |
| Sep 2026 | **Hot reload removed.** Not outranked - removed. Seven servers went to C and there is no dynamic linking, so nothing is left to reload. ramfs did not have to go and went anyway, at a known price: a milestone's test deleted and §9.1's only live demonstration with it | design.md §10 |
| Sep 2026 | The desktop **carries a drag it never reads**: a `kind` and an opaque string. It is the only process that knows what is under the pointer, and the reply is a one-shot right given to the window that was handed the drop | wm.lua |
| Sep 2026 | **Nothing in the shared image declares storage only one role needs.** There is one userland binary, so ramfs's 2.1 MB store was `.bss` in all sixteen processes. `kosmos_map` at startup instead: 7232 KB a process becomes 5156, and ramfs pays 7308 | ramfs.c |
| Sep 2026 | **The read-only half of the image is mapped where it lies**, one copy for the machine. Permissions live in the mapping, not the page, so sharing 2.8 MB nobody can write costs no isolation. A process holds 2224 KB where it held 7232 | design.md §4.1.1 |
| Sep 2026 | **MMIO is one hand-written instruction**, not a volatile store. GCC chose a post-indexed store and ARM sets ISV=0 for writeback, so no hypervisor can decode it - correct on metal, unbootable under `hvf` | arch/aarch64/mmio.h |
| Sep 2026 | **A query is scoped to the path it was asked at**, not the volume. One disk mounted at three prefixes was answering `/home` with `/system`'s files, and the namespace was putting the prefix on twice | run_queries.py |
| Sep 2026 | **The virtio transport is one file.** It was three copies of a handshake whose *order* is the protocol; the reason not to share it - "before there are two" - expired two devices ago, and the network card is the fourth | hal/qemu-virt/virtio.c |
| Sep 2026 | **A card is a device; a stack is someone you ask.** `/net` is a server behind `SPAWN_NET` - the disk's grant pointed outwards - and the card has no name in any namespace, because nothing but the stack may reach it | user/servers/net.c |
| Sep 2026 | **Ping needs no TCP**, and half a TCP is worse than none. Ethernet, ARP, IPv4 and ICMP is what a round trip costs; `netproto.h` records where the shared ring goes when a *stream* arrives, so nobody discovers a message worked for the first ten kilobytes | netproto.h |
| Sep 2026 | **A connection's bytes never travel in a message.** Two SPSC rings in a region both sides hold, and the receive window *is* the ring's free space - so a client that stops reading really does slow the sender down, and flow control costs nothing | tcpring.h |
| Sep 2026 | **One segment in flight, out-of-order dropped.** A queue and a sliding window buy throughput on a long fat link and buy nothing on a line protocol; a reassembly buffer is the well-known way to get a stack wrong. One timer instead of four | user/servers/net.c |
| Sep 2026 | **A FIN is a wish, not an act.** It takes a sequence number, so sending it while the ring still holds bytes numbers it as if they did not exist - a 152 KB image arrived as 141 KB with the server logging success and the client seeing a clean close | user/servers/net.c |
| Sep 2026 | **A manager is not the server.** `accept` blocks and a window that blocked would stop drawing, so `httpd` is a process and `webserver` reads what it wrote to `/data` - which is why daemons have log files rather than shouting | user/bin/webserver.lua |
| Sep 2026 | **Crypto is checked against its own specification's vectors, in `make test`.** It is the one place here where a bug is silent: a wrong counter still encrypts, and nothing about running tells you | user/lib/crypto.c |
| Sep 2026 | **A coroutine per connection is what this system has instead of threads.** There is no thread syscall and a process has one `lua_State`, so two threads inside it would want a lock around the interpreter and take turns anyway. `httpd` yields where it would have waited | user/bin/httpd.lua |
| Sep 2026 | **Waiting to read and waiting to write are two questions.** `poll` takes two masks. One mask reported writable as "room *and* something queued", so a client that acknowledged the whole ring left the server waiting for a signal that could no longer come - eight requests hung and none were logged | netproto.h |
| Sep 2026 | **Every park in the stack is bounded**, `accept` included. `poll` saying somebody arrived and `accept` reaching the stack are two moments, and a reset in between would wedge an event loop for good | user/servers/net.c |
| Sep 2026 | **A drain loop must not read from the buffer it is filling.** The console's `interrupted` took a byte off the stash, put it back, and took it again - one character typed ahead and the console server span for ever, which looked like whichever program had asked having hung | user/servers/console.c |
| Sep 2026 | **A namespace call behaves the same on every mount, or it is not one.** `fs.write` split long writes for `/data`, and *raised* out of the serialiser on the disk, which takes no offset to append at. It sends a value too big for a message through a region now - the route `write_from` and `files.copy` already used | user/init/init.lua |
| Sep 2026 | **A server lent a buffer gives the capability back, on every path.** diskfs's read side had paid that debt since a PDF found it on its fifteenth read; the write side never had, so it worked for thirty-one writes and then refused every one after with what reads like a bad pointer | user/init/init.lua |
| Sep 2026 | **The allocator scanned every block, allocated ones included**, so what an allocation cost was what the heap already held. Free blocks are binned by size now, with the links inside their own unused payloads: `alloc_table` 28138 -> 356, and `serialize` fell 40% without being touched | runtime/libc/malloc.c |
| Sep 2026 | **A heap that grows is a runtime answer to a runtime question.** 2 MB was fixed at compile time and `make DOOM=1` existed to rebuild the system with a bigger one; `malloc` asks the kernel for another arena instead, to 48 MB. Arenas are never adjacent, so each is its own physical list and coalescing cannot cross | runtime/libc/malloc.c |
| Sep 2026 | **64-bit only, and that is a constraint on word size rather than on architecture.** The Pi 1 was the best board to learn on and is ARMv6; a second `arch/` is expected and wanted, but x86-64 buys everything ARMv6 would have and gives up nothing. 16 AArch64 sites in 8656 lines of kernel say it is not a refactor | docs/hal.md |
| Sep 2026 | **A web page parses on Kosmos.** Hubbub, libdom and libcss running at EL0 on the freestanding libc - three `p` elements counted through a tree walk, `&amp;` decoded from a generated table, a stylesheet understood. Linking proved none of it | tools/run_web.py |
| Sep 2026 | **The kernel has no architecture-specific instruction in it.** Sixteen sites across four files became seven inlines in `arch/aarch64/cpu.h`; the two maskings stayed two, because `DAIFSet #3` is IRQ *and* FIQ where `#2` is IRQ alone, and merging them would silently change when a fast interrupt may arrive | arch/aarch64/cpu.h |
| Sep 2026 | **A browser window, and it says what it is not.** It fetches, parses and shows a document's text, with *text only, there is no layout engine yet* in the status line - the chrome is here early so each piece of engine has somewhere to appear when it lands | user/bin/browser.lua |
| Sep 2026 | **The text path read bytes where it needed characters.** Each byte became a codepoint, so one accented character was three failed lookups and three boxes - never a missing font. Both paths decode now, and glyphs outside ASCII rasterise on demand into a codepoint-keyed cache | user/lib/gfx.c |
| Sep 2026 | **A face is a name and a size, not one of four roles.** A page needs a heading and a paragraph on screen at once; `gfx` held one face per role. Adding weights then broke resolution silently - `-Bold` sorts before `-Regular` and both were called `ibmplexsans`, so the terminal would have gone bold with no error | user/lib/gfx.c |
| Sep 2026 | **A page is painted in one crossing, not one per box.** Layout and drawing both happen in C and what returns is a height; a call per box would cost more than the drawing. Lines break at a measured width, and the ascent that turns a line's top into its baseline stays inside `gfx`, which is the only place that knows it | user/lib/web_paint.c |
| Sep 2026 | **The browser draws its own pixels.** On the ordinary path the compositor rasterises an application's text, in the four faces the desktop chose - right for a dialog, and hopeless for a document wanting a heading at 28 pixels and a paragraph at 16. So it takes a direct window, and pays for it in widgets: the chrome is rectangles it knows the position of, as `pdfview`'s is | user/bin/browser.lua |
| Sep 2026 | **A page is drawn a word at a time, not a line at a time.** Drawing the byte range between the first word and the last is one call instead of a dozen, and it draws the markup's own newlines with it - a glyph nothing has, wider than the space the wrap budgeted, so the line overflowed as well. A word is also where a font change, a link and a selection attach | user/lib/web_paint.c |
| Sep 2026 | **Layout keeps its boxes, and one missing data structure was six missing features.** Painting as the tree was walked threw away where each word ended up: nothing could be clicked, nothing could be bold inside a paragraph, `pre` could not keep its spaces. Layout produces an array of runs; paint is one loop over it and hit-testing is the same loop with a comparison | user/lib/web_paint.c |
| Sep 2026 | **A rectangle is a run with no text.** A heading's rule, a list marker and a link's underline share the array the words are in, so painting is one pass in layout order and a rule cannot land on top of the line it belongs under | user/lib/web_paint.c |
| Sep 2026 | **`-b "wm blocks"` booted `wm` and dropped `blocks`.** The script's own documented example: the boot argument was built as a string and expanded unquoted, so a shell split it and QEMU got a stray word. Every single-word `-b` worked, which is why it survived. A POSIX shell holds a list with `set --`, and a string is not a substitute for one | run-kosmos.sh |
| Sep 2026 | **`/bin` reported 74 of its 82 programs and said nothing.** A listing reply holds 74 names and `BIN_OP_LIST` started at nought every time, so the Deskbar - which builds its menu from that list - could not offer Tracker, the Terminal, the top bar or the web server. The guard was a static assert claiming "a list must hold every program in the image", which is not something a static assert can see | user/servers/binfs.c |
| Sep 2026 | **`make qemu` is the whole system, at 1920x1080.** The variants exist so a suite can be quick, not so the machine you sit in front of is. `VARIANT` had to compose rather than choose - `DOOM=1 WEB=1` called itself `-doom` - and be computed after the block that sets them, because `:=` expands where it stands | Makefile |
| Sep 2026 | **A preference stays chosen; a service is running right now.** The network configuration, the process list, the log and the web server were filed under Applications among the calculator and the paint program. A fourth section, System, which is the one nobody could find the web server without | user/bin/deskbar.lua |
| Sep 2026 | **A browser ships with a page inside it.** Looking at a new build meant starting a web server on the computer running QEMU first, which an operating system has no business asking for - and it took being told so to see it, because the test harness serves pages from the host to stay offline and deterministic, and that got carried into the instructions as a requirement | user/bin/browser.lua |
| Sep 2026 | **`Host` was the literal string `kosmos`.** HTTP/1.0 made the header optional and the web stopped being like that twenty years ago: one address serves hundreds of sites and the header is how a server knows which. Every real host answered the wrong thing, and the only server that looked right was one serving a single site out of a directory - which is what it was tested against | user/bin/browser.lua |
| Sep 2026 | **A released image says whether it has the browser in it.** `-web` in the name, because on an ordinary one the browser opens and reports no web kit - which reads as a broken browser rather than the wrong file. One size rather than three: five vendored libraries take an image from 1.7 MB to 5.3 MB, and stripping saves a quarter of a megabyte because the bulk is the userland compiled in | Makefile |
| Sep 2026 | **A scheduler tick is four milliseconds, and three comments said ten.** `TICK_HZ` went 100 to 250 and `wm.lua` had already stopped assuming. It is not tidiness: every animating window sleeps one tick a frame on purpose, and whether that is a quarter of a frame or most of one is the whole question about what the trade costs | user/lib/sys_user.c |
| Sep 2026 | **Half of a browser frame is waiting, not computing.** Split three ways on this Mac's own cores: 0.1 ms of chrome, 2.8 of blitting the visible band, and 2.8 in a `commit` whose handler swaps an index and records a rectangle. The worst frame moved 10.8-15.6 ms across two runs of identical code, which is scheduling rather than cost - a commit landing mid-pass waits out the compositor's pass | user/bin/browser.lua |
| Sep 2026 | **A space is measured in the face it was written in**, which is neither of the words either side of it. `<code>h1</code> is set` puts the space in the paragraph's own text node, so it is a paragraph-width space; taking it from the following word put a monospace gap in front of every inline code span, and taking it from the preceding one moved that gap to the other side. Found in a screenshot, not in the code | user/lib/web_paint.c |
| Sep 2026 | **`sysinfo` carries `cpu_arch` and an opaque `cpu_raw[]`, not seven registers by name.** The kernel decodes nothing about the processor - the ID registers go out as they were read and what they mean is userland's problem. That division works exactly as long as there is one architecture; with two, "raw" is not enough information, because the same words are MIDR_EL1 on one machine and CPUID leaves on another. Each `arch/*/cpu.h` names its own indices. ARM's decoding did not change | kernel/syscall.h |
| Sep 2026 | **`process_may_read` decoded page descriptor bits, and a search for register names does not find that.** `*entry & 1` and `(*entry >> 6) & 3` - the check on every pointer a process hands the kernel. On x86 bit 6 is Dirty and bit 7 is page-size, so it would not have crashed: it would have answered wrongly about who owns a page, which is the worst way for a security check to be wrong. Now `as_user_may`, answered by each architecture from its own format | kernel/process.c |
| Sep 2026 | **Six places where AArch64 had leaked out of `arch/`, and every one of them is closed.** `kernel/` has never contained a line of assembly, which made it easy to believe it was portable; a second architecture is what asks properly. Seven registers planted by `thread.c`, `enter_el0` declared by hand in `process.c`, `tf->x[N]` ninety-six times in `syscall.c`, the descriptor bits above, the `cpu_info` ABI, and the literal string "MIDR_EL1" in the boot log. All thirteen `kernel/*.c` now compile for x86-64, with no `#ifdef` in any of them | arch/, kernel/ |
| Sep 2026 | **`hlt` is not `wfi`, and the idle loop is where that costs a machine.** AArch64's `wfi` wakes on a pending interrupt with PSTATE.I set - masking stops the exception being *taken*, not the wakeup - so the idle loop masks across "is anything runnable" and "sleep" to close the race between them. `hlt` with IF clear halts and nothing wakes it. The same loop, unchanged, gave twenty timer interrupts in the first fifth of a second and then a machine that had printed its prompt and stopped. `sti; hlt`, whose one-instruction interrupt shadow exists for exactly this | arch/x86_64/cpu.h |
| Sep 2026 | **A syscall preserves every register but three, and `syscall` saves none of them.** AArch64's `svc` is an exception like any other: the vector writes all thirty-one general registers into the trapframe and `eret` reads them back. `syscall` writes rcx and r11 and leaves the rest as the process had them, so the entry stub must save what the kernel's C is about to clobber. Getting it wrong is not a crash but a caller whose live variable changed under it - here, `lua_pushinteger(L, sys0(...))` with `L` quite correctly kept in rdi | arch/x86_64/user.S |
| Sep 2026 | **The kernel's boot log printed string literals about ARM hardware.** "PL011 UART at 0x09000000" and "250 Hz off the generic timer through a GICv3", on a PC that has neither. The same mistake as decoding a page descriptor in `process.c`, one layer up and in prose. The *why* of a boot stage is about the design and stays in the kernel; what this machine turned out to be is the board's to say, and the architecture's for the two in `arch/` | hal/hal.h |
| Sep 2026 | **Finding a device and claiming it are two steps, and they were one.** `virtio_open` reset what it found, which is fine for a driver that wants the first card of its kind and wrong for `input.c`: a keyboard and a tablet are both virtio-input, and telling them apart means opening each. The tablet's scan reset the running keyboard - no queues, no DRIVER_OK, silent for ever. It worked on ARM by luck, QEMU laying mmio windows out in reverse so the tablet came first; swapping two `-device` flags would have broken it there too. `virtio_begin` is now separate | hal/virtio/virtio.h |
| Sep 2026 | **A declaration left behind in a board's header is invisible to the second board.** `keyboard_getchar` moved into the shared `hal/virtio/input.c` and its declaration stayed in `hal/qemu-virt/qemu-virt.h`, so `hal/pc/uart.c`'s `hal_getchar` read only the 16550. The PC found a keyboard, said so in the boot log, and could not be typed at: every key went into a virtqueue nothing drained. A declaration belongs with the code that answers it, not with the first board that happened to call it | hal/virtio/virtio.h |
| Sep 2026 | **The x86 build had no screen size, so it used the fallback.** `FB_FLAGS` is on one file's compile line on ARM, because putting it in CFLAGS rebuilt seventy-eight objects to change a number seven of them have never heard of. The x86 kernel is a single compile-and-link with no objects to be finer than, and the flag was simply never added - so `ramfb.c` took its 1024x768 default and the machine came up at a resolution nothing had asked for. It reads as a display bug and is a Makefile line | Makefile |
| Sep 2026 | **A harness nothing ran had been failing for months, and took ten minutes to say so.** `run_disk.py` was reachable only through `disktest`, never `make test`, so nothing noticed when `mounted()` started formatting a blank disk on its own: the harness still insisted a zeroed disk report no filesystem, which by then it never would. The other half was self-inflicted - `each` was a *duration*, so every command waited twenty-five seconds whether the shell answered instantly or not at all. It is a deadline now, matched against the prompt the boot already waits on: 26 checks in 4.7s rather than ten minutes, on both boards | tools/run_disk.py |
| Sep 2026 | **The clipboard went behind the window manager's prefix, because Control-C is spoken for.** There are no modifier keys here - a virtio keyboard gives Control plus a letter, and Control-C is what stops a program, in nine display checks. The alternative was inventing a triple out of unclaimed control codes, where every candidate carries somebody's prior. `Control-W a c x v` instead: behind a prefix the letters can be the ones everybody knows, which is what a prefix is for. The window manager holds the buffer and is told the *intent* rather than the text, so it never looks inside - the same division it keeps with pixels | ui.md §16.11 |
| Sep 2026 | **A cap enforced in the server would be too late by one process.** A clipboard entry travels inside a message and `MSG_BYTES` is 2048, so a copy of a four-kilobyte report cannot fit - and a table too big to serialise makes `fs.send` *raise in the caller*, killing the application that copied it. So `wmproto.copy` cuts before sending, and `ui.editor` shrinks the highlight back to the run that actually left: the limit becomes something you can see, in the place you were already looking | user/lib/wmproto.lua |
| Sep 2026 | **On x86-64 you cannot step past a faulting instruction, so the suite uses the recovery that works on both.** AArch64's handler does `elr += 4` and the arithmetic is exact, every A64 instruction being four bytes; an x86 instruction is one to fifteen bytes and its length is unknowable without decoding it. The *unwind* form - which ARM already had, for the stack-overflow case where stepping would resume into the guard page - is what both use now, behind one `FAULT_EXPECT` macro. It cost ARM nothing and it is what let `tests.c` cross to a second board | tests/fault.h |
| Sep 2026 | **A test that writes out an address is a claim about one machine.** `as: the kernel region is refused` named 0x40000000, which is a kernel address on AArch64 and is *exactly* `USER_VA_BASE` on x86-64 - so on the second board it asked the kernel to refuse the first page of user space, the kernel correctly mapped it, and the test reported a failure entirely its own. Derived from `USER_VA_BASE` now | tests/tests.c |
| Sep 2026 | **`arch/x86_64/fp.c` said preemption was not safe, and it had been safe for as long as `switch.S` had two instructions in it.** The comment claimed the FP mechanism was not built, that the 512-byte area was not in `struct context`, and that "nothing here preempts yet" - all false, and the last dangerously so: the timer preempts on this board like any other, and the eager `fxsave`/`fxrstor` is exactly what makes it correct. Written before those instructions existed and never revisited | arch/x86_64/fp.c |
| Sep 2026 | **Every x86-64 vector had `ist = 0`, so a kernel stack overflow triple-faulted the machine.** A fault from ring 0 stays on the stack that faulted, which is fine for a deliberate one and fatal for an overflow: the handler pushes onto the guard page and faults again delivering that. AArch64 gets the separation from the architecture - the kernel runs on SP_EL0 and every exception switches to SP_EL1. Fixed with a 16 KB exception stack, its own guard page, and #PF and #DF pointed at `tss.ist[0]`. **Two vectors and not all of them**, because an IST stack is not reentrant: the processor loads the same address every time, so a fault taken while one is being handled overwrites the frame being handled | arch/x86_64/trap.c |
| Sep 2026 | **The block driver ignored interrupts, which is a choice on one bus and a defect on the other.** A virtio-blk request completed correctly on x86-64 - QEMU's trace says `virtio_blk_req_complete status 0` - and the guest then made no progress at all. Not the spin loop failing to see the used ring: **a PCI INTx line is level-triggered and shared**, a device holds it asserted until its own ISR byte is read, and `hal_irq_handle` offered the line to input, sound and net and to nothing else. The PIC was told the interrupt was handled, the line was still asserted, and it fired again immediately, forever. AArch64 never had it because every device there has an interrupt ID of its own and blk never enables its own - sharing is what turns "this driver ignores interrupts" into a bug | hal/virtio/blk.c |
| Sep 2026 | **A field called `ticks` meant scheduler ticks in three of the four operations that read it and counter ticks in the fourth.** `NET_OP_RESOLVE` added a caller's deadline to `kosmos_ticks()` without converting, so five seconds was twenty microseconds and **the resolver answered whichever query beat the next sweep and timed out the rest** - it had never worked twice in one boot. The browser was its only caller and passed `counter_hz`, wrong in the same direction by the same factor, so the two agreed and nothing noticed. This is 0.9.1's bug in another subsystem, and it has the same fix: the unit goes in the name, and the conversion is one function | user/include/netproto.h |
| Sep 2026 | **A resolver with no command is a resolver nobody can test.** DNS was complete in `/net` - query, reply, label lengths, compression pointers - with one caller four hundred lines inside a graphical application. So a lookup could not be tried alone, a bad name could not be told from a bad route, and `make test` had nothing to check; meanwhile five separate comments, two programs and the roadmap all said the system had no DNS. `host` is thirty lines and found a bug the same afternoon | user/bin/host.lua |
| Sep 2026 | **A deadline is a counter value, not a count of interrupts.** `wake_at` was `hal_ticks()` plus a number of scheduler ticks, and `hal_ticks` counts interrupts *actually taken* - `hal_ticks_missed` exists because they are not - so a machine under load ran every sleep long by however many it had missed. A clock that stretches exactly when something is already going wrong. It also cannot be handed to a comparator, so the periodic tick could never have been removed while deadlines were expressed in it. One conversion now, in `thread_deadline_in`, and `architecture.md` §5 is the account of all three clocks | kernel/thread.c |
| Sep 2026 | **A deadline is not a reason, and on x86-64 every timeout in the system was four milliseconds.** `thread_wake_sleepers_now` woke every thread carrying a `wake_at`, on the grounds that a deadline meant waiting - but a `sys.sleep` carries one and waits for nothing. `input_arrived` is cleared only by `SYS_WAIT_INPUT`, which nothing calls at a bare prompt, so on that board the flag latched and every timer tick ran the path: 1200 times in five seconds against 0 on AArch64, with identical code. `ping`'s one-second gap between echoes took 40 ms. Found by timing a sleep from outside the guest, because **nothing in the suite had ever checked that a sleep lasts** | kernel/thread.c |
| Sep 2026 | **Drivers group by what the device is, not by architecture and not by board.** The obvious grid - arch down one side, board along the other - breaks on a specific pair: QEMU's q35 and an Alienware x14 share an instruction set and *almost no device*, while that laptop and a Pi 5 share no instruction set and the entire USB stack. So three axes: `arch/` for the ISA, `hal/<platform>/` for **how a machine says what it has**, and a pool of drivers matched at run time. `hal/virtio/` and `hal_bus_scan` are already both | docs/targets.md |
| Sep 2026 | **A portable file explaining itself in one architecture's vocabulary hides whether the *claim* is portable or only the code is.** `kernel/`'s thirteen files compile for both boards with no `#ifdef` and their comments still said EL0, SP_EL1, TTBR0, CNTFRQ_EL0 and `svc`. Two were not merely parochial but wrong: `USER_TEXT_VA` and friends carried absolute addresses that are false on x86-64, and `process.c` justified a shared code mapping as an ARM rule about memory attributes when the requirement is general and x86 spells it with the PAT. The twelve references left name both boards on purpose | kernel/process.c |
| Sep 2026 | **On a PC there is no board, there is a bus.** Embedded machines do not enumerate - the Pi 5's UART is at an address you are told - so a board file is the right unit there. A PC enumerates itself through ACPI, PCI and USB, so a PC target is a discovery *mechanism* plus a driver pool; `hal/alienware-x14.c` would be wrong for the next laptop and wrong for this one after a firmware update. Which also makes `hal/pc/` an honest misnomer: it is QEMU's q35, and `power.c` says so about an address it hardcodes | docs/targets.md |

---

## Language

**Everything in this repository is written in English.** Documents, source code, comments, commit messages, identifiers, test names, log output. No exceptions.
