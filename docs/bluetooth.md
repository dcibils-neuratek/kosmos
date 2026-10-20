# Bluetooth

Keyboards and mice without a cable. Diego, 17 October 2026: "add for the
roadmap: bluetooth driver to support mouse and keyboard devices"; 20
October: "i want to be able to connect keyboards with bluetooth and mouses
as well", and "Can you start building the Bluetooth stack?".

This is the design, written before the stack is built, and then the account
of each step as it lands, as `usb.md` is.

---

## What a person can do

- **Plug in a USB Bluetooth adapter** and see it named: who made it, its
  chip, its Bluetooth version, its address. The M700 has none inside
  (`lsusb`, 20 October), so a dongle is the way in.
- **Turn Bluetooth on and off.**
- **See what is nearby and pairable**, by name and kind: a keyboard, a
  mouse.
- **Pair a keyboard or a mouse.** A keyboard that asks for a passkey shows
  it on the screen, to be typed on that keyboard and Return pressed. A
  mouse pairs with nothing to type.
- **Use it as any other**: the keyboard's keys reach the focused window, and
  the mouse moves the one pointer with every other mouse.
- **Find it again**: a paired device reconnects by itself when it wakes or
  when Kosmos starts, with no step from anyone.
- **Forget a device**, which removes its keys.
- **See what is connected**, and its battery where it says it: in the
  Bluetooth app, in This Machine, and in the devices indicator in the
  Deskbar.

All of it in **the Bluetooth app**, an application of its own (Diego, 20
October: "Also we neeed a Bluetooth app to add and manage devices"). Its
window is drawn in `docs/bluetooth.html` and agreed before it is written.

Not now: files, phones, tethering. Sound to headphones is B8, after the
rest. The stack is shaped so that they
can come later, and nothing here is built for them.

---

## The architecture

```
  the Bluetooth app, This Machine, the Deskbar            (Lua, the UI kit)
                 |  /Devices/bluetooth: a declared protocol, btproto.h
  ------------------------------------------------------------------------
  bluetooth: the host stack                         (C, a driver process)
     HCI        commands, events, connections, the controller's setup
     vendor     a chip's firmware loaded before use (Realtek, Intel)
     L2CAP      channels over ACL: Classic's and Low Energy's
     Classic    SDP's minimum, HID's control and interrupt channels,
                Secure Simple Pairing (the controller does its crypto)
     LE         ATT and GATT as a client, HID over GATT, SMP pairing
                (Secure Connections: P-256 from BearSSL, AES-CMAC from
                the Crypto Kit)
     HID        reports read as USB's are (usb_decode: a mouse's report
                descriptor, a boot keyboard's keys), keys to
                kosmos_key_push and movement to the pointer
     keys       link keys and long-term keys in the keyring server
                 |  a shared region: two single-producer rings, HCI
                 |  packets in and out, and a message to say "more"
  ------------------------------------------------------------------------
  xhci: the USB driver                              (C, a driver process)
     the adapter claimed: class E0h, subclass 01h, protocol 01h
     commands  on the control endpoint (a class request to the interface)
     events    on the interrupt IN
     ACL       on the bulk IN and OUT
```

**Why the stack is a process of its own, not part of `xhci.c`.** A mouse
or a MIDI keyboard is a few hundred lines inside the USB driver, and that
is right for them. A Bluetooth host is HCI, two link layers, pairing, its
keys and the cryptography those need: thousands of lines, one of whose
mistakes should not take every USB device on the machine with it. And
Bluetooth is not USB's: on the Pi the radio sits on a UART, so the host
stack must not know what carried its packets. `xhci.c` does the transport
and nothing more, and `user/drivers/bluetooth/` does the rest.

**Data and control, as everything else here.** A mouse sends a report every
8 ms, or every 1 ms for a gaming mouse, because its clock says so. That is
the stream the rule is about (`CLAUDE.md`, *Control by message*), so HCI
packets cross between the two processes in a shared region: a ring each
way, one writer and one reader each, and the indices the only thing both
touch. A message only says that a ring has something in it. What the UI
asks (scan, pair, forget, list) is a message in a declared shape,
`user/include/btproto.h`, as every server's is.

**What is C and what is Lua.** The stack is C: it is a driver, it runs on
behalf of every program that reads a key, and its pairing is cryptography
(`CLAUDE.md`: *Encryption is C, all of it*). The Bluetooth app and the
indicators are Lua, and hold nothing the stack does not: the app asks
`/Devices/bluetooth` on its own clock and draws what it last heard, never
waiting on a scan or a pairing inside a click.

**What already supplies a piece** (`CLAUDE.md`, *Kits, servers and drivers
supply*):

| what | from |
|---|---|
| the adapter's endpoints, transfers, plug and unplug | `xhci.c`, as for every USB device |
| a HID report read | `usb_decode.c`: `usb_decode_mouse_report` for a mouse's report descriptor, `usb_decode_keys` and `usb_decode_key` for a boot keyboard's, as USB's mouse and keyboard use them |
| keys into the system | `kosmos_key_push`, as the USB keyboard does |
| movement into the one pointer | `hal_pointer_move`, as the USB mouse does |
| P-256 for LE Secure Connections | BearSSL's `ec_p256_m31`, already built for TLS |
| AES and AES-CMAC for SMP | the Crypto Kit (`cmac.c`) |
| keeping a device's keys | the keyring server |
| a chip's firmware file | `/Kosmos/Firmware`, as a file the build installs |

New: the host stack, its protocol header, its pure-C decoders held on the
Mac, a simulated controller for the suites, and the Bluetooth app.

---

## The Bluetooth app

Diego, 20 October: "Also we neeed a Bluetooth app to add and manage
devices". Its window is `docs/bluetooth.html`, its diagram
`docs/bluetooth-architecture.png`. **Agreed** the same day - Diego: "Mockup
is perfect" - and built at B7 as drawn.

**What a person does in it**, one window:

- **Bluetooth on or off**, a switch in the header. Off powers the radio
  down, and every device with it.
- **The adapter**, a line under the header: its maker and chip, its
  Bluetooth version and its address - what B1 already says in the log.
- **My devices**: each paired keyboard and mouse, its kind's icon, its
  name, *Connected* or *Not connected*, and its battery where it reports
  one. Its menu: Disconnect, and Forget.
- **Nearby**: while the window is open and Bluetooth is on, what is
  pairable around it, by name and kind, nearest first, each with **Add**.
  Scanning stops when the window closes, because a scan costs the radio's
  time and a keyboard's battery is nobody's business but its own.
- **Adding**: a mouse is added with nothing to type. A keyboard shows a
  six-digit passkey to type on that keyboard, and the digits fill in as
  they are typed - the keyboard says each keypress (Secure Simple Pairing's
  Keypress Notification, and SMP's for Low Energy) - then Return. A
  failure says which: the passkey was wrong, the device stopped answering,
  or it refused.
- **Nothing to set**: no discoverable switch, because Kosmos connects to
  keyboards and mice and nothing connects to it, and no device name to
  choose, because none is shown to anyone.

**What it is made of.**

| piece | supplied by | new or as it is |
|---|---|---|
| the window, the switch, the lists, the menu, the passkey sheet | the UI kit | as it is |
| the devices' icons: keyboard, mouse | the icon set the Deskbar and Tracker draw with | as it is, or a line icon added |
| the list, scanning, adding, forgetting | `/Devices/bluetooth`, the host stack's door, in `btproto.h` (B4) | **new**, with the stack |
| speaking that door from Lua | `user/lib/bluetooth.lua`, a thin library over the declared shape as `usb.lua` is over `usbproto.h` | **new** - and the `bt` program, This Machine and the devices indicator use it too: one door, one library |
| the passkey, and pairing's cryptography | the host stack, in C; the app only shows the six digits it is given | **new**, with the stack (B5, B6) |
| a device's keys | the keyring server, written by the stack | as it is |
| a battery's level | the stack: LE's Battery Service, Classic's where a device reports one | **new**, with the stack |

The program at the prompt is `bt` rather than `bluetooth`, so that the
name typed at the prompt opens the app, as `music` and `tracker` do.

**What crosses where.** Everything between the app and the stack is a
message: a request in a declared shape and its reply. Nothing recurs at a
hardware rate - a device's reports go from the stack to the kernel and
never near the window - so no region is needed.

**Asked, never waited on** (`CLAUDE.md`, *Nothing on the desktop waits on
a server*). A scan's results and a pairing's progress come from the radio
when they come, so the stack keeps them and the app asks on its own
clock: every quarter of a second while it scans or pairs, every second
otherwise, and not at all when its window is hidden. A click paints its
answer at once from what the app knows - the switch moves, Add becomes
*Adding* - and the reply corrects it if the stack disagrees. Once Nebula
has pulses (`pulses.md`), the stack can say "something changed" and the
app stops asking on a clock.

**What is Lua and what is C.** The app and its library are Lua: lists of a
dozen names, read a few times a second. Everything with a deadline or a
byte loop - HCI, L2CAP, HID reports, pairing - is the stack's, in C.

**Its busiest path** is a scan with twenty devices near: the stack keeps
the list as advertisements and inquiry results arrive; four times a
second the app sends one request and receives one reply of at most a
page of devices, about 64 bytes each, and redraws the Nearby list when it
changed. A few kilobytes a second, all of it messages to a process that
answers from memory.

---

## Classic or Low Energy first

They share the adapter and HCI and little else. **Classic** HID keyboards
and mice (BR/EDR) pair through Secure Simple Pairing, in which the
controller does the cryptography and the host answers questions. Their
reports arrive on two L2CAP channels. **Low Energy** ones (HID over GATT)
need the host to do the pairing's cryptography (SMP), and to find the
device's HID service by GATT before a report arrives. Low Energy is more
work and more common in new devices.

**Which first is decided by the keyboard and mouse Diego will pair.** The
keyboard is a **Logitech K380** (Diego, 20 October, with a photograph):
Bluetooth Classic, a HID device of Bluetooth 3.0's time with three
Easy-Switch channels, which pairs by Secure Simple Pairing with a passkey
typed on it - the sheet in the agreed mockup. So **Classic comes first**,
B5 before B6, as the steps stand. The mouse is still to be named.

---

## The busiest path

**A key pressed on a Bluetooth keyboard**: the radio receives it, the
controller hands an ACL packet to the dongle's bulk IN, `xhci.c` completes
the transfer and writes the packet into the ring, and tells the stack once.
The stack takes L2CAP's header off, finds the HID channel, reads the
report, and calls `kosmos_key_push` for each key that changed. Two
processes and one copy. The radio's interval (7.5 to 30 ms for LE, set by
the device) dwarfs all of it, so the aim is that Kosmos adds nothing a
person could feel, and the measure is a report's time from the transfer's
completion to the key in the kernel.

---

## How it is tested

QEMU dropped its Bluetooth dongle years ago, and this Mac's QEMU has no
`usb-redir` for a program to pretend to be one. So:

- **On the Mac**: the stack's byte work in pure C, a decoder a layer
  (`hci_decode.c` first, and `btusb_decode.c` for the adapter's USB
  descriptor): HCI commands built and events read, L2CAP, SDP, ATT, SMP,
  and HID reports, all held by `tools/test_btdecode.c`.
  Each is held to the Bluetooth Core specification's own sample data where
  it gives some, which it does for SMP's functions (c1, s1, f4, f5, f6, g2).
- **In QEMU**: `btsim`, a simulated controller in test builds, put where
  the USB transport would be. It answers HCI as an adapter does, and has a
  keyboard and a mouse behind it that pair, connect and send reports. The
  suite checks a key reaching a program and the pointer moving, every gate.
- **On the ThinkPad T14**, first (Diego, 20 October: "The IBM thinkpad
  t14 I have has Bluetooth chip", "We can use that to test"): its Intel
  AX201 is Wi-Fi and Bluetooth 5.1 on one card, and the Bluetooth half is
  a USB device on the machine's own xHCI - `8087:0026`, class E0h, which
  Kosmos already saw on port 10 (`usb.md`). No dongle needed.
- **On the M700**, with a dongle, later: the same transport on another
  machine, and Realtek's firmware if the dongle is a UB500.

---

## Steps

- **B1, the adapter named.** `xhci.c` claims a Bluetooth interface; HCI
  Reset, then Read Local Version, Read BD_ADDR and Read Local Supported
  Features; the chip, version and address said in the log and in `lsusb`.
  `hci_decode.c` builds and reads those, held on the Mac.
- **B2, the stack as a process.** `user/drivers/bluetooth/`, the rings
  between it and `xhci.c`, and `btsim` in QEMU behind the same rings; the
  controller set up as Linux's `hci_core.c` sets one up.
- **B3, the chip's firmware**: Intel's first, for the T14's AX201 - it
  starts in a bootloader that answers Intel's own commands, and is a
  controller only once its `ibt-` file from `linux-firmware` is sent to it,
  as Linux's `btintel.c` does; Realtek's (`btrtl.c`) for a dongle after.
- **B4, what is nearby.** Inquiry and LE scanning, names and kinds;
  `/Devices/bluetooth` and `btproto.h`, and `user/lib/bluetooth.lua` over
  them; a `bt` program at the prompt to list, pair and forget.
- **B5, Classic HID.** Connection, Secure Simple Pairing, link keys in the
  keyring, L2CAP, HID's channels; keys and the pointer.
- **B6, Low Energy HID.** Connection, SMP Secure Connections, keys in the
  keyring, ATT and GATT, HID over GATT; keys and the pointer.
- **B7, the person's side.** Reconnecting at start and wake, forgetting;
  the Bluetooth app - its mockup `docs/bluetooth.html` and its diagram
  `docs/bluetooth-architecture.png` drawn and agreed before it is built -
  This Machine and the devices indicator.

- **B8, sound to headphones.** Diego, 20 October: "I also have AirPods
  and google airbuds that use Bluetooth for audio". Both play over
  Classic's A2DP: AVDTP over L2CAP to agree a stream, an SBC encoder -
  every A2DP device takes SBC, AAC is the better codec both also take -
  and AVRCP for the volume and play and pause. SBC is a loop over samples,
  so it is C and a kit (`CLAUDE.md`, *Kits supply*), and the headphones
  are one more output of the audio server, its periods in a region as
  every period is. After B7, on the Classic stack B5 builds.

B5 comes before B6: the K380 is Classic, and so is A2DP.

---

## B1: the adapter named

Built 20 October, and held on the Mac (`testing.md` 18.711); waiting on
an adapter to be seen on the M700.

**Two decoders**, each with no hardware in it. `btusb_decode.c`, beside
the USB driver's other decoders, finds the HCI interface in a
configuration: setting 0 of class E0h/01h/01h, and its interrupt IN and
two bulk endpoints - and nothing of a second radio's interface, nor of
interface 1's voice settings. `hci_decode.c`, in
`user/drivers/bluetooth/` because the host stack takes it at B2, builds
commands, reads Command Complete and Command Status, decodes the three
reads, names versions and makers, and puts events back together.

**Events come in pieces.** The interrupt endpoint's packet is 16 bytes on
the adapters this is written for and an event can be 257, so
`hci_assemble` takes bytes until the header's length is met and says how
many it took, leaving the rest of a packet for the next event - as
Linux's `btusb.c` does.

**`xhci.c`**: `use_bluetooth` configures the three endpoints as
`use_midi` does its two, and `bt_command` sends each command on the
control endpoint - bmRequestType 20h, bRequest 0, the packet as the data
(Volume 4 Part B 2.2) - and reads events from the interrupt IN until the
answer to that command arrives, passing over up to sixteen others. It is
synchronous, as every device's setup in the driver is: `bulk`'s wait
goes on serving the other devices. Then one line in the log, and the
kind `USB_KIND_BLUETOOTH`, which `lsusb` calls Bluetooth.

A Realtek chip answers these four before its firmware is loaded - which
is how Linux's `btrtl.c` tells which firmware it needs - so B1 names
Diego's adapter whichever he buys, and B3 is what makes it work.

## B2: the stack as a process

Built 20 October (`testing.md` 18.712).

**The host stack** is `user/drivers/bluetooth/bluetooth.c`, the role
`bluetooth`, which init starts after the USB driver and only with one. It
makes the ring (`hciring.h`), hands it to the transport with ATTACH
(`hciproto.h`), and sets the controller up as Linux's `hci_core.c` begins
to: Reset, Read Local Version, Read BD_ADDR, Read Local Supported Features
and Read Buffer Size, one at a time, each sent when the last is answered.
Then one line in the log, what the adapter is and what it can do -
"bluetooth: USB port 10, Bluetooth 5.1 from Intel, ...: Classic and Low
Energy, SSP, 8 ACL buffers of 1021 bytes". The USB driver says nothing of
the stack: a machine with no controller hears nothing from it.

**It never waits on the controller.** A command goes into the ring and the
transport is told with SEND; the answer comes back into `up`, and the
transport says so with a pulse, which the stack hears on the receive where
it hears everything else - so a controller that never answers costs the
stack nothing but a line in the log, two seconds later.

**Three pulses, one per thing that happens.** An adapter is ready; there is
something in `up`; the adapter left. Each carries the adapter's generation,
a number the transport gives each one as it arrives, so news from one that
has since left is told apart. The transport never calls the stack, and a
pulse never waits, so neither process can hold the other.

**`xhci.c` is the transport and nothing more.** B1's naming left it: it
configures the three endpoints, keeps an event read always out on the
interrupt IN, puts each whole event into the ring, and sends each command
from the ring on the control endpoint. ACL data waits for B5: its
endpoints are configured, and a packet sent before then is counted and
dropped. One adapter for the stack; a second plugged in is said and left
alone.

**`btsim`** (`btsim.c`) is a controller in software, pure C, held on the Mac
by `test_btdecode` against the decoders the stack reads it with. Under
`opt/kosmos/bluetooth=simulated` the USB driver puts it where an adapter
would be, so the stack, the ring, the requests and the pulses are the real
ones. Every boot of the test harness has it, so the stack sets an adapter
up in every suite; `x86-bluetooth` checks it, with a controller and
without.
