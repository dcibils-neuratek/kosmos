# The ThinkPad T14 - the first real machine

Everything about bringing Kosmos up on hardware that is not QEMU: what the
machine is, what already runs, what has been built for it, what is proven,
what is blocked, and what is next.

`docs/targets.md` is the general document about what a target costs and
holds the comparison with the other candidates. **This one is the log.**
Where the two overlap, the table in `targets.md` §7 is the summary and this
is the detail.

---

## 0. What the machine turned out to be

**It arrived, and it is a Gen 2 rather than a Gen 1.** Everything below was
written against a Comet Lake T14 Gen 1 from a specification sheet; the
machine on the desk is Tiger Lake. Corrected here rather than quietly, because
three of this document's conclusions rested on the wrong part number.

From its own CPU-Z report:

| | |
|---|---|
| Board | `20W1S1Y500` - ThinkPad T14 Gen 2 |
| Processor | Intel Core i5-1145G7, **Tiger Lake-U**, 4 cores / 8 threads |
| Chipset | Tiger Lake-U/Y PCH rev 20, host bridge `8086:9A14` |
| Memory | **16 GB** DDR4-3200, one channel |
| Firmware | **UEFI** |
| ECAM | `0xC0000000` |

And the devices, which is what the drivers care about:

| device | at | id | class |
|---|---|---|---|
| Iris Xe graphics | `0:02.0` | `8086:9A49` | 03:00 |
| xHCI | `0:20.0` | `8086:A0ED` | 0C:03 |
| **Wi-Fi (CNVi)** | `0:20.3` | `8086:A0F0` | 02:80 |
| LPSS I2C | `0:21.0`, `0:21.1` | `8086:A0E8/9` | 0C:80 |
| LPC / ISA bridge | `0:31.0` | `8086:A082` | 06:01 |
| **HDA** | `0:31.3` | `8086:A0C8` | **04:03** |
| **NVMe** | `4:00.0` | Micron CT500P1SSD8 | 01:08 |

**Three of §7's questions are answered, and one of them changes the plan.**

**The keyboard is i8042.** The USB device list holds a fingerprint reader, a
webcam and Bluetooth - **no keyboard and no mouse**. So the keyboard and the
TrackPoint are on the PS/2 controller behind the LPC bridge, which is the
driver that is already written, in the build, and exercised by the display
harness on every gate. The touchpad is I2C-HID on the LPSS controllers, not
USB either.

**There is no VMD.** The NVMe is a plain PCIe device on bus 4 behind the
bridge at `0:06.0`. `pci_find` walks all 256 buses, so it is reachable;
`hal_bus_scan` walks bus 0 only, so `devices` will not list it until that is
widened.

**And there is no Ethernet at all.** §9 sizes "Intel I219, which is the
e1000e family" at ~1500 lines. **That device is not on this machine.**
Networking here is an Intel AX201 over CNVi - firmware upload and a
mac80211-shaped stack, which is a different order of work - so the honest
path to a network is USB Ethernet, and that means xHCI first.

**HDA is at class 04:03**, which is what `pci_find_class` looks for. The
decision to find a sound controller by class rather than by identifier was
made against QEMU's `8086:2668`; this machine's is `8086:A0C8`, and it needs
no change at all.

## 0a. The first boot on it, and what the stick had to become

**It booted, and GRUB ran.** The firmware found the stick, read its
partition table and launched `BOOTX64.EFI` - so Secure Boot was off and the
image was acceptable to real firmware. Then:

```
error: file '/boot/grub/x86_64-efi/boot.mod' not found.
Entering rescue mode...
grub rescue>
```

**GRUB could not load its own modules.** `grub-mkrescue` keeps them only
inside the El Torito FAT image and leaves nothing at that path on the
ISO9660 filesystem beside it. Under QEMU, GRUB's idea of its own root
resolved to the FAT image and the modules were there; on this firmware it
resolved somewhere else and they were not. **The ISO passed thirteen checks
here and failed on the machine**, which is the sharpest possible argument
for testing the artifact that ships.

So the stick is built here now - `tools/mkusb_image.py` - and nothing in it
depends on which filesystem GRUB thinks it booted from: a GPT with one EFI
System Partition, FAT, holding the loader, its configuration, its modules
and the kernel. One filesystem, the one a UEFI firmware is required to be
able to read.

**Two further things went wrong while fixing it, and both are worth
keeping.**

Assigning a twelve-byte partition name into a seventy-two-byte slice of a
Python `bytearray` *resizes* it. The GPT's entry table came out ten bytes
short, its CRC was computed over the wrong buffer, and firmware that checks
that CRC ignored the disk entirely - the machine fell through to the EFI
shell with nothing on screen to say why.

And linking twenty-two modules into the core image, `efi_gop` among them,
left GRUB unable to set a video mode at all: Kosmos came up with `none
attached` on a machine whose firmware had a perfectly good panel. Dropping
`grub-mkrescue`'s own 200 KB binary into the same image fixed it at once, so
it was the core rather than the layout. The core is minimal now - what is
needed to read the partition and run `grub.cfg`, and nothing else, because
a module that fails to load cannot be the module that loads modules.

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

**The keyboard is in the build and is what the x86 board types with.** The
pointer half is written and blocked; see §5 and §6.

That took a split. Both input drivers defined the same nine HAL functions,
so a board could take *both* its keyboard and its pointer from one of them
or neither - which is why this driver sat out of the build for a while.
Each driver now has its own names, and a per-board file binds them:
`hal/qemu-virt/input_bind.c` takes both from virtio, and
`hal/pc/input_bind.c` takes the keyboard from the i8042 and the pointer
from virtio.

**When the auxiliary port works, one line in that file changes.** Until
then the real keyboard driver is exercised every time the display harness
types anything, which is what stops it rotting while the other half is
worked out - and the x86 board keeps a pointer, which eleven display checks
need.

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

### `hal/pc/acpi.c` - the machine says how many processors it has

**This board answered "one processor" for as long as it existed**, and the
comment in `hal/pc/cpus.c` was right to: the count is in the ACPI MADT, one
entry per local APIC, and nothing parsed it. AArch64 asks PSCI, which will
say whether a processor exists without starting it. x86 has no equivalent
and has to read a table.

It now reads them. RSDP out of the EBDA or the BIOS area, then the XSDT -
or the RSDT on a machine old enough to have only that - then MADT for the
processors and the I/O APIC, and MCFG for where PCIe keeps its
configuration space.

```
[3/12] processor
       -> 4 processors
       -> 1 of them given new threads; SMPWORK=4 spreads them
```

Three things worth keeping:

- **No AML**, and that is the line. These tables are fixed-layout
  structures a C compiler can describe; the ACPI namespace is a bytecode
  language with its own interpreter, and nothing here needs one.
- **Everything is checksummed before it is believed.** A table is a
  structure at an address the firmware chose, in memory this kernel did not
  write. A table that fails its sum is skipped, and a missing table is a
  fact rather than a failure.
- **Counting is not starting, and the two were kept apart.** `cpu_on.c`
  refused until there was a local APIC driver and `cpu_secondary_entry`
  until there was a trampoline below 1 MB - so the machine reported four and
  scheduled on one, and said so. A count that arrived before either would
  have claimed processors it could not use. Both exist now, and §11 has what
  became of them on this machine.

Two checks in the x86-64 suite, which went from 27 to 29: the machine finds
the four processors QEMU was told to give it, and does *not* claim to be
scheduling on them.

**What it also found and nothing uses yet**: the local APIC and I/O APIC
addresses, and the ECAM base. That last one is what lets `pci.c` reach the
4096 bytes of configuration space PCIe has rather than the 256 that port
0xCF8 can address - and everything PCIe added, including where MSI-X tables
live, is above the first 256.

### `hal/pc/fb.c` - a screen on a machine with no ramfb

**M0's code path.** `ramfb` is QEMU's - the guest allocates memory and tells
the hypervisor to scan it out - and a ThinkPad has nothing of the kind. What
it has is firmware that set a mode before anything of ours ran, and a loader
that can be asked to pass the address on. `boot/x86_64/start.S` now asks:
bit 2 of the multiboot header flags requests a linear framebuffer at 32 bits
per pixel with no size preference, because the firmware knows the panel
better than this does.

`hal_fb_init` takes the loader's answer first and ramfb second, so QEMU is
unchanged and a laptop takes the other branch.

**The framebuffer is captured early, and that matters more than the code.**
`pc.h` already warned that the multiboot structure sits in RAM the page
allocator considers free - it ate the command line once - and the comment
ended "it is the same trap for the next field somebody wants". This was that
field: `hal_fb_init` runs at boot stage six, long after the allocator, so an
address read then would be plausible and wrong. On a machine with no serial
port that is a black panel and no way to ask why.

**QEMU's `-kernel` does not answer the video request.** Measured rather than
assumed: the flag stays clear, the fallback runs, and the display is the
1920x1080 it always was. So the one path that cannot be exercised under
emulation is the one a laptop depends on entirely - which is why the
decision is a pure function and `tools/test_loaderfb.c` asks it the awkward
questions on the host: **14 checks**, every rejection something a loader has
really done. The flag set with nothing behind it, a palette, EGA text,
24bpp, zero dimensions, and a pitch narrower than its own row - that last
one against 7744 and 1920, because `gfx.md` insists the pitch is almost
never width * 4 and a driver that tidied it would shear every line.

What is left for the machine is one question - *does GRUB fill the fields
in* - rather than a driver to debug on a dark screen. The boot log answers
it in one line:

```
-> from ramfb, which is QEMU's and has no equivalent on hardware
-> from the loader's, from the multiboot video request
```

Two failures that look identical from outside: a laptop that fell back has
no screen because there is no ramfb, and a laptop whose loader ignored the
request has no screen for a different reason and a different fix.

### `hal/pc/hda.c` - the sound controller a laptop actually has

**The first driver in this tree written for hardware rather than for QEMU.**
Everything before it either exists on both machines - the i8042, the PIC,
the PIT - or is QEMU's own with no counterpart on a laptop, which is what
`ramfb` is. Intel HDA is neither: it is a specification Intel has shipped in
every chipset since 2004, and QEMU emulates the controller faithfully enough
to develop against. That combination is worth more than either half.

**Found by class, not by identifier.** `pci_find` matched a vendor and a
device id, which is right for virtio - being virtio is what a virtio device
*is* - and useless here: QEMU's controller answers 8086:2668 and a Comet
Lake's answers something else, so a driver written against either id would
work on exactly one machine. `pci_find_class` shares the same bus walk and
asks class 4 subclass 3, which is High Definition Audio wherever it is
fitted.

**The shape is not virtio's shape, and everything else follows.** A virtio
sound device is a queue: hand over a period, the device consumes it, and the
queue depth is how much is in hand. HDA is a cyclic buffer that never stops -
the controller walks a descriptor list for ever, playing whatever bytes are
under it when it arrives. So:

- the **read position is the hardware's**, in `SDLPIB`, and the depth is
  derived from it rather than from a counter this driver keeps. Two
  interrupts coalescing is all it takes for a counter to disagree with a
  device, and the way that bug presents is audio written into the period
  being played;
- **one slot is always left free**, which is what makes the depth
  unambiguous - `write - play` is zero for empty and never wraps to zero for
  full;
- **a finished period is zeroed** in the interrupt handler. A queue that
  runs dry goes quiet; a ring that runs dry *repeats*, and an underrun here
  would be the last 5.8 ms of audio played over and over at 172 Hz.

**What it cost, and it is written into the driver because nothing would ever
have found it by reading.** `GET_PARAMETER` on the root node answered
correctly and the same call on the function group underneath it timed out,
with the registers saying the command pointer had moved and the controller's
had not. `RINTCNT` is not only an interrupt threshold: it is also how many
responses the controller will write before it treats the response ring as
full and stops consuming commands, and what restarts it is the driver
acknowledging `RIRBSTS`. With the response interrupt disabled there is no
status to acknowledge, so the count never clears and the controller answers
exactly one verb and then nothing, for ever. The interrupt is enabled and
`INTCTL.CIE` is left clear, so the status bit is set, every response clears
it, and the pin never moves.

**The codec graph is walked for the hard case rather than the easy one.**
QEMU's codec has one function group, one DAC and one output pin whose
connection list holds the DAC directly - so a driver written against it
alone would look one level deep and find nothing at all on a laptop, where
the pin's list holds a mixer and the mixer's list holds the converter. Two
levels are walked, a selector is selected and a mixer's input for that entry
is unmuted, and amplifiers are set to the **offset field of their own
capabilities** - which is 0 dB, the one setting that means the same loudness
on two different codecs, where the maximum on a laptop's speaker amplifier
is distortion.

**A pin that is wired to nothing is skipped**, from the configuration
default's port-connectivity field. That is the field that will decide which
of the ALC3287's pins is the speaker and which is the headphone jack, and it
is the one thing here that QEMU cannot test, because its codec has one pin
and every answer is the same answer.

**The stream runs from initialisation and is never stopped**, which is 172
interrupts a second for as long as the machine is on. It buys the thing the
audio server is built around - a device that says when it wants more - and
the fix when it matters is named rather than guessed: stop `RUN` after some
number of consecutive silent periods and start it again on the next write.
That is a state machine and it is not worth writing before there is a
battery to measure it against.

**A board binds the HAL, and sound is the second subsystem to need it.**
`hal/virtio/snd.c` used to define `hal_snd_*` directly, which was fine while
both boards took sound from the same file. `hal/pc/snd_bind.c` now asks the
HDA controller first and virtio second, exactly as `fb.c` asks the loader
first and ramfb second, and `hal/qemu-virt/snd_bind.c` is the same file with
one answer in it. `hal_snd_describe()` is what the boot log prints, for
`hal_fb_describe`'s reason: "no sound" covers a controller that is not
there, a controller with no codec on its link, and a codec whose output pin
is wired to nothing, and those are three different faults with three
different fixes that all sound identical.

**`make x86` now gives the machine an HDA controller instead of a virtio
sound device**, for the argument `input_bind.c` makes about the keyboard: the
driver that has to work on a laptop should be the one that is exercised
every time somebody runs the system. `virt` still runs virtio-sound, so
neither driver is orphaned.

### The boot path, end to end - and the three faults it was hiding

**`-kernel` is not a loader, and every x86 boot in this project went through
it.** QEMU reads the multiboot header, copies the image in and jumps. It does
not answer the video request - measured, not assumed - so the one path a
laptop depends on entirely had never run once, while `test_loaderfb.c` asked
the decision fourteen careful questions on the host and every one passed.

`make x86-iso` builds the real artifact: GRUB, a filesystem, and an image
that boots the way the ThinkPad will. `make x86-uefi` runs it under OVMF -
the same EDK II a ThinkPad's firmware is built from. It works, and the
screenshot in `docs/screenshots/` is a 1280x800 framebuffer the *firmware*
set up.

**Three faults came out of it and not one of them is a driver.** Every one is
fatal on a machine with more than a gigabyte of memory, and every one is
silent on a machine with no serial port.

**1. The boot page tables mapped one gigabyte.** A multiboot loader hands
over a 32-bit pointer, so its information structure may be anywhere below
four. With `-m 4G` QEMU put it at 0x7ffe2349 and the first read of it faulted
at boot stage three - before the framebuffer exists. `start.S` builds four
page directories now, which is 16 KB of `.bss` and no CPUID check, because
2 MB pages have been mandatory since PAE where 1 GB pages are optional.

**2. `fine_end - ram.base` wrapped.** `mmu_init` mapped the bottom of RAM a
page at a time and started from `ram.base`, which worked for as long as the
largest usable region was the one the kernel had been loaded into. That is
what `-kernel` gives: RAM at 1 MB with the image at the bottom of it. Booted
through GRUB under UEFI the same machine reports its largest low region at
**0x900000**, because the firmware has its own allocations below that, and
the image sits at 0x100000 *outside* it. `fine_end` was 0x800000, the
subtraction wrapped, and the map asked for four quadrillion pages.

**3. The framebuffer was never mapped.** ramfb's pixels are memory the guest
allocated, so they are inside the identity map and a raw pointer works -
which is why the line did not exist and why nothing missed it. A firmware
framebuffer is at 0x80000000. The first pixel written faulted, the fault
handler tried to say so, and the console lock was already held by the write
that faulted: **`spinlock: console held by 0, wanted by 0`**, for ever. On a
laptop that is a dead black screen, dead in a way that cannot even reach a
serial port. It goes through `mmu_map_device` now.

**And one thing about GRUB worth writing down.** With `insmod all_video` it
picks its own driver - under QEMU the bochs one - and hands over **800x600
at 24 bits per pixel**, which `loader_fb.c` refuses because everything above
`struct fb` treats a pixel as one 32-bit word. `insmod efi_gop` asks the
firmware's own GOP instead and gives the panel's mode at 32 bits. A laptop
has no bochs, so this may be a QEMU artifact; it costs one line to be sure.

**What this settles about the CSM question.** `docs/targets.md` listed "is
there a CSM" as a blocking unknown worth half an hour on the machine. It is
no longer blocking: this GRUB is built for `x86_64-efi` only, and it boots a
Multiboot 1 kernel under UEFI perfectly well. If the T14's firmware has no
CSM at all, the plan is unchanged.

### The panel has the boot log from stage two

**§8 says the constraint that shapes everything is the missing serial port.
This is the part of the system that finally answers it.**

The display was stage six of twelve, so stages one to five went to a cable.
The three faults above were at stages three, four and five. On the machine
this is aimed at, each of them is a black panel and nothing to read - not a
message that is hard to find, *nothing*, with no way to tell a bad loader
from a bad page table from a bad memory map.

`hal_fb_early` asks the board for a framebuffer before there is a page
allocator. Only a board whose firmware already set one up can answer, which
is exactly the board that needs to: ramfb is the guest allocating pixels,
and there is nothing to allocate from yet, so `qemu-virt` says no and loses
nothing.

**The subtlety is the address.** What makes the early screen possible is the
identity map in `start.S`, and `mmu_init` replaces it - after which the same
pixels answer somewhere else. So `hal_fb_remap` is asked the instant
`mmu_init` returns, **before the next character is printed**, and
`console_rebase_screen` moves the console's pointer without repainting,
because what is on the screen is in the memory both addresses name.

The first version left a three-line gap between the two, and it deadlocked
exactly as an unmapped framebuffer does: the fault is inside a console
write, the handler blocks on the lock that write is holding, and the machine
says `spinlock: console held by 0, wanted by 0` for ever.

**And the check for it is asked of the machine rather than of a stopwatch.**
The first attempt screendumped seven seconds in, on the theory that the
display stage had not been reached - and passed with the whole feature
disabled, because OVMF, GRUB and twelve boot stages together take under two
seconds. Sampling every half second from two seconds found no window at all.
The boot log now says which of the two ways the panel got it, and the
harness reads that line. Verified by reinstating the fault: with
`hal_fb_early` returning false the check fails, and it is the only one that
does.

### `hal/pc/apic.c` - the controller a machine built this decade has

**The 8259 pair is from 1981 and may not be there.** Intel has been removing
the legacy PIC and the 8253 from UEFI-only platforms, and a kernel driving
only those gets no scheduler tick on such a machine - which presents as a
boot that prints all twelve stages and then stops, with nothing else visibly
wrong. That was the one failure on this target with no workaround: writing
an APIC driver with the laptop sitting idle is a day's work.

Both exist now and neither is chosen at build time. `irq_bind.c` asks ACPI
whether this machine described an I/O APIC and takes that path when it did.
`opt/kosmos/irq=pic` forces the other, so **both are exercised on every
gate** rather than the fallback rotting until the first old machine.

```
-> 250 Hz off the local APIC's own timer, calibrated against the 8253
-> interrupts: an I/O APIC and the local APIC's own timer

-> 250 Hz off the 8253 through a pair of 8259s
-> interrupts: a pair of 8259s, remapped clear of the exceptions
```

**MSI is the answer to a problem with no other one.** Under the 8259 a
device's Interrupt Line register says which line it uses. Under an I/O APIC
that register means nothing: the pin goes to one of the chipset's four
interrupt links, and which input those land on is described in ACPI's
`_PRT` - **which is AML**, and `acpi.h` says there is no interpreter here.
The standard PCIe swizzle was tried first and is right for slots and wrong
for a chipset's own integrated devices, which is exactly what the HDA
controller is. With MSI the question does not arise: the device writes a
word to the local APIC's address, with a vector this kernel chose, at a
processor this kernel named. No routing table, no interpreter, and no line
shared with three other devices that each have to be asked whether it was
theirs.

**And an ordering bug underneath it, which is the interesting half.**
`pci_enable` switches a device to MSI where it can, and that is only
possible on the APIC path - so it asks which controller is running. It was
asking at stage seven and the controller was not decided until stage eleven,
so every device was told *no*, took a line the I/O APIC was not routing, and
the tone played for three times its length because nothing ever retired a
period. The binding decides on first use now.

**Reordering the boot was the other answer and the worse one.** The display
exists at stage six precisely so that a failure after it is visible on a
machine with no serial port, and moving the riskiest new code ahead of the
only instrument there is would be the wrong trade.

**What this unblocks.** `hal/pc/cpu_on.c` has refused to start a second
processor because a processor is started by sending INIT and two STARTUP
inter-processor interrupts *through the local APIC*, and there was not one.
There is now. This machine has eight cores and Kosmos uses one.

**And on this machine it was refused, for a reason nobody could see.** The
size check turned down any I/O APIC above sixty-four inputs, and this
chipset has a hundred and twenty:

```
IOAPIC[0]: apic_id 2, version 32, address 0xfec00000, GSI 0-119
```

from a Linux boot log of a T14 Gen 2i. The refusal was stored in `apic.c`
and the 8259's description printed instead, so the boot log said `a pair of
8259s` and nothing about what had been turned down. 0.10.18 takes the size
the register reports, reads the local APIC's mode out of IA32_APIC_BASE
before touching any of its registers - a firmware that left it in x2APIC
mode is refused with a sentence rather than a hang - and prints the reason
on every fallback. No QEMU configuration has a 120-input I/O APIC, so the
decode is a pure function in `apic_decode.c`, asked on the host with this
machine's register value.

**And on this machine it now engages.** The next boot, read back with `log
interrupts` at the prompt:

```
[2.782] [11/12] timer and interrupts
[2.782]         -> interrupts: an I/O APIC and the local APIC's own timer, scheduling priority
```

### ACPI is invisible under UEFI, and that is the next thing to fix

**Measured, and it corrects something written above.** The same image, the
same machine, four processors, booted two ways:

```
UEFI, through GRUB:   -> 1 processor
                      -> interrupts: a pair of 8259s

BIOS, through -kernel: -> 4 processors
                      -> interrupts: an I/O APIC and the local APIC's own timer
```

`find_rsdp` looks in the two places a BIOS leaves the pointer: the word at
0x40E that names the Extended BIOS Data Area, and the read-only area from
0xE0000. **Both are legacy conventions.** UEFI hands the RSDP to the loader
in the EFI Configuration Table and is not obliged to leave a copy anywhere a
scan would find it. OVMF does not.

So on the machine this target exists for, booted the way it will actually
boot, Kosmos gets no processor count, no ECAM, and **no APIC** - falling
back to the legacy chips that may not be there. That is precisely the hang
the APIC was written to prevent, arriving by a different door.

**This section previously said a Multiboot 2 header was "not needed at
all".** That was right about booting and wrong about everything after it.
Multiboot 1 has no way to carry an RSDP; Multiboot 2 has a tag for exactly
this - two of them, one per ACPI revision - and GRUB fills them in from the
firmware it was launched by.

It is the next thing to build, and it is not large: a second header beside
the first, a different information structure to walk, and `multiboot2`
rather than `multiboot` in the generated `grub.cfg`. The image can carry
both headers and let the loader pick.

### The boot log stopped naming the wrong driver

`kernel/main.c` printed `keyboard: virtio-input, negotiated and polled like
the serial line` as a string literal - board knowledge in
architecture-independent code, and false the moment a second driver existed.

The board now answers `hal_keyboard_describe()` and
`hal_pointer_describe()`, and the log reads:

```
[7/12] input devices
       The i8042 at 0x60, then the PCI bus for everything else.
       -> keyboard: i8042, scancode set 1, on IRQ 1 - the chip a laptop still has
       -> pointer: the i8042 auxiliary port, relative counts made absolute over 0..32767
```

**A boot log that names the wrong driver is worse than one that says
nothing**, because it is the first thing anybody reads when a board does not
work - and on this machine it is the *only* thing, since there is no serial
port.

The pointer's line read `made absolute32767` on every boot until 0.10.17:
the board's sentence ended where the kernel appends the range, and the
`over 0..` quoted above was in this document and nowhere in the code. The
keyboard's still said `polled` after its interrupt had been armed.

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

**And the sound, read back off the wire rather than off the boot log.**
Every other check in `run_x86.py` is a string the machine printed, which is
the machine's own account of itself. Audio is the one subsystem where that
is not enough: a driver that programs the controller wrongly prints exactly
what a working one prints, and the whole of the difference is in bytes
nobody in the guest ever sees again. So QEMU's `wav` backend writes what it
was handed to a file and the harness reads it:

```
beep: 440 Hz, 333 ms of sound in 311 ms, 58 periods
   -> 333 ms of tone, 440 Hz measured from the zero crossings,
      and 0 samples outside it that are not silent
```

Three claims about the driver in one capture. The samples arrived, and in
the right format - a wrong rate or channel count reads back as the wrong
pitch rather than as an error. They arrived **once**, so the cyclic buffer
is not repeating a period it has already played. And the silence on either
side is real silence, which is the same claim from the other direction.

---

## 6. What is blocked, and exactly what is known

**The auxiliary port sent nothing under QEMU for months, and it streams
now.** What was established then, by instrumenting the drain rather than by
reading code, is worth keeping because every item on it stayed true:

- the handshake **succeeded** - after `0xA8` the configuration byte read
  0x40, translation on with both ports enabled, and `0xFF` reset, `0xF6`
  defaults and `0xF4` enable were each acknowledged;
- the drain **ran** and **saw keyboard bytes**, with the status register's
  auxiliary bit correctly clear for those - `st=29 d=30` is the make code
  for A and `d=158` its break;
- **no auxiliary byte ever arrived**, whether the movement was sent with
  `input-send-event` or with the monitor's own `mouse_move`;
- `info mice` named `QEMU PS/2 Mouse` as the current mouse;
- `vmport=off` - the obvious suspect, since q35 carries a vmmouse - changed
  nothing.

So the board stayed on virtio input and the driver was bound to nothing, on
the grounds that a green tree with a precise unknown is worth more than a
red one with a guess.

**The machine is what moved it.** On the T14 the TrackPoint said nothing
until the configuration byte stopped carrying `CFG_AUX_DISABLE`; its packets
were misframed until the framing check required the two overflow bits clear
as well as bit 3; and the controller was read once a scheduler tick until
IRQ 1 and 12 were armed and drained. With all three in the build, QEMU's
PS/2 mouse streams as well - `mouse_move -100 -100` arrives as `18 9c 64` -
and **the driver the laptop runs is exercised on every gate**:
`tools/run_x86.py` boots the desktop with no tablet and clicks the Deskbar's
menu open through it. Which of the three QEMU needed has not been isolated.

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

**And the missing port turned out to be a fault as well as a constraint.**
Nothing in `hal/pc/uart.c` asked whether COM1 existed, because QEMU always
provides one. On a machine without one every register reads 0xFF, and 0xFF
in the line status register means a byte has arrived - so every read of the
console's input came back with a phantom 0xFF, for ever. The shell still
worked, because the keyboard is asked first and the line editor drops a
non-printable byte without a word. What gave it away was a core at 100% with
the console server taking 94%, and every button on the desktop going down
and never coming up: the window manager posted sixty-four phantom keys a
pass into the focused window's queue, and the mouse release was the oldest
thing in it.

Reproduced under QEMU with `-serial none` - an idle desktop went from 20% of
a host core to 100% every time, and in one run the Deskbar's button stayed
down - and fixed by asking the 16550's scratch register first.
`tools/run_x86.py` boots with no serial port at all to keep it fixed. Nothing
can be read from a machine with no serial line, so it asks QEMU whether the
processor ever halts - which failed against the build before the fix, where
the stuck button, depending on timing a harness does not reproduce on demand,
did not.

---

## 8a. Where the register values come from

**Intel's own driver is a legitimate reference, and the licence is the
reason to check rather than the reason to stop.**

The Linux kernel as a whole is GPL-2.0, and that is what made this look
closed. `drivers/gpu/drm/` is not the whole kernel: it inherits the
X11/XFree86 lineage, and Intel contributed much of i915 under **MIT**.
`i915_pci.c` carries an Intel copyright and an MIT permission notice, which
is the same licence Kosmos uses - so the existing rule covers it exactly.
Vendored files keep their authors' notices byte for byte and never get the
Kosmos header; `LICENSE` names them.

    https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/i915_pci.c

**Check the notice on every file, every time.** The tree is mixed. One file
being MIT says nothing about the one beside it, and the SPDX line or the
header is the authority - not the directory it happens to sit in.

**What it is for, and what it is not.** i915 is six figures of lines welded
to DRM/KMS, dma-buf, workqueues and an allocator, none of which exist here
and one of which this kernel forbids outright. It cannot be dropped in and
nobody should try. What it *is* is the authoritative statement of what the
silicon's registers are, written by the people who built it - device
identifiers, register offsets, bit meanings, and the order operations have
to happen in. `CLAUDE.md` requires hardware offsets to be verified against a
datasheet rather than recalled, and this is a datasheet that compiles.

Intel also publishes the Tiger Lake Programmer's Reference Manuals, which
are the primary source and carry no licence question at all. Where the two
disagree, the PRM is the document and the driver is what somebody had to do
about it.

**The part of the graphics device this project would reach first is the
display engine** - the cursor plane and the scanout address - because it is
plain MMIO. The 2D blitter is not: Gen11 removed legacy ring submission, so
the BLT engine is behind execlists, contexts and per-context page tables.
That is a GPU driver, not a 2D driver, and it is not the small first step it
sounds like.

---

## 9. What is still missing

| | | rough size |
|---|---|---|
| ~~Multiboot 2~~ | **written and in the build.** Not to boot - Multiboot 1 boots fine under UEFI - but because ACPI cannot be found without it. Four processors and the APIC where there were one and the 8259 pair | done |
| Framebuffer from the loader's boot information | **written and proven end to end** under GRUB + OVMF; 14 host checks and 7 boot checks | done |
| i8042 keyboard | **in the build**, exercised by the display harness | done |
| More than a gigabyte of RAM | **fixed**: 4 GB of boot page tables, and the low region chosen by what can be mapped | done |
| A bootable stick | **`make x86-iso`**, hybrid GRUB image | done |
| i8042 auxiliary port | **in the build**: the TrackPoint moves the pointer on the machine, and a click through QEMU's PS/2 mouse is checked on every gate | done |
| ACPI: RSDP, XSDT, MADT, MCFG. **No AML** | **written and in the build** | done |
| ~~Local APIC / IOAPIC / MSI~~ | **written and in the build**, both paths chosen at run time and both tested | done |
| PCI over ECAM from MCFG | rework of `pci.c` | ~200 |
| ~~NVMe~~ | **written and in the build**, a file written, the machine killed, and the file read back | done |
| Intel I219, which is the e1000e family | new | ~1500 |
| Intel HDA | **written and in the build**, tone captured and measured | done |
| The ALC3287's pin layout - which pin is the speaker | needs the machine | §6 |
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
2. ~~**The desktop.**~~ **Running on the machine**, TrackPoint included. It
   cost more than the few hundred lines this said: a framebuffer mapped with
   the wrong memory type, a poll reply too large to send, an eighteen-second
   mount probe, and a serial port that was not there - the decision log from
   0.10.16 on has each.
3. **Cores and time.** ACPI MADT for the real processor count - eight or
   twelve, against the four this kernel has ever seen - and an APIC timer.
4. ~~**Storage.** NVMe, so `/home` outlives the boot.~~ **Written.** It has
   still never seen this machine's Micron drive - and the one thing it
   refuses is a namespace whose blocks are not 512 bytes, which it says
   rather than failing quietly.
5. **The rest.** I219, then HDA, then xHCI, then the touchpad.

---

## 11. Next three things

1. **xHCI and the USB core**, which is the largest thing left. The ordering
   argument for it - "this machine can run from a USB stick with no storage
   of its own, so USB comes before NVMe" - has expired: NVMe is written, so
   the reason to do USB first is now the keyboard and the touchpad rather
   than storage, and this machine's keyboard turned out to be an i8042. What
   USB buys here is the trackpad and anything plugged in.
2. **The other seven cores.** Three start under QEMU, through `-kernel`
   and through GRUB on OVMF: INIT and STARTUP through the local APIC, a
   trampoline under 1 MB, `swapgs` and a TSS per core, and a local APIC
   timer each. On this machine the APIC engages now and none of them has
   come up: the boot said `0 of the others in the kernel too` and nothing
   about why, so every processor asked about gets a `cpu_on:` line, and `log
   processor` is the next question. The slots stop at four (`NR_CPUS`), so
   four of its eight would run. Spreading threads across them waits for a
   TLB shootdown, which x86 needs and AArch64 does not.
3. **PCI over ECAM**, now that MCFG says where that is. `pci.c` reaches 256
   bytes per function through port 0xCF8 and PCIe has 4096.

And still open, whenever it is cheap: **which change made QEMU's PS/2 mouse
stream** - §6 names the three. One reading of `pckbd.c`, and it decides
nothing else.
