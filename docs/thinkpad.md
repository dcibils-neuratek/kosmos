# The ThinkPad T14 Gen 1 - the first real machine

Everything about bringing Kosmos up on hardware that is not QEMU: what the
machine is, what already runs, what has been built for it, what is proven,
what is blocked, and what is next.

`docs/targets.md` is the general document about what a target costs and
holds the comparison with the other candidates. **This one is the log.**
Where the two overlap, the table in `targets.md` §7 is the summary and this
is the detail.

---

## 1. The machine

From Lenovo's PSREF for the T14 Gen 1 (Intel), March 2022.

| | |
|---|---|
| processor | 10th generation Core i5/i7 - Comet Lake-U. i5-10210U/10310U and i7-10510U/10610U are 4 cores and 8 threads; i7-10710U/10810U are **6 cores and 12 threads** |
| graphics | Intel UHD Graphics, integrated. Optional NVIDIA MX330 |
| chipset | Intel SoC platform |
| memory | DDR4-2666: 8 or 16 GB soldered, plus one SO-DIMM slot, dual-channel |
| storage | M.2 2242 **PCIe NVMe 3.0 x2**, or M.2 2280 **PCIe NVMe 3.0 x4** with Opal2. **There is no SATA option** |
| ethernet | Intel I219-LM (vPro) or I219-V, one RJ-45, Wake-on-LAN |
| wireless | Intel Wi-Fi 6 AX201, 802.11ax 2x2, Bluetooth 5.1 |
| ports | 2x USB-A 3.2 Gen 1, USB-C 3.2 Gen 1, USB-C 3.2 Gen 2 / **Thunderbolt 3**, HDMI 1.4b, microSD, 3.5 mm audio, side docking connector |
| display | 14": 1366x768 TN, 1920x1080 IPS in four variants, or **3840x2160** IPS. Touch on two of them |
| input | 6-row spill-resistant keyboard, **TrackPoint** and a multi-touch touchpad - Lenovo calls the pair UltraNav |
| audio | Intel HD Audio, **Realtek ALC3287** codec, 2 W stereo |
| camera | HD 720p, or IR + 720p hybrid, with a ThinkShutter |
| security | firmware TPM 2.0 in the chipset, or a discrete TCG-certified TPM 2.0. Optional fingerprint reader, match-on-chip |
| firmware | UEFI with Secure Boot, self-healing BIOS |
| power | 45 W or 65 W USB-C PD adapter, 50 Wh battery |
| **serial port** | **none** |

---

## 2. Why this machine and not the other one

`docs/targets.md` §6 plans an Alienware x14 R1 in detail and then stops on
one question, saying in as many words that nothing else about that target
should be planned until it is answered:

> **Is the internal keyboard i8042 or USB?** … If this one is USB … there is
> **no input at all** until xHCI works, and the first satisfying milestone
> moves from ~2800 lines to ~9000.

**A ThinkPad answers it the good way, and the specification says so.** The
keyboard reaches the system as PS/2 through the embedded controller, and the
TrackPoint arrives on the *same controller's auxiliary port*. One driver of
around 250 lines is therefore a keyboard **and** a pointer - and on the x14
the pointer needed the whole USB stack first.

Two other differences point the same way:

- **A real RJ-45 with an Intel I219 behind it.** Networking does not have to
  go through a USB dongle and a CDC-ECM driver.
- **The panel is driven by the firmware's GOP**, like any other UEFI
  machine, so the framebuffer is the loader's boot information and nothing
  more.

And one that points the other way:

- **Storage is NVMe or nothing.** There is no SATA in the table, so AHCI -
  which would have been the smaller driver - buys nothing here.

---

## 3. What already runs, and must not be rebuilt

`arch/x86_64` is finished and tested: long mode, four-level paging with W^X
and NX, the GDT and IDT, the exception vectors, `syscall`/`sysret`, the
context switch, the TSC, and an interrupt stack table for #PF and #DF.
**136 checks** run against it.

And the whole of Kosmos above the kernel - 101 programs, the servers, Lua,
the window manager, the browser, the TCP/IP stack - already runs unchanged
on both architectures. That is the part a port normally spends its life on
and it is done.

**Two things in `hal/pc` are closer to this machine than they look:**

- `memory.c` already reads a **multiboot memory map**, which is what GRUB
  hands over on a UEFI machine as well.
- `timer.c` is the 8254 and `pic.c` the 8259, both of which Comet Lake's PCH
  still implements. **They may work unmodified on the first boot**, which
  would defer the whole APIC milestone past the first picture.

That last one is a hypothesis, written down as one. It is cheap to test and
expensive to assume.

---

## 4. What has been built for this target

### `hal/keys.c` - one input language, one place

The two keymaps, the escape sequences for keys that are not characters, and
the rules about caps lock and control, moved out of `hal/virtio/input.c` into
a file both drivers share.

**They can share it because of history.** `hal/virtio/input.c` is handed
Linux evdev key codes and a PS/2 controller sends scancode set 1 - and those
are the same numbers through the whole typing block, because evdev's codes
were taken from the XT scancodes in the first place. `KEY_A` is 30 because
the original PC keyboard sent 0x1E for A.

**So a laptop keyboard needs no keymap of its own**, which is worth stating
rather than relying on quietly. Above 83 the two diverge, and the extended
keys - the ones behind an 0xE0 prefix - never agreed at all; those are a
short table in the i8042 driver, holding only the keys somebody presses in
a shell.

In the build, on both architectures.

### `hal/pc/i8042.c` - the keyboard and the TrackPoint

Written; **the keyboard half is proven and the pointer half is blocked.**
See §5 and §6.

Design decisions worth keeping:

- **Polled, not interrupt-driven.** `hal.h` asks for `hal_getchar` and
  `hal_pointer_poll`, both of which are questions rather than
  announcements. The virtio driver uses interrupts because a virtqueue is
  asynchronous by construction; a controller with two bytes of buffer is
  not. It also means **the first hardware boot needs no APIC and no
  interrupt routing at all**, which moves a whole milestone after the first
  picture instead of before it.
- **One drain, two devices.** The keyboard and the auxiliary port share port
  0x60 and the status register says which one a byte came from, so there is
  one drain called by whichever question arrives first, sorting into two
  queues. Reading only when asked for a character would lose mouse packets,
  and the other way round.
- **A TrackPoint is relative and `hal_pointer_poll` is absolute.** A tablet
  reports where it is; a mouse reports how far it moved. So the position
  lives in the driver, and the range it reports is the driver's own
  invention - which `hal.h` permits, because its rule is the device's own
  units *with the range beside them*. A made-up range is honest as long as
  it is stated rather than assumed.

### The boot log stopped naming the wrong driver

`kernel/main.c` printed `keyboard: virtio-input, negotiated and polled like
the serial line` as a string literal - board knowledge in
architecture-independent code, and false the moment a second driver existed.

The board now answers `hal_keyboard_describe()` and
`hal_pointer_describe()`, and the log reads:

```
[7/12] input devices
       The i8042 at 0x60, then the PCI bus for everything else.
       -> keyboard: i8042, scancode set 1, polled - the chip a laptop still has
       -> pointer: i8042 auxiliary port, relative counts made absolute over 0..32767
```

**A boot log that names the wrong driver is worse than one that says
nothing**, because it is the first thing anybody reads when a board does not
work - and on this machine it is the *only* thing, since there is no serial
port.

---

## 5. What is proven

With the x86 board switched to the i8042 and the virtio input devices
detached from QEMU:

```
sendkey p, w, d, ret   ->  pwd
                           /
                           kosmos>

sendkey up             ->  recalled `pwd` from the shell's history
sendkey ret            ->  ran it again
```

That exercises the controller handshake, the configuration byte, scancode
set 1, the shared keymap, the modifier state, **and** the extended keys -
because the up-arrow arrives as 0xE0 0x48 and has to come out as `ESC [ A`.

`sendkey` goes through QEMU's own input plumbing rather than the serial
line, so what it proves is the driver rather than the console.

---

## 6. What is blocked, and exactly what is known

**The auxiliary port sends nothing under QEMU.** Established by
instrumenting the drain rather than by reading code:

- the handshake **succeeds** - after `0xA8` the configuration byte reads
  0x40, which is translation on with both ports enabled, and `0xFF` reset,
  `0xF6` defaults and `0xF4` enable are each acknowledged;
- the drain **does run** and **does see keyboard bytes**, with the status
  register's auxiliary bit correctly clear for those - `st=29 d=30` is the
  make code for A and `d=158` its break;
- **no auxiliary byte ever arrives**, whether the movement is sent with
  `input-send-event` or with the monitor's own `mouse_move`;
- `info mice` names `QEMU PS/2 Mouse` as the current mouse;
- `vmport=off` - the obvious suspect, since q35 carries a vmmouse in its
  device tree - changes nothing.

**So the x86 board is back on virtio input and the driver is not in the
build.** That is deliberate: the display harness spends eleven checks on the
pointer and a desktop without one is not a desktop. A green tree with a
precise unknown is worth more than a red one with a guess.

**The machine itself is not blocked by this.** A TrackPoint is a real PS/2
device rather than an emulated one, and the half that decides whether a
laptop can be typed on at all is the half that works.

---

## 7. The three questions the machine answers in half an hour

Boot any Linux stick and read:

1. **`dmesg | grep i8042` and `cat /proc/bus/input/devices`** - confirm the
   keyboard and TrackPoint are PS/2, and find out whether the touchpad is
   PS/2 or `i2c_hid`. The first two decide the milestone; the third only
   decides whether the touchpad works on day one.
2. **Is there a CSM or Legacy Boot option in the BIOS?** If there is, the
   first boot can use the multiboot 1 header that already exists and **no
   new loader code at all**. If there is not, it is Multiboot2 under a UEFI
   GRUB, or a stub of our own.
3. **`lspci -nn`** - whether the NVMe is behind Intel VMD or a BIOS "RAID"
   mode, which is the thing that complicated the x14.

---

## 8. The constraint that shapes everything: no serial port

There is nowhere to print to. `docs/hal.md` says a UART is the first thing a
new board needs, and this board does not have one, so the order inverts:
**the framebuffer is the debugger**, and the first milestone is not "it
boots" but "it can tell you that it did".

The kernel already draws its boot log to a framebuffer and
`console_attach_screen` already exists, so what is needed is the address of
the panel and nothing else. A netconsole becomes possible once the I219
works, and not before.

---

## 9. What is still missing

| | | rough size |
|---|---|---|
| Multiboot2 header, or a UEFI stub - depends on question 2 | new | ~300, or ~1000 |
| Framebuffer from the loader's boot information | `hal_fb_init` has the shape | ~100 |
| i8042 keyboard and auxiliary port | **written**, keyboard proven | done, unwired |
| ACPI: RSDP, XSDT, MADT, MCFG. **No AML** | new | ~400 |
| Local APIC / IOAPIC / MSI | new | ~600 |
| PCI over ECAM from MCFG | rework of `pci.c` | ~200 |
| NVMe | new | ~800 |
| Intel I219, which is the e1000e family | new | ~1500 |
| Intel HDA and the ALC3287 codec | new | ~1500 |
| xHCI and the USB core | new | ~5000-7000 |

**Almost all of it is testable under QEMU before the machine is touched**,
which is unusually favourable and decides the order of work:

| | how it is tested first |
|---|---|
| i8042 | q35 has a PS/2 controller |
| NVMe | `-device nvme` |
| Intel HDA | `-device intel-hda` |
| e1000e, which the I219 belongs to | `-device e1000e` |
| xHCI | `-device qemu-xhci` |
| ACPI, APIC, ECAM | q35 provides real tables |
| UEFI | `edk2-x86_64-code.fd`, already on the development machine |

Only the loader cannot be finished under emulation, because which loader is
right depends on a fact about the laptop's firmware.

---

## 10. Order

0. **A picture.** Boot from a stick; the boot log on the panel.
1. **A prompt.** i8042, and `neofetch` typed on real silicon.
2. **The desktop.** The TrackPoint through the auxiliary port. This is the
   photograph worth taking, and it is a few hundred lines away.
3. **Cores and time.** ACPI MADT for the real processor count - eight or
   twelve, against the four this kernel has ever seen - and an APIC timer.
4. **Storage.** NVMe, so `/home` outlives the boot.
5. **The rest.** I219, then HDA, then xHCI, then the touchpad.

---

## 11. Next three things

1. **Find out why QEMU's PS/2 mouse will not stream.** One reading of
   `pckbd.c`, and it decides nothing else.
2. **Split the keyboard entry points out of `hal/virtio/input.c`**, so a
   board can take its keyboard from one driver and its pointer from another.
   The PC board wants that either way.
3. **ACPI**, which is what turns four processors into twelve.
