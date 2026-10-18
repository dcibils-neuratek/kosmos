# This Machine, redesigned

Every piece of hardware the machine has, seen at once: what it is, whether
Kosmos drives it and with what, and what it is doing. Diego, 18 October
2026: "lets redesign the about this machine app to show all hardware
components detected, driven and not driven in a visual and nice way so i
can see all the hardware my computer has, usb drives and devices, disks,
network, cpu, gpu, memory, audio, etc", "mockup first".

This document is the feature set and the architecture; `machine.html` is
the window and `machine-architecture.png` the picture of what it stands
on. It replaces `machine.lua`'s monospaced report, which stays reachable
as the full text behind a button - it is what a bug report wants.

## What a person can do

- **See the machine whole**: a header with the machine's name as the
  firmware gives it (`LENOVO ThinkCentre M700`), Kosmos's version, and a
  strip of counts - so many devices, so many driven, so many with no
  driver yet.
- **See every device as a card**, grouped by what it is - Processor,
  Graphics, Memory, Storage, Network, Audio, USB, Input, Power, and Other
  (what the bus has that belongs nowhere above: the chipset's bridges, the
  management engine, a serial bus) - each card drawn as what it is, with
  its name, its maker, where it is (PCI 00:1f.6, USB port 3), and **a pill
  that says its state**:
  - **Driven** - by `e1000`, by `intelgpu`, by `xhci` - green;
  - **Detected, no driver yet** - grey, with the class it reported;
  - **In use** where a device is busy now - a stick mounted at /Drives/X,
    a card with an address.
- **Each card's picture is what the device is**, drawn as the monitors
  draw theirs (Diego: "I want the cards to use icons based on the hardware
  type", "We already have that for cpu monitor gpu monitor network monitor
  etc"): the CPU as CPU Monitor's chip with its maker's mark, the GPU as
  GPU Monitor's, the network card as Network Monitor's port, a disk and a
  stick as Disk Monitor's, and a memory module, a speaker, a USB port, a
  mouse, a keyboard and a plug in the same hand; a device with no driver a
  dim chip with its short name.
- **See how it is all connected** ("perhaps show some hierarchy on devices
  how are they connected?"): **By connection** beside **By kind** - the same
  devices as a tree from the processor down: memory beside it, the PCI bus
  behind its host bridge, each device at its address, under the USB
  controller each port and what is plugged into it, and under a stick its
  volumes. A device with no driver drawn dim in its place. Where the bus
  says nothing more, the tree does not guess.
- **See what each is doing**, in a line on its card: the processor's cores
  and speed, the GPU's clock and the rendering mode, memory used of
  installed, a disk's size and what is on it, the network's address and
  speed, the sound's rate, the battery's charge.
- **Filter**: All, Driven, No driver - so "what does Kosmos not drive on
  this machine" is one press.
- **Open the full report**, the text `machine.lua` writes today, to copy.
- **Live**: a stick plugged in appears; one pulled out goes - the USB
  driver already sees it happen.

## What it is made of

**The application is the thinnest layer there is**: it asks and draws.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window, the cards, the filter, the groups | the UI kit: `ui.cards`, the sidebar | as it is |
| **Every device, in one list** - kind, name, maker, place, driven by, state | **`devices.lua`, a new library**: one door that gathers the sources below into one shape, `{ kind, name, maker, place, driver, state, detail }` - so the Deskbar's devices indicator (`roadmap.md` 5q), `neofetch` and this window list the same devices the same way | **new** |
| PCI devices and whether a driver claimed each | `sys.bus()` (`claimed`), `hardware.lua`'s names | as it is; more class names and a table of drivers by vendor and device |
| **USB devices** - every one on every port, with its class and whether anything reads it | **the USB driver** (`xhci.c`), answering **`USB_OP_LIST`** in a declared shape (`usbproto.h`): port, speed, vendor, product, class, its name strings, and the kind the driver made of it - keyboard, mouse, stick, camera, MIDI, network, or none | **new**: the driver knows all of it already and writes it only to the log |
| Processor, memory | `/Devices/cpu`, `/Devices/memory`, `pulse.lua` | as it is |
| Graphics | `gpu.stats()`, the rendering mode | as it is |
| Disks, sticks, volumes | `drivelist.drives()` | as it is |
| Network | `fs.net_info`, `hardware.network` | as it is |
| Audio | `/Devices/audio`, `hardware.audio` | as it is |
| Battery, temperatures, power | `/Devices/battery`, `sys.info()` | as it is |
| The machine's name | `hardware.name(sys.info())` | as it is |
| Pictures for each kind | drawn by the kit as the monitors' are: `ui.processor_chip`, the GPU's chip, the port, the SSD and the stick - and a module, a speaker, a USB port, a mouse, a keyboard, a plug added beside them, for the monitors and the Deskbar's indicator to draw too | the five there are; **new**, the six beside them, in the kit |
| The tree | the PCI places from `sys.bus()`, the USB ports from `USB_OP_LIST`, the volumes from `drivelist` | from the sources above |

**One door, many sources**: `devices.lua` is a Lua library rather than a
server - it is run, not asked (`glossary.md`), and every source it reads is
already a door of its own. What is new below it is one verb in the USB
driver.

**C or Lua**: the USB list is C, in the driver, a copy of what it keeps;
everything else is Lua - a page of cards, read once and on a change.

## The busiest path

**Opening it**: one `sys.bus()`, one `USB_OP_LIST`, a handful of reads of
`/Devices` - well under a frame. **While open**: every two seconds the
lines that move (the processor's speed, the GPU's clock, memory, the
network's rates) and the USB list, so a stick plugged in shows. Nothing
when it is closed.

## Steps

1. **M1 - the USB driver says its devices**: `USB_OP_LIST` and `usbproto.h`;
   a program `lsusb` that prints it; under QEMU the virtual keyboard,
   tablet and stick listed, each with its kind; on the M700 its mouse,
   keyboard and stick, and any device with no driver. **Done in 0.11.276**
   (`testing.md` 18.674): under QEMU a keyboard, a mouse and a tablet the
   driver does not read, the tablet listed "no driver" with its class.
2. **M2 - `devices.lua`**: every source into one list; `devices` at the
   prompt prints it; a host test feeds it a bus and a USB list and holds
   the merged list. **Done in 0.11.277** (`testing.md` 18.675) - at the
   prompt as `lsdev` and `lsdev tree`, since `devices` is the shell's own
   word.
3. **M3 - the window**: as drawn, its suite under QEMU, the picture in the
   dated screenshot.
4. **M4 - the Deskbar's devices indicator** (5q) on the same library.

Only x86-64 runs the tests for now (`CLAUDE.md`).
