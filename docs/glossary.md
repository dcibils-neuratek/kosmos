# Glossary

Written to be re-read six months from now without having to look anything up.

---

## AArch64 architecture

**EL0 / EL1 / EL2 / EL3** — Exception Levels. ARMv8's privilege levels. EL0 is userland, EL1 the kernel, EL2 the hypervisor, EL3 the secure monitor. Which level you are handed depends on the machine and its options: `-M virt` starts at EL1, `-M virt,virtualization=on` at EL2, `-M virt,secure=on` at EL3. The Pi firmware hands off at EL2. Read `CurrentEL` rather than assuming.

**MMU** — Memory Management Unit. The hardware that translates virtual addresses to physical ones using the page tables you build.

**Long-descriptor page tables** — ARMv8's table format: four levels, 64-bit entries. ARMv6 (Pi 1) uses short-descriptor, a different format with two levels and 32-bit entries.

**TLB** — Translation Lookaside Buffer. The MMU's translation cache. If you change a page table you have to invalidate the TLB or the hardware keeps using the stale translation.

**TLB shootdown** — With multiple cores, invalidating the other cores' TLBs after changing a table. On AArch64 the broadcast is done in hardware (`TLBI` with an `IS` suffix), which saves considerable work compared to x86.

**Memory barrier** — An instruction that forces an ordering on memory accesses. `dsb` (data synchronization barrier), `dmb` (data memory barrier), `isb` (instruction synchronization barrier). Necessary because AArch64 has a weak memory model.

**Weak memory model** — The CPU may reorder memory accesses aggressively. x86 has TSO, which is far more conservative. Code that works on x86 can fail on ARM without barriers, once every thousand boots.

**MMIO** — Memory-Mapped I/O. A peripheral's registers appear as memory addresses. Writing there controls the hardware.

**ESR / ELR / FAR** — Registers the CPU fills in when an exception occurs. ESR says what kind of exception it was, ELR the address of the instruction that caused it, FAR the memory address that was accessed. The three together turn a hang into a useful message.

**GIC** — Generic Interrupt Controller. ARM's interrupt controller. GICv2 and GICv3 are quite different from each other; the Pi 5 and QEMU virt use v3.

**TPIDR_EL1** — A general-purpose register reserved by convention to point at the per-CPU struct. Using it from day one makes SMP cheap later.

**AltiVec / NEON** — The SIMD units on PowerPC and ARM. They process several values per instruction. Useful for the blitter, optional.

---


## The parts of the system, and which language each is

Eight words, and they are not interchangeable. The distinction that does the
most work is between **something you run** and **someone you ask**.

**The kernel** is Nebula: threads, address spaces, IPC and capabilities, and
nothing else. It does not know what a file is, what a pixel is, or what a
network is. It runs at EL1 and is the only thing that does. Its whole job is
to make processes exist, keep them apart, and let them send each other
messages.

**A driver** is the code that touches hardware, in `hal/`. A UART, a timer,
a block device, a virtio queue. One per board, behind a common interface.

**A server is a process that owns something.** That is the definition, and
it is about ownership rather than about code. A server was handed a
capability nobody else holds, and it rents the thing out through a message
protocol: the console server owns the serial port and the keyboard, so no
other process can print - it *asks*. The disk server is the only process
with `SPAWN_DISK`, so it is the only thing that can read a raw sector,
whatever any namespace says. The window manager owns the screen.

Mechanically a server is `serve(endpoint, state, handlers)`: own an
endpoint, receive a typed message, reply. What makes it worth the boundary
is not the loop, it is that the boundary is real - "only the disk server can
corrupt the disk" is true because nothing else can name the disk.

**A kit is C that runs inside your own process.** `use("/kits/compress")`
hands back a table of C functions compiled into your address space. No
process, no message, no ownership: calling it is a function call. `kits` at
the prompt lists what a machine has; today that is `/kits/compress` to
inflate, `/kits/pdf` to scan a content stream, `/kits/gl` for TinyGL, and
`/kits/console` for the console's wire format.

That last one is the odd one and worth knowing about, because it is a kit for
a reason none of the others share. `/dev/console` is the only protocol here
with **two implementations**: a terminal window mounts itself as its child's
console, so an application answers the same ABI the server does. The kit is
where that layout is compiled once, rather than living as a format string in
the namespace and a second copy inside the terminal.

So: **a kit is code you run; a server is someone you ask.** That is why
inflate is a kit - it computes - and the disk is a server - it owns.

**A library is the same position, in Lua.** `use("/lib/ui.lua")` loads Lua
source into the caller's own environment. `ui`, `panel`, `pdf`, `kfs`.

**A program** is console-based: it prints, it reads lines, it lives in
`/bin` and you type its name at the prompt. `ls`, `cat`, `htop`, `stress`.

**An app** is graphical: it opens a window, it is driven with the pointer
and the keyboard, and it appears in the Deskbar. It says so with
`-- kosmos: application` in its opening comment, which is how the program
store knows to list it. Graphical is the intended way to use this system;
when something here says "let us build an app", it means a window.

**A tool does not run on Kosmos at all.** `tools/` is host-side: the test
runners, the benchmark harness, `kfs.lua` writing a disk image from the Mac,
`bdf2c.py` converting a font at build time. They are the workshop, not the
machine.


## Which language, and why

C is for anything that touches hardware, defines the isolation boundary, or
sits where a pause would be felt. Lua is for anything whose bugs can only
kill their own process and whose shape will keep changing.

| | language | the reason |
|---|---|---|
| kernel | C | it is the isolation boundary |
| drivers | C | hardware, and loops over bytes |
| kits | C | finished algorithms, and no collector |
| servers | C | something else's timing depends on them |
| the namespace and init | Lua | run in the caller's own process; not servers |
| libraries | either | whichever the code is shaped like |
| apps and programs | Lua | a crash kills only itself |
| tools | the host's language | they never run here |

The servers row used to read *"servers on the frame or packet path"*, with a
second row keeping the policy servers in Lua. That was a judgement made per
server, and it failed: the audio server sits on the period path, which is the
frame path with a different clock, and nobody recognised it as a third case.
A rule that needs you to spot a third case will miss the fourth, so the layer
decides now and all seven moved. `diskfs` is the exception and not for
language reasons: `kfs.lua` runs on the host too, which is what tests the
journal without booting a machine.

**The argument for C is jitter rather than speed**, and that is worth being
exact about. Structure-shaped code in Lua costs about 2%, measured - nothing.
A loop over bytes costs 30%, and the PDF scanner was 110x. But the number
that decides a *server* is `gc_pause_max`: about 1.25 ms, arriving when the
collector decides. A frame is 16 ms. No amount of optimising the Lua removes
that pause, and responsiveness is a promise about the worst case.

**The argument for Lua used to be hot reload**, and it was not sentimental:
M5's definition of done was replacing a server's code while a client was
mid-conversation with it. There is no dynamic linking, so a C server cannot
be reloaded at all, and moving one gave that up rather than trading it.

That argument is gone, because reload is - removed in September 2026 when
the last reloadable server became C. See `design.md` §10; the honest word is
*removed* rather than *outranked*.

**What argues for Lua now is the shape of the bug.** A Lua module cannot have
a buffer overflow. Isolation is identical either way, since both are EL0
processes behind an address space, but one failure is a stack trace and the
other is an evening. So C is for a loop over bytes or a place where a
collector pause would be felt, and 2% is not worth the difference anywhere
else.


## Microkernel and IPC

**Microkernel** — A design where the kernel does the minimum (threads, memory, IPC) and everything else (drivers, filesystem, networking) runs in userland processes. The opposite is monolithic, where all of that lives inside the kernel.

**IPC** — Inter-Process Communication. How two processes that share no memory talk to each other.

**Synchronous IPC / rendezvous** — The sender blocks until the receiver is ready. No buffering in the kernel. Simpler and faster than asynchronous, at the cost of code being harder to write. Coroutines solve that.

**Capability** — A permission that is also the only way to name something. A process has an array of endpoints and syscalls take an index. If you were not handed the capability, you cannot even name the resource.

**Endpoint** — The point where a server receives messages. A capability points at an endpoint.

**Address space** — A process's memory map. Two processes with different address spaces cannot see each other's memory.

**Context switch** — Saving the outgoing thread's registers and loading the incoming one's. Written in assembly, and getting it wrong corrupts things that only show up five functions later.

**SMP** — Symmetric Multiprocessing. Several cores running at once.

**IPI** — Inter-Processor Interrupt. An interrupt one core sends to another. Needed for cross-core IPC and for TLB shootdown.

---

## Plan 9 and namespaces

**Namespace** — The map of paths to resources a process sees. In Plan 9 and in Kosmos it is per-process, not global. What is not mounted does not exist.

**9P** — Plan 9's protocol. Every resource, local or remote, is spoken to the same way. The Kosmos protocol is 9P with typed records instead of byte streams.

**Mount** — Placing a server at a point in a process's namespace. `/proc`, `/dev/temp` and `/home` can be three different servers mounted in the same tree.

---

## BeOS

**app_server** — The process that handles windows, decoration and compositing in BeOS. Kosmos has its equivalent, in Lua.

**BLooper / BHandler / BMessage** — BeOS's message model. A `BLooper` was a thread with a message queue; a `BMessage` a typed dictionary. In Kosmos they are coroutines and Lua tables.

**BView** — A rectangle that knows how to draw itself and receive events, with coordinates relative to its parent. The Kosmos equivalent is the UI kit's view.

**Follow modes** — BeOS's layout system: each view declares which edges of the parent it stays attached to. Fifty lines, against the thousands of a constraint solver.

**Live query** — Registering a predicate over attributes and receiving a message when the result set changes. No polling. It is the BeOS feature nobody replicated.

**Replicant** — A view dragged from one app into another that keeps working. In BeOS it was a binary add-on; in Kosmos it is Lua source plus state plus declared capabilities.

**Attributes** — Typed metadata alongside a file, indexed by the filesystem. What makes live queries possible.

---

## Graphics

**Framebuffer** — The memory buffer the display controller scans out to the screen. Writing there is what makes pixels appear.

**Uncached / device memory** — Memory mapped without a cache. The framebuffer usually is, and writing there is 10-50x slower than to normal RAM. Hence the backbuffer rule.

**Backbuffer** — A buffer in cached RAM where everything is drawn, so that a single blit to the framebuffer follows.

**Blit** — Copying a block of pixels from one place to another.

**Pitch / stride** — The bytes one row of the framebuffer occupies. Almost never `width * 4`, because the firmware aligns it.

**Damage tracking** — Keeping the list of rectangles that changed so only those get repainted, instead of the whole screen.

**vblank** — The interval between the end of one scanout and the start of the next. Updating the framebuffer there avoids tearing.

**Tearing** — The artifact of seeing half the screen with the new frame and half with the old.

**Compositing** — Combining the windows into the final image.

**Z-buffer** — In 3D, storing each pixel's depth to know what is in front.

**16.16 fixed point** — Representing decimals with integers: 16 bits of integer part and 16 of fraction. Doom used it, and in Kosmos it is necessary until lazy FP save exists.

---

## Lua

**REPL** — Read-Eval-Print Loop. The interactive prompt where you type an expression, it is evaluated immediately, and the result comes back. In Kosmos it is the shell and the way the system is modified while running.

**Coroutine** — A function that can suspend and resume where it left off. Concurrency without parallelism and without locks. It is what makes it possible to write sequential code over synchronous IPC.

**`lua_State`** — An interpreter's complete state: heap, stack, globals. In Kosmos there is one per process.

**Incremental / generational GC** — Lua 5.4's two collector modes. Incremental takes steps bounded in work, not in time, so it can blow the frame budget. Generational usually does better with many short-lived tables.

**`longjmp`** — Jumping up the stack without unwinding it. Lua uses it for error handling. It is the reason Rust does not fit well as the kernel language here: destructors never run.

**Freestanding** — Compiling without assuming an operating system underneath. No full libc, no syscalls.

---

## Boot and firmware

**Bare metal** — Running with no OS underneath. Your code is all there is.

**Linker script** — The file that tells the linker where each section (`.text`, `.data`, `.bss`) goes in memory.

**`.bss`** — The section of globals initialized to zero. It takes no space in the binary, so it has to be zeroed by hand at boot.

**Mailbox** — The interface through which the Pi's firmware exposes services, among them configuring the framebuffer.

**RP1** — The Pi 5's southbridge, connected over PCIe. USB, Ethernet and the header GPIO hang off it. It is what makes the Pi 5 hostile to bare metal.

**Open Firmware** — The firmware on PowerPC Macs. A Forth environment that stays alive after loading your kernel and exposes a console and a device tree.

**Big-endian / little-endian** — The byte order of an integer in memory. PowerPC is big-endian; ARM and x86 are little-endian. Mixing them forces you to define an explicit wire format.

---

## Project terms

**Surface** — A handle to pixel memory shared between an app and the app server. The justified exception to the drawing-command model. Used by Paint, the 3D demo and Doom.

**Drawing commands (model B)** — An app sends tables describing what to draw, instead of writing pixels. The Kosmos default model.

**Hot reload level 1** — *Removed, September 2026.* A server reloaded its code without losing state or clients. It went when the last Lua server became C: there is no dynamic linking, so nothing that runs here can have its code replaced. `design.md` §10 is the record.

**Hot reload level 2** — A server dies and a supervisor relaunches it; clients reconnect through the namespace. Unaffected by the above, and the architectural property of the two: it never depended on what language a server was written in.

**doomgeneric** — The Doom port that separated the engine from the platform. Five functions to implement.

**Nebula** — the microkernel. Threads, address spaces, IPC, capabilities, and
nothing else. What `kernel/`, `arch/` and `hal/` build into.

**Kosmos** — the operating system: Nebula plus the servers, the namespace, the
desktop and the applications. What somebody uses.

