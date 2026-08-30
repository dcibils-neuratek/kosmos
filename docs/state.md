# State

**Update at the end of every session.** This file is what keeps you from starting over each time.

Last updated: 2026-08-30

---

## Current milestone

**M5 — Namespaces and servers.** Its definition of done is met, both halves. Userland init and taking Lua out of the kernel are what remain.

**M4 — Lua to userspace.** Its definition of done is met. Two of its listed pieces are not built; see below.

**M3 — Microkernel. Done.**

**M2 — Lua in the kernel + second target**, whose remaining half is the second target and is blocked on cables.

Definition of done: a `>` prompt over serial where `2+2` returns `4`, under QEMU **and** on real hardware.

**The QEMU half is done.** The prompt runs Lua 5.4.8 with coroutines, closures, the string and math libraries, and errors caught by `pcall`.

**The hardware half is blocked on cables** and is the only thing left in M2.

**M0 and M1 are closed.**

## Active target

QEMU `virt` aarch64, and nothing else. Real hardware arrives at M2.

## Working

`make qemu`, `make test`, `make bench`, `make bench-record`, `make debug`, `make disasm`, `make size`, `make clean`.

94 tests. Five benchmarks. 471 KB of kernel image, which now carries the userland image inside it.

**The prompt is a process.** What you type is read by the console server, sent to the shell over IPC, evaluated in the shell's own `lua_State`, and printed back the same way.

```
Kosmos shell. A process, talking to servers.
Try: fs.list("/data")   fs.read("/data/sensor")   2+2

kosmos> 2+2
4
kosmos> fs.write("/data/sensor", { celsius = 47.2, unit = "C" })
true	nil
kosmos> fs.read("/data/sensor").celsius
47.2
kosmos> fs.list("/data")[1]
sensor
kosmos> sys.write("direct")
-102
kosmos> fs.read("/nowhere")
nil	no such path: /nowhere
```

`sys.write` returning −102 is the point: the shell **cannot** print directly. Only the console server holds the serial port, and everything else has to ask it.

## M5 — where it stands

- [x] The protocol: `list`, `read`, `write`, `getattr`, `setattr`, over typed records
- [x] Per-process namespaces: a mount table in the process, so what is not mounted does not exist
- [x] Lua coroutines as the servers' concurrency layer
- [x] Console server, owning the device
- [x] ramfs
- [x] Capability transfer over IPC, so userland can do its own mounting
- [x] The Lua shell as a REPL against the system
- [x] **Definition of done, both halves: the same server mounted at two paths in two processes, each seeing only its own; and the console server's code replaced while the shell was talking to it, without the shell noticing**
- [x] Hot reload level 1: `load()` of new code while preserving state and clients
- [ ] Init and supervision in userland — needs a spawn syscall
- [ ] Removing Lua from the kernel

**The kernel is playing init**, which is the only temporary part of the arrangement. `roadmap.md` puts init in userland and that needs a process able to start processes.

**Lua is out of the kernel's boot path but still compiled in**, because the test suite drives the kernel through it. Taking it out entirely means moving those tests to userland, which is its own piece of work rather than a line to delete.

**Hot reload works because state and behaviour are separate things.** A server is a `state` table plus a factory that turns it into handlers, and `serve` takes the factory rather than the handlers. Anything captured in a closure built at startup is lost on reload; anything in `state` survives, because the new handlers are handed the same table. That is the whole mechanism, and getting it wrong is silent: the server keeps working and quietly forgets.

**A reload that does not compile is refused with the old code still serving.** Load, run, and install, in that order, each checked. A server that half-reloads is worse than one that refuses.

**Device access is a boolean, not a capability.** `process_grant_console` sets a flag, and the flag is checked by `sys.write` and `sys.getchar`. It enforces the property that matters — exactly one process owns the console and everything else asks it — but a device should be named the way everything else is, by a capability the process holds. That needs a capability that names a device rather than an endpoint.

## M4 — where it stands

- [x] One `lua_State` per process, with a bounded heap (2 MB, mapped by the kernel and not growable)
- [x] The process running at EL0, in its own address space
- [x] Syscall bindings validating capabilities, and every pointer checked before it is touched
- [x] The Lua table serialiser for IPC
- [x] Deciding which Lua libraries exist inside a process
- [x] Loading Lua code from an image embedded in the kernel
- [x] **Definition of done: two processes at EL0, separate address spaces, exchanging a Lua table. And `*(nil)` kills only the process.**
- [ ] Removing Lua from the kernel
- [ ] Benchmarks: allocating and freeing a table; a syscall from Lua versus the same one from C

**Why Lua is still in the kernel.** The REPL runs on it, and moving the REPL out means a process that owns the console, which is the console server, which is M5. Taking Lua out first would leave nothing to type at. It goes when the REPL does.

**Two things are deliberately temporary, and both are recorded where they are written:**

*The kernel is in TTBR0, alongside every process.* `state.md` previously said M4 would move it to TTBR1. It does not, because that is a large refactor of boot, the linker script and every kernel pointer, and it is not what buys isolation. Isolation is the AP bits: every kernel mapping is `AP=00`, EL1 read/write and no EL0 access, with PXN and UXN set. A process faulting on the kernel image gets a **permission** fault, not a translation fault — the page is in its own tables and still untouchable. TTBR1 becomes worth doing when a process needs the whole low half.

*The reply token is a raw kernel pointer.* `sys.receive` hands EL0 the address of a `struct thread`. It is safe only because `ipc_reply` checks the target is really waiting, and a value that is safe only because of what the callee checks is one audit away from not being safe. At M5 it becomes a capability index like everything else.

## M3 — done

- [x] Threads with their context, and a context switch in assembly
- [x] Round-robin scheduler with a per-CPU runqueue, behind a pluggable `struct scheduler`
- [x] Synchronous IPC: rendezvous, call, receive, reply
- [x] A per-thread capability table, indexed, with generation numbers
- [x] A separate exception stack, so a stack overflow is readable rather than a double fault
- [x] **Definition of done: 100,000 round trips, cost per round trip printed and baselined**
- [x] Address spaces: create, destroy, map pages
- [x] Syscalls exposed as Lua functions, as `sys`
- [x] Preemption, in the vector's epilogue

**One thing is deliberately temporary.** Every address space contains the kernel, because there is no TTBR1 split yet: the kernel is identity mapped through TTBR0 like everything else, so a space without it would fault on the instruction after the switch. A new space copies the kernel's top level and shares the tables below it, which is why user mappings are confined to virtual addresses at or above 2 GB and anything lower is refused rather than allowed to quietly edit the kernel's own map.

At M4 that is replaced by the real arrangement: the kernel in TTBR1 at the top, TTBR0 belonging entirely to the process, and a space containing no kernel at all.

## Concrete next step

**M4, or M2's second target when a cable arrives.**

M4 is the milestone `roadmap.md` calls the hardest in the project, and the one where the design gets tested: one `lua_State` per process with a bounded heap, the process running at EL0, syscall bindings validating capabilities, the Lua table serialiser for IPC, and Lua out of the kernel entirely.

Two things from M3 are the ground it stands on and are already correct: every thread owns both of its stacks, and `struct context` saves `SPSel` and `DAIF`, which is what makes a switch work from either thread context or an exception epilogue.

Two things will have to change rather than extend. The kernel has to move to TTBR1 so a process's address space contains no kernel. And `sys` becomes real syscalls across a privilege boundary, with the inspection half of it destined for `/proc` at M5.

**M2 cannot be closed without a cable**, and its remaining half is the point of the milestone: the HAL takes its real shape once there are two implementations to compare, and `hal.md` is explicit that writing that interface against one target produces the shape of QEMU wearing generic names. Nothing is gained by guessing at it now.

So the choice is between waiting and starting M3, whose work is all CPU-side and needs no hardware: address spaces, threads, a context switch, a round-robin scheduler, synchronous IPC, and a capability table.

**One thing from M1 has to be settled before M3's threads, not after.** An exception taken on an exhausted stack is currently a double fault, because the handler builds its frame on the stack that just overflowed. With one stack that is a hang; with a thread per stack it is a hang that is hard to attribute. The fix is a separate exception stack, and M3 is where it belongs.

**And the FP question comes due at the same moment.** The context switch will not save FP state, and Lua's numbers are doubles. While Lua is on one thread that is fine. The moment it is not, this needs lazy FP save.

**Under QEMU, now:** the minimal freestanding libc (`memcpy`, `memset`, `memmove`, `strlen`, `strcmp`, `strcpy`, `strchr`, `setjmp`/`longjmp`, a minimal `snprintf`, and the `math` functions Lua asks for), then upstream Lua 5.4 built freestanding with its allocator pointing at the page allocator, then a REPL over the UART.

`memset` and `memcpy` already exist in `kernel/string.c`, written because GCC emits calls to them from a zeroing loop even under `-ffreestanding`. They are the kernel's own and deliberately naive; the userland libc is a separate thing.

**`setjmp`/`longjmp` is the one that demands care.** Lua uses it for error handling, and getting it wrong produces a bug that only appears the first time something raises an error, long after it was written.

**When a cable arrives:** `hal/pi1/` or `hal/pi5/`, and then the HAL interface takes its real shape with two implementations in front of it. Not before. Today's interface is the shape of QEMU with generic names, and that is fine until there is something to compare it against.

## Decisions taken while implementing

Decisions that came out of writing code go here. Format: date, what was decided, why.

Design decisions (as opposed to implementation ones) go in the decision log in `README.md` and are propagated to `design.md` and `roadmap.md` in the same session.

**2026-08-30 — The toolchain is the official ARM GNU 14.2.Rel1 `aarch64-none-elf`, unpacked under `~/toolchains`.** The Homebrew recipe `setup.md` used to recommend (`aarch64-unknown-linux-gnu`) targets Linux and brings glibc and Linux start files, which is what `-ffreestanding -nostdlib -nostartfiles` exists to avoid. It also produces differently named binaries. `setup.md` corrected in the same session. Homebrew is not installed on this machine and MacPorts has no `aarch64-elf-gcc` port, so the ARM tarball was the only path that lands on the documented binary names.

**2026-08-30 — A capability travels out of band, never inside the serialised bytes.** An index means something only in the table it came from, so the kernel translates it on delivery and what arrives is the receiver's own index. Stored as index plus one, so that zero — which is what `{0}` gives — means none.

**2026-08-30 — Anything that is "one per running thing" is stored on the running thing.** `current_process` and the address space were both global-shaped and both broke the moment there were two processes. The same shape appears again wherever a global is "enough for now".

**2026-08-30 — A thread is runnable the instant it exists, and preemption makes that instant real.** Anything created and then configured on the next line has already run. `thread_create_suspended` exists for that, and the race was found three separate times before the habit stuck: once in processes, once in `sys.spawn`, and once in tests.

**2026-08-30 — Isolation is the AP bits, not the address space layout.** Every kernel mapping is `AP=00` with PXN and UXN, so a process cannot touch kernel memory whether or not the kernel is mapped in its space. A process reading the kernel image gets a permission fault at level 3, not a translation fault, which is the difference stated as plainly as it can be.

**2026-08-30 — The address space follows the thread, and the process pointer lives on the thread.** Both were global-shaped and both broke the moment there were two processes: whichever ran last owned TTBR0 and owned `current_process`, so one process ran with another's memory underneath it and its syscalls checked pointers against the wrong address space. Anything that is "one per running thing" has to be stored on the running thing.

**2026-08-30 — A process is built before it is startable.** `process_create` leaves it suspended and `process_start` makes it runnable, because a runnable process runs: one created and granted its capabilities on the next line had already exited by then. Removing a race by construction beats masking interrupts around it.

**2026-08-30 — Copy the length, never the buffer.** A round trip moves a message five times, and copying all 512 bytes regardless of use made it thirty-six times slower. The benchmark caught it on the first run, which is the entire argument for having had one since M3.

**2026-08-30 — Preemption switches in the vector's epilogue, never in C.** A context switch moves `SP_EL1`, and everything after the switch reads the frame at `sp`, so that frame has to belong to the thread about to be resumed. Splitting the decision (`thread_tick`, in the handler, asking the policy) from the act (`thread_preempt_if_needed`, in the epilogue) is what lets the decision stay a C function the policy owns.

**A consequence that cost an instruction abort at an address that was never code:** `context_switch` had assumed `SPSel` was 0. True for a thread that yields, false for one arriving from the epilogue where taking the exception already set it to 1. `SPSel` is now part of the saved context.

**2026-08-30 — Every address space contains the kernel, and that is temporary.** There is no TTBR1 split, so a space without the kernel would fault on the instruction after the switch. A space copies the kernel's top level and shares the tables below, so user mappings are confined to 2 GB and above and anything lower is refused. M4 replaces this with the kernel in TTBR1.

**2026-08-30 — `sys` is a preview of the interface, not the interface.** At M4 these become real syscalls across a privilege boundary, and at M5 the inspection half disappears into `/proc`, read through the namespace protocol like every other resource. `design.md` §9.5 is emphatic that there must not be a second way to reach it, so `sys.threads` and `sys.memory` are scheduled for deletion rather than for extension.

**2026-08-30 — Every spawned Lua thread gets its own `lua_State`.** Not a design choice so much as the only thing that works: a `lua_State` is not reentrant, and two kernel threads inside one would corrupt it. `design.md` §2's share-nothing userland arrives early because the alternative does not run. They currently share one `malloc` heap; per-state heap limits are M4's problem.

**2026-08-30 — The scheduling policy is behind an interface, not wired into the thread code.** `thread.c` owns the mechanism and `struct scheduler` owns which runnable thread runs next, so a different algorithm is a new file. Per-thread policy state is embedded in `struct thread` rather than reached through a pointer, because there is no allocator to hand a policy its own storage; the fields are named for what algorithms need generally rather than for round robin. A test installs a deliberately terrible LIFO policy and asserts both exact orderings, which is the only proof the seam is real.

**2026-08-30 — Kernel threads run on SP_EL0 and take exceptions on SP_EL1.** Taking an exception always sets `SPSel`, so the hardware hands the handler a different stack with no code to switch it. That is what makes a stack overflow readable rather than a double fault, and it makes a double fault name itself: a fault in ordinary code lands in the first quarter of the vector table and one inside the handler lands in the second.

**A consequence that cost time:** `SP_EL1` cannot be named from EL1, because its system-register encoding is an EL2 one. `mrs x10, sp_el1` there is not a trap, it is an undefined instruction, and it arrives as EC 0x00 "unknown reason" explaining nothing. Reaching it means making it the current stack pointer briefly with `SPSel`, which in turn means the context switch has to mask interrupts.

**2026-08-30 — A thread is on exactly one IPC queue at a time.** All three of an endpoint's queues thread through the same link field, so a thread on two of them silently truncates one. Written as a warning in a comment and then done anyway; the symptom appeared three tests away from the cause.

**2026-08-30 — Dead threads release their slot, and their stacks go with it.** The stacks are already allocated with their guard pages already unmapped, which is the state a new thread wants. A recycled slot has its capability table cleared: inheriting the dead thread's capabilities would let a new thread reach endpoints it was never handed, and everything would appear to work.

**2026-08-30 — FP and SIMD are enabled at EL1, and the kernel's own C still is not allowed to use them.** `CPACR_EL1.FPEN` resets to trapping every FP access at EL0 and EL1 alike. Two things need them: `setjmp`/`longjmp` save `d8`–`d15` because AAPCS64 makes them callee-saved, and Lua's numbers are doubles. Neither is optional, so the trap has to go. `-mgeneral-regs-only` stays on every C file, so the kernel still cannot emit an FP instruction by accident; the flag restricts code generation, not what hand-written assembly may save.

Found the hard way: the first `setjmp` panicked with EC 0x07, whose name ("unhandled exception class") says nothing about floating point. There is now a test that executes an FP instruction, not merely one that reads `CPACR`.

**The consequence to carry into M3:** the context switch will not save FP state. While Lua is on a single thread that is fine. The moment it is not, this needs lazy FP save, which `roadmap.md` schedules at M10 for Doom and which will be needed sooner.

**2026-08-30 — The libc lives in `runtime/` and the kernel shares it.** At M2 there is one address space and one image, so a second copy in `kernel/` would be a duplicate symbol rather than a boundary. `kernel/string.c` is gone. The split happens at M4, when Lua moves to EL0 and `runtime/` becomes what the design calls it: the libc inside a process, whose I/O resolves against that process's namespace and nowhere else.

**2026-08-30 — QEMU `virt` defaults to a GICv2; Kosmos drives a GICv3 and passes `gic-version=3`.** Read out of QEMU's own device tree rather than assumed, after the same documents turned out to be wrong about the entry exception level. The flag is on the QEMU line in the Makefile and in `tools/run_tests.py`, and the two have to stay in agreement. `hal.md` corrected, including the claim about the Pi 5's GIC, which is now marked as an open question instead of an assumption.

**2026-08-30 — The timer rearms from the previous deadline (CVAL), never from the current time (TVAL).** TVAL sets the comparator to "now plus interval", where "now" is when the handler runs, so every period absorbs the cost of taking the interrupt and the error accumulates. Under QEMU that cost is about 195,000 counter ticks, roughly 3 ms against a 10 ms period, and a nominal 100 Hz ran at 73: eight ticks in eleven seconds of wall clock. Measured, not reasoned about. There is a test that fails if it is ever changed back, and the "ticks advance" test passes with the bug, which is why the second one exists.

**2026-08-30 — A null dereference cannot be written in C.** GCC is entitled to assume undefined behaviour never happens, so it emits the store and then treats the rest of the function as unreachable, appending a `brk`. The store faults, the handler steps ELR past it, and execution lands on the `brk`: a second fault with the expectation already spent, and a panic. Every deliberate fault in the tests goes through an inline-assembly store.

**2026-08-30 — Expected faults recover by stepping ELR, not by `setjmp`.** There is no `setjmp` until the libc arrives at M2, and every A64 instruction is four bytes, so the arithmetic is exact. Only synchronous exceptions are recoverable this way: an IRQ did not come from the instruction at ELR.

**2026-08-30 — On AArch64, `SYS_EXIT` takes a pointer, not a status.** `x1` holds the address of a two-field block: the reason code (`ADP_Stopped_ApplicationExit`, `0x20026`) and then the exit status. Passing the status directly in `x1` is the AArch32 form; it assembles, it runs, and QEMU exits 0 no matter what the guest meant. A harness that always reports success is worse than no harness, so the failure paths were exercised rather than assumed: a failing test, a hanging test, a missing banner, a build error, and QEMU's exit code on its own. All five produce a non-zero exit.

**2026-08-30 — The test image is a separate build under `build/test/`.** Same sources plus `tests/`, with `-DKOSMOS_TEST`. Two directories rather than one so the normal image never carries test code and the two cannot share a stale object file.

**2026-08-30 — `README.md` and `CLAUDE.md` moved from `docs/` to the repository root.** Their links were written relative to the root (`docs/design.md`), so from inside `docs/` every one of them resolved to `docs/docs/...` and was broken. `CLAUDE.md` also has to be at the root for Claude Code to load it automatically.

## Known bugs

**Characters typed before the prompt appears are lost.** The PL011's receive FIFO is sixteen bytes and nothing drains it until the REPL starts, so anything pasted into the terminal during boot overflows it silently. A person typing at a live prompt never sees this; it showed up feeding the REPL from a pipe. The fix is an interrupt-driven receive path with a ring buffer, which is worth doing when there is a real terminal at M6 and not before.

**An exception taken while the stack is exhausted is a double fault.** The handler builds its frame on the stack that just overflowed, so it faults again inside the vector and the kernel hangs with no output. The guard page turns a silent overflow into a readable abort, which is the improvement; it does not survive one. The fix is a separate exception stack, and it belongs with the thread work at M3.

**A minimal `kmain` with no stack faults silently.** Found while smoke-testing the toolchain. On QEMU `virt` the reset value of `sp` is 0, so any function prologue touching the stack writes to unmapped memory, takes an exception with no vector installed, and hangs with no output. It is not a bug in the system, it is the reason `boot/start.S` sets `sp` before branching to C. Recorded because the failure mode is a silent hang, which is indistinguishable from twenty other causes.

## Hardware pending

- [ ] 3-pin JST-SH debug UART cable (Pi 5) — blocks M2 on that target
- [ ] 3.3V USB-serial adapter (Pi 1) — cheaper and arrives sooner

---

## Overall progress

| # | Milestone | Status |
|---|---|---|
| 0 | Boot under QEMU | **done** |
| 1 | MMU, exceptions, timer | **done** |
| 2 | Lua in the kernel + second target | **in progress** |
| 3 | Microkernel | **done** |
| 4 | Lua to userspace | **definition of done met** |
| 5 | Namespaces and servers | **in progress** |
| 6 | Graphics and app server | |
| 7 | Attributes, live queries, replicants | |
| 8 | Own filesystem | |
| 9 | Software 3D demo | |
| 10 | Doom | |
| 11 | Drivers (GPIO, USB, network) | |
| 12 | SSH client | |
