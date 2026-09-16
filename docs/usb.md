# USB

How USB works in Kosmos, from the host controller up. **Written as it is
built**: each step adds its section when it lands, and a section describes
what exists, not what is planned. The plan is the table below and nothing
else.

| step | what it ends in | state |
| ---- | --------------- | ----- |
| 1. controllers up | every xHCI controller found, taken from the firmware, reset, and its ports read | built, and run on the ThinkPad |
| 2. enumeration | a device's descriptors read: what it is, who made it | built, and run on the ThinkPad |
| 3. a mouse | a HID mouse's reports moving the pointer the TrackPoint moves | built, and run on the ThinkPad |
| 4. bulk transfers | bytes to and from an endpoint | built, and run under QEMU |
| 5. mass storage | the stick Kosmos booted from, mounted as its disk | built, 5a to 5f, and run on the ThinkPad: `/home` on the stick it booted from (`roadmap.md`) |
| 6. drives | every drive shown and named - Tracker, a Drives app, one Open and Save window - and FAT16, FAT32 and exFAT read, read only (`drives.html`) | 6a built: FAT's bytes, read on the Mac |
| 7. Ethernet | a USB-C adapter carrying the network stack | not started |

`roadmap.md` has why USB is first, and `thinkpad.md` §6a the evening that
decided it: the ThinkPad carries its disk as memory because Kosmos cannot
read the stick it booted from. **The mouse went before bulk transfers on
13 September**, Diego's call, because his USB mouse did nothing on the
ThinkPad's desktop.

**Where step 5 is going, as Diego put it**: once USB works, the drive is
mounted over USB, so big files live on the disk. Today the loader reads the
whole disk image into memory before Kosmos starts, and a stick's image is kept
to 32 MB or less until the ThinkPad has booted a bigger one through Kosmos's
own loader (`boot.md`). Reading the stick directly removes the copy in
memory, and with it the limit. Decided on 14 September, in five calls Diego
approved together: the driver hears requests through the kernel's interrupt
wait, a Kosmos stick carries kfs in a partition of its own, only that
partition is mounted, as `/home`, it is written as well as read, and bytes
cross between processes through a copy - `README.md` has each and why.

**And step 6 is Diego's too**, the same week: "we need fat32 driver so we can
mount usb drives that i have with content that i would like to have avaiable
on kosmos", "then you have exfat as well". Decided on 14 September: **Kosmos's
own reader, read-only first** - `README.md` has the decision and why. Built
in six pieces, in the order Diego agreed ("go with your order"): **6a** the
FAT reader, tested on the Mac; **6b** the drive server, and every FAT
partition at `/drives/<label>`; **6c** Tracker's sidebar - Places, System,
Drives - and the whole trail; **6d** one Open and Save window; **6e** the
Drives app; **6f** exFAT.

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
ERDP with Event Handler Busy set, which lets the controller interrupt again
if more are waiting (5.5.2.3.3).

**Every one of those 64-bit registers is written low half first and high
half second**, because 5.1 says so in as many words: "low Dword-first,
high-Dword second". ERDP was written the other way round from 0.10.54 to 13
September, under a comment saying the low half's write is where the
controller looks - which is QEMU's model and not the specification:
`hcd-xhci.c` clears Busy and raises the interrupt again on the low write,
and only stores the high one. The ThinkPad's controller behaved as one that
takes the register when its high half arrives would (§5, On the ThinkPad).
QEMU cannot show the difference, so `run_x86.py` reads the order out of
QEMU's own trace of the writes (`testing.md` §18.47).

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
   of eight devices - six since step 3 - and the scratchpad pages;
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
5. for every port: its change bits cleared, and if it has a device, a reset
   if it is USB 2 and its speed printed after; Enable Slot; Address Device,
   once more if that fails; the device descriptor and the product string,
   printed as one line;
6. **keep every controller that started, and watch it** - below. One that did
   not start is stopped and reset: the kernel takes the region back when the
   process ends, and a controller still running would go on writing events
   into pages that may be somebody else's by then. With none running, the
   driver exits.

### On the ThinkPad

0.10.54 ran on the T14 on 12 September. Read off the photograph:

- **Both controllers answered the No-Op by interrupt**, on interrupts 21 and
  22: 00:14.0, the chipset's, and 00:0d.0 - most likely the one behind the
  USB-C sockets. It is the first time a real
  chipset's interrupt has reached a process on x86.
- **Both asked for 34 scratchpad pages**, and ran with them - a path QEMU
  never takes. **Both use 32-byte contexts**, so the 64-byte path has still
  run nowhere.
- **Three devices named themselves:** port 1, `04d9:fc38`, "USB Gaming
  Mouse", full-speed - Diego's mouse; port 4, `04f2:b724`, "Integrated
  Camera", class 239; port 3, `06cb:00bd`, class 255 and no product string.
  Ports 3, 4, 7 and 10 are the four step 1 found on 00:14.0.
  06cb is Synaptics, so port 3 is most likely the fingerprint reader; that is
  an inference, not a lookup.
- **Two did not**: port 7, high-speed, and port 10, full-speed, each "no slot
  and address for the device" - which said nothing about which command
  failed, or how. Diego sees three things plugged in, the stick, the mouse
  and the camera, so port 7 is probably the stick Kosmos booted from and
  port 10 something inside the laptop - and on 13 September both named
  themselves, below.

So the driver now says why, tries once more, and stays to watch - the two
sections below.

### Why a device was not named, and a second try

**Every failure now says which step failed and what the controller
answered**: the completion code in its event (6.4.5, Table 6-91), named for
codes 1 to 9 and given as a number beyond them, or "no answer within a
second" when no event came. The form, which the ThinkPad has not yet
printed:

```
xhci: 00:14.0 port 7: Address Device failed: USB Transaction Error (4)
```

**Then once more**, as 4.6.5's notes allow: a failed Address Device leaves
its slot in Default, and software may disable the slot or reset the device
and try again. So the slot is given back, a USB 2 port is reset again, the
driver waits 50 ms, and Enable Slot and Address Device run from the start. A
line says whether that worked. The wait stood for USB 2.0's recovery
interval (9.2.6.3), which was not in the references then, so 50 ms was
chosen well above it as remembered rather than quoted. **Since step 3 the
first attempt waits what USB 2.0 asks** - 10 ms after the reset and 2 ms
after the address (§5) - and the second 50 ms on top of that, so a photograph
still says which of the two a device needed.

**Every slot is given back** - after an unplug, after a first failure, and
after a second, which the first version of the retry forgot: the port
records no slot for a device it could not address, so nothing later would
have disabled it, and the ThinkPad's two failing ports would have held two of
the eight slots from boot. A slot numbered past the ones enabled, which QEMU
hands out and a controller should not (5.4.7), is given back too. Its entry
in the context array goes to 0 once the controller has said it is disabled
(4.6.4, 6.1).

### Plugged in and pulled out

**Diego asked for it on 12 September**: unplug the mouse and see which port
went quiet, so the sockets on the outside of the laptop can be matched to the
port numbers inside it. There were two ways. A `usbscan` command could run
the driver again, but it would need a way to ask init for device authority.
Or the driver could stay up and print changes as they happen. He chose the
second, which is also the driver a mouse needs.

**After the boot scan the driver stays**, with every controller that started.
It waits up to 50 ms for an interrupt - on each controller in turn until
step 3, and on all of them at once since (§5) - then empties the event rings
and reads every port's status. **The ports decide, not the
events.** 4.19.2 promises no agreement between a read of PORTSC and the
events already on the ring, so an event only makes the driver look sooner. A
plug interrupts at once; the 50 ms bounds a controller whose interrupt never
comes.

**A port's change bits are cleared before anything is done about it**,
because a port raises no further change events until every one is clear
(4.19.2). The boot scan clears them too, before it looks at each port, so a
change after that instant belongs to the watch. On a connect change:

- a device recorded on the port has left: a line saying "unplugged" and what
  it was, and its slot given back;
- and if the port holds a device now, it is reset, addressed and named
  exactly as at boot - after USB 2.0's debounce, since step 3.

What QEMU prints, with a keyboard pulled out and put back:

```
xhci: watching for devices plugged in and out
xhci: 00:03.0 port 5: unplugged, 0627:0001 "QEMU USB Keyboard"
xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3), after its reset
xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Keyboard"
```

**What it did not do was talk to a device after naming it.** A mouse needs
its configuration set, an interrupt endpoint read, and a way for a process to
move the pointer - which is step 3.

### On the ThinkPad, 13 September

The first boot with the second try and the plug lines, through Kosmos's own
loader. Read off the photographs:

- **Port 7 is the stick Kosmos booted from**, and it needed the second try:
  Address Device had no answer within a second, and the device was addressed
  after a reset and a pause - `abcd:1234`, class 0, its product string `UDisk`
  padded with spaces.
- **Port 10 is `8087:0026`, class 224**, a wireless controller under Intel's
  vendor ID; `thinkpad.md` §0 lists Bluetooth among the machine's USB devices,
  so most likely that. An inference, not a lookup.
- **Five devices named on 00:14.0**, and both controllers by interrupt again.
- **The plug lines on metal**: the mouse pulled out of port 1 and put back,
  twice.

```
[37.501] xhci: 00:14.0 port 1: unplugged, 04d9:fc38 "USB Gaming Mouse"
[44.850] xhci: 00:14.0 port 1, USB 2: a Full-speed device (speed ID 1), after its reset
[44.862] xhci: 00:14.0 port 1: GET_DESCRIPTOR for 8 bytes failed: USB Transaction Error (4)
[44.874] xhci: 00:14.0 port 1: the device did not say what it is
[134.186] xhci: 00:14.0 port 1: unplugged
[150.794] xhci: 00:14.0 port 1, USB 2: a Full-speed device (speed ID 1), after its reset
[151.807] xhci: 00:14.0 port 1: 04d9:fc38, USB 2.0, class 0, "USB Gaming Mouse"
```

**Every change was seen, and a replug is not yet reliable.** The first time the
mouse came back, its first request failed 12 ms after the line saying its port
was reset; the second time those two lines are a second apart, and it was
named. On every boot photographed the same mouse was named on the same port.
The first attempt deliberately has no wait (above), so the likeliest reading is
a device asked before its reset recovery was over - a reading, not a
measurement. USB 2.0 came into the references with the mouse, and step 3
gives every device the intervals it is owed (§5); whether their absence was
the fault is the ThinkPad's to say.

### What QEMU cannot show

- **Scratchpad pages, 64-byte contexts, and a controller that addresses only
  32 bits.** QEMU's asks for no scratchpads (HCSPARAMS2 reads 0Fh), has CSZ
  clear and AC64 set. All three paths are written, and the "runs:" line says
  which apply. The ThinkPad has run the first: both its controllers asked for
  34 pages. Both use 32-byte contexts, so the second has run nowhere.
- **A second attempt at an address.** QEMU addresses every device the first
  time, so the retry, and the slots it gives back, are read and not run. The
  ThinkPad's ports 7 and 10 are where they first run.
- **A SuperSpeed device pulled out.** The check pulls the keyboard, a USB 2
  device, and the stick stays in.
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

**Then a keyboard is pulled out and put back** through QEMU's monitor, on
the port it left, once for every slot its controller enabled - eight - and
each time must bring exactly one unplug line and one named keyboard on that
port. **Eight, because a replug alone cannot catch a driver that keeps its
slots**: QEMU forgets a slot's port when its device leaves, and that driver
passed two rounds. What QEMU keeps is the slot enabled, so the same driver
now runs out on the eighth. 17 checks, with two negative controls in
`testing.md` §18.38.

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

## 5. Step 3: a mouse

**Diego's ThinkPad has a TrackPoint and a USB mouse, and the mouse did
nothing on the desktop.** On 13 September his call was the mouse before bulk
transfers: "yes / lets make the mouse work". It is the first time the driver
talks to a device after naming it, and the first time anything goes round a
ring.

### The pointer: relative devices add

The window manager asks one question, `hal_pointer_poll`, and a PC had one
answer to it: a virtio tablet under QEMU, or the TrackPoint on the i8042's
auxiliary port. `hal.h` said pointing devices are not merged, because a
position has one source and a second would be a second opinion to choose
between.

**That is true of a tablet, and not of a mouse.** A tablet says where it is;
a TrackPoint and a mouse say how far they moved, and two of those are only
more movement. So the position, its range and its speed moved out of
`i8042.c` into the board, `hal/pc/pointer.c`, and every relative device adds
into it:

- **each source holds its own buttons**, and the pointer reports them
  together, so a TrackPoint packet with nothing held cannot release a button
  the mouse is holding;
- **down is positive**, USB's way round - HID 1.11 5.9 has a report's values
  increase "from far to near" - and the i8042 turns PS/2's count over before
  calling in;
- **the speed is one number for both**, `pointer` at the prompt, which is a
  follow-up in `roadmap.md`.

**A driver in a process reaches it through `SYS_POINTER_MOVE (dx, dy,
buttons)`**, and the kernel the board through `hal_pointer_move`, which a
board whose pointer is a tablet refuses. The call is gated on device
authority, because a process able to move the pointer and press its buttons
can click anything on the screen; it clamps each count to fifteen bits; and
it wakes whoever is asleep waiting for input, which the i8042's interrupt
does for its own packets in the trap handler. `syscall.h` has why a report
is a call and not a shared region - the question `CLAUDE.md`'s rule about
streams asks. **And when that process ends** - killed, faulted, or gone
between a press and its release - the kernel reports no movement and no
buttons for it (`process_exit`), so a button it held comes up. The window
manager did not change.

### The intervals a device is owed

USB 2.0 came into the references with the mouse, and with it three waits the
driver had never given:

| interval | USB 2.0 | when |
| -------- | ------- | ---- |
| 100 ms | 7.1.7.3, TATTDB | after a device is plugged in, before its reset; a disconnect starts it again |
| 10 ms | 7.1.7.5, 9.2.6.2, TRSTRCY | after a port's reset, before the first request |
| 2 ms | 9.2.6.3 | after SET_ADDRESS - which Address Device sends, and whose timing xHCI 4.6.5 leaves to software - before a request to the new address |

**The debounce is only for a plug**: a device found at boot has been in its
socket since before the firmware ran. A connection that drops during the
interval starts it again, five times at most, and each drop's change is
cleared so the watch does not see it twice. The mouse's failed replug on the
ThinkPad (§4) came 12 ms after its reset with none of these given; whether
that was the fault is the ThinkPad's to say.

### Choosing a mouse

Once a device has said what it is, **its configuration is asked for** - nine
bytes for the total, then all of it - and walked by
`user/servers/usb_decode.c`. That file has no hardware and no system calls in
it, so a host test can hand it what QEMU never sends: a length of zero, a
descriptor past the end, a total longer than what arrived. The walk steps by
each descriptor's own length (USB 2.0 9.5) and refuses one that does not add
up.

**One kind is taken: a HID boot mouse** - interface class 3, subclass 1,
protocol 2 (HID 1.11 4.1 to 4.3) - at alternate setting 0, with an interrupt
IN endpoint. Its HID descriptor, between the interface and the endpoint
(7.1), gives the length of its Report descriptor (6.2.1), which is kept. Any
other HID interface is said, with its subclass and protocol, and left alone. A SuperSpeed mouse is said and not read: its
endpoint's largest payload an interval comes from a companion descriptor this
does not walk (xHCI 4.14.2).

Then **the controller before the device**, the order xHCI 4.3.5 gives,
because a SET_CONFIGURATION after a Configure Endpoint that failed is
undefined:

1. **Configure Endpoint** (4.6.6), with an input context that adds the slot
   and the endpoint and drops nothing; the slot's Context Entries raised to
   the endpoint's index (6.2.2.2); and the endpoint as 4.8.2.4 describes an
   interrupt IN one - three errors allowed, its packet and extra
   transactions, its interval, its ring with the cycle bit at 1, its largest
   payload an interval, and an average TRB length that is its one request's
   (Tables 6-8 and 6-9). **The interval is translated** (6.2.3.6, Table
   6-12): a full- or low-speed device gives milliseconds, rounded down to a
   power of two in 125 µs steps, and a high-speed one gives the power
   already, plus one.
2. **SET_CONFIGURATION** (USB 2.0 9.4.7), a request with no data stage: in
   xHCI a Setup stage of transfer type 0 and a Status stage that is IN
   (Table 4-7).
3. **Its Report descriptor**, asked of the interface - GET_DESCRIPTOR with
   request type 10000001, type 0x22 in wValue and the interface in wIndex
   (HID 1.11 7.1.1) - printed a byte at a time, and laid out by
   `usb_decode_mouse_report` (below).
4. **SET_PROTOCOL** (HID 1.11 7.2.6): the report protocol when the descriptor
   lays out relative X and Y in a report that fits one packet, and the boot
   protocol otherwise, with a line saying why. The host sets it either way
   rather than assume. **SET_IDLE is not sent**: a boot mouse need not
   support it (Appendix G), and a mouse's idle rate starts at infinity - a
   report only when something changes (7.2.4).

### Its Report descriptor, because a boot mouse need not honour the boot protocol

**This used to ask for the boot protocol and read every report as B.2 lays
one out**: a byte of buttons, a byte of X, a byte of Y. That is what a boot
mouse is required to send once asked, and it is what QEMU's sends. **The
ThinkPad's 04d9:fc38 "USB Gaming Mouse" took the request without an error
and kept sending its own reports**: moved right, the arrow went down; left,
up; up and down, nothing; and it never left the middle of the screen
sideways. Its first report said `buttons 0, moved 0,-1`. A byte 0 of zero is
no Report ID (6.2.2.7 reserves 0), so the second byte must be something that
is zero while the mouse moves - the upper eight of sixteen buttons being the
likeliest - with X third and Y fourth. Linux, Windows and macOS never meet
this, because they never use the boot protocol: they read the Report
descriptor and do what it says.

So does `usb_decode_mouse_report` now, and it is the whole of the change:

- **Items** (6.2.2.2): a prefix of tag, type and size, then 0, 1, 2 or 4
  bytes, low byte first. A long item (6.2.2.3) is stepped over.
- **Globals** (6.2.2.7) hold until changed - Usage Page, Logical Minimum,
  Report Size, Report Count, Report ID - and Push and Pop keep four of them.
  **Locals** (6.2.2.8) - Usage, Usage Minimum and Maximum - end at the next
  Main item, and a usage of one or two bytes takes the page in force at that
  Main item; a four-byte one carries its own.
- **An Input item** (6.2.2.4) adds Report Size bits Report Count times. A
  constant or an array one is stepped over; a variable one takes its usages a
  field at a time, the last going on to any fields past them. X and Y are
  Generic Desktop 0x30 and 0x31 and a button is the Button page, 0x09 - the
  values E.10 encodes them with - and a field is signed when its Logical
  Minimum is negative (5.8).
- **Report IDs**: once one appears every report starts with its ID, so
  offsets are kept for each ID, and the mouse is the first report with both
  X and Y. A report under another ID - a consumer control's volume keys, say -
  is skipped when it arrives.
- **Refused rather than guessed**: an item running past the end, a Pop with
  nothing pushed or a Push too deep, ID 0, a report over 65535 bits, and X or
  Y that is absolute - a tablet, which the pointer takes from no driver - or
  wider than 32 bits.

Buttons 1 to 3 go to the pointer in the boot report's order - primary,
secondary, tertiary - and a field is read least significant bit first, bit 0
of byte 0 upwards (5.8).

### Reading reports

**One request on the ring at a time**: a Normal TRB over the mouse's report
page, as long as its packet, that interrupts when it completes and when a
report comes back short (6.4.1.1), and the doorbell with the endpoint's
context index as its target. A Transfer Event says how many bytes did not
arrive (Table 6-38), and a report is read by its descriptor's layout - after
its Report ID, when it has one - or, for a mouse read as a boot mouse, as B.2
lays it out: the buttons, then X and Y as signed bytes. The movement and the
buttons go to the pointer when there is something new in them, and the next
request goes on the ring whatever there was. The first report is said.

- **A report that arrives while the driver waits for a command or a control
  transfer is kept, not passed over.** Passed over, its mouse would have no
  request on its ring and would never send another. There are never more
  kept than there are mice, since each has one request out.
- **A mouse pulled out stops being read before its slot goes, and a button it
  was holding is let go** - or the desktop would drag until something else
  pressed and released that button. QEMU sends no release for a device it
  deletes, which is the case the check makes.
- **A report that failed stops the mouse**, with a line. Its endpoint is
  halted (4.10.2.1, 4.10.2.3), and bringing it back is a Reset Endpoint, a
  CLEAR_FEATURE and a new dequeue pointer - none of which QEMU's mouse can be
  made to need, so none of which is written. Plugged in again, it is read
  again. `roadmap.md` has it.
- **The ring goes round.** A ring is 255 requests and a Link back to its
  start (§2), and the mouse is the first thing to reach the Link - two seconds
  after it starts moving - so the check sends past two rounds before it
  clicks.

### Waiting on every controller at once

**The watch waited on each controller in turn, 50 ms apiece**, which a plug
can afford and a mouse cannot: a mouse on the second of the ThinkPad's two
controllers would have its reports sit behind the first's wait and reach the
pointer in bursts. The kernel gained `SYS_IRQ_WAIT_ANY` for it
(`drivers.md` §4), and the watch waits on every running controller's
interrupt at once, then looks at all of them; since step 5c the same wait
can take an endpoint too (§7). A controller whose
interrupt could not be claimed is looked at when the wait's deadline comes
round.

**Found while testing it: under `opt/kosmos/irq=pic`, QEMU's two controllers
share line 11.** The second's claim is refused, its line says "not claimed,
so polled", and the shared line wakes the driver for both - so on the 8259s
the wait on two lines never runs. That is a machine on the legacy path. The
ThinkPad's ACPI describes an I/O APIC, its controllers each have an MSI of
their own (§4), and the check boots the same way.

What QEMU prints, with the mouse on the second controller - its Report
descriptor byte for byte the one in `hw/usb/dev-hid.c`, and the layout read
from it - moved, clicked, held and pulled out, and plugged back in at full
speed:

```
xhci: 00:02.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 20
xhci: 00:03.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 21
xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3), after its reset
xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Mouse"
xhci: 00:03.0 port 5: its Report descriptor, 52 bytes: 05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02 95 01 75 03 81 01
xhci: 00:03.0 port 5:   from byte 32: 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 c0 c0
xhci: 00:03.0 port 5: a mouse, read from endpoint 1, up to 4 bytes every 8 ms
xhci: 00:03.0 port 5: its reports, by its descriptor: 5 buttons from bit 0, X from bit 8 in 8, Y from bit 16 in 8, no report ID
xhci: 2 controllers (00:02.0, 00:03.0), 1 port with something plugged in, 1 device named
xhci: watching for devices plugged in and out
xhci: 00:03.0 port 5: the mouse's first report: buttons 0, moved 3,-2
wm: button down at 965,537 raw=1
wm: button up at 965,537 raw=0
wm: button down at 965,537 raw=1
xhci: 00:03.0 port 5: unplugged, 0627:0001 "QEMU USB Mouse", after 4 reports
wm: button up at 965,537 raw=0
xhci: 00:03.0 port 5, USB 2: a Full-speed device (speed ID 1), after its reset
xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Mouse"
xhci: 00:03.0 port 5: its Report descriptor, 52 bytes: 05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02 95 01 75 03 81 01
xhci: 00:03.0 port 5:   from byte 32: 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 c0 c0
xhci: 00:03.0 port 5: a mouse, read from endpoint 1, up to 4 bytes every 8 ms
xhci: 00:03.0 port 5: its reports, by its descriptor: 5 buttons from bit 0, X from bit 8 in 8, Y from bit 16 in 8, no report ID
```

The last `button up` is the driver letting go: nothing released the button
in QEMU.

### A plug does not hold a mouse

**Naming a device held every mouse until it was done.** The driver has one
thread, and a plug's waits were that thread's: USB 2.0's debounce, the port's
reset and its recovery, and one command or control transfer at a time, each
waiting on its own controller's interrupt and keeping any report that came
meanwhile to read afterwards. A mouse's next request goes on its ring only
when its report is read, so it sent nothing until the plug was done - on
either controller. In QEMU's trace, with the mouse moved every 10 ms and a
keyboard plugged in, the longest gap between two of the mouse's requests
went from 12 ms to 107 with the keyboard on the mouse's own controller and 104
on the other: the debounce, since QEMU answers everything else at once. On
the ThinkPad, the stick that dropped off its bus on 14 September took 2.3
seconds to be named again.

**So every one of those waits reads mice** (`wait_serving`): it waits on
every running controller's interrupt, reads each mouse's report as it comes,
and answers the waiter only with an event that is not one. The same trace
gave 12.5 ms and 11.4. What it does not do is attach or detach: a port that
changes meanwhile keeps its change bits for the watch, so nothing re-enters.

**It is not enumeration rewritten as steps**, which is what `roadmap.md` had
proposed: the sequence stays a sequence, and a failure is still said where it
happened. Two consequences are worth writing down. **The deadline is the
counter's** (`counter_hz`, from `sysinfo`), because a wait counted in wakes
would let a mouse's thousand interrupts a second use up a command's second;
and **the kept reports are gone**, since nothing waits without reading them.

### What QEMU cannot show

- **A full-speed mouse whose endpoint 0 is bigger than 8 bytes.** QEMU's
  mouse attaches at full speed when asked to (`usb_version=1`), and the check
  plugs it back in that way, so turning milliseconds into an interval runs
  here; but its endpoint 0 is 8 bytes, so Evaluate Context has still run
  nowhere.
- **A mouse that keeps sending its own reports after SET_PROTOCOL(boot)**,
  which the ThinkPad's does. QEMU's honours the request and its report is
  its boot report, so the clicks and movements here pass whichever way it is
  read: the line naming the layout is what says the descriptor was read, and
  `test_usbdecode.c` has the layouts QEMU cannot send.
- **A mouse that refuses SET_PROTOCOL**, or GET_DESCRIPTOR for its Report
  descriptor. QEMU's answers both; the line would name the step and the code,
  and a STALL on endpoint 0 is not recovered from (`roadmap.md`).
- **A halted endpoint**, above.
- **Contacts that bounce.** QEMU's plug is one clean change, so the debounce
  is always a single interval.
- **A thousand reports a second**, which a gaming mouse may send.
- **A command or a transfer that takes time.** QEMU answers at once, so the
  check measures what a plug's debounce cost a mouse and nothing else a plug
  waits for. The commands wait through the same `wait_serving`, and the
  ThinkPad's 2.3 seconds are what they can cost.
- **A controller that takes a 64-bit register when its high half arrives.**
  QEMU acts on ERDP's low half, so a driver writing the halves in the wrong
  order works perfectly here; the order is read out of QEMU's trace instead
  (§2).

### How it is tested

- **`tools/test_usbdecode.c`, 56 checks** on the host: QEMU's mouse as its
  device model declares it, HID 1.11 Appendix E's keyboard and mouse (the
  mouse behind the keyboard, and not the keyboard's endpoint, nor its Report
  descriptor's length), a stick, and every length a device can get wrong -
  each of which must end the walk. **And Report descriptors**: E.10's and
  QEMU's, a sixteen-button layout made to match what the ThinkPad's mouse
  did, Report IDs beside a consumer control and behind a keyboard, a Usage
  Page given after its usages, signedness from the Logical Minimum, and the
  descriptors to refuse (`testing.md` §18.46).
- **The suite, on both boards**: `irq: a wait on two lines takes whichever
  has one`, and `input: a driver's movement adds to the pointer` - which on
  the ARM board is the refusal, since its pointer is a tablet.
- **`tools/run_x86.py`'s `usb_mouse`**, 22 checks, on q35 with two
  controllers and QEMU's mouse on the second: the driver reads it there, by
  the layout its Report descriptor gives, on two interrupts; 640 movements a little over 10 ms apart, and at least four
  in five come back as reports of their own - QEMU folds a movement into the
  one before while that is unread, so a driver reading late reads fewer - and
  more than two rounds of a ring; then a click on the Deskbar's button opens
  its menu, through USB alone; a button held as the mouse is pulled out comes
  up, and the line it leaves counts no more than one report in ten found by
  looking rather than brought by the controller's interrupt; and a full-speed
  mouse plugged back in is read every 8 ms, from its first report. **And a
  keyboard plugged in while it moves**, into the other controller and then
  into its own: each named, and no gap over 50 ms between two of the mouse's
  requests in the second and a half after, read out of QEMU's trace.

The controls, each watched fail, are in `testing.md` §18.43, §18.46, §18.47
and §18.53.

### On the ThinkPad

**0.10.62, 13 September**: the driver named Diego's mouse - `04d9:fc38, USB
2.0, class 0, "USB Gaming Mouse"`, full speed, on `00:14.0` port 1 - and read
it, `a boot mouse, read from endpoint 1, up to 8 bytes every 1 ms`, first
report `buttons 0, moved 0,-1`. **With its axes wrong**: sideways moved the
arrow up and down, and up and down did nothing - the boot protocol asked for
and not given, above. The TrackPoint and the touchpad, through the board's
merge, worked "great".

**The Report descriptor version, `a543f20`, 13 September**, with the mouse in
port 7 at boot and moved to port 1 after 25 minutes. Its descriptor, 67
bytes, lays out sixteen buttons in two bytes, then X and Y in sixteen bits
each from -32767 to 32767, a wheel and a horizontal pan - eight bytes, no
Report ID - and the driver read it so:

```
[1507.858] xhci: 00:14.0 port 1, USB 2: a Full-speed device (speed ID 1), after its reset
[1509.874] xhci: 00:14.0 port 1: 04d9:fc38, USB 2.0, class 0, "USB Gaming Mouse"
[1511.881] xhci: 00:14.0 port 1: its Report descriptor, 67 bytes: 05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 10 15 00 25 01 75 01 95 10 81 02 05 01 09 30 09 31
[1511.893] xhci: 00:14.0 port 1:   from byte 32: 16 01 80 26 ff 7f 75 10 95 02 81 06 09 38 15 81 25 7f 75 08 95 01 81 06 05 0c 0a 38 02 95 01 81
[1511.893] xhci: 00:14.0 port 1:   from byte 64: 06 c0 c0
[1512.900] xhci: 00:14.0 port 1: a mouse, read from endpoint 1, up to 8 bytes every 1 ms
[1512.913] xhci: 00:14.0 port 1: its reports, by its descriptor: 16 buttons from bit 0, X from bit 16 in 16, Y from bit 32 in 16, no report ID
[1513.177] xhci: 00:14.0 port 1: the mouse's first report: buttons 0, moved 207,-391
```

**The axes were right, and the mouse was not.** Diego: "really jumpy and
slow. the trackpad is perfectly smooth though", and then "the mouse feels
like the kernel is reading the mouse coordinates in intervals of 20ms". The
same log says why, three ways:

- **882 reports in 25 minutes** on port 7, for clicks, drags and a Deskbar
  menu - the clicks with no `i8042` line beside them, which the i8042 prints
  for its first twenty button changes - from a mouse that sends one a
  millisecond while it moves.
- **Each step of naming it took a second, or two**: 1507.858 its reset,
  1509.874 named, 1511.881 its Report descriptor, 1512.900 read - and 43.327
  to 44.335 on port 7. A second is the driver's deadline for an answer that
  comes by interrupt; when it passes, the driver looks at the ring and finds
  the answer there.
- **Its first reports moved hundreds of counts** at once, `-446,102` and
  `207,-391`: movement kept in the mouse because nobody had asked for it.

So the answers were on the ring and their interrupts did not come, and a
mouse's reports would be read only when the watch's 50 ms deadline came
round. **The reading is the ERDP order** (§2): the driver wrote the high half
first, and a controller that takes the register when its high half arrives
clears Event Handler Busy one write late - so once events were taken, Busy
stayed set and no interrupt came for the next ones. It fits every line
above, and the No-Op answered by interrupt on 0.10.54 too, which came while
Busy was still clear.

**Ruled out under QEMU first**, with scratch copies of the mouse check:
plain MSI, as that machine has, rather than QEMU's MSI-X (`msix=off,msi=on`),
and one, four and eight processors - every run passed, and those that
printed the count read 658 to 668 reports for 640 movements. And the four
MSI vectors do not run out on the stick's boot: its disk is in memory, so
the NVMe driver does not start, and audio and the two controllers take at
most three.

**The fix writes ERDP low half first**, and the line when a mouse leaves
counts the reports found by looking rather than brought by their
controller's interrupt, so the next photograph says whether the reading was
right: naming steps in milliseconds, and almost no report found by looking.

**And it was right.** The stick built from the fix booted the ThinkPad on the
morning of 14 September, and Diego: "Mouse works perfectly now!" - the mouse,
the touchpad and the TrackPoint together. Its `log xhci`, with the mouse
pulled out 201 seconds into the boot:

- **13736 reports, 0 found by looking**: every one brought by the
  controller's interrupt, where the build before read 882 in 25 minutes.
- **Each step of naming it milliseconds apart**: 4.670 its reset, 4.686
  named, 4.698 its Report descriptor, 4.709 read.
- **Its last report failed as it left** - `USB Transaction Error (4)` twelve
  milliseconds before the unplug line - which is a request that was out when
  the device went, not a failure while it was there.

---

## 6. Step 4: bulk transfers

**Bytes each way on a stick's two bulk endpoints**, and a stick to prove it
with: nothing on a stick's bulk endpoints means anything except Bulk-Only
Transport, so the smallest real exchange is one command carried by it. Diego's
word, on 14 September, once a plug stopped holding the mouse: "then start bulk
transfers".

### What a stick declares

**A configuration walked for a stick as well as a mouse** (`usb_decode.c`): an
interface of class 08h, subclass 06h and protocol 50h - mass storage, SCSI
transparent, Bulk-Only (Bulk-Only 1.0 Table 4.5; the Mass Storage Overview 1.4,
Tables 1 and 2) - at alternate setting 0, with a bulk IN and a bulk OUT
endpoint (4.4). At SuperSpeed each endpoint is followed by a SuperSpeed
Endpoint Companion, whose `bMaxBurst` is the burst the controller is told;
its layout is the one xHCI 1.2 gives its own Debug Capability (Table 7-37).
A mouse is still taken first, then a stick, and mass storage that is neither
is said: USB Attached SCSI, another subclass, an interface short of an
endpoint, or a burst past fifteen.

### The endpoints

**One Configure Endpoint for both** (4.8.2.3): bulk OUT at context index twice
its number and bulk IN at one more (4.5.1), Bulk Out and Bulk In from Table
6-9, three errors allowed, the packet and the burst, no streams, and an
Average TRB Length of three kilobytes, which 4.14.1 gives as a reasonable
first value for a bulk endpoint. Then SET_CONFIGURATION, in the order a mouse
takes. The two rings are the device's pages 4 and 5 - a mouse's ring and
reports otherwise - and what is sent and received goes in the device's buffer
page, which enumeration has finished with.

**A transfer is one Normal TRB** (6.4.1.1): its length, interrupt on
completion and on a short IN, the endpoint's doorbell, and the Transfer Event
for that TRB, which says how much was left. It waits through `wait_serving`,
so a stick answering slowly holds no mouse.

### INQUIRY, through Bulk-Only Transport

**A 31-byte wrapper out, 36 bytes in, and a 13-byte status in** (Bulk-Only 1.0
5.1 to 5.3): the wrapper with its signature, a tag, the length expected, the
direction, LUN 0, and INQUIRY's six bytes - operation code 12h and an
allocation length of 36 (SPC, as Seagate's reference gives it, Table 58). The
status is held to what 6.3 asks of a host: thirteen bytes, its signature, the
same tag, a residue no larger than asked for. And the standard data's first
36 bytes say what the stick is: the device type in byte 0, then vendor,
product and revision (Table 59).

What QEMU's stick makes the driver say, at SuperSpeed on the second
controller:

```
xhci: 00:04.0 port 1, USB 3: a SuperSpeed device (speed ID 4)
xhci: 00:04.0 port 1: 46f4:0001, USB 3.0, class 0, "QEMU USB HARDDRIVE"
xhci: 00:04.0 port 1: a stick: SCSI over Bulk-Only, bulk IN endpoint 1 and OUT endpoint 2, up to 1024 bytes a packet in bursts of 16
xhci: 00:04.0 port 1: the stick says it is "QEMU" "QEMU HARDDISK", revision "2.5+", device type 0
```

"QEMU", "QEMU HARDDISK" and the revision come from `hw/scsi/scsi-disk.c`, and
the endpoints from `hw/usb/dev-storage.c`.

### What is not done yet

- **Recovery.** A stall on either bulk endpoint, or a status that is not
  valid, is what Bulk-Only 1.0 answers with a Reset Recovery - the class
  reset, then CLEAR_FEATURE on both endpoints (5.3.4). The driver says which
  step failed and leaves the stick; `roadmap.md` has it.
- **LUN 0 only.** Get Max LUN is not asked, because a stick with one unit may
  stall it (3.2) and a stall on endpoint 0 is not recovered from either.
- **No streams**, which Bulk-Only does not use; USB Attached SCSI does.

### What QEMU cannot show

- **A stick that stalls, is slow, or answers wrongly.** QEMU's answers every
  command at once and well.
- **A stick at high speed on a real controller**: here it is SuperSpeed, on a
  USB 3 port, and the decoder's high-speed case is its host test.

### How it is tested

- **`tools/test_usbdecode.c`**, 71 checks, 15 of them new: a high-speed stick,
  QEMU's SuperSpeed one with its companions, bursts given to the right
  endpoint and one past fifteen refused, OUT before IN, USB Attached SCSI,
  subclass 00h, alternate setting 1, an interrupt endpoint, endpoint 0, two
  INs, a packet size of 0, a keyboard with a stick behind it, a mouse with a
  stick behind it, and a hub's interface.
- **`tools/run_x86.py`'s `usb`**, 15 checks, 2 of them new: the stick's
  endpoints as QEMU declares them, and INQUIRY answered through them as QEMU's
  disk answers it. Controls in `testing.md` §18.54.

---

## 7. Step 5: mass storage

**The stick Kosmos booted from, mounted as its disk**, in six parts that each
end in something visible. `roadmap.md` has the list, and `README.md` the five
calls Diego approved under it on 14 September. This section grows a part at a
time.

### 5a: the stick's size, and its first blocks

**Ready, how big, and two blocks read.** After INQUIRY the driver asks TEST
UNIT READY until the stick is ready, READ CAPACITY (10) for its size, and READ
(10) for block 1 and for the last block, and asks of each whether it holds a
GUID partition table's header. A stick made by `mkusb_image.py` holds one at
each end, and that table is what a disk server will find Kosmos's partition
by.

**Laid out and read in `storage_decode.c`**, with no hardware in it: the
command and status wrappers, the command blocks, READ CAPACITY's answer, sense
data and a GPT header. The driver's `inquire` became `transact`, which carries
any command: its wrapper out, its data in - copied out of the buffer page
before the status comes back through the same page - and the status held to
6.3, valid first and then meaningful. A status of 01h is not the transport
failing. It is the stick saying the command did, and REQUEST SENSE says why.

**TEST UNIT READY, and why it may fail at first.** A device reports a unit
attention after a reset, and the driver has just reset the stick's port; a
real stick may also say NOT READY while it wakes. So a failure is asked why
with REQUEST SENSE, which also clears what it reports (Seagate's REQUEST SENSE,
3.37), a stick that says NOT READY is given 250 ms, and the command is tried up
to eight times. What the stick said is printed, first and last. Sense data is
read in both formats: fixed, which is what is asked for, and descriptor, which
a device may send anyway (Tables 27 and 28).

**READ CAPACITY (10)** answers the last block's address and the block's size,
big-endian (Table 120). FFFFFFFFh means more blocks than it can count - about
2 TB - which READ CAPACITY (16) would answer, and nothing here asks it yet.
**READ (10)** takes a 32-bit address and a 16-bit count (Table 97). One block
at a time for now, through the 4 KB buffer page, so a block larger than a page
is said and not read; the 256 KB transfer buffer is 5d's.

**A GPT header** is held to its signature, a header size from 92 bytes to the
block's, its CRC-32 over that size with its own field taken as zero, and MyLBA
equal to the block it was read from - so a backup read from the wrong block
does not pass. Those are the UEFI specification's checks as `mkusb_image.py`
already writes to them, for sticks OVMF and the ThinkPad's firmware both
boot; a copy of the specification was not downloaded for this.

What QEMU's stick makes the driver say:

```
xhci: 00:04.0 port 1: the stick says it is "QEMU" "QEMU HARDDISK", revision "2.5+", device type 0
xhci: 00:04.0 port 1: the stick holds 32768 blocks of 512 bytes, 16 MB
xhci: 00:04.0 port 1: block 1 holds a GUID partition table's header, and block 32767 its backup
```

QEMU's stick passes the first TEST UNIT READY, so the line saying what a stick
answered does not appear here. On the ThinkPad it may.

### What QEMU cannot show

- **A stick that is not ready, or reports a unit attention.** The sense paths
  are `test_storagedecode`'s.
- **Blocks of 4096 bytes, and a stick past 2 TB.**
- **A stick that stalls a command it does not support**, which is 5b.

### How it is tested

- **`tools/test_storagedecode.c`**, 48 checks: INQUIRY's wrapper byte for byte
  as Table 5.1 lays it out, one with no data, one for LUN 3, and the lengths a
  wrapper has no room for; each command block; statuses that pass, fail, end
  in a phase error with any residue, are twelve bytes, carry the wrong
  signature or tag, a residue past the length or a status of 03h; QEMU's
  capacity for 16 MB, 4096-byte blocks, a count past 32 bits, FFFFFFFFh, seven
  bytes and a block of no bytes; sense in fixed format as QEMU sends it and
  for a unit attention, with VALID set, deferred, short and with an additional
  length that stops short, and in descriptor format; the sense keys' names;
  CRC-32's check value, and carried across two calls; and GPT headers written
  by Python's `zlib`, at their own block and the wrong one, changed by a bit,
  sized to the whole block, 91 bytes, larger than the block, with a lowercase
  signature, and empty.
- **`tools/run_x86.py`'s `usb`**, 17 checks, 2 of them new: its stick is laid
  out by `mkusb_image.write_gpt` as a real one is, and the driver has to say it
  holds 32768 blocks of 512 bytes and find the header at block 1 and its
  backup at block 32767. Controls in `testing.md` §18.56.

### 5b: Reset Recovery

**A command that goes wrong is recovered from, and sent again once.** What
Bulk-Only 1.0 asks of a host after a stall, a status that is not valid, or a
phase error (6.4 to 6.6) - and done here after a transfer that never answered
as well, because the other choice is a stick left in the middle of a
command: the class reset to the stick's interface (3.1), then the halt
cleared on bulk IN and then on bulk OUT (5.3.4).

**Clearing a halt has two halves.** The stick's is CLEAR_FEATURE with
ENDPOINT_HALT, to the endpoint's address (USB 2.0 9.4.1, Table 9-6). The
controller's is xHCI's "reset a pipe" (4.6.8), which puts that request in the
middle: Reset Endpoint, which takes a Halted endpoint to Stopped; the
CLEAR_FEATURE; then Set TR Dequeue Pointer (4.6.10), which moves the
controller past the TRB that stalled to where the next one will go, with the
cycle bit it will carry. Without that last command a doorbell tries the
stalled transfer again. The endpoint that did not halt - usually the other
one - refuses Reset Endpoint with a Context State Error and is stopped with
Stop Endpoint instead (4.6.9), so both are Stopped before their dequeue
pointers move.

**Then the command once more**; a second failure is said, and the stick left.
One stall is a line in the log rather than a stick unused. A command the stick
answers with a failed status is not sent again, because that is the stick
working: REQUEST SENSE says why, now after any command rather than only TEST
UNIT READY.

**A fault on request, because QEMU has none.** QEMU's stick stalls nothing a
driver sends it well, and nothing in QEMU makes it misbehave; a wrapper with
the wrong signature it stalls at once (`hw/usb/dev-storage.c`). So a machine
started with `opt/kosmos/stickfault=signature` sends each stick's first
wrapper with its signature's first byte turned over, and the driver says so
on a line of its own, so a log with that stall in it also says why. It is
the one fault this driver makes when asked (`README.md`).

What QEMU's stick makes the driver say, started that way:

```
xhci: 00:04.0 port 1: its first command goes out with a wrong signature, as opt/kosmos/stickfault asks
xhci: 00:04.0 port 1: the INQUIRY's command failed: Stall Error (6), so the stick is reset
xhci: 00:04.0 port 1: the stick is reset, and the INQUIRY sent again
xhci: 00:04.0 port 1: the stick says it is "QEMU" "QEMU HARDDISK", revision "2.5+", device type 0
xhci: 00:04.0 port 1: the stick holds 32768 blocks of 512 bytes, 16 MB
xhci: 00:04.0 port 1: block 1 holds a GUID partition table's header, and block 32767 its backup
```

### What QEMU cannot show

- **The stick's half.** QEMU's stick answers CLEAR_FEATURE and does nothing
  with it, and its class reset only returns it to waiting for a wrapper -
  which after a bad signature it already is (`dev-storage.c`). What QEMU
  shows is the controller's half; whether a real stick's halt is cleared,
  only a real stick can say.
- **A stall on endpoint 0**, which the class reset or a CLEAR_FEATURE could
  meet. Endpoint 0 is still not recovered (`roadmap.md`).
- **A command the stick fails, in a permanent check.** QEMU's stick fails
  nothing this driver sends it; the line that says why is shown by a control
  in `testing.md` §18.57, and the sense it reads is `test_storagedecode`'s.

### How it is tested

- **`tools/run_x86.py`'s `usb`**, 19 checks, 2 of them new: started with
  `opt/kosmos/stickfault=signature`, the driver has to say it spoiled the
  first wrapper, meet the stall with Reset Recovery, and send INQUIRY again -
  so the checks before them, INQUIRY's answer, the size and both GPT headers,
  all pass on a stick that was recovered. `usb_hotplug` and `usb_mouse` run
  without the fault. Controls in `testing.md` §18.57.

### 5c: one wait for interrupts and callers

**The first of the five calls** (`README.md`): a driver with clients of its
own waits for them on the same wait as its interrupt lines. The xHCI driver
is one thread, waiting on every controller's interrupt so that no mouse
waits behind anything; a request from a disk server, from 5d on, has to reach
that same thread without it looking at its endpoint between interrupts -
which on an idle machine would make every block wait out a nap.

**`SYS_IRQ_WAIT_ANY` takes endpoints**, as its fourth argument and - since
storage at full speed - its fifth, each -1 for none. A caller queued on the
first answers `IRQ_WAIT_CALLER` and on the second `IRQ_WAIT_CALLER + 1`, never
a line's place in the array, and the driver collects the message with a
receive that does not block. **A line with an interrupt is answered before a
caller**, and the caller on the next wait, at once - so a stream of requests
cannot hold off a mouse.

**Two locks, and no wake lost between them** (`kernel/irq.c`). The endpoint
belongs to `ipc.c`, under its own lock, and the lines to `irq.c`, under
theirs. Each round takes the endpoint's lock and then the lines', looks at
both, and records the thread as the lines' waiter and the endpoint's watcher
with both held. It then lets the endpoint's go - into the masked state,
since the lines' is still held - and blocks releasing the lines'. A caller
wakes a watcher only under the lines' lock (`irq_wake_watcher`), so its wake
waits until the thread is blocked: a bare `thread_wake` does nothing to a
thread that has not blocked yet, and there it would be lost. Nothing takes
the two locks in the other order.

`ipc.c` exports four small things for it and for nothing else: an endpoint
locked by a capability, whether a caller is queued on it, and a watcher
recorded or taken off, each under that lock. The driver passes -1 on both of
its waits until 5d gives it an endpoint.

### How it is tested

- **The guest suite, on both boards**: `irq: a wait on lines and an endpoint
  takes a caller too`. With an interrupt pending and a caller queued, the
  line comes first and the caller at once on the next wait; a caller ten
  ticks into a two-second wait ends it long before its deadline, with the
  wait off both lines; and an interrupt ending another leaves it no longer
  the endpoint's watcher, so a second thread's watch is not refused.
  Controls in `testing.md` §18.58.
- **The USB checks**, `usb_mouse` among them, on the driver's new call.

### 5d: the block protocol, served by the driver

**A program reads a stick's blocks through the driver**, and `sticks` is the
program: each stick's size and names, then its GUID partition table - the
header at block 1, held to the block it says it is at, and the entries it
points to - all asked of the driver through `/dev/blocks`.

**The shape is `blockproto.h`**, declared as `audioproto.h` is: a 24-byte
request - operation, unit, first block, count, handle - and a 48-byte reply -
error, block size, blocks, count moved, handle, and the vendor and product
INQUIRY answered. Info, open, read and close; a write is refused. The driver
answers a request of any other length with `BLOCK_ERR_BAD_OP`, and
`/lib/blocks.lua` writes the layout a second time in Lua and asserts its
sizes when it loads, so a disagreement is loud.

**Control by message, data by shared memory** (the fifth call, `README.md`).
A client creates a region and hands it over once, with `BLOCK_OP_OPEN`; the
driver maps it and answers a handle whose low byte is its place and whose
other bits a generation, so a handle kept past its close, or guessed, names
nothing. A read goes to the stick through the stick's own transfer buffer - a
run the controller reaches - and is copied from there into the client's
region; a client's pages are never the controller's to write. Eight opens at
once. Every command's data now comes through that buffer, INQUIRY's
included, and every status through the device's page, so neither is written
over the other.

**One read moves at most 124 KB**, and the proposal was wrong to promise 512
sectors a command: a read is one Normal TRB, whose length is seventeen bits -
at most 131,071 bytes (xHCI 1.2 6.4.1.1). 124 KB is the largest whole number
of pages under that, and so of 512- and 4096-byte blocks; each stick's buffer
is 128 KB. Chaining TRBs would lift it, when a measurement says it matters.

**Checked before the stick is asked anything**: a unit that is ready, a handle
that names an open region, a count from one to what one read can move, and a
last block no further than the stick's - so a block past the end is refused
by the driver, by name, rather than failed by the stick. A read that goes
wrong at the stick goes through Reset Recovery and is sent again (5b), and
one the stick fails says why (`say_why_failed`).

**A unit is the number a stick is given as it becomes ready** - the next never
given out, and never given to another stick. That is 5e's correction: here it
was the Nth stick ready, counting controllers and then slots, which is a
position rather than a name (5e, below). A stick becomes a unit once its
size is known, keeps its own copy of its device from its first command -
`attach` holds the device in a variable of its own, and Reset Recovery
during a client's read goes on that device's endpoint 0 ring - and gives its
buffer back when it is unplugged.

**How it is wired.** init makes the endpoint and hands it to the driver as its
second capability; the driver waits for callers on the same wait as its
interrupts (5c) and serves every request waiting after each wake. The shell
is given it too and mounts it as `/dev/blocks`, and so does every program it
starts, as `/dev/audio` is. **Read only, and mounted for everybody for that
reason**: writing is given to one process, the disk server, on 5e's second
endpoint (`README.md`). A driver that finds no controller stays, and answers every
request with "no stick at that unit", as the audio and network servers answer
on a machine with no card: the endpoint is in every program's capability
list, and a destroyed one has the kernel refuse every spawn - which
`run_headless.py` caught the first time the driver ended by destroying it.

What `sticks` prints under QEMU, for the stick `mkusb_image.write_gpt` lays
out:

```
kosmos> sticks
unit 0: 32768 blocks of 512 bytes, "QEMU" "QEMU HARDDISK"
  partition 1: "KOSMOS", blocks 34 to 32734, type C12A7328-F81F-11D2-BA4B-00A0C93EC93B
```

### 5e: `/home` on a stick's Kosmos partition, read and written

**A machine started with `opt/kosmos/home=usb` keeps `/home` on a USB stick**:
the first partition of Kosmos's own type,
`8A9DC8A8-83CF-4F7F-962B-43157A68F14A`, on the first stick that has one.
Without the option nothing changes - the disk server's disk is the kernel's,
as it was - so every boot that exists keeps its one path, and this is a
second beside it. From 5f a stick names its own partition instead of `usb`.

**`kfs.lua` does not change, and cannot tell.** It reaches blocks through
`sys.disk`, `sys.disk_read` and `sys.disk_write` and nothing else, and `sys`
is a plain table - so the disk server replaces those three in its own process
with ones over the partition, as `tools/kfs.lua` replaces them with a file on
the Mac. A block number has the partition's first block added to it, and one
outside the partition is refused before the driver is asked.

**Found through `/dev/blocks`, written through an endpoint of its own.** The
disk server walks the units, reads each 512-byte stick's GPT header at block 1
and the entries it points to, and takes the first entry of the Kosmos type. A
server cannot tell its callers apart - it knows which endpoint a message came
in on and nothing else - so the right to write is a second endpoint: init makes
it and gives it to the driver and the disk server, and to nobody else. The
driver answers a write or a flush there, and refuses both on `/dev/blocks`.

**One wait, both endpoints.** The driver's wait watches the write endpoint
and `/dev/blocks` together, and a caller on either wakes it at once; after
every wake it answers the write endpoint and then `/dev/blocks`.

**It watched one, and that cost 17 requests a second.** `SYS_IRQ_WAIT_ANY`
took a single endpoint in 5c, and the write endpoint had it: the disk server
was the busy client, and `sticks` reads a handful of blocks when somebody types
it, so a read on `/dev/blocks` waiting for the watch's next deadline, 50 ms,
looked affordable. Disk Benchmark reads `/dev/blocks` continuously, and on the
ThinkPad `diskbench usb 0` gave 2.1 MB/s and 17 IOPS - 58 ms a request. QEMU
gave the same 17 on a stick whose `/home` read at 938, and that is what said
the stick was not the cost. The disk server's search for its partition asks
`/dev/blocks` too, four or five requests a look, and every one of them waited
the same way.

So the wait takes a second endpoint (`kernel/irq.c`): both locked in the order
of where they are, lower first - nothing else in the kernel holds two
endpoints' locks - one endpoint named twice refused, and a caller on the first
answering `IRQ_WAIT_CALLER`, on the second `IRQ_WAIT_CALLER + 1`. **Two
endpoints rather than one for both**, because a server knows which endpoint a
message came in on and nothing else, so the right to write stays an endpoint of
its own. Under QEMU the same stick's blocks then read at 759 MB/s and 9765 IOPS
(`testing.md` §18.71) - numbers that say the wait is gone, not how fast a stick
is.

A driver with no controller has nothing to wait on but `/dev/blocks`, and
nothing is left waiting on the write endpoint there: the disk server asks it
only once a stick with its partition has been found, which on such a machine
never happens.

**Not there yet is not blank, and it is waited for.** The disk server starts
before the driver has named any stick, and init does not wait for the driver -
on the ThinkPad naming a stick takes seconds. The shell, meanwhile, decides
where `/home` is from one read of `/home/.super` as it builds its namespace: a
filesystem, or memory for the life of the machine. So the first time the disk
server looks for its partition and does not find it, it keeps looking, a
tenth of a second apart, for at least twenty seconds, and after that each
request looks once. Until the partition is found the disk answers nothing, so
nothing is formatted: a stick not named yet must never read as a blank disk.
Once found, a blank partition is formatted the first time it is asked for, as
a blank disk always was.

**A commit the stick has kept.** kfs's journal promises one instant: once the
journal's header block says COMMITTED, the transaction survives a power cut.
A stick with a write cache can acknowledge a write it has not kept, so after
any write that begins with the journal's magic - the header marked committed,
and the header cleared again - the disk server asks for `BLOCK_OP_FLUSH`:
SYNCHRONIZE CACHE (10), operation 35h, with block 0 and a count of 0 meaning
every block, and IMMED clear so the status comes once the cache is written
(Seagate's *SCSI Commands Reference Manual*, rev. J, 3.51). Two flushes a
transaction, and none for the blocks between, which the journal covers.

**Said through `diskinfo`, because the disk server cannot print.** It owns no
console, and the kernel refuses a write from a process that does not -
`run_disk.py` says the same of its format line. So where `/home` is, why a
stick's cache is not written out, and what finding the stick took come back in
`sys.disk()`'s answer and through `/home/.super`; and the driver, which can
print, says a stick's first flush that it kept, because nothing else shows one
was ever sent:

```
kosmos> save notes.txt kept on a stick
xhci: 00:04.0 port 1: the stick wrote out its cache when asked, by SYNCHRONIZE CACHE (10)
saved notes.txt: 15 bytes, 1 extent(s)
kosmos> diskinfo
disk: 28639 sectors of 512 bytes, 13 MB
  on the Kosmos partition on USB unit 0, blocks 4096 to 32734
filesystem: version 1, 3579 blocks of 4096 bytes
```

**A stick that does not do SYNCHRONIZE CACHE is told once.** The ThinkPad's
Kingston answers every one with ILLEGAL REQUEST, 20h/00h - INVALID COMMAND
OPERATION CODE - and the driver asked it twice a commit, each time with a
REQUEST SENSE after it and a line on the screen. A command block that never
changes gets the same answer every time, so `scsi_not_supported` - ILLEGAL
REQUEST with 20h/00h, or 24h/00h, INVALID FIELD IN CDB - marks the stick, the
driver says so once, and every later flush is answered `BLOCK_ERR_NO_FLUSH`
without a transfer. **The disk server still asks every time**: one memory of
the answer, in the driver that owns the stick. A second memory in the disk
server was tried first and hid the driver's - it stopped asking after the first
refusal, so the driver was never asked twice, and the check passed with the
driver's memory taken out. QEMU's stick does every flush, so to see this under
QEMU blkdebug fails the image's flushes with EINVAL, which QEMU's SCSI disk
answers as 24h/00h:

```
xhci: 00:04.0 port 1: the stick does not do SYNCHRONIZE CACHE (10), so it is not asked again: ILLEGAL REQUEST (24h/00h)
saved a.txt: 3 bytes, 1 extent(s)
saved b.txt: 3 bytes, 1 extent(s)
saved c.txt: 5 bytes, 1 extent(s)
kosmos> diskinfo
disk: 28639 sectors of 512 bytes, 13 MB
  on the Kosmos partition on USB unit 0, blocks 4096 to 32734
  found at 1.36 s, by look 1; the first look was at 0.10 s
  its cache: not written out when asked, so a commit is only as safe as the stick (the stick does not do SYNCHRONIZE CACHE)
```

Before, the same stick printed `the SYNCHRONIZE CACHE (10), which the stick
failed: ILLEGAL REQUEST (24h/00h)` twice for every save.

**What finding the stick took.** On the ThinkPad the driver had the Kingston
ready at 4.963 s and the prompt came at 22, and the disk server may spend up
to twenty seconds looking without a word to anyone. So it counts every look,
the step each look that found nothing stopped at - no stick named yet, a stick
with no GPT, no Kosmos partition, and the rest - and the counter at the first
look and at the one that found it. `diskinfo` says them in the log's own
seconds: the kernel hands out the counter's reading at the log's zero as
`sys.info().log_origin`, so they read beside the driver's stamped lines in
`log`. A stick plugged in five seconds after the driver started watching:

```
kosmos> diskinfo
disk: 28639 sectors of 512 bytes, 13 MB
  on the Kosmos partition on USB unit 0, blocks 4096 to 32734
  found at 6.52 s, by look 47; the first look was at 0.10 s
    46 look(s) before it found no stick named yet
```

**The log's zero is the moment the kernel first knew the counter's rate**,
not power-on and not necessarily the kernel's first line: lines before it are
stamped from the scheduler tick, which has not started, so they read `0.000`.
Under QEMU on x86 the rate is measured rather than stated, and a first look at
0.10 s says that happened shortly before init first asked for `/home`.

**And a diagnosis off the stick, on the Mac.** A photograph of the screen
was the only way anything reached this Mac from the ThinkPad. `diagnose`
writes the build, the machine as `sys.info()` has it, the device server's
nodes, `/home/.super` and `/home/.device`, `/home`, the sticks, the processes
and the whole log to `/home/diagnose.txt`, the log last so that a file cut
short shows it; `log save` writes the log alone to `/home/log.txt`. Both go
through pages (`fs.write_from`), since the log is a quarter of a megabyte and
a message is two kilobytes. Then, with the stick in the Mac:

```
make stick-log                          # /home/diagnose.txt -> build/stick-diagnose.txt
make stick-log FILE=/home/log.txt       # what `log save` wrote
```

`tools/sticklog.sh` offers only external physical drives, as `mkusb.sh` does,
and `tools/sticklog.py` reads the stick's GPT as the disk server does - the
header at block 1, the first entry of Kosmos's type - and copies that
partition out through the raw device, as root because macOS lets nobody else
read a whole disk. **Nothing opens the stick for writing.** `kfs.lua get` takes
the file from the copy, which is the filesystem code the machine itself runs.

Three names in `/dev` are not read by `diagnose`, because they are other
servers mounted there: `/dev/audio` and `/dev/blocks` speak their own
protocols, and a read of `/dev/console` is a line somebody types - which the
first version found by waiting at the prompt for one and writing nothing.

**A unit is a name, and 5d's was not.** In 5d a unit was the Nth stick ready,
counting controllers and then slots. The disk server keeps the unit it found
its partition on, so a stick plugged into an earlier controller would have
made itself unit 0, moved `/home`'s stick to 1, and taken `/home`'s next
requests. It was found by reading 5d's own paragraph on units, before any
stick was written, and `usb_second_stick` is the check. Now a stick is given
the next number never given out as it becomes ready and keeps it until it
leaves; `BLOCK_OP_INFO` answers how many have been given, so `sticks` and the
disk server walk up to it and step over the gaps. A stick that leaves takes
`/home` with it until the machine starts again, rather than another stick's
blocks being written.

### 5f: a stick whose `/home` is a partition of its own

**`make MEGA=1 x86-usb-image USB_HOME=partition`** writes the stick's kfs disk
into a second partition, of Kosmos's type, right after the EFI system
partition, rather than as `\boot\disk.img` inside it; and it puts
`opt/kosmos/home=` and that partition's unique GUID on the kernel's command
line, in `\boot\kosmos.cmdline`. Kosmos starts with no disk in memory, and the
disk server opens the partition through the USB driver as `/home`, on the
stick the machine started from. Without `USB_HOME` nothing changes: the stick
is the layout that has booted (`boot.md` §1).

**The stick names its partition, and the loader passes the name on.** The
call was that the loader passes the partition's unique ID (`README.md`), and
there were two ways to do it: the loader finds the Kosmos partition on the
disk it started from, through the firmware's Block I/O and the GPT; or the
image builder writes the GUID into the words the loader already passes. This
is the second, for two reasons. The loader does not change at all, on the
machine where the loader is what has failed (`boot.md` §3). And the GUID is
written in the same moment as the partition it names, so a stick names its
own. What it gives up: a loader that searched could not name a partition on
another disk, where this one passes on whatever the stick says - which the
disk server holds to the partition's type as well as its GUID.

**One partition, by name.** `opt/kosmos/home` takes `usb`, or a GUID in either
case; given a GUID, the disk server takes only the Kosmos partition that has
it, and waits for it once as it waits for any (5e), so a second Kosmos stick
plugged in beside the one the machine started from is never taken for it.

**The kernel's command line is longer than any the loader passes.** The
loader passes up to 384 characters - a stick's words first, then 117 of its
own `kosmos-boot/...` words - and the kernel kept 255. So long `KOSMOS_ARGS`
lost the loader's words from the end without a sign, and would have lost the
partition's name with them. It keeps 511 now.

**The partition is compared, and not checked.** The loader holds the kernel,
and a `\boot\disk.img`, to the build's page sums; nothing holds the partition
to anything before Kosmos mounts it, because the loader never reads it. What
`mkusb.sh`'s read-back compares is every sector of it, and `stickcheck.py`
names that partition when one differs, rather than calling it the backup GPT.

### What is not done yet, and what QEMU cannot show

- **A client that ends without closing** keeps its open slot, and the region's
  address in the driver, for the life of the driver. Nothing tells a server
  that a client has gone.
- **A handle's generation** is checked and nothing here presents a stale one;
  **the endpoint on the wait** is what makes a request prompt, and the check
  does not time one - a request left off the wait still waits at most 50 ms,
  and passes (`testing.md` §18.59).
- **A stick's blocks on the ThinkPad were read on 14 September**, from the
  Kingston DataTraveler Exodia 128 GB it booted from (`b8c6f10`, `boot.md`):
  `0951:1666` at SuperSpeed on `00:14.0` port 14, in bursts of 4 where QEMU's
  stick bursts 16, 242155520 blocks of 512 bytes, the GPT's header at block 1,
  and `sticks` reading its partition through `/dev/blocks`. Its last block
  holds **no backup** table, which is the image rather than the stick:
  `mkusb_image.py` writes the backup where the image ends, 475202 blocks in,
  and the firmware boots it anyway. **And written**: the `c70d9df` stick, whose
  `/home` is its own partition (5f), booted there with no disk in memory, and
  a file saved to `/home` read back.
- **A stick that leaves while `/home` is on it.** The disk server's requests
  are refused as no stick at that unit, and `/home` stays gone until the
  machine starts again: a stick put back is a new unit. The journal is what
  covers a write in flight. QEMU can pull a stick (`device_del`), and no check
  does it yet.
- **A stick that never comes**: with `opt/kosmos/home=usb` and no Kosmos
  partition anywhere, the machine waits twenty seconds for `/home` and then
  keeps it in memory.
- **What a flush buys** is not visible under QEMU, whose stick writes straight
  to a file: the check sees the driver say one was sent and kept, not a power
  cut survived.
- **Why that stick's desktop came 20 seconds late on the ThinkPad**, with its
  bar and windows appearing only once the pointer moved. Not the USB driver
  held back by init: under QEMU it starts at 0.126 s with `opt/kosmos/home`
  and without it. The ThinkPad's own timestamps are what is needed.
- **Two sticks written from one image** carry the same partition GUID, and the
  disk server takes the lower unit of the two.

### How it is tested

- **`tools/run_x86.py`'s `usb_blocks`**, 4 checks: `sticks` at the prompt says
  unit 0 is 32768 blocks of 512 bytes, "QEMU" "QEMU HARDDISK", through
  `/dev/blocks`; it reads the partition `write_gpt` wrote, "KOSMOS", blocks 34
  to 32734, an EFI system partition; a two-line program written to `/ramfs`
  and run by its file reads one block past the last and is refused as past the
  last; and another sends a write and a flush there, each refused as read only
  (5e). `usb`, `usb_hotplug` and `usb_mouse` pass with every command's data
  going through the transfer buffer. Controls in `testing.md` §18.59.
- **`usb_home`**, 4 checks (5e): a stick with an EFI partition and a blank
  Kosmos partition, booted twice with `opt/kosmos/home=usb`. `diskinfo` says on
  both boots that the disk is 28639 sectors, on the Kosmos partition, blocks
  4096 to 32734; the first boot formats it and saves a file; the driver says the
  stick kept the save's flush; and the second boot - a machine that has never
  seen the stick - reads the file back.
- **`usb_second_stick`**, 4 checks (5e): `/home` on a stick on the second
  controller and a file saved; then a stick with a partition of its own plugged
  into the first controller through QEMU's monitor, read by the driver, and a
  second file saved. Not one of the new stick's blocks differs from what it
  held; both files are in `/home`; and `sticks` shows `/home`'s stick as unit 0
  and the new one as unit 1.
- **`usb_home_late`**, 3 checks (5e): the machine started with
  `opt/kosmos/home=usb` and no stick in, and the stick plugged in five seconds
  after the driver says it is watching. The driver reads it and a prompt comes;
  `diskinfo` says `/home` is the Kosmos partition; and a file saved there has
  extents on a disk. Controls in `testing.md` §18.60.
- **`usb_home_named`**, 3 checks (5f): two sticks with a Kosmos partition each,
  and `opt/kosmos/home` naming the second's GUID in small letters. `/home` is
  that stick's partition, on unit 1; a file saved there has extents; and the
  other stick's blocks are unchanged.
- **`cmdline_long`**, 1 check (5f): a word at the end of a 335-character
  command line reaches `sys.boot`.
- **`tools/run_uefi.py`'s home stick**, 4 checks (5f): the stick `mkusb_image.py
  --home` makes, booted through OVMF with no screen. The loader hands over no
  disk and a kernel that is the build's; `sys.boot` gives the partition's GUID
  from the stick's command line; `diskinfo` says `/home` is that partition; and
  a file saved there has extents. `test_stickcheck.py` asks about the same
  stick: itself, and a byte of its partition changed, named as `/home`'s
  partition. Controls in `testing.md` §18.61.

---

## 8. Step 6: drives

**Every drive shown and named, and other machines' filesystems read, read
only.** `drives.html` is the design; the table at the top has the six pieces
and their order.

### 6a: what a FAT volume's bytes mean

**`user/servers/fat_decode.c` reads FAT16 and FAT32, and nothing on the
machine uses it yet.** It is what the drive server stands on from 6b, and it is
a file of its own for `storage_decode.c`'s reason: no hardware and no system
calls, so the host can ask it anything. Every rule in it is Microsoft's, from
the FAT32 File System Specification, version 1.03, with the section named
beside it. It writes nothing.

- **A boot sector is held to what one must be before anything in it is
  trusted**: 0x55 0xAA at byte 510, a jump at byte 0, 512 to 4096 bytes a
  sector, a power of two sectors a cluster, clusters of 64 KB at most, a
  reserved sector, a FAT, a sector count, a data region after the FATs, a
  table long enough for every cluster, and for FAT32 FAT16's fields empty, a
  root cluster the volume has, and version 0.0 - drivers "must check this
  field and not mount the volume" otherwise.
- **The kind is the count of clusters and nothing else**: under 4,085 is FAT12,
  under 65,525 FAT16, and the rest FAT32 - "when it says <, it does not mean
  <=". The name in the boot sector decides nothing. FAT12 is named and
  refused.
- **A table entry says where a file goes next**: two bytes on FAT16,
  twenty-eight bits of four on FAT32, whose top four are reserved and ignored.
  The end of a chain, a free cluster, the bad cluster mark and a number the
  volume does not have are told apart, so a damaged chain is reported rather
  than followed.
- **A directory, an entry at a time**: 0x00 ends it, 0xE5 is a free entry, and
  0x05 is a live one whose name begins with the character 0xE5. A long name is
  gathered from its pieces, each carrying its ordinal and the short name's
  checksum, and counts only when every piece came in order for that short
  entry; otherwise they are orphans and the short name is shown, as the
  specification says. UTF-16 comes out as UTF-8.
- **Short names in the case they were saved in.** `hello.txt` is stored as
  `HELLO   TXT`, and Windows NT keeps the lower case in two bits of
  `DIR_NTRes` - 0x08 for the name, 0x10 for the extension. The specification
  calls that byte reserved; mtools writes the bits, and its `hello.txt` came
  back `HELLO.TXT` until they were read.
- **A name is found as FAT finds it, without regard to case**, and a search
  matches the long name or the short one. ASCII letters only: an accented
  letter is compared exactly. A short name's bytes outside ASCII are shown as
  `_`, because which code page wrote them is not recorded - the specification's
  own rule for a character that cannot be translated.

**`tools/fatls.c`** is the same file walking a volume in an image on the Mac:
every directory and file, with its size, a hash of its bytes and how many runs
of clusters it is in, or one path found in whatever case it is typed.

**What mtools showed that the specification did not.** FAT32 keeps a hint of
where to look for a free cluster (`FSI_Nxt_Free`), and mtools follows it: a
file deleted from the middle leaves a hole the next file never goes into. The
test sets the hint back to cluster 2, which is where the specification says a
driver with no hint begins, so a file really does land in two runs. A reader
never needs the hint; a writer will.

### What is not done yet

- **6b is built, as far as naming volumes goes** (16 September). A drive
  server in C owns the whole `/drives` prefix - one server, not a mount per
  volume, because a volume plugged in later could never be given a mount in a
  namespace that already exists. It walks every unit, reads whichever
  partition table the drive has, identifies FAT16, FAT32 and kfs, names each
  volume (`Untitled` for no label, `PHOTOS 2` for a repeat) and measures free
  space. Whether a volume's sector count fits its partition is checked there,
  since only the caller knows the partition.

  **What it does not do yet is open one.** A FAT volume is named, sized and
  listed; asking for its contents answers `DRIVES_ERR_UNREADABLE` until the
  directory walk is written. kfs volumes are listed and never opened, because
  kfs's reader is `user/lib/kfs.lua` and this server is C - reading one here
  means kfs in C, which is its own piece of work behind Disk Benchmark.
- **exFAT is 6f.** FAT12 is refused, short names are not read through a code
  page, and nothing is written.

### How it is tested

`tools/test_fatdecode.c` builds its bytes from the specification: 75 checks
on each field a boot sector is held to, both type boundaries a cluster either
side, the table entries of both kinds, and long names in order, out of order,
orphaned and in UTF-16 surrogates. That cannot catch a field read at the wrong
offset, since the test would write it at the same wrong offset - so
`tools/test_fat.py` has **mtools**, somebody else's reading of the format, make
FAT16 and FAT32 volumes with no partition table and inside an MBR partition,
fill them with files, and `fatls` read every one back: 24 checks. Controls in
`testing.md` §18.62.

---

## Sources

- Microsoft, *FAT32 File System Specification*, version 1.03, 6 December 2000
  ("fatgen103") - every rule in `fat_decode.c`, with its section beside it.
  Downloaded from download.microsoft.com on 14 September 2026 to read, and not
  kept in the repository.
- Microsoft, *exFAT File System Specification*, learn.microsoft.com - for
  6f. Read on 14 September 2026, and not kept.
- GNU mtools 4.0.49 - the FAT volumes `test_fat.py` holds the reader to,
  used as a program and never read for how: its `hello.txt` is what showed
  `DIR_NTRes`'s case bits, and its allocation what showed FAT32's free cluster
  hint. Nothing is copied from it.
- Intel, *eXtensible Host Controller Interface for Universal Serial Bus
  (xHCI)*, revision 1.2 - every offset and bit in `xhci.c`, with the table
  or section beside it. Downloaded from intel.com on 12 September 2026 to
  read, and not kept in the repository.
- QEMU 11.1.1, `hw/usb/hcd-xhci.c` and `hcd-xhci.h` - the model the tests
  run against, read for how it behaves: that it has no Legacy Support
  capability, that its ports are USB 3 first and USB 2 after, four of each
  by default. Nothing is copied from it.
- USB-IF, *Universal Serial Bus Specification*, revision 2.0, as usb.org
  distributes it with its errata and engineering change notices - the
  descriptors, the standard requests and the intervals a device is owed
  (§5), and CLEAR_FEATURE with ENDPOINT_HALT (§7). Downloaded from usb.org on 13 September 2026 to read, and not kept
  in the repository.
- USB-IF, *Universal Serial Bus Mass Storage Class Bulk-Only Transport*,
  revision 1.0, and the *Mass Storage Class Specification Overview*, revision
  1.4 - a stick's interface, its endpoints, the command and status wrappers,
  and what a host checks and does when they are wrong (§6). Downloaded from
  usb.org on 14 September 2026 to read, and not kept.
- Seagate, *SCSI Commands Reference Manual*, rev. J (SPC-5 and SBC-4) -
  INQUIRY's command and its standard data (§6); TEST UNIT READY, REQUEST
  SENSE, READ CAPACITY (10) and READ (10), sense data in both formats, and the
  sense keys (§7). Downloaded from seagate.com on 14 September 2026 to read,
  and not kept.
- QEMU 11.1.1, `hw/usb/dev-storage.c` and `hw/scsi/scsi-disk.c` - the stick
  the check runs against, read for its descriptors at each speed, what it does
  with a wrapper and a status, and what INQUIRY, READ CAPACITY (10) and
  REQUEST SENSE answer; what makes the stick stall, and what it does with
  CLEAR_FEATURE and the class reset. Nothing is copied from them.
- USB-IF, *Device Class Definition for Human Interface Devices (HID)*,
  version 1.11 - the boot subclass and the mouse protocol, SET_PROTOCOL and
  SET_IDLE, which of them a boot mouse must support, and a boot mouse's
  report. Downloaded from usb.org on 13 September 2026, and not kept.
- QEMU 11.1.1, `hw/usb/dev-hid.c` and `hw/input/hid.c` - the mouse the check
  runs against, read for how it behaves: that it attaches at high speed with
  a four-byte packet every 8 ms, honours SET_PROTOCOL, clamps a report's
  movement to 127 and keeps the rest for the next, and folds movement into a
  report the guest has not read yet. Nothing is copied from it.
