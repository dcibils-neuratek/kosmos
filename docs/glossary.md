# Glossary

Written to be re-read six months from now without having to look anything up.

---

## AArch64 architecture

**EL0 / EL1 / EL2 / EL3** — Exception Levels. ARMv8's privilege levels. EL0 is userland, EL1 the kernel, EL2 the hypervisor, EL3 the secure monitor. QEMU `virt` starts at EL2 and you have to drop to EL1 by hand.

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

**Hot reload level 1** — A server reloads its code without losing state or clients.

**Hot reload level 2** — A server dies and a supervisor relaunches it; clients reconnect through the namespace.

**doomgeneric** — The Doom port that separated the engine from the platform. Five functions to implement.
