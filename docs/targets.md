# Targets

**What a target is, how they share code, and what a new one costs.**

`hal.md` owns the HAL interface and the argument for where its boundary
sits. This is the layer above that: what the set of machines looks like,
why it is three axes rather than two, and what would actually have to be
written to run Kosmos on a specific piece of hardware. `smp.md` is the
model for this file — a plan for something not built, counted rather than
guessed.

Nothing here is built. Two of the three axes exist and the third is
implied by code that already runs.

---

## 1. Two axes is not enough, and the evidence is specific

The obvious model is a grid: architecture down one side, board along the
other. `arch/aarch64` + `hal/pi5`, `arch/x86_64` + `hal/alienware-x14`.
Boards under one architecture share drivers.

That is very nearly right, and the place it breaks is worth being exact
about, because it decides how the tree is laid out.

**Take three machines and ask what any two of them share.**

| | QEMU q35 | Alienware x14 | Raspberry Pi 5 |
|---|---|---|---|
| instruction set | x86-64 | x86-64 | AArch64 |
| how it says what it has | QEMU's `fw_cfg` | ACPI tables | a device tree |
| boot | multiboot 1 | UEFI | firmware to a fixed address |
| screen | QEMU `ramfb` | UEFI GOP | mailbox |
| interrupts | 8259 PIC | IOAPIC + MSI | GIC |
| timer | 8254 PIT | HPET / TSC-deadline | generic timer |
| keyboard | virtio-input | i8042 **or USB** | USB |
| storage | virtio-blk | USB mass storage | USB or SD |
| network | virtio-net | USB CDC-ECM | onboard, or USB |

**The x14 and QEMU q35 share the instruction set and almost no device.**
Every row below the first one differs. `arch/x86_64` is common and correct
for both; not one line of `hal/pc/` is.

**The x14 and the Pi 5 share no instruction set and will share the entire
USB stack** - xHCI, the USB core, the hub class, mass storage, HID - which
on both machines is where the keyboard, the disk and the network arrive.
That is thousands of lines, and it is the largest single body of driver
code either of them needs.

So drivers do not group by architecture and they do not group by board.
They group by **what the device is**, and which of them a machine wants is
a question its firmware answers at run time.

## 2. On a PC there is no board. There is a bus.

This is the asymmetry the grid hides, and it is the whole reason for the
third axis.

**Embedded machines do not enumerate.** The Pi 5's UART is at an address
you are told, its interrupt controller is a design you are told about, its
framebuffer comes from a mailbox whose protocol you are told. Nothing on
the machine will tell you any of it. So a board file is exactly the right
unit, and `hal/pi4/` sharing most of `hal/pi5/` is exactly right too.

**A PC enumerates itself.** ACPI says what the platform is; PCI says what
is plugged into it; USB says what is plugged into that. There is no
`alienware-x14.c` worth writing, and if it were written it would be wrong
for the next laptop and wrong for this one after a firmware update - the
brief for the x14 makes the point itself: *"the firmware may reassign BARs
between boots. Enumerate PCI on every boot."*

So a PC target is not a description of a machine. It is **a way of asking
the machine what it is**, plus a pool of drivers to match against the
answers.

## 3. The three axes

### `arch/` - the instruction set

Page tables, the exception vector, the context switch, barriers, the
syscall entry. Not abstracted across architectures; reimplemented. Two
exist and both are finished: `aarch64` and `x86_64`, with all thirteen
`kernel/*.c` compiling for each with no `#ifdef` in any of them.

A new architecture is a large, well-understood piece of work. A new
*machine* almost never needs one.

### `hal/<platform>/` - how this machine says what it has

The narrow, genuinely machine-specific part: how to boot, how to find the
first console, how to learn the memory map, how to discover devices, how
to turn the machine off.

**This is much smaller than it looks**, and it shrinks further the more a
machine can be asked. `hal/qemu-virt/` is large because a device tree is
parsed by hand and the virtio windows are at fixed offsets. A UEFI+ACPI
platform is mostly table walking, and after that everything is a driver.

The name is the discovery mechanism rather than the vendor: `pc-uefi`
serves every UEFI x86-64 machine, and the x14 is one of them.

### The driver pool - matched at run time

`hal/virtio/`, and in future `hal/usb/`, `hal/hda/`, `hal/nvme/`,
`hal/i2c/`. Code that knows a device and not a machine.

**This already exists and already works across architectures.**
`hal/virtio/blk.c` is one file driving one card over two entirely
different transports - fixed offsets in a device-tree window on ARM, a walk
of the PCI capability list on x86 - and the split between `hal/virtio/`
(the device) and each board's `virtio.c` (the transport) is the pattern the
rest of the pool should copy.

`hal_bus_scan` is the discovery half, and it is already written: it walks
the bus, reports what it found, and says whether a driver claimed each one.
`machine` prints that list. What is missing is drivers to match against it,
not the mechanism for matching.

---

## 4. What is true today, stated plainly

**`hal/pc/` is `hal/qemu-q35/` wearing a general name.** `fwcfg_port.c` is
QEMU's fw_cfg; the framebuffer is QEMU's `ramfb`; `power.c` says so in its
own comment - *"this is QEMU's, not a PC's"* - about an ACPI address it
hardcodes. The genuinely generic parts (PCI config access, the 8259, the
8254, the RTC, the 16550) would survive a rename; the rest would not.

Renaming it is not urgent and is cheap whenever a second x86 platform
arrives. What is worth doing now is not pretending.

**Every driver is in the kernel.** `architecture.md` §7 lists this as the
largest gap between the design in `design.md` and what runs. It matters
here because the tempting move when writing a large new driver - "make this
one a process, the way the design says" - braids two hard things together.
A new driver goes in `hal/` with the others; moving drivers out is its own
piece of work with its own tests.

---

## 5. The targets

| Target | Arch | Platform | Status |
|---|---|---|---|
| QEMU `virt` | aarch64 | device tree | **the development target.** Everything runs here first. |
| QEMU `q35` | x86-64 | fw_cfg, multiboot | **runs the desktop.** 125 in-guest checks, 65 display checks, disk, network, sound. |
| Alienware x14 R1 | x86-64 | UEFI + ACPI | **not started.** §6. |
| Raspberry Pi 5 | aarch64 | firmware + mailbox | **not started**, and blocked on a serial cable that is not here. §7. |

---

## 6. Alienware x14 R1 - the first real machine

An Alder Lake-P laptop: i7-12700H, 6 P-cores and 8 E-cores, 16 GB soldered
LPDDR5, a 1920x1080 panel on the Iris Xe, Windows on an NVMe behind Intel
VMD. The plan is to boot from a USB stick and never touch the internal
disk.

### What is already done, and must not be rebuilt

`arch/x86_64` is finished and tested: long mode, four-level paging with
W^X and NX, the GDT and IDT, the exception vectors, `syscall`/`sysret`,
the context switch with eager `fxsave`, the TSC, and an interrupt stack
table for #PF and #DF. 125 checks run against it.

And the whole of Kosmos above the kernel - eighty-four programs, the
servers, Lua, the window manager, the browser, the TCP/IP stack - runs
unchanged on both architectures already. That is the part a port normally
spends its life on and it is done.

### What is missing

Everything between those two, which is to say the platform:

| | needed | rough size |
|---|---|---|
| UEFI loader (PE/COFF, GOP, memory map, `ExitBootServices`) | new | ~1000 lines |
| ACPI: RSDP, XSDT, MADT, HPET, FADT. **No AML** | new | ~400 |
| Local APIC / IOAPIC / MSI and MSI-X | new | ~600 |
| HPET, and TSC-deadline for the local timer | new | ~300 |
| PCI over ECAM from MCFG | rework of `pci.c` | ~200 |
| Framebuffer from the loader's boot-info | `hal_fb_init` already has the shape | ~100 |
| i8042 keyboard | new | ~200 |
| **subtotal - boots and shows the desktop** | | **~2800** |
| xHCI + USB core + hub + mass storage + HID | new | **~5000-7000** |
| USB CDC-ECM networking | new | ~500 |
| Intel HDA audio | new | ~1500 |
| LPSS I2C + HID-over-I2C touchpad | new | ~1000 |

**Those numbers are rough and the kind that are wrong by a factor of two.**
What they are for is the shape, and the shape is the point: **the first
fifth of the work gets a booting desktop on the panel, and it is all
developable under QEMU with OVMF before the laptop is touched at all.**
Then xHCI is a step change, and it is the largest single thing this project
would have attempted.

### The blocking unknown, and it costs half an hour to answer

**Is the internal keyboard i8042 or USB?**

Most laptop keyboards reach the system as PS/2 through the embedded
controller, in which case the ~2800-line milestone produces a machine that
can be typed on. If this one is USB - and there is a Holtek composite
device in the USB list that might be it - then there is **no input at all**
until xHCI works, and the first satisfying milestone moves from ~2800 lines
to ~9000.

That is the difference between a few weeks and a few months, and it is
answered by booting any Linux stick on the machine and reading
`dmesg | grep i8042` and `/proc/bus/input/devices`.

**Nothing else about this target should be planned before that answer
exists.**

### Order

1. **UEFI loader, GOP, ACPI, APIC, PCI over ECAM, i8042** - all under QEMU
   with OVMF, all with the existing display harness as the measure.
2. **First boot on the machine.** Nothing new: the same image, from a
   stick. What this phase buys is a panic screen worth having, because it
   is the only debugger there is.
3. **xHCI and the USB stack**, and after it everything else becomes
   ordinary.
4. Storage on the stick, then networking through a USB ethernet dongle,
   then audio, then the touchpad.

### Where the machine's own brief and this document disagree

The target brief for the x14 is well researched - the PCI inventory, the
reasoning about VMD and the NVMe, and the observation that an RTL8153
dongle exposes a standard CDC-ECM configuration are all worth keeping. Four
things in it would be wrong for this system:

- **It says Kosmos is AArch64-only.** It has not been since 0.9.0. Rebuilding
  `arch/x86_64` would be throwing away a working, tested architecture port.
- **`-march=x86-64-v3` for the kernel.** The kernel is built
  `-mno-mmx -mno-sse -mno-sse2` deliberately: that is this architecture's
  spelling of `-mgeneral-regs-only`, and it is what turns "the kernel does
  not touch an FP register" from a promise into a compile error. v3 is
  right for userland and wrong here.
- **`xsave` and AVX.** The context switch does `fxsave`, which covers SSE,
  which is what Lua's doubles use. AVX would mean a larger context for a
  benefit nothing has asked for.
- **xHCI as the first userspace driver.** Every driver here is in the
  kernel today. Writing the largest driver in the project's history *and*
  the userspace-driver framework at the same time is two hard things
  braided; `architecture.md` §7 already names the second as its own work.

And one reordering. The brief puts the xHCI Debug Capability before the USB
stack, to get a serial-like console on a machine with no serial port. The
instinct is right and the ordering is not: **QEMU cannot emulate DbC**, so
it would be developed blind on hardware, which is the opposite of the same
document's own rule that real hardware is where you confirm rather than
where you debug. Its own fallback is better and much cheaper - **a log in a
reserved RAM region that the next boot's loader prints to the screen**,
because RAM survives a warm reset. That plus a good panic screen is a real
debug loop for the price of about fifty lines.

---

## 7. Raspberry Pi 5

Named in `CLAUDE.md` as the hardware target and chosen because it is hard:
a fast desktop on it is a result rather than an emulator number.

It shares `arch/aarch64` entirely and `hal/qemu-virt` not at all - a
different UART, a different interrupt controller, a mailbox instead of
`ramfb`. `hal.md` records the argument for why the Pi 1 was refused and the
Pi 5 was not.

**Two things it has that the x14 does not.** It has a documented UART on
GPIO pins, which means a serial console from the first instruction - the
single most valuable thing in a bring-up, and the reason it is an easier
first machine than a laptop despite being a less familiar one. And it is
documented, publicly and completely.

**And it will want the same USB stack**, which is the strongest argument for
`hal/usb/` being a shared pool rather than anything's board code.

Blocked on a 3-pin JST-SH debug cable that has not arrived.

---

## 8. What a new target costs

For a machine whose architecture already exists:

1. **A way in.** How the firmware hands over, and what it hands over.
2. **A console, before anything else.** A UART on pins, or a framebuffer
   from the firmware, or a reserved RAM log. Without one the machine cannot
   tell you why it stopped, and every later step is guesswork.
3. **Discovery.** Device tree, ACPI, or hardcoded because the machine
   cannot be asked.
4. **The four the HAL requires**: memory range, interrupt controller,
   timer, console. `hal/hal.h` is the authority on the exact list.
5. **Drivers for what discovery found** - and this is where a second
   machine on the same bus costs almost nothing, which is the entire point
   of the pool.

Steps 1 to 4 are the platform and are usually small. Step 5 is where the
time goes, and it is shared.

## 9. What not to do

- **Do not add a directory per machine on a bus that enumerates.** A PC
  target describes a discovery mechanism, not a laptop.
- **Do not expand the HAL for a target that is not in front of you.**
  `CLAUDE.md` is explicit and `hal.md` gives the reason: an interface
  written against one target is the shape of that target with generic
  names. Every entry in `hal/hal.h` arrived with the thing that needed it.
- **Do not port to hardware you cannot see.** A console comes first, and on
  a machine with no serial port that means the framebuffer or a RAM log
  before anything else is attempted.
- **Do not start a target whose first milestone depends on an unknown.**
  The x14's keyboard is the live example: half an hour with a Linux stick
  decides whether the first milestone is weeks or months away.
