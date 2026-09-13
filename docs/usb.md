# USB

How USB works in Kosmos, from the host controller up. **Written as it is
built**: each step adds its section when it lands, and a section describes
what exists, not what is planned. The plan is the table below and nothing
else.

| step | what it ends in | state |
| ---- | --------------- | ----- |
| 1. controllers up | every xHCI controller found, taken from the firmware, reset, and its ports read | built, and run on the ThinkPad |
| 2. enumeration | a device's descriptors read: what it is, who made it | built, and run on the ThinkPad |
| 3. a mouse | a HID boot mouse's reports moving the pointer the TrackPoint moves | built, and run under QEMU |
| 4. bulk transfers | bytes to and from an endpoint | not started |
| 5. mass storage | the stick Kosmos booted from, mounted as its disk | not started |
| 6. Ethernet | a USB-C adapter carrying the network stack | not started |

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
memory, and with it the limit.

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
streams asks. The window manager did not change.

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
IN endpoint. HID 1.11 fixes a boot mouse's report (B.2), so nothing reads its
report descriptor. Any other HID interface is said, with its subclass and
protocol, and left alone. A SuperSpeed mouse is said and not read: its
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
3. **SET_PROTOCOL for the boot protocol** (HID 1.11 7.2.6), because a device
   starts in the report protocol and the host is to ask for the one it
   wants. **SET_IDLE is not sent**: a boot mouse need not support it
   (Appendix G), and a mouse's idle rate starts at infinity - a report only
   when something changes (7.2.4).

### Reading reports

**One request on the ring at a time**: a Normal TRB over the mouse's report
page, as long as its packet, that interrupts when it completes and when a
report comes back short (6.4.1.1), and the doorbell with the endpoint's
context index as its target. A Transfer Event says how many bytes did not
arrive (Table 6-38), and a report of three or more is read as HID 1.11 B.2
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
pointer in bursts. The kernel gained `SYS_IRQ_WAIT_ANY (&caps, count,
ticks)` for it (`drivers.md` §4), and the watch waits on every running
controller's interrupt at once, then looks at all of them. A controller whose
interrupt could not be claimed is looked at when the wait's deadline comes
round.

**Found while testing it: under `opt/kosmos/irq=pic`, QEMU's two controllers
share line 11.** The second's claim is refused, its line says "not claimed,
so polled", and the shared line wakes the driver for both - so on the 8259s
the wait on two lines never runs. That is a machine on the legacy path. The
ThinkPad's ACPI describes an I/O APIC, its controllers each have an MSI of
their own (§4), and the check boots the same way.

What QEMU prints, with the mouse on the second controller, moved, clicked,
held and pulled out, and plugged back in:

```
xhci: 00:02.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 20
xhci: 00:03.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 21
xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3), after its reset
xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Mouse"
xhci: 00:03.0 port 5: a boot mouse, read from endpoint 1, up to 4 bytes every 8 ms
xhci: 2 controllers (00:02.0, 00:03.0), 1 port with something plugged in, 1 device named
xhci: watching for devices plugged in and out
xhci: 00:03.0 port 5: the mouse's first report: buttons 0, moved 3,-2
wm: button down at 965,537 raw=1
wm: button up at 965,537 raw=0
wm: button down at 965,537 raw=1
xhci: 00:03.0 port 5: unplugged, 0627:0001 "QEMU USB Mouse", after 6 reports
wm: button up at 965,537 raw=0
xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3), after its reset
xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Mouse"
xhci: 00:03.0 port 5: a boot mouse, read from endpoint 1, up to 4 bytes every 8 ms
```

The last `button up` is the driver letting go: nothing released the button
in QEMU.

### What QEMU cannot show

- **A full-speed mouse whose endpoint 0 is bigger than 8 bytes.** QEMU's
  mouse attaches at full speed when asked to (`usb_version=1`), and the check
  plugs it back in that way, so turning milliseconds into an interval runs
  here; but its endpoint 0 is 8 bytes, so Evaluate Context has still run
  nowhere.
- **A mouse that refuses SET_PROTOCOL**, or keeps sending report-protocol
  reports anyway. QEMU's honours it; the line would name the step and the
  code.
- **A halted endpoint**, above.
- **Contacts that bounce.** QEMU's plug is one clean change, so the debounce
  is always a single interval.
- **A thousand reports a second**, which a gaming mouse may send.

### How it is tested

- **`tools/test_usbdecode.c`, 32 checks** on the host: QEMU's mouse as its
  device model declares it, HID 1.11 Appendix E's keyboard and mouse (the
  mouse behind the keyboard, and not the keyboard's endpoint), a stick, and
  every length a device can get wrong - each of which must end the walk.
- **The suite, on both boards**: `irq: a wait on two lines takes whichever
  has one`, and `input: a driver's movement adds to the pointer` - which on
  the ARM board is the refusal, since its pointer is a tablet.
- **`tools/run_x86.py`'s `usb_mouse`**, 16 checks, on q35 with two
  controllers and QEMU's mouse on the second: the driver reads it there, on
  two interrupts; 640 movements a little over 10 ms apart, and at least four
  in five come back as reports of their own - QEMU folds a movement into the
  one before while that is unread, so a driver reading late reads fewer - and
  more than two rounds of a ring; then a click on the Deskbar's button opens
  its menu, through USB alone; a button held as the mouse is pulled out comes
  up; and a full-speed mouse plugged back in is read every 8 ms, from its
  first report.

The controls, each watched fail, are in `testing.md` §18.43.

### On the ThinkPad

Not run yet.

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
- USB-IF, *Universal Serial Bus Specification*, revision 2.0, as usb.org
  distributes it with its errata and engineering change notices - the
  descriptors, the standard requests and the intervals a device is owed
  (§5). Downloaded from usb.org on 13 September 2026 to read, and not kept
  in the repository.
- USB-IF, *Device Class Definition for Human Interface Devices (HID)*,
  version 1.11 - the boot subclass and the mouse protocol, SET_PROTOCOL and
  SET_IDLE, which of them a boot mouse must support, and a boot mouse's
  report. Downloaded from usb.org on 13 September 2026, and not kept.
- QEMU 11.1.1, `hw/usb/dev-hid.c` and `hw/input/hid.c` - the mouse the check
  runs against, read for how it behaves: that it attaches at high speed with
  a four-byte packet every 8 ms, honours SET_PROTOCOL, clamps a report's
  movement to 127 and keeps the rest for the next, and folds movement into a
  report the guest has not read yet. Nothing is copied from it.
