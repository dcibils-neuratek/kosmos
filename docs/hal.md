# HAL and targets

---

## The distinction that matters

It is easy to blur, and blurring it leads to a useless abstraction.

**`arch/` is "which CPU are you".** Page table format, exception vector, context switch, memory barriers, privilege modes. **It is not abstracted across architectures, it is reimplemented.** Trying to abstract ARM and PowerPC page tables under a common interface produces an interface that is no good for either.

**`hal/` is "which peripheral do you have".** UART, interrupt controller, timer, framebuffer, storage. Common interface, one implementation per board. Here abstraction does work, because the operations are genuinely equivalent: pushing out a character is pushing out a character.

Two boards with the same CPU share `arch/` and differ in `hal/`. QEMU virt and the Pi 5 are both AArch64: they share all of `arch/aarch64/` and only the HAL changes.

---

## The interface

Minimal on purpose. **Do not expand it speculatively.**

```c
void     hal_early_init(void);        // the minimum needed to have output
void     hal_putchar(char c);         // serial
void     hal_fb_init(struct fb *out); // address, w, h, pitch, format
void     hal_irq_init(void);
void     hal_timer_init(uint32_t hz);
uint64_t hal_ticks(void);
```

An abstraction written against a single target is always the shape of that target with generic names. The right interface appears at milestone 2, once there is a second real target in front of you. Until then, the only rule that matters is this:

**Zero MMIO addresses outside `hal/`.** Not one. That single rule gets you 80% of the way there without designing anything up front.

---

## Targets

### QEMU `virt` aarch64 — the development target

Where everything gets written. PL011, GIC and virtio: standard, documented, and with the serial console going straight to your terminal.

**The GIC version is not the default.** `-M virt` gives a **GICv2** (`arm,cortex-a15-gic` in its device tree, distributor at `0x08000000` and a memory-mapped CPU interface at `0x08010000`). GICv3 has to be asked for:

```
-M virt,gic-version=3
```

which moves the redistributors to `0x080a0000` and puts the CPU interface in system registers instead of MMIO. Kosmos drives a GICv3, so that flag is on the QEMU line in the Makefile and in `tools/run_tests.py`, and booting the image on a plain `-M virt` finds no redistributor and never receives an interrupt.

The fastest way to settle a question like this is to ask QEMU rather than to remember:

```
qemu-system-aarch64 -M virt -machine dumpdtb=virt.dtb
dtc -I dtb -O dts virt.dtb
```

The device tree also gives the RAM size and the timer's interrupt numbers.

Detail: **which exception level you get depends on the options, so read `CurrentEL` instead of assuming.** Measured against QEMU 9.1.2:

| Machine | Entry level |
|---|---|
| `-M virt` | EL1 |
| `-M virt,virtualization=on` | EL2 |
| `-M virt,secure=on` | EL3 |

The plain `-M virt` this project develops against lands at EL1, so the drop is not exercised there. Real hardware is the case that needs it: the Pi firmware hands off at EL2. `boot/start.S` handles both and parks on anything else, and `virtualization=on` is the way to exercise the EL2 path under QEMU.

The secondary cores start too. If you do not park them by checking the core ID in the entry code, you will see the output four times and not understand why.

### Raspberry Pi 1 — the best hardware to learn on

Counterintuitive, but probably the first real hardware worth using.

**In favor:**

- **GPIO 14/15 are UART directly.** Three wires to a 3.3V USB-serial adapter and you have a console. None of the Pi 5's problem.
- It is the hardware with the most bare-metal material in existence. Cambridge, dwelch67, the whole OSDev corpus from 2012-2015. Every BCM2835 register documented in twenty places.
- A single core. No temptation to do SMP, no races.
- The BCM2835 mailbox framebuffer is the canonical example, and **the Pi 5 uses the same conceptual interface**. What you learn here transfers.
- Boot by copying a file to the SD card. Iteration in seconds.
- QEMU emulates `raspi1ap`, so you can develop the port without touching the board.

**Against:**

- **It is ARMv6, 32-bit.** ARM1176JZF-S. A different architecture, not a variant: different instruction set, short-descriptor page tables instead of long-descriptor, CPU modes with banked registers instead of exception levels, an `arm-none-eabi` toolchain.
- It requires a complete `arch/armv6/`. This is not "swapping the HAL".
- 512MB and 700MHz. Lua runs fine; compositing will be slow.

**The real value:** porting to ARMv6 forces you to genuinely separate `arch/` from `hal/`. With two AArch64 targets that boundary stays fuzzy because everything works the same. With ARMv6 in the mix, the assumptions surface on their own: you will find out where you assumed 64 bits, where you assumed exception levels, where you assumed a GIC.

### Raspberry Pi 5 — the main target

Notably more hostile to bare metal than the Pi 4.

**The wall:** nearly all I/O (USB, Ethernet, GPIO and the 40-pin header's UART) hangs off the **RP1**, a southbridge that talks over PCIe. To get a character out of the header UART you would have to bring up PCIe and write an RP1 driver before you even have a `printf`.

**The way out:** the dedicated debug UART connector, the **3-pin JST-SH** next to the HDMI. It goes straight to the BCM2712's UART without passing through RP1. That cable is the only lifeline for months.

Known traps:

- **The framebuffer pitch is almost never `width * 4`.** The firmware aligns it. Use the pitch the mailbox returns, or the image comes out skewed.
- **BGRA channel order**, not RGBA.
- **The framebuffer is mapped uncached.** Writing directly to it is 10-50x slower than to RAM. Always a cached backbuffer and a blit of the dirty rectangle at the end.
- **The Pi 5's MMIO addresses differ from the Pi 4's.** There is a lot of stale material floating around. Verify against the BCM2712 datasheet.
- **Which GIC the BCM2712 has is unverified.** Written down here as an open question rather than an assumption: QEMU's default turned out not to be the version these documents assumed, and the same guess about the Pi would cost a day. Settle it against the datasheet before writing a line of `hal/pi5/`.
- The framebuffer mailbox interface changed from the Pi 4.
- All four cores start at once and the firmware leaves them in a spin loop waiting for an address in a mailbox.

**Weak memory model.** AArch64 is far more aggressive than x86. Orderings x86 forgives fail here once every thousand boots. It is the most expensive class of bug in the project.

### NVIDIA Jetson — a cheap port

AArch64, same ISA. All of `kernel/`, `lua/`, `runtime/`, `servers/` and `apps/` port without touching anything. Only `hal/` changes. It is a week.

Check which one you have: the original Nano (2019) is a Tegra X1, Cortex-A57, GICv2. The Orin Nano is Cortex-A78AE, GICv3.

In favor: Tegra UARTs are 8250/NS16550, better documented than the PL011, and the header exposes a UART with no southbridge in the way.

Against: NVIDIA's boot chain is signed. BootROM, TegraBoot, USB flashing in recovery mode with SDK Manager. Compared to copying a file to an SD card, the iteration cycle is considerably worse, and that matters when you reboot two hundred times a day.

Milestone 3 or 4, once the HAL has been exercised.

### PowerPC G4 — the port that teaches the most

A Mac mini G4. Far out at the end, as a deliberate exercise.

**What makes it valuable: it is big-endian.** Every other target is little-endian. For Kosmos that lands in one precise place: the Lua table serializer for IPC. Today you can write a `uint32_t` and read it on the other side without thinking. With big-endian in the mix, you have to define an explicit wire format and honor it. That discipline never develops if all your targets agree.

**Open Firmware solves the console.** It stays alive after loading your kernel and exposes a client interface. `putchar` is an OF call. And its device tree has the framebuffer already configured, with address, width and pitch. It is the smoothest bring-up of any target, with no driver to write.

**The cost: the MMU is another world.** PowerPC uses segment registers plus an inverted hashed page table. It is not a radix tree like ARM or x86: you do not choose where the entry goes, you hash the virtual address and the hardware searches a bucket. Collisions and evictions are your responsibility. It is a genuinely different model with no analogy to what you already know.

QEMU emulates `mac99`, so the port can be developed without touching the hardware.

### x86-64 — not on the Mac Pro 2013

**The Mac Pro 2013 has no serial output. None.** No port, no header. To get a character out you would need a full USB stack before having `printf`. It is the RP1 problem without the back door.

Without debug output, the project does not exist.

Add Apple's EFI, all I/O over Thunderbolt, no SD card, and two FirePros. It is the worst possible x86 machine for this.

The irony is that **x86-64 in general is a better learning target than ARM**: the OSDev wiki is written for x86, there is far more material, and the TSO memory model saves you half the barrier bugs. The problem is not x86, it is that machine.

If you ever want x86, get a cheap mini-PC **with a serial header**, and treat it as a deliberate exercise.

---

## The rule for choosing a target

**If you cannot get a character out over serial in the first two hours, it is not a target.**

Everything else can be solved with time. That cannot.

---

## Recommended order

**QEMU virt → Pi 1 → Pi 5 → Jetson → G4.**

From easiest to debug to hardest, not the other way around.

Each jump breaks a different assumption. The Pi 1 breaks "64 bits and exception levels". The G4 breaks "little-endian" and "page tables are a tree I control". That last one teaches the most about which part of the design was essential and which part was an assumption about the hardware.

None of them before milestone 8, except the first port at milestone 2. Porting to a foreign architecture before the design is proven makes you generalize over assumptions you do not yet know are correct.
