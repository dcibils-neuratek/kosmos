# USB

How USB works in Kosmos, from the host controller up. **Written as it is
built**: each step adds its section when it lands, and a section describes
what exists, not what is planned. The plan is the table below and nothing
else.

| step | what it ends in | state |
| ---- | --------------- | ----- |
| 1. controllers up | every xHCI controller found, taken from the firmware, reset, and its ports read | built, and run on the ThinkPad |
| 2. enumeration | a device's descriptors read: what it is, who made it | not started |
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
  (`SYS_IRQ_CLAIM`, `WAIT`, `ACK`). The kernel does not know what USB is.
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

### What is not built yet

Named so the next step can be read against it. None of this exists in
Kosmos:

- **Device slots and contexts.** Each attached device gets a slot, and the
  controller keeps its state in a *device context* in memory software gives
  it, reached through the Device Context Base Address Array (DCBAAP).
- **Rings.** Work is exchanged through circular buffers of 16-byte *TRBs*: a
  **command ring** (software to controller: "enable a slot", "address this
  device"), an **event ring** per interrupter (controller to software:
  "command done", "transfer done", "port changed"), and a **transfer ring**
  per endpoint.
- **Doorbells**, written to say a ring has new work.
- **Interrupts**, which arrive when an event ring has something in it.

That is step 2's work, and it is where the kernel's contiguous memory and
interrupt capabilities are first used for real.

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

What QEMU prints:

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

## Sources

- Intel, *eXtensible Host Controller Interface for Universal Serial Bus
  (xHCI)*, revision 1.2 - every offset and bit in `xhci.c`, with the table
  or section beside it. Downloaded from intel.com on 12 September 2026 to
  read, and not kept in the repository.
- QEMU 11.1.1, `hw/usb/hcd-xhci.c` and `hcd-xhci.h` - the model the tests
  run against, read for how it behaves: that it has no Legacy Support
  capability, that its ports are USB 3 first and USB 2 after, four of each
  by default. Nothing is copied from it.
