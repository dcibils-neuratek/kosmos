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

`/Devices/audio` is already exactly this: the HDA driver, the audio server, and
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

## 4. The three primitives, all of which now exist

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

3. **Receive an interrupt.** ✅ `SYS_IRQ_CLAIM`, `SYS_IRQ_WAIT` and
   `SYS_IRQ_ACK` (0.10.40), with the claims in `kernel/irq.c`: a fixed pool
   of sixteen, one lock, no allocation. A claim is a *capability*, so a
   driver names its line by an index into its own table.

   **A wake, not a message.** An interrupt carries no data - the data is in
   registers the driver already has mapped - so a message would be an empty
   envelope built and copied on the one path that runs with interrupts off.
   The handler bumps a count and wakes a thread, and that is all.

   **The line is masked on delivery**, because a level-triggered source is
   still asserted when the handler returns and would arrive again before the
   driver - a process - could run: a livelock in the handler. The driver
   unmasks with `SYS_IRQ_ACK` once it has quietened the device. On an MSI
   there is no line, so both directions are no-ops, and that is the normal
   case on the ThinkPad rather than an edge.

   **And MSI-X** (0.10.54), for a device that has no MSI capability - QEMU's
   xHCI controller, whose list is MSI-X and PCI Express and nothing else.
   The same message, written into entry 0 of a table in one of the
   device's BARs (`hal/pc/pci.c`), with the capability walk shared between
   the two.

   Refused: a number the board spends on itself (`hal_irq_available` - the
   tick above all), and a line somebody already holds.

   **And a wait can have a deadline** (0.10.52): `SYS_IRQ_WAIT (cap, ticks)`
   returns `SYS_NO_INTERRUPT` when it runs out, and zero still waits for
   ever. A driver on real hardware cannot know its device will ever
   interrupt, and a wait that never ends is a driver that never says so. On
   a deadline wake the waiter takes itself off the line, because delivery
   and release clear it before they wake and a deadline does not. The
   blocking half was first exercised by a device - the power button
   (0.10.43) - when nothing else could end that wait; the deadline lets the
   suite test it as well (`testing.md` §18.35).

   **And a wait on several lines at once** (0.10.61): `SYS_IRQ_WAIT_ANY
   (&caps, count, ticks, endpoint)` answers which of the capabilities had
   an interrupt. The xHCI driver drives every controller from one thread, and
   waiting on each in turn left a mouse on the second behind the first's
   wait. It keeps every promise `SYS_IRQ_WAIT` makes, by the same lock, and
   takes its thread off every line before it returns, so a line never names
   a waiter that has gone (`testing.md` §18.43).

   **And a caller, on the same wait** (USB step 5c): given an endpoint, it
   answers `IRQ_WAIT_CALLER` when somebody is queued there - a line with an
   interrupt first - so a driver with clients serves them without looking
   at its endpoint between interrupts. The thread is the endpoint's watcher
   and the lines' waiter at once, recorded under the endpoint's lock and then
   the lines', and a caller wakes a watcher only under the lines' lock, so
   the wake cannot fall between the two releases (`usb.md` §7, `testing.md`
   §18.58).

**The two mappings do not overlap, and that is deliberate**: registers are
device memory and uncached, buffers are ordinary memory and cached, and a
driver that confuses them is refused rather than left to find out. With the
interrupt beside them a driver in a process has everything a driver in the
kernel had: it can reach the device, give it memory, and be told when it has
something to say. **And the first one exists.** `user/drivers/power/powerbutton.c` (0.10.43) drives QEMU
`virt`'s PL061, where the power key is wired, and uses all of it: the board
says where the controller is (`SYS_DEV_FIND` - init is userland too, so the
address has to come from `hal/`), the driver maps it, checks its PrimeCell ID
before writing a byte, claims interrupt 39, and blocks. The display harness
presses the key twice with QMP's `system_powerdown`. The second press is the
real test: with the driver's ack removed the first is reported and the second
never arrives. It reports through the console server as a client, because
owning the console would also let it read every key on the machine.

A device this simple was chosen so a failure would point at the primitives
rather than at the device. The next driver is xHCI, and its first step
exists: `user/drivers/usb/xhci.c` (0.10.48) finds every controller, takes each
from the firmware, resets it and reads its ports; its second (0.10.54) gives
each device a slot and an address and reads what it is, and since 0.10.55 it
stays and does the same for a device plugged in later. `usb.md` is how USB
works here, written as each step lands.

**The third is the backlight** (0.10.81): `user/drivers/display/backlight.c` maps one
page of the Intel graphics device - the block holding its two backlight PWM
controllers - and reads them. It only read at first, because those offsets
are Linux's rather than a datasheet's (`thinkpad.md` 8b), and a driver that
has not seen its registers on the machine does not get to write them. The
ThinkPad's reading confirmed them - controller 0 on at a third - and since
0.10.83 it raises that to 80% at boot, and reads the value back. Since
0.10.86 it stays and serves `/Devices/backlight` - a level from 0 to 256, for the
brightness keys (`backlightproto.h`, `thinkpad.md` 8c). It is the first
userland driver for a device the kernel also knows is there and has never
touched: the firmware left the screen lit, and nothing since has had any
reason to change that.

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

## 4b. Where a driver's file goes

`user/drivers/`, under the device's kind: `net/`, `usb/`, `display/`,
`power/`. Diego, 23 September, looking at a `user/servers/` that had reached
28 files: "shouldnt drivers have their on directory sorted by device type?
drivers/net/e1000.c".

**The test for which is which is his, and it is one line: a driver drives
hardware.** Asked whether `drives.c` counted, since it is about USB sticks:
"drives.c is a server then, not a driver, a driver drives hardware". So
`/Drives` serves a namespace out of FAT that something else read off the
wire, and stays a server; `e1000`, `xhci`, `backlight` and `powerbutton` each
own a piece of silicon.

**Nothing about a driver changes because of this.** It is still an EL0
process with an endpoint and a role number, built and spawned exactly as a
server is, on the three primitives above - which is §3's whole argument and
is why the two sat in one directory until now. What the split buys is not a
distinction in the system; it is a tree somebody can find something in. Two
of the four directories hold a single driver, which is the point rather than
an accident: a GPU driver has somewhere to go, and so does a battery.

`say.c` went to `user/lib/` in the same move, because it is neither one: it
is how a process that was handed the console's *endpoint* reports anyway, and
its fifth caller is a server.

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

- **Mass storage deletes the loader's disk.** The disk the loader carries
  into memory exists only because Kosmos cannot read the stick it booted
  from. See
  `thinkpad.md` §6a.
- **A USB Ethernet adapter puts the network stack on real hardware.** ARP,
  IP, TCP, DNS, `host` and the browser are written and have only ever run
  against virtio-net under emulation. The T14 has no RJ45.
- **And a real mouse**, which is its own reason.

Build order, each step ending in something visible: `usb.md`'s table is the
order and says which steps are built. This paragraph used to repeat it -
controller up, enumeration, bulk transfers, mass storage, Ethernet, "the
first two are built" - and was three steps behind the table by the time
bulk transfers landed.

### Intel Ethernet - written, and what it found underneath

`user/drivers/net/e1000.c`, 23 September 2026. The M700's card is an **I219-V**
at `00:1f.6`, `8086:15b8`; the same controller sits in the ThinkPad T14
variants that have a socket (`thinkpad.md` §9), so one driver serves both
machines and most Skylake-era PCs.

**A driver at EL0 like every other**, on the three primitives of §4:
`SYS_DEV_MAP` for the registers, a contiguous region for the descriptor
rings and the frame buffers, and `SYS_IRQ_CLAIM`/`WAIT`/`ACK` for the line.
The register decoding - the link out of STATUS, the MAC out of RAL0/RAH0,
the error bits a received descriptor can carry - is in `e1000_decode.c` so
it is checked on this Mac by `tools/test_e1000decode.c` rather than only on
a booted machine. `net.c` needed nothing: it already takes frames from a
ring (`ethring.h`), and where they come from is the driver's business.

QEMU has no model of the I219. What it has is the **82540EM**, which is the
same legacy 16-byte descriptors and the same register family, so that is
what the gate boots - the driver checked, not the exact silicon.

**The driver was the easy half and the interrupt path was not.** It worked
on the first boot, brought the link up, attached to the stack and answered
every ping - at 103 ms, which is its own fallback deadline of 25 ticks and
not the card's interrupt. What was wrong was all in `hal/pc`, and all three
are written up where they live: the input a PCI link lands on and the MSI
numbering in `pci.c`, and the mask that wrote a trigger mode away in
`apic.c`. The lesson that generalises is the one in §3's spirit: **none of
the three broke anything.** Frames moved, pings came back, no log said a
word. They cost 150x on the round trip and were found only because somebody
looked at the number - so the permanent test checks the *time*, not the
answer.

**On the I219, the firmware's state is not a starting point** (`testing.md`
18.346 to 18.353, October 2026). The driver first took the M700's card as
its firmware left it - no reset, because a reset is where Linux's `e1000e`
has its warnings for this chip, and the firmware's own network boot had
already brought it up. It sent; it received late, or not at all, through
six builds, each fixing a register that turned out to be innocent. The
firmware's receive ring was still in the card, and the card went on working
from what it had fetched of it, writing frames into memory that is the
system's now. **So an I219's MAC is reset before anything is handed to it**,
as `e1000e` always resets it; the firmware-first way is kept behind
`opt/kosmos/e1000path=pch` for the gate and for comparing. The lesson that
generalises: a device a firmware has *used* - for its own network boot, its
own disk - carries that use in state no register dump shows, and QEMU's
models carry none of it.

### Broadcom Ethernet - read, decoded, a draft unproved

Diego, 14 November 2026 (`roadmap.md`). The Mac Pro 2013 has a **BCM57762**
and the iMac 2019 a Broadcom gigabit controller of the same **57765 family**
(believed a BCM57766; its PCI id is read on its first boot). Both are
Linux's `tg3`, so one driver, `user/drivers/net/tg3.c`, serves the two Macs.

**Where it comes from.** Linux's `drivers/net/ethernet/broadcom/tg3.c` and
`tg3.h` (master aacb7797, read 14 November), and Broadcom's *BCM57785
Programmer's Reference Guide*, 57785-PG105-R, March 2012
(docs.broadcom.com/doc/1211168564335) - the 57765 family's own guide, whose
Table 2 (p. 47) lists the 57762 and the 57766 among its members. The PRG is
cited by page below; Linux by function or `tg3.h` macro. Neither is in the
tree.

**What is built.** `tg3_decode.c` says what the registers, the descriptors
and the status block mean, and `tools/test_tg3decode.c` holds it to 60
vectors worked out by hand from those two sources. `tg3.c` is a draft on
it in `e1000.c`'s shape, **never run**: QEMU has no model of the chip, so
the Mac Pro is where it is proved, its screen console the log. Before it
can run, the kernel needs a Broadcom device kind (a vendor 0x14E4 match in
`hal/pc/devices.c`, `DEV_BROADCOM_ETHERNET`, configuration reads for it) -
and probably a configuration write (below).

**The identifiers** (`TG3PCI_DEVICE_TIGON3_*`, vendor 0x14E4): 57761
0x16B0, 57765 0x16B4, 57781 0x16B1, 57785 0x16B5, 57791 0x16B2, 57795
0x16B6 (the last two 10/100 only, `TG3_DRV_DATA_FLAG_10_100_ONLY`), and
the 57766 ASIC's 57762 **0x1682**, 57766 **0x1686**, 57782 0x16B7, 57786
0x16B3. Every one has 0xF in the top nibble of MISC_HOST_CTRL's revision
(ASIC_REV_USE_PROD_ID_REG) and its real revision in configuration space at
0xFC, TG3PCI_GEN15_PRODID_ASICREV (`tg3_detect_asic_rev`; PRG p. 47).
ASIC_REV_57765 is 0x57785 and ASIC_REV_57766 0x57766; both are
57765_CLASS, which brings 57765_PLUS, a CPMU, hardware TSO 3, MSI and
MSI-X, and no APE. **A finding in Linux:** CHIPREV_57765_AX is 0x577650,
which no chip of the class can produce (an A0 reads 0x57785000), so the
"not AX" transmit-hang fix in `tg3_reset_hw` is applied to every one of
them; the test holds the draft to that.

**The reset** (`tg3_chip_reset`; PRG pp. 145-146): take the NVRAM
arbitration (NVRAM_SWARB); write MAGIC1 0x4B657654 to SRAM's firmware
mailbox 0xB50 (`tg3_write_sig_pre_reset`); GRC_FASTBOOT_PC = 0;
GRC_MISC_CFG bit 29, then CORECLK_RESET with it; wait; restore PCI_COMMAND,
MISC_HOST_CTRL and PCISTATE through configuration space and clear PCIe
DEVCTL's relaxed-ordering and no-snoop bits; MEMARB_MODE enable; then poll
0xB50 for the complement, ~0x4B657654, up to a second (`tg3_poll_fw`),
GRC_MODE, MAC_MODE, and the MDIO clock (MAC_MI_MODE_500KHZ_CONST on CPMU
chips). The two sources disagree on detail and the draft follows Linux:
the PRG sets GRC_MISC_CFG bit 26 (KEEP_GPHY_POWER) as well, Linux only
where there is no CPMU; the PRG waits a millisecond, Linux 120 us twice;
the PRG puts MAC_MODE in GMII and waits 40 ms, Linux writes 0.

**The on-chip CPUs are not halted.** The RX CPU's boot code runs after
every reset, and its answer in 0xB50 is what the driver waits for; 5705_PLUS
chips have no TX CPU at all (a BUG_ON in `tg3_halt_cpu`). Linux halts the
RX CPU only to load firmware, and for the 57766's patch it pauses it
instead (`tg3_rxcpu_pause`, after RX_CPU_HWBKPT reads 0x51,
TG3_SBROM_IN_SERVICE_LOOP).

**The 57766 runs without `tigon/tg357766.bin`; what is lost is EEE.**
`tp->fw_needed = FIRMWARE_TG357766` is the "EEE patch": Linux loads it
only when the chip has no NVRAM (`tg3_load_57766_firmware`, at SRAM
0x30000), a failed request prints "EEE capability disabled" and clears
TG3PHYFLG_EEE_CAP (`tg3_open`), and `tg3_reset_hw` says "If download fails,
the device will operate with EEE disabled". So 802.3az energy saving is
off and nothing else changes. The draft loads nothing.

**The rings and the status block.** A host-based send ring
(GRC_MODE_HOST_SENDBDS) of TG3_TX_RING_SIZE 512 sixteen-byte descriptors
(addr hi, lo, length << 16 | flags, VLAN; PRG Table 6, p. 69); a standard
receive producer ring of 512 thirty-two-byte entries, its control block at
RCVDBDI_STD_BD by MMIO, NIC address 0x6000, buffers of 1536; jumbo
disabled; one receive return ring, whose control block is in NIC SRAM at
0x200 (send's at 0x100), the others disabled. The return ring is 1024 in
Linux (TG3_RX_RET_MAX_SIZE_5700) and 32 to 512 in the PRG (p. 147, step
36); the draft takes 512. Control blocks in SRAM are reached through the
memory window, TG3PCI_MEM_WIN_BASE_ADDR / _DATA at 0x7C / 0x84, by MMIO.
The **status block** is 0x50 bytes the chip writes by DMA (`struct
tg3_hw_status`): the status word (UPDATED, LINK_CHG, ERROR), the tag, the
standard ring's consumer, and at 0x10 the return ring's producer and the
send ring's consumer; PRG Table 14 (p. 81) agrees, except that it puts the
jumbo consumer at 0x14 where Linux has 0x08. A received length includes
the four-byte FCS; the receive entry's opaque word says ring and index.

**The PHY and the link.** An internal BCM57765 PHY, TG3_PHY_ID_BCM57765
0x5C0D8A40, at MDIO address 1 (TG3_PHY_MII_ADDR), reached through
MAC_MI_COM 0x44C. The link is BMSR's LSTATUS, read twice, and speed and
duplex are MII_TG3_AUX_STAT 0x19's bits 10:8; MAC_MODE then takes MII at
10/100 or GMII, and half duplex where the link is. A change arrives as the
status block's LINK_CHG, with MAC_EVENT_LNKSTATE_CHANGED and
MAC_MI_STAT_LNKSTAT_ATTN_ENAB set (USE_LINKCHG_REG polling is the 5700's
only).

**MSI, with tagged status.** The class has MSI, one-shot MSI and MSI-X.
Linux uses MSI only with tagged status (`tg3_ints_init`): MSGINT_MODE
enabled, MISC_HOST_CTRL_TAGGED_STATUS, and an interrupt re-armed by
writing the last tag to the top byte of interrupt mailbox 0 (PRG p. 234,
step 9); 1 masks. Legacy INTx works the same way through the mailbox. The
class can lose an MSI, which Linux catches on a timer
(`tg3_chk_missed_msi`); the draft's 25-tick deadline is that.

**The MAC address** (`tg3_get_device_address`): the platform first
(device tree or an ACPI property), then the boot code's SRAM mailbox at
0xC14 / 0xC18 with 0x484B in the top half, then NVRAM at 0x7C, then
MAC_ADDR_0. Linux has nothing Apple-specific; on a Mac the boot code is
expected to fill the mailbox and the MAC registers from the chip's own
storage (self-boot parts may have no NVRAM and keep it in OTP; PRG Table
3, p. 63). 00:10:18:00:00:00 is the unprogrammed default and means "ask
the platform". The draft reads the mailbox, then the registers.

**What did not fit `e1000.c`'s shape.** A configuration write: the reset
can clear what Linux restores through configuration space, and Kosmos's
SYS_DEV_CONFIG only reads. The draft writes MISC_HOST_CTRL and PCISTATE
through their MMIO mirror and says whether PCI_COMMAND survived; if
memory decode did not, the kernel has to grow a write. Rings whose size is
the chip's (512) rather than the driver's, configured in NIC SRAM through
a window. A status block in place of descriptor write-back. Tagged
interrupts. A PHY driven over MDIO where e1000 only sets CTRL.SLU. A boot
CPU whose answer is waited for.

### SATA (AHCI) - the M700's drive, found and read

`user/drivers/storage/ahci.c`, a process with device authority as the
Ethernet card's driver is, from the specification (Serial ATA AHCI 1.3.1,
ACS-3) and checked against QEMU's `hw/ide/ahci-internal.h`. The board finds
the controller by class 01.06.01 and gives its register window from BAR5,
the ABAR, rather than the first BAR (`HAL_DEV_AHCI`, `DEV_AHCI`); the ABAR
need not begin a page - Intel's is 2 KB - so the driver maps the page holding
it. **Its first step, 0.11.198, finds and reads and writes nothing**: the
controller taken from the firmware (§10.6) and put in AHCI mode, each port
with a disk on it brought up in §10.1.2's order, IDENTIFY DEVICE asked, and
the first two sectors read to say whether there is a GPT. The FIS, the
command header and IDENTIFY's page are `ahci_decode.c`, held on the host by
`test_ahcidecode` to the standards' bytes. Polled with a deadline, as a
probe before it serves anyone; serving `/Devices/blocks`' protocol - which
has one server today, xHCI, so diskfs and drives will ask two - waits on the
controller's interrupt, and is the next step (`roadmap.md`).

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

## 6. The order

**This section opened with "before any of it: bring the display up early",
and that was already done.** It rested on a machine with no serial port
showing nothing until boot stage six; the panel had shown the boot log from
stage two since 0.10.12, through `hal_fb_early`, which takes the framebuffer
the firmware set up before there is a page allocator. Found on 12 September
when the work was started. **And on the ThinkPad it had not**, which nobody
knew until 13 September: that firmware's screen is at `0x4000000000`, above
the 4 GB `hal_fb_early` would take, so the machine was dark until stage six on
every boot until 0.10.62 mapped it (`boot.md` §5).

`roadmap.md` holds the order now, with USB at the front. The order as this
section first wrote it:

1. **NVMe** - a boot, not a project.
2. **Audio** - dump the topology, then write the walk.
3. **USB** - the driver primitives, then xHCI as the first userland driver,
   then mass storage and Ethernet.
4. **WiFi** - `iwx` into a server, once the shape above is proven.
