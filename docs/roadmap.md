# Roadmap

Thirteen milestones. Each has a verifiable **definition of done**: something that either runs or does not, with no room for interpretation.

The rule: a milestone does not start until the previous one meets its criterion. The temptation to pull things forward "because they are cheap" is the main way projects like this scatter and die.

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
- ARM generic timer, ticking at 100Hz
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
- `hal/pi1/` or `hal/pi5/`, depending on which cable arrives first
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
- Hot reload level 1: `load()` of new code while preserving state
- Init and basic supervision
- The Lua shell as a REPL against the system

**Definition of done:** from the shell, mount the same ramfs server at two different paths in two different processes, and have each see only its own. And reload the console server's code while a client is connected, without the client noticing.

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

---

## M8 — Own filesystem

FAT32 is enough to boot and it is horrible: no attributes, no journaling.

**What gets built**

- An own format with native typed attributes
- **Name, size and modification date always indexed**, for every file, without anyone declaring them. It is what BFS did and it is why a query by name was fast regardless of how many files there were. Other attributes get indexed when declared
- Journaling, with writes batched into the journal before going to their final location. Beyond protecting the structure, **it improves performance**: disks are good at writing large blocks, and writing 100K costs almost the same as writing 1K
- Format and fsck tools

**What does NOT get built:** a relational database underneath the filesystem. BeOS tried it through DR8 and pulled it at DR9 because maintaining it was hideously complex and cost too much performance. They lost very little functionality replacing it with a filesystem *shaped like* a database. It is the most useful warning that project left behind. See `beos.md` §17.1

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
- Lazy FP save in the context switch
- Loading the 4MB WAD from the filesystem

**Definition of done:** Doom at 35fps in an app server window, on real hardware.

**Why it matters:** Doom is the first userland process that is not Lua. It proves the message boundary is a real boundary and not a language convention. If Doom runs as a normal citizen, with its own namespace and declared capabilities, Kosmos is an OS and not a Lua runtime with pretensions.

---

## M11 — Drivers, by interest

No fixed order. Picked up as the appetite appears.

- **GPIO, I2C, SPI.** On the Pi 5 this requires an RP1 driver over PCIe, which is serious work. On the Pi 1 it is direct.
- **USB: XHCI + HID.** A real keyboard. Tedious but bounded and well documented.
- **Networking: ported lwIP**, with connections exposed as namespace nodes, Plan 9 style. The property worth testing: mount another machine's `/net` into your local namespace, and have a process use that computer's network without knowing.

**Out of scope, no discussion:** WiFi, Bluetooth, audio with decent latency, GPU-accelerated 3D, and any system-level POSIX personality.

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
