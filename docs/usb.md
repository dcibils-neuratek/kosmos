# USB

How USB works in Kosmos, from the host controller up. **Written as it is
built**: each step adds its section when it lands, and a section describes
what exists, not what is planned. The plan is the table below and nothing
else.

| step | what it ends in | state |
| ---- | --------------- | ----- |
| 1. controllers up | every xHCI controller found, taken from the firmware, reset, and its ports read | built, and run on the ThinkPad |
| 2. enumeration | a device's descriptors read: what it is, who made it | built, and run under QEMU |
| 3. bulk transfers | bytes to and from an endpoint | not started |
| 4. mass storage | the stick Kosmos booted from, mounted as its disk | not started |
| 5. Ethernet | a USB-C adapter carrying the network stack | not started |

`roadmap.md` has why USB is first, and `thinkpad.md` §6a the evening that
decided it: the ThinkPad carries its disk as memory because Kosmos cannot
read the stick it booted from.

**Where step 4 is going, as Diego put it**: once USB works, the drive is
mounted over USB, so big files live on the disk. Today GRUB loads the whole
disk image into memory as a module before Kosmos starts, and on the ThinkPad
that image must be 32 MB or less or the machine does not boot
(`thinkpad.md` §6a). Reading the stick directly removes the module, and with
it the limit.

---

## 1. The shape: a driver is a process

**USB lives in userland**, decided on 12 September and recorded in
`drivers.md`. Three pieces, and each knows only its own part:

- **The board** (`hal/`) knows where a controller is. On a PC that is a
  question for the PCI bus, answered in `hal/pc/devices.c`.
- **The kernel** relays that answer and hands out four things a driver
  cannot take for itself: the controller's address (`SYS_DEV_FIND`), its
  registers mapped uncached (`SYS_DEV_MAP`), memory a device can reach
  (`MEM_CONTIGUOUS` and `SYS_MEM_PHYS`), and its interrupt as a capability
  (`SYS_IRQ_CLAIM`, `WAIT` - with a deadline since 0.10.52 - and `ACK`).
  The kernel does not know what USB is.
- **The driver** (`user/servers/xhci.c`) is a C server, spawned by init with
  device authority and the console's endpoint to report through.

The power button proved those primitives on a device where a failure could
only be *theirs* (`drivers.md` §4). xHCI is the first driver that uses them
for something that matters.

---

## 2. How an xHCI controller works

Enough of the controller to read the code by. Section numbers are the
Intel specification's, revision 1.2.

### Finding one

An xHCI controller is a PCI function with **class 0Ch** (serial bus),
**subclass 03h** (USB) and **programming interface 30h** (xHCI). The same
class and subclass with interface 00h, 10h and 20h are the three older
controller types - UHCI, OHCI, EHCI - so all three bytes are compared.
QEMU's `qemu-xhci` reads as `1b36:000d`, class `0c0330`, in this system's
own bus listing.

**A machine can have several.** A laptop of the ThinkPad's generation is
expected to carry one in its chipset and one for its USB-C ports; which of
them a given socket is wired to is the machine's business. So the driver
asks for controller 0, 1, 2 until it is told there are no more.

### Its registers

Everything is in the controller's first BAR, in four groups:

| group | where | what it is for |
| ----- | ----- | -------------- |
| capability | the start of the BAR | what this controller can do; read-only (5.3) |
| operational | + CAPLENGTH | command, status, and one register set per port (5.4) |
| runtime | + RTSOFF | the interrupters and their event rings (5.5) |
| doorbells | + DBOFF | how software tells the controller there is work (5.6) |

The ones step 1 uses:

| register | offset | fields used |
| -------- | ------ | ----------- |
| CAPLENGTH, HCIVERSION | 00h | 7:0 operational offset; 31:16 version in BCD |
| HCSPARAMS1 | 04h | 7:0 device slots; 31:24 ports |
| HCCPARAMS1 | 10h | 31:16 extended capability pointer, in 32-bit words |
| USBCMD | op + 00h | bit 0 Run/Stop; bit 1 reset |
| USBSTS | op + 04h | bit 0 halted; bit 11 not ready |
| PORTSC *n* | op + 400h + 10h × (*n* − 1) | bit 0 connected; 13:10 speed |

**Ports are numbered from one**, and a port speaks one protocol. Which ports
are USB 2 and which USB 3 is not in the port register: it is in the
*Supported Protocol* capability (7.2), one per protocol, giving a first port
and a count. A USB 3 socket on the outside of a machine is usually *two*
ports inside it - one USB 3, one USB 2 - because the two protocols run on
separate wires.

**Speeds** are a number in PORTSC, and Table 7-13 gives the defaults: 1
Full-speed, 2 Low-speed, 3 High-speed, 4 SuperSpeed, 5 to 7 SuperSpeedPlus.
A controller may define its own numbering in its protocol capability, which
is why the driver prints the number beside the name.

**And on a USB 2 port the number means nothing yet.** Table 5-27: the field
"is invalid on a USB2 protocol port until after the port is reset" - a USB 2
device says how fast it is during that reset. Step 1 resets no port, so it
names a speed only on a USB 3 port. The ThinkPad is how this was found.

### Extended capabilities

A linked list inside the BAR (7): each entry's first word holds an ID (7:0)
and the distance to the next one in 32-bit words (15:8), zero at the end.
Two IDs matter now - **1, USB Legacy Support**, and **2, Supported
Protocol**. The pointers are the device's to set, so the driver checks every
one against the size of its window before reading, and gives up after 64
entries in case the list is a loop.

### Taking it from the firmware

**On a real machine the firmware is using the controller first** - that is
how a USB keyboard works in a boot menu - and it must be asked to let go
(4.22.1, 7.1). The Legacy Support capability has two one-bit semaphores in
adjacent bytes: the firmware's at bit 16, the operating system's at bit 24.
The OS sets its own and waits for the firmware to clear its, for no more
than a second. They are in separate bytes so each side can write its own
without rewriting the other's, so the driver writes that one byte and not
the word.

The specification gives the firmware no way to refuse, only to be slow. If
it has not let go after a second the driver says so and carries on.

### Reset

In order (4.2, 5.4.1, 5.4.2):

1. **Wait for Controller Not Ready to clear.** Nothing operational may be
   written before it does.
2. **Halt it** if it is running: clear Run/Stop and wait for Halted. The
   specification allows 16 ms.
3. **Reset it**: set HCRST. Never on a running controller.
4. **Wait for HCRST to clear itself, and Not Ready again.** The controller
   decides when the reset is over; software cannot end it early.

After a reset every operational register, and every port, is back at its
initial state. **A port still reports a device while the controller is
halted** (4.19.2), which is what lets step 1 read ports without starting
anything.

### Rings and TRBs

Work crosses between software and the controller in circular buffers of
16-byte **TRBs** (6.4): four little-endian words, the type in word 3's bits
15:10 and a **cycle bit** in its bit 0. A reader takes TRBs while their cycle
bit matches its own copy, and the first that does not is where the writer has
got to (4.9). A ring ends in a **Link TRB** pointing back to its start with
Toggle Cycle set, so the cycle bit turns over each time round and the last
pass's TRBs stop matching. **Software writes a TRB's last word last**, because
the cycle bit is in it and the other three have to be in place first.

Three kinds, and step 2 uses all three: the **command ring**, software to
controller - No-Op, Enable Slot, Address Device, Evaluate Context; the
**event ring**, controller to software - a command completed, a transfer
completed, a port changed; and a **transfer ring** for each endpoint, here
only the default control endpoint's. A **doorbell** says a ring has new work
(5.6): doorbell 0, target 0, for commands; a device's own doorbell, target 1,
for its control endpoint.

| register | offset | fields used |
| -------- | ------ | ----------- |
| HCSPARAMS2 | 08h | 25:21 and 31:27 scratchpad pages |
| HCCPARAMS1 | 10h | bit 0 64-bit addressing; bit 2 64-byte contexts |
| DBOFF, RTSOFF | 14h, 18h | where the doorbells and runtime registers are |
| USBCMD | op + 00h | bit 2 interrupt enable |
| USBSTS | op + 04h | bit 3 event interrupt |
| PAGESIZE | op + 08h | bit 0: 4K pages |
| CRCR | op + 18h | the command ring, and its cycle bit in bit 0 |
| DCBAAP | op + 30h | the context array |
| CONFIG | op + 38h | 7:0 slots enabled |
| PORTSC *n* | op + 400h + 10h × (*n* − 1) | bit 1 enabled; 4 reset; 9 power; 21 reset changed |
| IMAN, IMOD | rt + 20h, 24h | interrupt pending and enable; moderation |
| ERSTSZ, ERSTBA, ERDP | rt + 28h, 30h, 38h | the event ring |

### The event ring and its interrupt

The event ring is described by a table of segments (6.5), here one of 256
TRBs. **The order of the writes matters**, and QEMU's model shows why: ERSTSZ,
then ERDP, then ERSTBA's low half and **its high half last**, the write that
starts the ring. Software takes events while their cycle bit matches its own,
which starts at 1 and turns over where the segment wraps, and then writes
ERDP - high half first, low half with Event Handler Busy set - which lets the
controller interrupt again if more are waiting (5.5.2.3.3).

An interrupt needs Interrupter Enable in IMAN **and** INTE in USBCMD (4.17).
After one, USBSTS's EINT is cleared before IMAN's IP (5.4.2). On a PCI
controller it is an MSI or an **MSI-X** message (5.2.8); QEMU's controller
offers only MSI-X, and `hal/pc/pci.c` programs whichever the device has.

### Slots, contexts and an address

**Enable Slot** gives a device a slot ID (4.3.2), with the slot type the port's
Supported Protocol capability declares (Table 7-9): the number is the
controller's own, and software may not assume one. A device's state lives in
a **device context** software provides, found through the Device Context Base
Address Array - DCBAAP points at it, entry *n* is slot *n*'s context, and entry
0 is the scratchpad array when the controller asks for scratchpad pages
(4.20).

**Address Device** takes an **input context** (6.2.5): an input control
context whose add flags say which entries follow - A0 the slot, A1 endpoint 0 -
then the slot context, with the root port in 23:16 and one context entry, and
endpoint 0's, a control endpoint with its packet size by speed and its ring.
Contexts are 32 bytes, or 64 when HCCPARAMS1's CSZ is set. QEMU refuses any
add flags but those two.

**A USB 2 port is reset first** (4.3.1): Port Reset written with Port Power and
nothing else - the change bits are write-1-to-clear and must not be echoed -
and Port Reset Change awaited. After that, and not before, its speed is valid.
A USB 3 port enables itself.

### Asking a device what it is

A **control transfer** (4.11.2.2, 6.4.1.2) is three TRBs: Setup, with the
eight setup bytes inline; Data, pointing at a buffer; Status, which
interrupts on completion. GET_DESCRIPTOR - request type 80h, request 6 - with
value 0100h returns the **device descriptor**: the USB version, the class, the
vendor and product numbers, and which string names the product. A full-speed
device's control packet size is not known until its first eight bytes are
read, and Evaluate Context tells the controller (4.3.4). Strings are UTF-16,
and string 0 lists the languages.

**The descriptors' layout is the USB 2.0 specification's (9.6)**, which is not
in this project's references. The fields read are few and universal; the
product strings are checked against the emulator's own binary, and the vendor
and product numbers have no second source here.

---

## 3. Step 1: controllers up

### The board: `hal/pc/devices.c`

`hal_device_find(HAL_DEV_XHCI, index, out)` walks the USB-class devices on
PCI and counts only those with interface 30h. For the one asked for it:

- **sizes the BAR** (`pci_bar_size` in `pci.c`): all ones written, read
  back, the original restored, with memory decoding off meanwhile so the
  device never answers at an address that is somebody else's. Needed
  because a driver maps a window of a size and the address does not give
  it: extended capabilities can sit tens of kilobytes in;
- **enables it once** (`pci_enable`: memory decoding, bus mastering, and an
  MSI where the machine can) and keeps the answer. `pci_enable` takes an
  MSI vector from a range of four every time it runs, so a second call would
  spend a second vector.

It reports the BAR's address and size, the interrupt, and **`where`**: the
PCI address as bus << 8 | slot << 3 | function, which is what the driver
prints to name a controller. The ARM board has no xHCI, and says so for
every index.

### The kernel: `SYS_DEV_FIND (kind, index, &info)`

It gained **`index`** for the reason above, and `struct dev_info` gained
**`where`** in what was a reserved word, so the structure did not change
size. Past the last device of a kind the answer is `SYS_ERR_NO_DEVICE`.
Kind 2 is `DEV_XHCI`, held equal to the board's `HAL_DEV_XHCI` by a static
assertion as `DEV_PL061_POWER_KEY` already was.

### The driver: `user/servers/xhci.c`

For controller 0, 1, 2 until there are no more:

1. map the BAR, in pages rounded up from its size;
2. read the version, the port count and the slot count, and **print them**;
3. check the port registers lie inside the window;
4. walk the extended capabilities: take the controller from the firmware
   if there is a Legacy Support capability, and note which ports are USB 2
   and which USB 3;
5. wait for ready, halt, reset, wait for ready again;
6. give the ports half a second - a USB 3 link retrains after a reset - and
   print every port that has a device, with its protocol, and its speed where
   the field holds one: on a USB 3 port.

Then one closing line naming the controllers, with the totals, and **exit**. With no rings there is
nothing to wait for, and no interrupt is claimed: every question in this
step is a register read and every wait is polled and bounded.

A machine with no xHCI controller - the ARM board, and every x86 test boot
that does not ask for one - hears nothing at all: the driver is told there
is none and exits.

**It reports through the console server as a client**, with `say.h`, which
moved out of `powerbutton.c` when this became the second driver to need it.
A driver cannot own the console, because `owns_console` also grants every
key and pointer event on the machine.

### What QEMU cannot show

**The firmware handoff.** QEMU's controller has no Legacy Support
capability, so under emulation the driver prints *has no firmware handoff
to make* and that code never runs. The ThinkPad is where it first runs, and
its line says which of three things happened: the controller was not the
firmware's, it was taken after so many milliseconds, or the firmware had not
let go after a second. It also says if any of the firmware's SMI enables
(USBLEGCTLSTS bits 0, 4, 13, 14, 15; 7.1.2) are still set afterwards.

### On the ThinkPad

0.10.48 ran on the T14 on 12 September, from a stick with a 32 MB disk - a
64 MB one does not boot at all, `thinkpad.md` §6a. The driver's lines, as
photographed:

```
xhci: 00:14.0, version 1.2, 16 ports, 64 slots
xhci: 00:14.0 was not the firmware's; claimed
xhci: 00:14.0 port 3, USB 2: a Full-speed device (speed ID 1)
xhci: 00:14.0 port 4, USB 2: a Full-speed device (speed ID 1)
xhci: 00:14.0 port 7, USB 2: a Full-speed device (speed ID 1)
xhci: 00:14.0 port 10, USB 2: a Full-speed device (speed ID 1)
xhci: 2 controllers, 4 ports with something plugged in
```

What that established:

- **The chipset's controller is at 00:14.0**: xHCI 1.2, sixteen ports.
- **It has a Legacy Support capability**, which QEMU's does not, so the
  handoff code ran for the first time. The firmware no longer held the
  controller when Kosmos asked, and none of its SMI enables were left set.
- **The reset completed**, and four USB 2 ports have something behind them -
  most likely devices built into the laptop, which enumeration will name.
- **There is a second controller**, and none of its lines are on the
  photograph: they came before the shell's banner and scrolled away. The
  closing line names the controllers now, so one photograph is enough.

**And the four speeds were wrong.** Every USB 2 port said "Full-speed", from
a field the specification says is invalid on a USB 2 port until the port is
reset - which this step does not do. The driver now says a USB 2 port's
speed is unknown until then.

### How it is tested

`tools/run_x86.py` boots q35 with two `qemu-xhci` controllers, a USB stick on
the second and a USB keyboard on the first, and checks what the driver
prints: two controllers at different addresses, the stick on the second as a
SuperSpeed device, the keyboard's USB 2 port with its speed unknown, and a
closing line naming both controllers. A boot with no controller must hear
nothing from the driver. `testing.md` §18.32 has the negative controls, all
run and watched fail.

What QEMU printed at step 1 - step 2 resets the keyboard's port and names its
speed after it (§4):

```
xhci: 00:03.0, version 1.0, 8 ports, 64 slots
xhci: 00:03.0 has no firmware handoff to make
xhci: 00:03.0 port 5, USB 2: a device, its speed unknown until the port is reset
xhci: 00:04.0, version 1.0, 8 ports, 64 slots
xhci: 00:04.0 has no firmware handoff to make
xhci: 00:04.0 port 1, USB 3: a SuperSpeed device (speed ID 4)
xhci: 2 controllers (00:03.0, 00:04.0), 2 ports with something plugged in
```

The stick is on port 1 because QEMU numbers its USB 3 ports first
(`hcd-xhci.c`), and QEMU attached it at SuperSpeed - speed ID 4. The keyboard
is on port 5, the first USB 2 port, and its speed is left unsaid: before a
port reset QEMU's field reads High-speed, and the specification says not to
believe that field yet.

---

## 4. Step 2: enumeration

### The kernel: a deadline, a wrapper put right, and MSI-X

- **`SYS_IRQ_WAIT (cap, ticks)`** returns `SYS_NO_INTERRUPT` when the deadline
  passes (0.10.52, `testing.md` §18.35). A driver waiting on an interrupt that
  never comes would otherwise hang and say nothing, and the ThinkPad is where
  that would happen.
- **`kosmos_mem_create` passes the flags argument the kernel reads** - found
  wrong while writing this driver's wrappers (0.10.53, §18.36).
- **MSI-X, in `hal/pc/pci.c`** (0.10.54). QEMU's xHCI controller has no MSI
  capability - measured: its list is MSI-X at 90h and PCI Express at A0h, and
  nothing else - so the controller was left on a PCI line and no interrupt
  reached the driver. The same message MSI uses is written into entry 0 of a
  table in one of the device's BARs. Enable and the mask bits are PCI 6.8.2's,
  which is not in the references; a probe in the kernel then saw every
  interrupt delivered to the driver's claim, and the check below fails when
  MSI-X is not enabled (`testing.md` §18.37).

### The driver

For each controller, after step 1:

1. read the page size, the context size, the scratchpad count and whether it
   addresses 64 bits, and refuse to drive what this does not support, saying
   which;
2. one contiguous region: the context array, the command ring, the event
   segment, the segment table with the scratchpad array, four pages for each
   of eight devices, and the scratchpad pages;
3. program the slots enabled, DCBAAP, CRCR, the event ring in its order,
   moderation at 1 ms and both interrupt enables; claim the interrupt; Run -
   and print a line saying the context size, the scratchpads, the slots and
   the interrupt;
4. a **No-Op command**, answered on the event ring, and then **the interrupt
   line asked** whether its interrupt came - "by interrupt", or "found by
   looking" when a second passes without one. Asked, because QEMU completes a
   command inside the doorbell write: the answer is on the ring before anybody
   waits, and the first version of this reported no interrupt while one sat
   pending on its line;
5. for every port with a device: reset it if it is USB 2 and print its speed
   after; Enable Slot; Address Device; the device descriptor and the product
   string, printed as one line;
6. **stop the controller and reset it before exiting.** The kernel takes the
   region back when the process ends, and a controller still running would go
   on writing events into pages that may be somebody else's by then.

### What QEMU cannot show

- **Scratchpad pages, 64-byte contexts, and a controller that addresses only
  32 bits.** QEMU's asks for no scratchpads (HCSPARAMS2 reads 0Fh), has CSZ
  clear and AC64 set. All three paths are written, and the "runs:" line says
  which apply, so a photograph from the ThinkPad shows it.
- **A full-speed device's packet size.** QEMU attaches the keyboard at high
  speed and the stick at SuperSpeed, so Evaluate Context never runs.
- **An interrupt that takes time.** QEMU's arrive with the answer; a real
  controller's do not, which is the path `wait_event` waits on.
- **The firmware handoff**, as in step 1.

### How it is tested

`tools/run_x86.py`'s USB check boots q35 with the same two controllers, the
stick on the second and the keyboard on the first, and asks for: both
controllers running with their interrupts claimed; a No-Op answered **by
interrupt** on each - the first MSI-X to reach a process on x86; the
keyboard's port reset and named high-speed after it, and no USB 2 speed named
without a reset; two devices' descriptors and product strings; and the closing
line's two devices.

**What the devices said is compared with QEMU, not with this driver's idea of
it.** A second QEMU with the same devices, never started, is asked `info usb`
over its monitor, and the speeds must match. The product strings must each be
one the QEMU binary carries: `info usb`'s "Product" is QEMU's name for the
device model, and for the stick that is "QEMU USB MSD" while the stick itself
says "QEMU USB HARDDRIVE" - which the first run found. 14 checks,
with four negative controls in `testing.md` §18.37.

What QEMU prints:

```
xhci: 00:03.0, version 1.0, 8 ports, 64 slots
xhci: 00:03.0 has no firmware handoff to make
xhci: 00:03.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 20
xhci: 00:03.0 answered a No-Op command on its event ring, by interrupt
xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3), after its reset
xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Keyboard"
xhci: 00:04.0, version 1.0, 8 ports, 64 slots
xhci: 00:04.0 has no firmware handoff to make
xhci: 00:04.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 21
xhci: 00:04.0 answered a No-Op command on its event ring, by interrupt
xhci: 00:04.0 port 1, USB 3: a SuperSpeed device (speed ID 4)
xhci: 00:04.0 port 1: 46f4:0001, USB 3.0, class 0, "QEMU USB HARDDRIVE"
xhci: 2 controllers (00:03.0, 00:04.0), 2 ports with something plugged in, 2 devices named
```

---

## Sources

- Intel, *eXtensible Host Controller Interface for Universal Serial Bus
  (xHCI)*, revision 1.2 - every offset and bit in `xhci.c`, with the table
  or section beside it. Downloaded from intel.com on 12 September 2026 to
  read, and not kept in the repository.
- QEMU 11.1.1, `hw/usb/hcd-xhci.c` and `hcd-xhci.h` - the model the tests
  run against, read for how it behaves: that it has no Legacy Support
  capability, that its ports are USB 3 first and USB 2 after, four of each
  by default. Nothing is copied from it.
