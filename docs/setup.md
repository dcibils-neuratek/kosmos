# Setup

---

## Toolchain

Two pieces: the cross-compiler and QEMU. They come from different places.

### The cross-compilers

**Homebrew's bare-metal GCC, for both architectures** - GCC 16.2, which
ships `gcc` and `binutils` and nothing else:

```
brew install aarch64-elf-gcc x86_64-elf-gcc
brew install aarch64-elf-gdb      # for make debug
```

**And clang, for C++** (`docs/webkit.md` W1c): Homebrew's `llvm`, which
builds libc++, libc++abi and libunwind and every C++ file. GCC links.

```
brew install llvm
```

**Until 11 November 2026 AArch64 was built by Arm's own toolchain, 14.2**
(`aarch64-none-elf-gcc`, a tarball from developer.arm.com). Two things kept
it, and neither holds any more:

- **It brought newlib's `libm`**, which the userland linked for thirteen
  functions - `pow`, `exp`, `sin` and the rest - and Homebrew's GCC ships
  no C library at all. The userland now carries its own,
  `runtime/upstream/musl-math/`, so neither compiler's library is linked.
- **It was what there was.** But Arm's GCC is configured `--disable-tls`,
  so C's `_Thread_local` became `__emutls` calls - one copy for the whole
  process rather than one a thread - while clang's C++ used `TPIDR_EL0`
  directly. Two answers in one image; W1b's check caught it. Diego chose
  Homebrew's 16.2 that day ("yes to all"), which is also what x86-64 had
  been built with all along: one compiler, two targets.

**Do not use `aarch64-unknown-linux-gnu` from Homebrew.** It targets Linux:
it brings glibc, Linux start files and a dynamic loader, which is exactly
what `-ffreestanding -nostdlib -nostartfiles` exists to avoid.

**A change of compiler does not rebuild anything by itself**: make compares
the sources' times, not the compiler's. After switching, remove the
objects - `make clean`, or the `.o`, `.a` and `.elf` files under the
`build/` directories of that architecture - or old and new objects are
linked together.

### QEMU

From MacPorts or Homebrew, whichever is already on the machine:

```
sudo port install qemu       # MacPorts
brew install qemu            # Homebrew
```

### mtools

`make test` makes FAT volumes with it and reads them back through Kosmos's
own reader, and reads a stick image's boot partition with it
(`tools/test_fat.py`, `tools/test_stickcheck.py`). Somebody else's reading of
FAT is the point, so nothing in the tree stands in for it:

```
sudo port install mtools     # MacPorts
brew install mtools          # Homebrew
```

### iasl, from ACPICA

Intel's ACPI compiler and disassembler, and the reference for how every
ACPI table is laid out. It turns a machine's DSDT - brought off a stick by
`dsdt` and `make stick-log` - into something a person can read, and it is
where this tree's table offsets come from rather than from memory:
`iasl -T FACP`, compiled and disassembled with `iasl -d`, prints every field
with its offset. Installed on 18 September for the ThinkPad's brightness,
with Diego's yes. Nothing in `make test` needs it.

```
brew install acpica          # Homebrew; the binary is `iasl`
```

### Verify

```
aarch64-elf-gcc --version
x86_64-elf-gcc --version
aarch64-elf-gdb --version
qemu-system-aarch64 --version
```

`gdb` earns its keep from milestone 1 onward.

---

## Build

```
make qemu        # build and run
make test        # run the suite, exit code 0 or 1
make debug       # QEMU with a gdbserver on :1234, waiting for a connection
make clean
```

The QEMU line:

```
qemu-system-aarch64 \
  -M virt -cpu cortex-a72 -m 512M \
  -nographic \
  -kernel build/kosmos.elf
```

`-nographic` sends the serial to the terminal. To exit: `Ctrl-A` then `x`.

For `make test`, add semihosting so the guest can set the exit code:

```
  -semihosting-config enable=on,target=native
```

When the framebuffer arrives at M6, swap `-nographic` for `-serial stdio -device virtio-gpu-pci`, so you get a window **and** serial at the same time. Never turn the serial off.

---

## Debugging without a debugger

For the first few milestones the UART is the only tool. Worth taking seriously rather than suffering through it.

**A `putchar` that works from the third instruction.** It is the first thing to build and it is what makes everything else possible. Before the MMU, before anything.

**When something hangs:** instrument until you find the last line that executed. Bisecting with `puts` is primitive and it is the fastest thing there is.

**The exception handler that prints ESR, ELR and FAR** (M1) is the best investment in the project. It turns a silent hang into a message telling you which instruction faulted and on what address. Doing it well is worth more than any other tool.

**GDB against QEMU** works well and earns its keep on context switch bugs, which are nearly impossible to find with prints:

```
make debug          # in one terminal
aarch64-elf-gdb build/kosmos.elf
(gdb) target remote :1234
```

On real hardware there is no GDB. Only UART.

---

## Where output goes

From M6 on, the same `putchar` goes out over serial **and** to the framebuffer, both at once.

The reason: when something breaks, the framebuffer breaks too. A page fault in the compositor leaves the screen frozen on the last frame. A stack overflow leaves garbage. Serial takes a different path and keeps talking after everything else has died.

On screen you see the system. In the terminal you have the full log — scrollable, copyable, and it survives the hang.

---

## Upstream Lua

```
lua/upstream/     Lua 5.4.x exactly as shipped, untouched
lua/kosmos/       the build configuration and the table serialiser, for the user image
```

There is no `lua/patches/`. Every hook Kosmos overrides is guarded
upstream by `#if !defined`, so `-include lua/kosmos/kosmos_lua.h` in front
of every translation unit is enough, and nothing has had to be patched
(`Makefile`, `LUA_FLAGS`).

**Never edit `upstream/` directly.** When a new version comes out, you update without fighting your own changes. Ten minutes now, a week at milestone 4.

What freestanding Lua needs from libc: `memcpy`, `memset`, `memmove`, `strlen`, `strcmp`, `strcpy`, `strchr`, `setjmp`/`longjmp`, a minimal `snprintf`, and several from `math`. You discover them one at a time, through link errors.

`setjmp`/`longjmp` is the one that demands the most care: Lua uses it for error handling, and getting it wrong produces a bug that only shows up when something raises an error, long afterwards.

---

## Hardware pending

- [ ] **3-pin JST-SH debug UART cable** for the Pi 5. Blocks M2 on that target.

Until then, all work happens under QEMU.

---

## Reference material

- **The 9P spec.** Short, readable in an afternoon. It is the conceptual origin of the Kosmos protocol.
- **`rust-raspberrypi-OS-tutorials` by Andre Richter.** It is in Rust, but the Pi bring-up sequence is the best reference available.
- **The OSDev wiki.** Written for x86, but the concepts and the AArch64 section are useful.
- **The Haiku source.** For BeOS internals and app_server design.
- **seL4.** For thinking about IPC and capability design.
- **eLua.** Prior art for Lua on bare metal.
- **ARM Architecture Reference Manual (ARMv8-A).** Enormous, but it is the source of truth for exceptions, page tables and barriers.
- **Datasheets:** BCM2712 (Pi 5).
