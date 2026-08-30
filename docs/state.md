# State

**Update at the end of every session.** This file is what keeps you from starting over each time.

Last updated: 2026-08-30

---

## Current milestone

**M1 — MMU, exceptions, timer**

Definition of done: the timer prints one tick per second, and a deliberate `*(volatile int*)0 = 1` prints a readable data abort with the causing address instead of hanging.

The second half matters more than the first. An exception handler that says what happened and where is the tool used for the entire rest of the project.

**M0 is closed.** `make qemu` prints "Kosmos" and `make test` exits 0.

## Active target

QEMU `virt` aarch64, and nothing else. Real hardware arrives at M2.

## Working

`make qemu`, `make test`, `make debug`, `make disasm`, `make size`, `make clean`.

M0 in full: boot at EL1 on core 0 with the secondaries parked, PL011 output, and a host-side test runner reading TAP over the serial line with the exit code set by the guest through semihosting. 620 bytes of text, 16 KB of .bss (all of it the boot stack). Four tests passing.

## M0 — done

- [x] `aarch64-none-elf-gcc` toolchain installed and verified (ARM GNU 14.2.Rel1)
- [x] `qemu-system-aarch64` installed (9.1.2, MacPorts)
- [x] Repository directory structure
- [x] Linker script
- [x] `boot/start.S`: core ID, `CurrentEL` drop to EL1, stack, zero `.bss`, jump to C
- [x] `hal/qemu-virt/uart.c`: PL011
- [x] `kernel/main.c`
- [x] Makefile with `qemu`, `test`, `debug`, `clean`
- [x] `tools/run_tests.py`: launches QEMU, reads TAP over serial, exit code
- [x] Guest exit via semihosting (`SYS_EXIT` through `HLT #0xF000`)

## Concrete next step

M1, in this order: the physical page allocator (a bitmap over available RAM), then the exception vector, then the MMU, then the timer.

**The exception vector goes before the MMU on purpose.** Turning the MMU on is the step most likely to fault, and doing it without a handler that prints ESR, ELR and FAR means debugging a silent hang. Doing it with one means reading a message that says which instruction faulted on what address.

Two things to get right while writing the vector, because retrofitting them is worse:

- **Expected-exception support.** A flag plus `setjmp` so a test can fault on purpose, record it, and carry on. Several M1 tests need it and `testing.md` §18.2 calls it out.
- **The 16 entries.** Not just the one that is being used.

## Decisions taken while implementing

Decisions that came out of writing code go here. Format: date, what was decided, why.

Design decisions (as opposed to implementation ones) go in the decision log in `README.md` and are propagated to `design.md` and `roadmap.md` in the same session.

**2026-08-30 — The toolchain is the official ARM GNU 14.2.Rel1 `aarch64-none-elf`, unpacked under `~/toolchains`.** The Homebrew recipe `setup.md` used to recommend (`aarch64-unknown-linux-gnu`) targets Linux and brings glibc and Linux start files, which is what `-ffreestanding -nostdlib -nostartfiles` exists to avoid. It also produces differently named binaries. `setup.md` corrected in the same session. Homebrew is not installed on this machine and MacPorts has no `aarch64-elf-gcc` port, so the ARM tarball was the only path that lands on the documented binary names.

**2026-08-30 — On AArch64, `SYS_EXIT` takes a pointer, not a status.** `x1` holds the address of a two-field block: the reason code (`ADP_Stopped_ApplicationExit`, `0x20026`) and then the exit status. Passing the status directly in `x1` is the AArch32 form; it assembles, it runs, and QEMU exits 0 no matter what the guest meant. A harness that always reports success is worse than no harness, so the failure paths were exercised rather than assumed: a failing test, a hanging test, a missing banner, a build error, and QEMU's exit code on its own. All five produce a non-zero exit.

**2026-08-30 — The test image is a separate build under `build/test/`.** Same sources plus `tests/`, with `-DKOSMOS_TEST`. Two directories rather than one so the normal image never carries test code and the two cannot share a stale object file.

**2026-08-30 — `README.md` and `CLAUDE.md` moved from `docs/` to the repository root.** Their links were written relative to the root (`docs/design.md`), so from inside `docs/` every one of them resolved to `docs/docs/...` and was broken. `CLAUDE.md` also has to be at the root for Claude Code to load it automatically.

## Known bugs

**A minimal `kmain` with no stack faults silently.** Found while smoke-testing the toolchain. On QEMU `virt` the reset value of `sp` is 0, so any function prologue touching the stack writes to unmapped memory, takes an exception with no vector installed, and hangs with no output. It is not a bug in the system, it is the reason `boot/start.S` sets `sp` before branching to C. Recorded because the failure mode is a silent hang, which is indistinguishable from twenty other causes.

## Hardware pending

- [ ] 3-pin JST-SH debug UART cable (Pi 5) — blocks M2 on that target
- [ ] 3.3V USB-serial adapter (Pi 1) — cheaper and arrives sooner

---

## Overall progress

| # | Milestone | Status |
|---|---|---|
| 0 | Boot under QEMU | **done** |
| 1 | MMU, exceptions, timer | **in progress** |
| 2 | Lua in the kernel + second target | |
| 3 | Microkernel | |
| 4 | Lua to userspace | |
| 5 | Namespaces and servers | |
| 6 | Graphics and app server | |
| 7 | Attributes, live queries, replicants | |
| 8 | Own filesystem | |
| 9 | Software 3D demo | |
| 10 | Doom | |
| 11 | Drivers (GPIO, USB, network) | |
| 12 | SSH client | |
