# State

**Update at the end of every session.** This file is what keeps you from starting over each time.

Last updated: 2026-08-30

---

## Current milestone

**M0 — Boot under QEMU**

Goal: get a character out over the PL011 on QEMU `virt`.

Definition of done: `make qemu` prints "Kosmos" in the terminal, and `make test` exits 0.

## Active target

QEMU `virt` aarch64, and nothing else. Real hardware arrives at M2.

## Working

Nothing runs yet. What is verified is the ground under it:

- Toolchain and QEMU installed and exercised end to end
- A throwaway freestanding image built with the mandatory flags, loaded at `0x40080000`, printing a character over the PL011 at `0x09000000` under QEMU `virt`
- Repository structure, with `README.md` and `CLAUDE.md` at the root

## M0 remaining

- [x] `aarch64-none-elf-gcc` toolchain installed and verified (ARM GNU 14.2.Rel1)
- [x] `qemu-system-aarch64` installed (9.1.2, MacPorts)
- [x] Repository directory structure
- [ ] Linker script
- [ ] `boot/start.S`: core ID, EL2→EL1, stack, zero `.bss`, jump to C
- [ ] `hal/qemu-virt/uart.c`: PL011
- [ ] `kernel/main.c`
- [ ] Makefile with `qemu`, `test`, `debug`, `clean`
- [ ] `tools/run_tests.py`: launches QEMU, reads TAP over serial, exit code
- [ ] Guest exit via semihosting (`SYS_EXIT` through `HLT #0xF000`)

## Concrete next step

The linker script, then `boot/start.S`, `hal/qemu-virt/uart.c`, `kernel/main.c`, and the Makefile, in that order. That gets to `make qemu` printing "Kosmos".

Then the test harness, which closes M0 and is not deferred: it is fifty lines and it is what gives every later milestone a safety net.

## Decisions taken while implementing

Decisions that came out of writing code go here. Format: date, what was decided, why.

Design decisions (as opposed to implementation ones) go in the decision log in `README.md` and are propagated to `design.md` and `roadmap.md` in the same session.

**2026-08-30 — The toolchain is the official ARM GNU 14.2.Rel1 `aarch64-none-elf`, unpacked under `~/toolchains`.** The Homebrew recipe `setup.md` used to recommend (`aarch64-unknown-linux-gnu`) targets Linux and brings glibc and Linux start files, which is what `-ffreestanding -nostdlib -nostartfiles` exists to avoid. It also produces differently named binaries. `setup.md` corrected in the same session. Homebrew is not installed on this machine and MacPorts has no `aarch64-elf-gcc` port, so the ARM tarball was the only path that lands on the documented binary names.

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
| 0 | Boot under QEMU | **in progress** |
| 1 | MMU, exceptions, timer | |
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
