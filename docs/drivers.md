# Drivers: where they live, and where they come from

**Decided 12 September 2026**, after a night spent failing to boot a
ThinkPad and a morning spent asking whether Kosmos should do what Haiku
does.

The question was: *can we vendor FreeBSD drivers and put a compatibility
layer under them, the way Haiku does?* The answer turned out to be four
different answers, and a fifth thing that has to exist before any of them.

---

## 1. The shape, which already exists

Diego put it in three lines and they are the right three:

```
a device   in C
a server   in C
a program  in Lua
```

`/dev/audio` is already exactly this: the HDA driver, the audio server, and
`beep`. So is the disk: a block driver, `diskfs`, and Tracker. The shape is
not in question.

**What is in question is which side of the kernel boundary the device sits
on**, and today the answer is the wrong one.

---

## 2. Haiku's answer, and why Kosmos cannot copy it

Haiku runs FreeBSD's drivers **inside its kernel**, under a compatibility
layer (`src/libs/compat/freebsd_network/`) that emulates enough of FreeBSD's
kernel API - newbus, `bus_dma`, `bus_space`, mbufs, callouts, taskqueues,
`malloc(9)`, ifnet, miibus - that the driver source compiles unmodified.

It works, and it is the right answer *for Haiku*, which has a monolithic
kernel and therefore nowhere else to put a driver.

**Kosmos cannot follow it, and the obstacles are load-bearing rather than
stylistic:**

- **No allocator in the kernel.** FreeBSD drivers call `malloc(9)`,
  `contigmalloc` and `bus_dmamem_alloc` constantly. Hosting them means
  building the one thing `CLAUDE.md` forbids, in order to serve somebody
  else's code.
- **The 10k smoke alarm.** The compatibility layer alone is thousands of
  lines before a single driver arrives.
- **Sleeping locks and kernel threads.** The FreeBSD KPI assumes `msleep`,
  `callout`, taskqueues. Kosmos's rule is that every lock masks interrupts
  and every critical section is short.

---

## 3. What Kosmos does instead, and why it is better

**A microkernel can do the thing Haiku cannot: run the driver at EL0, as a
server.**

A vendored driver in its own address space is a driver whose bug is a crash
and a restart rather than a compromise. And it dissolves three of the four
objections above by itself - a *process* may have an allocator, threads and
sleeping locks, because it is a process.

This is not a novel idea; it is what a microkernel is for, and it is the
arrangement QNX and Genode use. It is simply one Kosmos has never taken,
because until now every driver has been small enough to live in `hal/`.

**There is a tell that the current arrangement is unfinished.**
`SYS_DISK_READ` and `SYS_DISK_WRITE` are *syscalls*: the disk driver lives
in the kernel and userland asks the kernel to read sectors for it. That is
the monolithic shape wearing a microkernel's clothes, and it is there
because there was no other way to express it.

---

## 4. The three primitives — two of them now exist

This is the actual work, and everything below depends on it.

1. **Map a device's registers.** ✅ `SYS_DEV_MAP` (0.10.39). A physical
   window into the driver's own address space, with the memory type that
   makes a register a register: Device-nGnRnE on ARM, PCD|PWT on x86, both
   execute-never. The type is the feature rather than a detail - mapped as
   ordinary memory a doorbell write may sit in a cache line waiting for
   company while the device waits for ever.

   Gated on `owns_devices`, and **RAM is refused**: physical memory mapped
   uncached into a process is an alias for somebody else's pages that
   bypasses their cache, which is a way to corrupt them and a way to watch
   them. Nothing legitimate wants it, because a driver's buffers come from
   the next primitive instead. `dev_range_ok` is a separate function so the
   suite can ask it directly, and its test is nine assertions of which seven
   are refusals.

2. **Get memory a device can reach.** ✅ `SYS_MEM_CREATE` with
   `MEM_CONTIGUOUS`, and `SYS_MEM_PHYS` to ask where it landed (0.10.35).
   Physically contiguous pages, mapped *normally* - a driver's rings are
   read and written constantly and want a cache - with the physical address
   reported so the hardware can be told where they are. Refused for a
   scattered region, since reporting a base for one would point hardware at
   somebody else's heap.

3. **Receive an interrupt.** ✖ Still missing. There is no route from a
   hardware IRQ to a thread waiting in a driver.

**The two that exist do not overlap, and that is deliberate**: registers are
device memory and uncached, buffers are ordinary memory and cached, and a
driver that confuses them is refused rather than left to find out. Between
them a driver can already *drive* a device; what it cannot do is be told
that the device has something to say, so it would have to poll.

Estimated at 500-800 lines in the kernel, and the largest architectural
addition since capabilities - because it turns "a driver is kernel code"
into "a driver is a server you were handed a capability to".

**The first two came in well under that**, and it is worth saying why rather
than claiming the estimate was pessimistic: the machinery was already there.
`as_map` has always taken attributes and the kernel has always mapped its own
MMIO with them, so `SYS_DEV_MAP` is one attribute constant per architecture
and a gated syscall shaped exactly like `SYS_MEM_MAP`. The interrupt is the
one with real design in it, because it is the only one that has to happen
*while the machine is in an interrupt handler with everything masked*.

**Authority, not capability, for the first two.** `owns_devices` is the right
to mint a device mapping rather than a mapping itself - the same shape
`owns_screen` and `owns_net` already have. That is weaker than the rule this
system runs on: a process holding it can map any device on the machine, so a
driver server given it could touch the disk controller as well as its own
sound card. What makes that acceptable today is that nothing but init holds
it. What makes it *right* eventually is a devices server that holds the
authority and hands out windows, so a driver is given the window and not the
right to make one - and that server is a process, which is exactly where a
policy about which driver gets which device belongs.

**The cost, named rather than discovered later:** this creates two driver
worlds for a while. `i8042`, `hda`, `nvme` and `pci` are in `hal/` today and
stay there until there is a reason to move them. Two models in one tree is a
real tax; the mitigation is that new drivers go to userland and old ones
migrate only when they are being touched anyway.

---

## 5. The four subsystems, and where each one's code comes from

**They do not share a source, and assuming they did was the error worth
correcting.** "It all works under Haiku, and Haiku got it from FreeBSD" is
true of some of it and not the rest.

| | where the code comes from | why |
|---|---|---|
| **NVMe** | **ours, already written** | `hal/pc/nvme.c`, 644 lines |
| **Audio** | **read Haiku's own HDA driver** | native Haiku code, not FreeBSD's |
| **USB** | **write it** | public Intel xHCI spec |
| **WiFi** | **OpenBSD's `iwx`** | names this exact card |

### NVMe - written, never tried on the machine

`blk_bind.c` tries the loader's memdisk, then NVMe, then virtio-blk. On the
ThinkPad the memdisk always wins, so **the NVMe path has never run on that
laptop**. It may simply work. A no-disk image answers it in one boot.

*Safe to try*: a disk that will not mount is only formatted when its first
block is **all zeros**. A Windows drive has a GPT protective MBR at LBA 0,
so Kosmos finds it, fails to mount, and says so.

### Audio - one graph walk away

`hal/pc/hda.c` finds the ThinkPad's codec - a **Realtek ALC257**,
`VEN_10EC&DEV_0257` - and reports *"an HDA codec with no output path"*.

**Haiku's HDA driver is Haiku's own code**, not FreeBSD's:
`src/add-ons/kernel/drivers/audio/hda/`. Native, small, MIT, and solving
precisely our problem - walking a Realtek widget graph to a speaker. That is
a file to read beside ours, not a stack to import.

One flaw is already visible by reading: `connections()` masks entries with
`0x7f`/`0x7fff` and so **ignores HDA range entries**, where a set high bit
means "everything from the previous node up to this one". Realtek codecs use
them, so `{0x02, 0x85}` reads as `{0x02, 0x05}` and loses what is between.

**The next step is not to guess the topology but to print it**: when the
walk fails, dump every widget, its type, its connection list and each pin's
configuration default. Same principle as the image canary - make the machine
say what it found.

### USB - write it, and it pays for itself twice

Public Intel specification, no firmware, no crypto.

- **Mass storage deletes the loader-disk problem.** The 64 MB GRUB module
  exists only because Kosmos cannot read the stick it booted from. See
  `thinkpad.md` §6a.
- **A USB Ethernet adapter puts the network stack on real hardware.** ARP,
  IP, TCP, DNS, `host` and the browser are written and have only ever run
  against virtio-net under emulation. The T14 has no RJ45.
- **And a real mouse**, which is its own reason.

Build order, each step ending in something visible: controller up, then
enumeration, then bulk transfers, then mass storage, then Ethernet.

### WiFi - OpenBSD, not FreeBSD

The card is an **Intel AX201**, `VEN_8086&DEV_A0F0`. It is **CNVi**: the MAC
is inside the chipset and only the radio is on the M.2 module.

**Haiku does not appear to drive it.** Its own ticket #16970 is "Intel WIFI
6 AX200 / ax201 not functioning", and the discussion there notes FreeBSD
reaches this card only through a *Linux* compatibility layer, which does not
fit Haiku's FreeBSD one. Two shims deep and it stops.

**OpenBSD's `iwx(4)` names this card explicitly** - "AX201/AX211 Integrated
Connectivity (CNVi) adapters with companion RF M.2 modules". Written
natively for OpenBSD rather than being Linux's driver under two layers, ISC
licensed, and it does WPA1 and WPA2. Its limits are stated honestly: no
802.11ac or ax rates. A working network, not a fast one.

**The firmware cannot be vendored.** Intel grants no redistribution rights,
which is why OpenBSD ships the driver and not the blob. Same rule this
project already applies to `doom1.wad` and `pak0.pak`: it lives on the disk,
never in the repository.

---

## 6. The order, and one thing in front of it

**Before any of it: bring the display up early.**

A machine with no serial port shows nothing until boot stage six, when the
framebuffer attaches and the log replays - so a panic in memory setup and a
hang inside the loader are the same blank screen. That is what turned one
evening into four hours. The loader hands over the framebuffer address
before `pmm_init` runs, so there is no reason to wait.

It costs an afternoon and it is the difference between debugging the three
subsystems below and guessing at them.

Then:

1. **NVMe** - a boot, not a project.
2. **Audio** - dump the topology, then write the walk.
3. **USB** - the driver primitives, then xHCI as the first userland driver,
   then mass storage and Ethernet.
4. **WiFi** - `iwx` into a server, once the shape above is proven.
