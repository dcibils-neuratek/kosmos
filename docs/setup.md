# Setup

---

## Toolchain

Two pieces: the cross-compiler and QEMU. They come from different places.

### The cross-compiler

**The official ARM toolchain, downloaded from ARM's site.** It is the one that is genuinely bare metal, with no Linux assumptions, and it ships `gcc`, `binutils` and `gdb` in one tarball with nothing to build.

```
mkdir -p ~/toolchains && cd ~/toolchains
curl -fLO https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-aarch64-none-elf.tar.xz
tar xf arm-gnu-toolchain-14.2.rel1-darwin-x86_64-aarch64-none-elf.tar.xz
xattr -dr com.apple.quarantine arm-gnu-toolchain-14.2.rel1-darwin-x86_64-aarch64-none-elf
```

Then put it on the `PATH`, in `~/.zprofile`:

```
export PATH="$HOME/toolchains/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-aarch64-none-elf/bin:$PATH"
```

The `xattr` line is not optional. macOS quarantines anything downloaded through a browser or `curl`, and without clearing it Gatekeeper kills the binaries with a message that does not say why.

Swap `darwin-x86_64` for `darwin-arm64` on an Apple Silicon machine.

**Do not use `aarch64-unknown-linux-gnu` from Homebrew.** It targets Linux: it brings glibc, Linux start files and a dynamic loader, which is exactly what `-ffreestanding -nostdlib -nostartfiles` exists to avoid. It also produces binaries named `aarch64-unknown-linux-gnu-gcc`, not the `aarch64-none-elf-gcc` this project calls for.

**And `aarch64-elf-gcc` from Homebrew does not work either, for a different and more interesting reason.** It is a genuine bare-metal ELF target, so the objection above does not apply to it, and it compiles this tree cleanly - the kernel and all of upstream Lua, GCC 16.2 against `-Werror`, no warnings. It cannot *link* the user image, because it ships no C library at all.

That matters because the userland - not the kernel - depends on the toolchain's `libm` for thirteen functions: `pow`, `exp`, `log`, `log2`, `log10`, `sqrt`, `fmod`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan2`. Lua's `math` library wants most of them and `stb_truetype` wants `sqrt` and `fmod`. `runtime/libc/math.c` supplies only the six that are exact bit-manipulation - `fabs`, `trunc`, `floor`, `ceil`, `frexp`, `ldexp` - and the rest come from newlib, which ARM's tarball bundles and Homebrew's does not.

So `-lm` on the user link is real and load-bearing, and the split is worth stating plainly: **the kernel needs a compiler, the userland needs a compiler and a libm.** Writing the missing thirteen by hand is not a tidy-up - `pow` and `exp` and the trigonometric functions are numerically delicate, and a plausible wrong answer in a font rasteriser looks like a rendering bug rather than an arithmetic one.

This is also the shape of the question the Pi will ask, and it is not a HAL question. Whatever toolchain builds for a second target has to bring a `libm` or the userland has to grow its own.

### QEMU

From MacPorts or Homebrew, whichever is already on the machine:

```
sudo port install qemu       # MacPorts
brew install qemu            # Homebrew
```

### Verify

```
aarch64-none-elf-gcc --version
aarch64-none-elf-gdb --version
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
aarch64-none-elf-gdb build/kosmos.elf
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
lua/patches/      the freestanding changes, applied during the build
lua/kosmos/       the build configuration and the table serialiser, for the user image
```

**Never edit `upstream/` directly.** When a new version comes out, you update without fighting your own changes. Ten minutes now, a week at milestone 4.

What freestanding Lua needs from libc: `memcpy`, `memset`, `memmove`, `strlen`, `strcmp`, `strcpy`, `strchr`, `setjmp`/`longjmp`, a minimal `snprintf`, and several from `math`. You discover them one at a time, through link errors.

`setjmp`/`longjmp` is the one that demands the most care: Lua uses it for error handling, and getting it wrong produces a bug that only shows up when something raises an error, long afterwards.

---

## Hardware pending

- [ ] **3-pin JST-SH debug UART cable** for the Pi 5. Blocks M2 on that target.
- [ ] **3.3V USB-serial adapter** for the Pi 1, to GPIO 14/15. Cheaper and arrives sooner.

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
- **Datasheets:** BCM2712 (Pi 5), BCM2835 (Pi 1).
