# HAL and targets


---

## x86-64, and what a second architecture actually cost

**It boots.** `make x86` puts Kosmos into long mode on QEMU's q35 and prints
over COM1. That is the whole of the first step, and it is deliberately the
same first step this project took on ARM: a kernel that cannot print is a
kernel debugged by bisecting a hang.

**The shape of the problem is different in one way that decides the rest.**
ARM hands you 64-bit execution and asks which privilege level you would
like. An x86 starts in 16-bit real mode, a multiboot loader has already put
it in 32-bit protected mode, and long mode has to be *built*: page tables, a
GDT with the L bit set, `CR4.PAE`, `EFER.LME`, `CR0.PG`, and a far jump - in
that order, before one 64-bit instruction may run.

**Three things were wrong on the first attempt and all three are worth
recording**, because each looks like something else when it fails:

  * *The page tables live in `.bss`, and `.bss` is zeroed by the kernel.*
    Zeroing it after building them walks over `pml4` while the processor is
    translating through it, and what kills the machine is not the write but
    the first TLB miss after it. Zeroing happens first now.
  * *`rdmsr` answers in EDX:EAX*, two 32-bit halves, always. The `"=A"`
    constraint means that pair on 32-bit x86 and means "rax or rdx,
    whichever" on x86-64, so asking for it reads half the register and looks
    like it worked.
  * *A multiboot loader parses ELF32.* This image is ELF64 and QEMU says so
    plainly. Bit 16 of the flags - the a.out kludge - makes the loader use
    five explicit addresses from the header instead of parsing the format,
    and then the file may be a flat binary and its bitness stops being the
    loader's business.

**`-mno-red-zone` is not a tuning flag.** The red zone is 128 bytes below
`rsp` that a leaf function may use without adjusting the stack pointer, and
an interrupt handler pushes straight over it. Nothing notices until there
are interrupts, which is the worst time to find out.

### Endianness, which turned out to be free

Both targets are little-endian - AArch64 *can* be big-endian through
`SCTLR_EL1.EE` and Kosmos never sets it - but nothing here depends on that,
which was checked rather than assumed:

  * every `string.pack` in the on-disk format carries an explicit `<`, and a
    search for native-endian packs across the whole tree finds none. A disk
    written on one architecture reads on the other;
  * the network stack's `put16`/`get16` do explicit byte shifts, so they are
    big-endian by construction on any host;
  * the IPC structs never cross architectures - both ends of a message are
    processes on the same machine - so their layout is a local question.

### What is not done

Everything else. `arch/x86_64/` is a boot stub and a serial port; the six
headers `kernel/` includes from `arch/` - `context.h`, `cpu.h`, `mmu.h`,
`page.h`, `trap.h`, `mmio.h` - are 2,551 lines on ARM and none of them
exists here yet. In order: an IDT and exceptions, four-level paging from C,
the context switch, then ring 3.


---

## The distinction that matters

It is easy to blur, and blurring it leads to a useless abstraction.

**`arch/` is "which CPU are you".** Page table format, exception vector, context switch, memory barriers, privilege modes. **It is not abstracted across architectures, it is reimplemented.** Trying to abstract ARM and PowerPC page tables under a common interface produces an interface that is no good for either.

**`hal/` is "which peripheral do you have".** UART, interrupt controller, timer, framebuffer, storage. Common interface, one implementation per board. Here abstraction does work, because the operations are genuinely equivalent: pushing out a character is pushing out a character.

Two boards with the same CPU share `arch/` and differ in `hal/`. QEMU virt and the Pi 5 are both AArch64: they share all of `arch/aarch64/` and only the HAL changes.

---



## The display size, and changing it

`make FB=1920x1080` builds for that size. It is a compile-time constant
because there is no allocator: the pixels are a static array in a NOLOAD
section, so a bigger screen costs memory and nothing in the image file.

Nothing above the HAL needs telling. The console works out its rows and
columns from the geometry it is handed, `gfx.screen()` reports what the
kernel reports, and the window manager scales the pointer against the size
it was given - which is exactly why `hal_pointer_poll` hands out the
device's own range rather than screen coordinates.

**Changing it while the machine runs is a different question**, and worth
writing down before somebody assumes it follows for free. ramfb makes the
device side easy: the guest owns the configuration, so a new width, height
and stride written back through fw_cfg is a mode change, and QEMU resizes
its window. Three things do not follow:

- **The buffer is one fixed size.** A runtime mode would have to be capped
  at a maximum chosen at build time, and the array sized for that maximum
  rather than for the mode in use.
- **Everything holding geometry is holding a copy.** The console has its
  rows and columns, the window manager has a backbuffer that is a surface of
  a particular size, and every process that called `gfx.screen()` has a
  mapping of a particular length. A mode change is a message all of them
  have to be able to receive, which is a protocol that does not exist.
- **virtio-gpu does not work this way at all.** It is the target that earns
  the HAL a `hal_fb_flush`, and it is also the one where a mode set is a
  command to the device rather than a write to a config blob. Designing the
  runtime interface against ramfb alone would produce the shape of ramfb
  with a general name - the exact mistake `CLAUDE.md` warns about for the
  HAL as a whole.

So: build-time now, and the runtime version is worth doing after virtio-gpu
rather than before it, when there are two implementations to design against
instead of one.


## Two devices that are the same device

The keyboard and the tablet are both virtio-input. Both answer to device id
18. Nothing in the MMIO register window distinguishes them, so a driver that
scans for "the input device" finds whichever window comes first and calls it
a keyboard.

Telling them apart means asking each one what kinds of event it can produce:
write `VIRTIO_INPUT_CFG_EV_BITS` into the selector and `EV_ABS` into the
sub-selector, then read the length that comes back. Non-zero means the device
has absolute axes to describe, and a keyboard has none.

Two consequences worth knowing before touching `hal/qemu-virt/input.c`:

**Configuration space is bytes.** The first three fields of a virtio config
space are single bytes - selector, sub-selector, length - and reading them
as one 32-bit access works here and is allowed to fail on a device that
decodes the access width. `mmio_read8`/`mmio_write8` exist for this and for
nothing else.

**The probe costs a partial bring-up.** Configuration space is read after the
device has been told a driver is present, so a window holding the wrong kind
of device has been reset and acknowledged and then left alone. That is
harmless - claiming it later starts with another reset - and it is the price
of the two devices being indistinguishable from outside.

The file is instance-based for the same reason. It held one set of globals
when there was one device; the ring, the buffers and the position in the used
ring now belong to a `struct vinput`, because two devices of the same kind
cannot share them.


## The interface

Minimal on purpose. **Do not expand it speculatively.**

```c
void     hal_early_init(void);        // the minimum needed to have output
void     hal_putchar(char c);         // serial
void     hal_fb_init(struct fb *out); // address, w, h, pitch, format
void     hal_irq_init(void);
void     hal_timer_init(uint32_t hz);
uint64_t hal_ticks(void);

// M11. Frames, and nothing above them: no addresses, no protocols, no
// checksums. What an IP address means is not a driver's business, and a HAL
// that grew one would be a HAL with an opinion about the internet.
bool     hal_net_init(struct netdev *out);   // false when there is no card
bool     hal_net_send(const void *frame, unsigned bytes);
int      hal_net_recv(void *frame, unsigned max);   // 0 when nothing waiting
bool     hal_net_arrived(void);
bool     hal_net_present(void);
bool     hal_net_info(struct netdev *out);
```

`hal.h` is the authority and this list is a summary; where they disagree,
that file is right and this one is stale.

**And below the HAL, `virtio.c`.** Four devices on this board speak
virtio-mmio - input, block, sound, network - and the handshake is identical
for all four while the *rings* are not: block chains three descriptors and
waits, input hands the device empty buffers, sound has four queues with
different jobs. So the conversation is shared and the ring is not. That is
not a HAL interface - no other board implements it - and it lives in
`hal/qemu-virt/` for exactly that reason.

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

### Raspberry Pi 1 — the argument, and why the answer is no

**Decided against in September 2026: Kosmos is 64-bit only.** The section
below was an argument *for* the Pi 1 as the first real hardware, and it is
kept rather than deleted because it was a good argument and the cost of
refusing it is real.

**What was in favour, and all of it still true:**

- **GPIO 14/15 are UART directly.** Three wires to a 3.3V adapter and you
  have a console. None of the Pi 5's problem.
- More bare-metal material than any other board in existence, and every
  BCM2835 register documented in twenty places.
- A single core. No temptation to do SMP, no races.
- The BCM2835 mailbox framebuffer is the canonical example, and the Pi 5
  uses the same conceptual interface. What you learn transfers.
- Boot by copying a file to the SD card. Iteration in seconds.
- QEMU emulates `raspi1ap`, so the port could be developed without the board.

**And the strongest point, which is what is being given up:** porting to
ARMv6 would force `arch/` and `hal/` genuinely apart. With two AArch64
targets that boundary stays fuzzy, because everything works the same way and
nothing punishes an assumption. With ARMv6 in the mix the assumptions
surface on their own - where 64 bits was assumed, where exception levels
were, where a GIC was.

**What decided it:** the Pi 1 is ARM1176JZF-S, ARMv6, 32-bit, with no
64-bit mode at all. Different instruction set, short-descriptor page tables
instead of long-descriptor, CPU modes with banked registers instead of
exception levels, a different toolchain. That is a complete second
`arch/armv6/` - the layout has always said architectures are *reimplemented*
rather than abstracted - and it would be written against a 700 MHz core
where compositing is slow, for a system whose stated question is whether it
can be made fast.

**And the thing it was for arrives anyway, from a better direction.**
x86-64 is intended at some point, and it is a second instruction set, a
different interrupt controller, a different boot protocol and a different
memory model. Every assumption the ARMv6 port was supposed to surface gets
surfaced by it - with none of the Pi 1's cost, because it is 64-bit, it is
fast, and the machines are already on the desk.

So the `arch/` boundary does get tested. It gets tested by a target that
does not require giving up the one constraint worth keeping.

Recorded here rather than deleted, because a rejected option with its
reasoning intact is worth more than a tidy document: if the reason changes,
the argument is already written.

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

### x86-64 — the second architecture, not a second board

**Intended, unscheduled, and the first thing that will be a genuine
`arch/` rather than a `hal/`.** Worth writing down now because it changes
what "keep the layers apart" is protecting: with two AArch64 targets the
boundary is a promise, and with x86-64 it becomes a compile error.

**What is a second `arch/x86_64/`, and it is most of one:**

- Page tables. Four levels, different bits, one root in `CR3` rather than
  the `TTBR0`/`TTBR1` split - so the "everything below `USER_VA_BASE` is the
  kernel" arrangement is expressed by a canonical-half convention instead of
  by hardware.
- Exception and interrupt entry. An IDT, not a sixteen-entry vector table.
- Context switch, and a different calling convention under `setjmp`.
- The syscall path: `syscall`/`sysret` rather than `svc`.
- Barriers, which mostly become *nothing*: x86-64 is strongly ordered, so
  `mmio_read32`/`mmio_write32` lose their `dmb` and keep their shape. That is
  the accessor pair earning its existence a second time.
- **`-mno-red-zone`, and it is not optional.** The SysV ABI lets a leaf
  function use 128 bytes below the stack pointer without adjusting it, and an
  interrupt arriving on that stack writes straight through them. It is the
  x86-64 equivalent of `-mgeneral-regs-only`: a rule turned into a flag,
  where forgetting it produces corruption rather than an error. `-mno-sse
  -mno-mmx -mno-80387` go with it, for the same reason the kernel may not
  touch an FP register here.
- Lazy FP save translates directly: `CR0.TS` and `xsave` play the part
  `CPACR_EL1.FPEN` plays now.

**What is a `hal/x86_64-pc/` and nothing more:** the interrupt controller
(local APIC and IO-APIC, or MSI, where the GIC is now), the timer (the APIC
timer or HPET, where the generic timer is now), the console UART, and the
framebuffer - which under UEFI arrives as a linear buffer the firmware
chose, which is exactly the shape `hal_fb_init` already has.

**What does not change, and this is the whole point of the microkernel:**
everything above the kernel. Lua, the servers, the namespace, the window
manager, the applications. A Lua server neither knows nor cares which
instruction set it is running on, and the day that turns out to be false
will be worth knowing about.


## The rule for choosing a target

**If you cannot get a character out over serial in the first two hours, it is not a target.**

Everything else can be solved with time. That cannot.

---

## Recommended order

**QEMU virt → Pi 1 → Pi 5 → Jetson → G4.**

From easiest to debug to hardest, not the other way around.

Each jump breaks a different assumption. The Pi 1 breaks "64 bits and exception levels". The G4 breaks "little-endian" and "page tables are a tree I control". That last one teaches the most about which part of the design was essential and which part was an assumption about the hardware.

None of them before milestone 8, except the first port at milestone 2. Porting to a foreign architecture before the design is proven makes you generalize over assumptions you do not yet know are correct.
