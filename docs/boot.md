# Boot

How Kosmos gets from firmware to its first instruction on a PC, and why it is
done this way. **Written because the ThinkPad failed to boot, in different
ways, on 11, 12 and 13 September 2026, and the fixes that did not work are as
worth keeping as the one that did.**

The short version: **Kosmos has its own UEFI loader, `boot/efi/`, and it
replaced GRUB on 13 September.** It claims the kernel's memory from the
firmware by address, refuses on the screen what it cannot claim, checks the
kernel byte for byte before and after the firmware lets go, repairs what
changed and says so - on the screen by its own hand as well as through the
firmware's console, because the ThinkPad's console showed none of it. The
x86-64 kernel moved from 1 MB to 16 MB with it, because the firmware QEMU runs
keeps memory under the old place.

AArch64 is not in this document: QEMU `virt` loads `build/kosmos.elf` at
0x40080000 and jumps, and that has never been the problem.

---

## 1. The paths

| how it boots | who loads the kernel | used by |
| ------------ | -------------------- | ------- |
| QEMU `-kernel` | QEMU's own Multiboot loader, straight into `_start` | `make x86`, `make test`'s x86 suites |
| UEFI | the firmware runs `\EFI\BOOT\BOOTX64.EFI`, Kosmos's loader, which enters `_start` | the ThinkPad, `make x86-uefi`, `run_uefi.py` |

**The stick**, which `make usb` writes and `tools/mkusb_image.py` builds: a
GPT disk with one FAT32 EFI System Partition of 192 MB (the size is FAT32's,
`mkusb_image.py` says why), holding

| file | what it is |
| ---- | ---------- |
| `\EFI\BOOT\BOOTX64.EFI` | the loader, about 37 KB |
| `\boot\kosmos.bin` | the kernel, a flat image with a Multiboot 2 header |
| `\boot\kosmos.cmdline` | the kernel's command line, when `KOSMOS_ARGS` gave one |
| `\boot\disk.img` | a kfs disk, when there is one; 32 MB at most for now |
| `\boot\kosmos.sums` | the build's sums of the kernel, a 64-bit FNV-1a a page, which the loader holds its read to |
| `\boot\disk.sums` | the same for the disk, when there is one |

**Or, asked for with `USB_HOME=partition`** (`usb.md` §7, step 5f): the same
ESP without `\boot\disk.img` or `\boot\disk.sums`, and the kfs disk in a
second partition right after it, of Kosmos's type
`8A9DC8A8-83CF-4F7F-962B-43157A68F14A`, named `KOSMOS HOME`. The stick's
command line gets `opt/kosmos/home=` and that partition's unique GUID, and
Kosmos opens the partition as `/home` through its own USB driver. **It booted
on the ThinkPad on 14 September** (§3's table), and is not yet what `make
x86-usb-image` builds when not asked.

Both paths end in the same place: `boot/x86_64/start.S` in 32-bit protected
mode, paging off, `eax` the Multiboot 2 magic and `ebx` the information
structure. The kernel does not know which loader it had.

---

## 2. What happened with GRUB

Every stick until 13 September was GRUB 2.12 (Homebrew's `x86_64-elf-grub`)
loading `kosmos.bin` with `multiboot2` and the disk with `module2`.

| date | build | disk | on the ThinkPad |
| ---- | ----- | ---- | --------------- |
| 11 Sep | 0.10.31 | 8 MB | booted once; on another boot `diskfs` died |
| 11 Sep | 0.10.31 | 64 MB | twelve stages, then `diskfs` died, every time |
| 11 Sep | the next build, 4 KB larger | 64 MB | booted to the desktop |
| 11 Sep | 0.10.38 | 32 MB | booted to the desktop |
| 12 Sep | 0.10.48 (then numbered 0.10.46) | 64 MB | nothing after GRUB's last line, twice |
| 12 Sep | 0.10.48 | 32 MB | booted, and ran the USB driver |
| 12 Sep | 0.10.54 (then 0.10.52) | 32 MB | booted, and ran USB enumeration |
| 13 Sep | 0.10.55 (then 0.10.53) | 32 MB | nothing after GRUB's last line |

**What was established, so nobody has to establish it again:**

- **The bytes changed before Kosmos ran.** The userland image canary
  (`testing.md` §18.22) checks the image at stage 2, before the kernel has
  done anything but install a trap table, and on 11 September it found eleven
  contiguous pages at 0x7fb000..0x806000 already changed. The same eleven,
  every boot of that image.
- **It followed the layout, not an address.** Four kilobytes more kernel
  moved everything up a page and the damage was simply gone, rather than
  shifted.
- **The ThinkPad's map says that memory is free.** GRUB's map on that machine
  is usable from 0x00100000 to 0x8e36f000, with nothing reserved under the
  kernel.
- **Kosmos writes nothing there first.** Before the canary's first check,
  `start.S` zeroes `.bss` between its own symbols (the boot page tables are
  inside it) and runs on its own stack section; the multiboot structure is
  copied out before the page allocator exists, and the allocator keeps clear
  of the kernel and the disk. Read again on 13 September.
- **GRUB said nothing.** With `echo` around every load it printed "loading
  the kernel", "loading the disk", "loaded", and handed over an image already
  changed. A `set debug=relocator,mm,efi` run showed its allocations correct
  and non-overlapping, the kernel at `0x100000+0xade000` and the module right
  after - and that **the ThinkPad's firmware gave GRUB the kernel's final
  address**, where OVMF has GRUB stage the image elsewhere and move it at
  `boot`. That difference is a large part of why no QEMU run ever showed the
  fault.
- **Sizes were a knob, not a cause.** A bigger disk, a smaller disk, a kernel
  a page larger: each moved where things landed, some placements survived and
  some did not, and each "fix" lasted until the next build grew. The 32 MB
  rule was the last of those, and 0.10.55 broke it.

**And then the loader's first run found the same thing under QEMU.** Asked
for the kernel's old range by address, OVMF refused, and the loader printed
why:

```
kosmos-boot: the firmware will not give the kernel 0x00100000..0x009c0000, which it needs exactly:
kosmos-boot:   the firmware's map: 0x00100000..0x00800000  type 7
kosmos-boot:   the firmware's map: 0x00800000..0x00808000  type 10
kosmos-boot:   the firmware's map: 0x00808000..0x0080b000  type 7
kosmos-boot:   the firmware's map: 0x0080b000..0x0080c000  type 10
kosmos-boot:   the firmware's map: 0x0080c000..0x00811000  type 7
kosmos-boot:   the firmware's map: 0x00811000..0x00900000  type 10
kosmos-boot:   the firmware's map: 0x00900000..0x01780000  type 4
```

Type 7 is free, 10 is ACPI NVS - memory the firmware keeps for good - and 4 is
boot services data. **GRUB had been loading Kosmos over OVMF's ACPI NVS for
months**, and the kernel's own boot log had printed `UNDER THIS KERNEL` for
those three entries on every UEFI boot. It survived because nothing wrote
there. It is the same shape as the ThinkPad's fault, found on the machine that
can be watched.

---

## 3. The loader

`boot/efi/loader.c`, `mbi.c` and `mbi.h`, and `trampoline.S`. What it does, in
order, and why each step is there:

0. **Makes itself seen.** Where the firmware has the console-control
   protocol - older than UEFI and not in its specification - the loader asks
   for text mode, then sets the console light grey on black and clears it. The
   GUID and both calls are read out of GRUB 2.12's own `kernel.img`, and that
   switch is how GRUB's lines reached the ThinkPad's screen when this loader's
   first ones did not. It finds the framebuffer, and from then on **every line
   is drawn by the loader itself as well**, in the lower half of the screen
   with the kernel's 8x16 font on the kernel's ground, so a refusal can be read
   on a machine whose firmware console shows nothing.

1. **Reads the kernel's Multiboot 2 header out of the first 36 KB** of
   `kosmos.bin`, into the loader's own memory. `mb2_image_parse` in `mbi.c`
   finds where the image goes, what is loaded from the file and what is
   reserved past it, and where to enter.
2. **Checks the kernel's range against the firmware's map, before anything
   large is allocated** - a pool allocated first could be given a piece of the
   very range. Each part of it is one of three things:

   | the firmware says | the loader |
   | ----------------- | ---------- |
   | free (type 7) | **claims** it by address, so nothing the firmware allocates later lands there |
   | boot services code or data (3, 4) | **borrows** it: the firmware's until ExitBootServices, nobody's after, so the kernel is copied in then |
   | anything else - ACPI, runtime services, reserved, a device window, or a loaded image's pages | **refuses**, printing each entry and waiting for a key |

3. **Reads the kernel once, holds it to the build's sums, copies it twice
   and takes a fingerprint of every page** (FNV-1a), then checks that every
   buffer it has allocated lies outside the kernel's range. **The sums are
   what the build wrote**: `mkusb_image.py` puts `\boot\kosmos.sums` beside the
   kernel, and a read that differs from them is refused on the screen, with
   how many pages and the first (`boot/efi/sums.h`). **The fingerprints are of
   what was read**, and catch memory changing afterwards. Until 14 September
   there were only fingerprints, so a stick that gave back bytes the build
   never wrote passed everything here. A stick with no sums, made before they
   existed, says so and is used.
4. **Reads the disk** into memory below 4 GB, because a module's addresses
   are 32 bits, holds it to `\boot\disk.sums` the same way, and fingerprints
   it.
5. **Reads `\boot\kosmos.cmdline`**, keeping only the characters
   `mkusb_image.py` allows, and appends five words of its own.
6. **Finds the screen** - the firmware's current GOP mode, as GRUB passed on -
   **ACPI's root pointer** from the configuration table, the newer revision
   first, and **copies the trampoline** to a page below 4 GB allocated as
   loader code.
7. **Checks both copies against the fingerprints**, repairing a page wrong in
   one from the other, and says what it found. Then it prints its last line.
8. **ExitBootServices**, retrying with a fresh map if the map changed - the
   specification allows that, and printing can cause it - with the
   information structure rebuilt from each map and nothing allocated in
   between.
9. **With the firmware gone**: checks the copies again, copies the kernel into
   its place, zeroes what the header reserves past the file, and checks every
   placed page, repairing from a copy. The counts go into the command line the
   structure already holds.
10. **Jumps** through the trampoline: long mode to 32-bit protected mode with
    paging off, a flat 32-bit code and data segment of its own, PCIDE and CET
    cleared first, `eax` the magic and `ebx` the structure.

**What a boot says.** Under OVMF, with a 32 MB disk:

```
kosmos-boot: Kosmos's own loader, on EDK II firmware revision 0x00010000
kosmos-boot: this loader: 0x10000000..0x10029000, where the firmware put it
kosmos-boot: the kernel's place: 0x01000000..0x01b47000, 3868 KB claimed now and 7680 KB the firmware's until it lets go
kosmos-boot: the kernel: 10504 KB in two copies, 2627 pages fingerprinted, entry 0x01001000
kosmos-boot: the disk: 0x79b3c000..0x7bb3c000, 32768 KB
kosmos-boot: the screen: 1280x800, 5120 bytes a row, at 0x0000000080000000, mode 0 of 30
kosmos-boot: both copies of the kernel are the file, page for page
kosmos-boot: handing over: entry 0x01001000, information at 0x7be38000, trampoline at 0x7e3cd000
```

**And what the kernel says it was handed**, at boot stage 4:

```
[4/12] physical memory
       ...
       -> this kernel is inside the loader's 0x00900000..0x7ea2b000 (usable), of 23 entries
       -> a disk from the loader: 32768 KB at 0x79b3c000..0x7bb3c000, kept from the allocator
       -> this kernel is 0x01000000..0x01b47000
       -> the loader: kosmos-boot, 0 pages repaired before the firmware let go, 0 after, 0 lost; the disk: same
```

Those are the only memory lines left. Under GRUB the same OVMF boot printed
`UNDER THIS KERNEL` for its three ACPI NVS entries; with the kernel at 16 MB
there is nothing of the firmware's under it to name.

The five words the loader adds to the command line, after the stick's own,
are `kosmos-boot/before=`, `/after=`, `/lost=` - five digits each, pages
repaired before ExitBootServices, after it, and pages that could not be -
`kosmos-boot/disk=same`, `diff` or `none`, and `kosmos-boot/build=same` or
`none`, whether what was read was held to the build's sums. `kernel/main.c`
reads them with `hal_boot_option` and prints the line above; a boot through
anything else has no such words and no such line. **The kernel keeps 511
characters of the line**, more than the loader's 384: it kept 255, and a
stick's long words cut the loader's off the end (`usb.md` §7, 5f).

**The information structure** carries the seven tags GRUB's did, because
those are the seven the kernel reads: the command line (1), the disk as a
module (3), the memory map (6), the framebuffer (8), the EFI system table
(12), and ACPI's root pointer (14 or 15). The map is sorted and merged, and
typed as GRUB typed it - free memory, boot services and loader memory usable;
ACPI tables 3, NVS 4, unusable 5, everything else reserved. **Merged, which
is not tidiness**: a real firmware's map is hundreds of fragments and the
kernel adopts the largest single usable range.

**Where the layouts come from.** The UEFI structures are the specification's,
checked against gnu-efi's transcription of it (`efiapi.h`, `efiprot.h`,
`eficon.h`, `efidef.h`, `efierr.h`), read from a local copy and not in this
repository; `loader.c` asserts the offsets that matter at compile time,
including the two `hal/pc/smbios.c` reads the system table at. The Multiboot 2
specification is not in this project's references: the tags are the layouts
`hal/pc/multiboot2.h` reads, which GRUB's output satisfied on every boot
before this loader and this loader's output satisfies under OVMF.

**How it is built**: the kernel's own `x86_64-elf-gcc`, freestanding,
position-independent with hidden visibility and no FP or SIMD registers, and
`x86_64-elf-ld -m i386pep --subsystem 10` - binutils' own PE32+ output. No
relocation in it is anything but PC-relative, so the image needs no fixing up
wherever the firmware loads it. `make build/x86_64/BOOTX64.EFI`.

**On the ThinkPad, 13 September.** Every stick through this loader, in order:

| stick | disk | on the ThinkPad |
| ----- | ---- | --------------- |
| 0.10.59 as committed | 32 MB | a black panel, no text; a key press went back to the firmware's Boot Menu |
| the same kernel with 0.10.60's loader, which draws its own lines | 32 MB | booted to the desktop |
| 0.10.60 (`13037a4`), built twice, with each disk | 32 or 64 MB | booted to the desktop - the 64 MB image was the one to write, but nothing photographed said which disk it carried |
| 0.10.61 (`9ca683a`), MEGA | 64 MB | the loader's lines, `both copies of the kernel are the file`, `handing over` - and then nothing |
| 0.10.61 (`9ca683a`), MEGA, from `make MEGA=1 x86-usb-image` | 32 MB | the same: the kernel's place all claimed, the disk at `0x5d125000`, `handing over` - and then nothing |
| 0.10.60 (`13037a4`), MEGA, from `make MEGA=1 x86-usb-image` at that commit, the stick read back and every sector the image's | 32 MB | booted to the prompt and the desktop, on the stick both 0.10.61 sticks had stopped on: `LENOVO 20W1S1Y500 ThinkPad T14 Gen 2i`, 8 cores, both xHCI controllers and the USB mouse, camera and stick named |
| 0.10.62 (`56625f2`), MEGA, from `make MEGA=1 x86-usb-image`, written with the fixed `mkusb.sh` - its read-back output not yet seen | 32 MB | booted to the prompt, and the USB mouse driver reads the mouse on the machine, `a boot mouse, read from endpoint 1, up to 8 bytes every 1 ms` - with its axes wrong: sideways moves the arrow up and down, and up and down does nothing |
| `a543f20`, the 0.10.62 kernel with the mouse read by its Report descriptor, MEGA, from `make MEGA=1 x86-usb-image`, offered as an experiment - its read-back output not seen | 32 MB | booted to the desktop and ran for 25 minutes; the mouse's axes right and its movement in jumps, each step of naming it a second or two and 882 reports read (`usb.md` §5) |
| `d6e7e12`, the 0.10.62 kernel with ERDP written low half first, MEGA, from `make MEGA=1 x86-usb-image`, offered as an experiment | 32 MB | back to the firmware's Boot Menu twice with nothing drawn, and to the desktop at the third try; `log loader` clean - 0 pages repaired before, 0 after, 0 lost, the disk the same, the userland image as the build left it - and the mouse smooth (`usb.md` §5) |
| `b8c6f10`, the 0.10.62 kernel with USB mass storage's 5a to 5d, MEGA, from `make MEGA=1 x86-usb-image`, on a Kingston DataTraveler Exodia 128 GB, offered as an experiment - its read-back output not seen | 32 MB | booted to the prompt; `log loader` clean - 0 pages repaired before, 0 after, 0 lost, the disk the same, the stick against the build the same - and the stick it booted from read by Kosmos's own USB driver: `0951:1666` at SuperSpeed on `00:14.0` port 14, "Kingston" "DataTraveler 3.0", 242155520 blocks of 512 bytes, 115 GB, the GPT's header at block 1, and `sticks` reading its `KOSMOS` partition through `/dev/blocks` (`usb.md` §7) |
| `c70d9df`, the 0.10.62 kernel with USB mass storage's 5e and 5f, MEGA, from `make MEGA=1 x86-usb-image USB_HOME=partition` - `/home` in a partition of its own, no disk in memory - on a Kingston DataTraveler Exodia 128 GB, offered as an experiment - its read-back output not seen | none; a 32 MB Kosmos partition | booted to the prompt and the desktop; `log loader` clean with `the disk: none`; `diskinfo` said the Kosmos partition on USB unit 0, blocks 393250 to 458785, kfs version 1; a file saved to `/home` read back. **The desktop came about 20 seconds late** - `neofetch` gave an uptime of 20 seconds at the shell - **and its bar and windows appeared only once the pointer moved**; neither is explained yet (`usb.md` §7) |
| `9af841c`, 0.10.63 with storage at full speed's steps 1 to 3 - the kernel's disk call moving 124 KB and kfs reading and journaling in runs - MEGA, from `make MEGA=1 x86-usb-image USB_HOME=partition`, on the Kingston DataTraveler (it names itself "DataTraveler 3.0", 0951:1666) | 32 MB, in a partition of its own | booted to the prompt on 14 September; `log loader` clean - 0 pages repaired before the firmware let go, 0 after, 0 lost, the disk: none (the line's end, the stick against the build, ran off the photograph); `/home` kfs on the stick, 8 of 32 MB free; the stick at SuperSpeed on port 14, 115 GB; neofetch's uptime 22 seconds at the prompt |
| `895aa3f`, 0.10.65-development - 0.10.64's `diagnose`, `make stick-log`, the refused flush told once and the search for the stick counted, and the USB driver's wait on both of its endpoints - MEGA, from `make MEGA=1 x86-usb-image USB_HOME=partition`, on the Kingston DataTraveler | 32 MB, in a partition of its own | booted to the prompt on 15 September, **read from `make stick-log` rather than photographs**; `log loader` clean - 0 pages repaired before the firmware let go, 0 after, 0 lost, the stick against the build the same; the kernel's first line 16.6 s after the counter started (`log_origin`), the prompt at 3.0 s into the log - 19.6 s from the counter's zero, neofetch's 19 seconds; `/home` found on the first look, at 2.81 s, 10 ms after the USB driver began watching; the flush refused once, `ILLEGAL REQUEST (20h/00h)`; and the sound controller up - `sound: Intel HDA, 44100 Hz stereo` - where `9af841c`'s codec did not answer |
| `b5ce4a4`, 0.10.70-development - 0.10.65 plus the desktop started by itself, the HDA codec waited for in milliseconds, the Processes window without its idle row, and EAPD set with the jack driven beside the speaker - MEGA, from `make MEGA=1 x86-usb-image USB_HOME=partition` | 32 MB, in a partition of its own | booted to the desktop by itself on 15 September and played Basket Case through the laptop's speaker - Diego: "basket case works!", "it works great"; **made the stable build the same day** ("this build is stable"), renamed `kosmos-usb-0.10.70-stable.img` with the bytes unchanged |
| `1b4e29c`, 0.10.80-development - 0.10.70 plus the volume keys and a master mute, the Sound level bar, and `acpi save` - MEGA, from `make MEGA=1 x86-usb-image USB_HOME=partition` | 32 MB, in a partition of its own | booted on 18 September and **the volume keys work on the ThinkPad's own keyboard** - Diego: "sound keys work!", "on thinkpad" - so its controller sends the bytes QEMU's PS/2 keyboard does (e0 20, e0 2e, e0 30) - and **the Sound bar showed and faded** ("the bar showed up and faded"). `log loader` not yet photographed |
| `01b1794`, 0.10.83-development - 0.10.80 plus the backlight driver, Space Grotesk and the brightness raised at boot - MEGA, from `make MEGA=1 x86-usb-image USB_HOME=partition` | 32 MB, in a partition of its own | booted on 18 September and **the screen came up brighter**: the driver raised controller 0 from a third to 80% at boot - Diego: "it worked! the brightness worked!" - the first hardware setting Kosmos has changed on the ThinkPad by itself. `log loader` not yet photographed |
| `d5f2f83`, 0.10.85-development - 0.10.83 plus the F5/F6 probe, the wallpapers and the Super Nintendo's File menu - MEGA, from `make MEGA=1 x86-usb-image USB_HOME=partition` | 32 MB, in a partition of its own | booted on 18 September; `diagnose` and `make stick-log` read back |
| `41413f5`, 0.10.87-development - 0.10.85 plus ACPI mode (F5, F6, the power button, S5), the battery, the Super Nintendo's View and Game menus and Xbox 360 pads - MEGA, same layout | 32 MB, in a partition of its own | **crawled, 19 September**: ACPI mode on, the battery read (24%), then the sound card's setup took about three minutes and the boot stopped at stage 10. **The 8253 stopped counting in ACPI mode**, and every wait before the tick was a bounded spin on it - ten seconds each. Fixed by measuring the TSC before the switch and waiting on it (`timer.c`). The layout was not the fault. |
| `90ec487`, 0.10.88-development - 0.10.87 plus the waits on the TSC and `/home` made fresh from `~/Kosmos/home` - MEGA, from `make MEGA=1 x86-usb-image` | **512 MB, in a partition of its own** - the first over 32 MB | **booted on 19 September**, to the desktop: Diego - "battery works!", "brightness keys work!", "snes controller works!". The first `/home` over 32 MB to boot on the ThinkPad. **Diego made it the stable build the same day** - "10.88 is the new stable" - as `kosmos-usb-0.10.88-stable.img`, the same bytes; 0.10.70's went to the Trash |
| 0.10.99-development - 0.10.98 (`1fd87eb`) with a revision of its own: the solar system's rasterizer in C - MEGA, from `make MEGA=1 x86-usb-image` | 512 MB, in a partition of its own | **booted on 21 September**: the solar system at 100 fps at maximum detail; clicks lost on the desktop and the Deskbar, which was 0.10.100's bug |
| `52cdd45`, 0.10.101-development - 0.10.99 plus a click as an event and the solar system full screen - MEGA, same layout | 512 MB, in a partition of its own | **booted on 21 September**: full screen at 100 fps; a film played once and not twice (0.10.102's bug), the wallpaper not restored (0.10.103's), and `diagnose` written to the stick and read back |
| `3160169`, 0.10.115 - the scale, the looks, the Deskbar at 32 - MEGA, same layout, and the first stick built for a **GitHub release**, so its `/home` is empty rather than made from `~/Kosmos/home` | 512 MB, in a partition of its own | **booted on 22 September**, written by Diego on his MacBook Pro from the release: `log loader` says the userland image is as the build left it, the kernel inside the firmware's usable range of 13 entries, "0 pages repaired before the firmware let go, 0 after, 0 lost; the disk: none; the stick against the build: same", and write-combining for the kernel and the compositor. `neofetch`: 8 cores, 198 of 767 MB, 1920x1080, `/home` 509 of 512 MB free. The desktop at 150 per cent, kept across a restart |
| 0.10.115, the same stick, on a **second machine**: a Lenovo ThinkCentre M700 (10J0/S1CK00), a 6th-generation Core i7 with HD Graphics 530 | 512 MB, in a partition of its own | **booted on 22 September, first time, on a machine nobody had written a line for** - Diego: "i have some fantastic news.. i bought a lenovo thinkcentre m700 mini pc and kosmos boots!! it works!". Read back with `diagnose` and `make stick-log`: 8 cores at 3.4 GHz, 767 of 8192 MB (the kernel's ceiling, `hal_ram_capped`), the machine named out of SMBIOS 3.0 in the EFI system table, xHCI at `00:14.0` with 22 ports, the Kingston at SuperSpeed on port 18. **Two faults it found**: the screen at 800x600 on a monitor that does 3440x1440 - the loader took the firmware's own mode (5zd-a, fixed in 0.10.124) - and its Apple keyboard, `05ac:0220`, read as *a mouse*, so a machine with no PS/2 port had no keys at all (5zd-b, fixed in 0.10.123) |

| `211a652`, 0.10.125-development - the USB keyboard, the loader's mode choice and the network stack on a USB adapter - MEGA, same layout, on the ThinkCentre M700 | 512 MB, in a partition of its own | **booted on 23 September at 3440x1440**, the monitor's own size, where 0.10.115 had come up at 800x600. Its USB keyboard reads as one - `port 4: a keyboard, read from endpoint 1, up to 8 bytes every 10 ms` - and Diego: "USB keyboard works great!". **One fault**: the desktop painted only the top of the screen, with the kernel's boot log through the rest - a process's window for the framebuffer was sixteen megabytes and 3440x1440 is 18.9, so the compositor's first surface was mapped over the bottom of it (`testing.md` 18.154, fixed in 0.10.126) |
| `401f3d6`, 0.10.128-development - 0.10.125 plus the whole display painted at 3440x1440 and an Intel Ethernet driver - MEGA, same layout, on the ThinkCentre M700 | 512 MB, in a partition of its own | **Booted on 23 September and was unusable.** Three minutes to a desktop, then too slow to work with. The log named it: `xhci: 00:14.0 answered a No-Op command on its event ring, found by looking: no interrupt within a second`, and `e1000: ... interrupt 23`. Both devices had fallen back to INTx lines. 0.10.127 had changed two chipset numbers on a q35 measurement - the input a PCI link lands on, and MSI numbering derived from the I/O APIC's count of inputs - and the second put `pci.c` one line ahead of the call that initialises the controller, so the *first* device probed lost its MSI. Here that was the xHCI holding the mouse, the keyboard and `/home`. Every suite was green throughout (`testing.md` 18.155). Fixed in 0.10.129 |
| `f21ae96`, 0.10.129-development - 0.10.128 with both chipset numbers put back and MSI allocation moved into `apic.c` - MEGA, same layout, on the ThinkCentre M700 | 512 MB, in a partition of its own | **Booted on 23 September, and Diego: "0.10.129 booted perfectly on m700".** So the regression in 0.10.128 was the two chipset numbers and the ordering, exactly as `testing.md` 18.155 reads it: MSI numbering back to a constant of 20, PCI links back to 16-19, and allocation moved into `apic.c` where the count of inputs lives. What is kept from 0.10.127 is `apic_mask` setting one bit rather than composing the word, which is machine independent. **This is the layout a stick carries for that machine now**, and the first build with an Intel Ethernet driver in it |
| `9a2637a`, 0.10.136-development - 0.10.129 plus all of the machine's memory, the drivers and kits in directories of their own - MEGA, same layout, for the ThinkCentre M700 | 512 MB, in a partition of its own | **Built and checked on 23 September, not yet booted there.** 32 checks under OVMF, `make test` green (33 suites, 5:25), `make stress` clean (60 rounds, nothing leaked). It is 0.10.129 - the layout that booted here - plus the memory work: the kernel reaches RAM through a window in its own half of the address space, the board adopts the range holding the kernel whole, and one bitmap spans both sides of the PCI hole. QEMU reports 8190 MB of 8192 and 16382 of 16384 where every size used to report 767. **What to look at**: `neofetch`'s Memory row, which should say about 8100 MB rather than 767, and no boot line beginning `of NNNN MB this machine has` |
| `0686135`, 0.10.146-development - 0.10.136 plus Preferences, Tracker rebuilt, the Endeavour look with flat shading and rounded controls, rounded corners and an optional shadow, the one header in every window, and the title bar's face at the size of the rest of the interface - MEGA, same layout, for the ThinkCentre M700 | 512 MB, in a partition of its own | **Booted on the M700 on 24 September, and Diego: "it booted and worked!"** - with two faults found in it, fixed in 0.10.147: Tracker's Find ended the program, and Preferences changed nothing on the system. Built and checked on 23 September: 32 checks under OVMF (`run_uefi.py`), `make test` green (33 suites, 6:02), `make stress` clean (60 rounds, nothing leaked). **The window since 0.10.129 - the last build Diego confirmed on that machine - is wide**: 0.10.136 was handed over and never reported on, so this carries the memory work, the tree reorganised by what things are, and the whole of the new look. What to look at: every window's top is one header now, the title bar's text is the size of the Deskbar's beside it, and Preferences > Appearance has Theme, Rounded corners and Drop shadows in it |
| `c1ba5ee`, 0.10.150-development - 0.10.146 plus every window drawn from `docs/apps.html` and Tracker from `tracker2.html`, the screen at the mockups' sizes, Preferences with Appearance folded in, traffic-light title bar buttons, a 6-pixel frame with the page rounded inside it, Endeavour the default look, and the fixes for what 0.10.146 found - MEGA, same layout, for the ThinkCentre M700 | 512 MB, in a partition of its own | **Built and checked on 24 September, not yet booted there.** 32 checks under OVMF (`run_uefi.py`), `make test` green (33 suites, 5:53), `make stress` clean (60 rounds, nothing leaked). The layout is 0.10.146's, which booted there. **What to look at**: Tracker's Find no longer ends it; a look chosen in Preferences changes the desktop at once; text at the mockups' size; the three coloured buttons and the frame round every window; Tracker's sidebar of places; the Mixer showing what is playing |

The black panel was a refusal: only `refuse()` waits for a key, and what it
printed went through a console that machine does not show. **Why it refused
is not known** - its reason never reached a screen, and the second loader
differs only in what makes it seen. On the boot that worked, Kosmos's own USB
driver needed a second Address Device for that stick, so a read error is one
candidate; the loader growing by 5 KB is another. A refusal from here on says
which.

**The two returns to the Boot Menu on 14 September were not the loader.**
Every way `efi_main` gives up draws a line and waits for a key, so a return
with nothing drawn is the firmware not starting it at all. The stick is the
likeliest reason: Kosmos's own driver named it - "UDisk", `abcd:1234`, an ID
that looks like a placeholder rather than a maker's - only at a second Address
Device on the boot that worked, as on the boot before, and saw it unplugged
and back twice in three and a half minutes with nobody pulling it out, as far
as anyone knows. A stick that drops off its bus fits both returns, and would
fit the stops after the loader's last line too; it is a lead, not a finding.
**Diego's answer, the same morning, is branded sticks**, "so we dont keep
stumbling into issues with this generic one".

What the kernel was handed on the boot that worked, read off the photograph:

```
-> the userland image as the loader left it: 0x01022000..0x01a3cb34, as the build left it
-> this kernel is inside the loader's 0x00100000..0x8e36f000 (usable), of 13 entries
-> a disk from the loader: 32768 KB at 0x5d134000..0x5f134000, kept from the allocator
-> the loader: kosmos-boot, 0 pages repaired before the firmware let go, 0 after, 0 lost; the disk: same
```

**Nothing was repaired and nothing lost**, in the map GRUB saw on that
machine: usable from 1 MB to 0x8e36f000. The loader's own lines were not
photographed - on a boot that works they last until the kernel draws - so
whether that firmware has the console-control protocol is not known either.

**And the first stick meant to carry a disk over 32 MB did not boot.** 0.10.61
with the 64 MB disk stopped after the loader's last line, with nothing
plugged in but the stick; the same image booted under OVMF, a USB mouse
clicking through it. Its loader's screen line is the first ever photographed
on the ThinkPad:

```
kosmos-boot: the screen: 1920x1080, 7680 bytes a row, at 0x0000004000000000, mode 0 of 6
```

**Above 4 GB**, so `hal_fb_early` refused it - the boot page tables end at
4 GB (`hal/pc/fb.c`) - and the kernel drew nothing until stage 6 mapped the
screen itself. That is the graphics aperture's address, and nothing records a
boot where it was anywhere else, so the machine was dark from the hand-over to
stage 6 on the boots that worked too, and a stall anywhere in between looked
exactly like this one. 0.10.62 maps that screen into the boot page tables and
draws from stage 2 there (§5, `testing.md` §18.44).

**And the 32 MB stick stopped in the same place**, so the disk's size is not
what stops it; the 32 MB refusal in `mkusb_image.py` stays, and it is not a
guarantee of anything.

**Everything about the ThinkPad that QEMU can reproduce was then run against
that exact image, and it booted to the prompt every time**: the screen at
`0x4000000000` at 1920x1080 - put there by stopping QEMU at the kernel's
entry and rewriting the loader's framebuffer tag, over memory at that address
that is outside the firmware's map, as a graphics aperture is - its memory
shape, eight processors, and an Intel processor with SMEP on. The screen above
4 GB takes the ThinkPad's path: nothing drawn until stage 6, then `attached
here; everything above it was replayed`, and the whole log on the panel.

**What nothing in the chain has ever checked is that the machine gets the bytes
the build wrote.** `tools/mkusb.sh` writes with `dd` and does not read back.
The loader fingerprints what it read (step 3). The kernel's canary knows the
build's sums for the userland image only, and on this machine cannot say
anything before stage 6. A stick that gives back wrong bytes stops a boot
exactly here, follows the build and the disk size because they decide which
bytes land where, happens under GRUB and this loader alike, and is invisible
to QEMU, which reads the image file and not the stick. The stick that booted
on the 13th enumerated as `abcd:1234 "UDisk"`, an ID no registered vendor
has, and needed a second Address Device. **That stick, read back on the Mac
and compared with its image, is the next measurement - not another boot.**

**It could not be read back**: by then it held Pop!_OS, and Pop!_OS and
elementary OS both booted from it perfectly. **So it was written again, with
0.10.60 built at its own commit, and read back - every one of its 475203
sectors the image's - and 0.10.60 booted to the prompt from it.** The machine
boots Kosmos and the stick holds what is written to it. 0.10.61 stopped twice
on that stick and 0.10.60 does not; the 0.10.61 writes were never read back,
so a bad write is not excluded for them, but a stick that has since held three
images exactly makes it the least likely explanation.

**And 0.10.62, built and read back the same way, booted as well** - and it is
0.10.61 with the early screen added, which moves the kernel's end by the three
table pages the early screen keeps. So what stopped 0.10.61 is one of two
things, and one stick tells them apart: its two writes, which nothing read
back, or its exact layout. **The 0.10.61 image written again and read back is
that stick**: if it boots, the stops were the writes, which `mkusb.sh` now
catches; if it stops with its bytes checked, it is that layout, and a stop
that can be repeated is one that can be found.

---

## 3b. The screen, in the largest mode the firmware has

**Diego's ThinkCentre M700 came up at 800x600 on a monitor that does
3440x1440** (`roadmap.md` 5zd-a), and had driven 3440x1440 under Linux on the
same machine. The mode was in the firmware's list all along and nobody asked
for it: the loader read `gop->mode`, which is whatever mode the firmware
happened to be in - the one it drew its own setup screen in.

**It asks now.** Every mode, through `QueryMode`, and the largest by pixel
count is taken, with the wider one first where two hold the same number. A
mode whose format is `PIXEL_BLT_ONLY` has no framebuffer to write into and is
passed over; so is one whose pixels cannot be described, since the screen
would then be lost rather than small. Nothing here can fail: a firmware that
will not answer, or will not take `SetMode`, leaves the mode as it was, which
is what every machine before this got.

**Before the first line**, because `SetMode` clears the screen. The loader
says what it did afterwards:

```
kosmos-boot: the screen: 2048x2048, 8192 bytes a row, at 0x0000000080000000, mode 5 of 30, the largest this firmware offers
```

### `video=WxH`, the escape hatch

The largest mode is the right default and it is a default that can go wrong.
A firmware that offers a mode the monitor will not show leaves a black
screen - and a machine with no keyboard, which is exactly the machine this
was written for, cannot be told anything at all.

So `\boot\kosmos.cmdline` may say `video=1024x768`. That file is on the
stick's FAT partition, which every computer mounts, so the way out of a black
screen is a text editor and a USB port.

An exact match only - a near miss would be a mode nobody asked for - and the
loader says either way, including what the largest was:

```
kosmos-boot: video=1024x768 on the command line: mode 2, where the largest is 2048x2048
kosmos-boot: the screen: 1024x768, 4096 bytes a row, at 0x0000000080000000, mode 2 of 30, named on the command line
```

**It is read where the command line first exists**, which is after the screen
has already been chosen and drawn on - so a named mode changes the screen
under the loader's own lines. That is the rarer case and worth the flicker;
the alternative is opening the stick's filesystem before there is anywhere to
report a failure.

### What a larger screen cost

**Two checks that had always passed began to fail on a machine drawing
exactly what it had always drawn.** The wordmark and the loader's refusal
text were held to a *fraction* of the screen - 0.05% of 1280x800 is about
five hundred pixels - and a drawing of a fixed size is a smaller fraction of
a larger screen. They are counted now. A fraction is right for the ground,
which is however much of the screen nobody wrote on; it is wrong for anything
drawn.

That is the first of what a wider screen will find. `roadmap.md`
4k-and-no-hard-limits is where the rest of it lives.

## 4. Why the kernel is at 16 MB

`boot/x86_64/kosmos.ld` put the image at 1 MB, because that is where a
Multiboot loader traditionally puts a kernel. On a UEFI machine nothing
promises that memory is free, and on OVMF it is not: ACPI NVS at 8 MB, boot
services data from 9 MB. 16 MB is past OVMF's NVS and inside its boot services
data - which is the useful case, because it makes every UEFI test here take
the borrowed path, the one the ThinkPad may need.

**It is still a fixed address**, and the loader is what makes that honest: it
asks, and says on the screen when the answer is no, instead of loading over
whatever is there. A relocatable kernel would remove the question; it is not
built.

What it costs: the 15 MB below the kernel is not handed out, because the page
allocator counts from the image's end. `kernel/main.c` prints where the kernel
is on every boot a loader describes, and no longer says 1 MB whatever the link
address is.

---

## 5. Reading a boot on a machine with no serial port

- **The loader's lines are on the screen twice** before the kernel's: at the
  top through the firmware's console, where that console shows, and in the
  lower half on a dark band the loader draws itself. Photograph them if
  anything goes wrong. A black panel that goes back to the firmware's menu at
  a key press is what a refusal looked like before the loader drew its own.
- **A refusal waits for a key**, with the reason and the firmware's entries
  for the kernel's range on the screen. Nothing is loaded over them.
- **"N pages of the kernel changed in memory and were repaired"** before
  handing over, or a kernel line with a non-zero `after`, is the ThinkPad's
  old fault happening and being survived. `lost` above zero means a page was
  wrong in both copies, and the kernel was started anyway: its canary
  (`testing.md` §18.22) will say which pages if they are userland's.
- **"handing over" and then nothing** is the trampoline or the kernel's first
  instructions, from 0.10.62 on. The ThinkPad's screen is at `0x4000000000`,
  above the 4 GB the boot page tables map, and before 0.10.62 `hal_fb_early`
  refused it: the kernel drew nothing until stage 6, on the boots that worked
  as well, so a stall anywhere before that looked exactly like this. This line
  said the kernel draws from stage 2, which was true only under OVMF, and it
  was read as true of the ThinkPad. `mmu_boot_map_high` adds that screen to
  the boot page tables now, and `run_uefi.py` boots a stick with its screen
  moved there to hold it to it.
- `the disk: diff` in the kernel's line is the disk changing after the loader
  read it.

---

## 6. How it is tested

- **`tools/test_efiboot.c`**, in `make test`: the header parser on synthetic
  images and on the build's own `kosmos.bin`, the map conversion on an
  unsorted, fragmented map with oversized descriptors, and an information
  structure built by `mbi.c` and read back with the kernel's own `mb2_find` and
  `mb2_framebuffer_from`; and whether a read is the build's (`sums.c`), from
  FNV-1a's own vectors to a file a byte short. `testing.md` §18.42 and §18.50
  have the controls.
- **`tools/run_uefi.py`**, in `make test`: the stick `mkusb_image.py` makes,
  with a 4 MB disk `kfs.lua` made, booted under OVMF on an xHCI controller as
  a USB drive - the loader's lines, the kernel's line about what it was
  handed, `the disk: same` in it (`none` for a stick without one), the kernel
  at 16 MB, nothing of the firmware's under it, and the screen, ACPI, SMBIOS
  and the other processors through that path. **And a refusal**: a second
  stick, whose kernel is zeros, must be refused on the serial line and drawn
  on in the lower half of the screen by the loader itself. **And a stick
  holding other bytes**: a copy of the first with one byte of its kernel
  changed, its sums as the build wrote them, refused with the page named -
  where the first stick's loader and kernel both say it was the build's.
  **And the
  ThinkPad's screen**: the first stick again, stopped at the kernel's entry
  with QEMU's gdbstub and its framebuffer tag rewritten to `0x4000000000` at
  1920x1080, over memory that is in no map the firmware hands over - the boot
  log has to be in those pixels when the page allocator starts (`testing.md`
  §18.44). **And a stick with `/home` in a partition**, `USB_HOME=partition`'s
  layout, booted with no screen: no disk handed over, the partition's GUID
  reaching `sys.boot` from the stick's command line, and `diskinfo` saying
  `/home` is that partition (`testing.md` §18.61).
- **`tools/stickcheck.py`, run by `tools/mkusb.sh` after every write**: the
  stick read back and compared with its image sector by sector, naming the
  file, and in the kernel the page and section, of anything that differs, and
  telling a macOS mount's bookkeeping from damage (`testing.md` §18.45).
- **A stick from a GitHub release, on any Mac** (`tools/getstick.sh`, 22
  September): `bash getstick.sh 0.10.115` downloads the release's image and
  tools into `~/Downloads/kosmos-<version>`, holds both to the release's
  `SHA256SUMS`, unpacks them, checks the image again, and hands it to the
  release's own `mkusb.sh`, which asks for the drive as always. A release
  with no `SHA256SUMS` is refused. A released stick's `/home` is empty: the
  repository is public, and the sticks made on the development Mac carry
  files that must never be published. `tools/test_getstick.py` runs it
  against a release made on the local disk (`testing.md` §18.146).

**What QEMU cannot show**: the ThinkPad's own map at the moment the loader
runs, whether its firmware writes into memory it has handed out, and the
console-control switch - under OVMF the loader never has to make it, so that
call has run only on the ThinkPad, if at all - and whether that firmware
reads back the bytes the stick holds. QEMU reads the image file; `mkusb.sh`
now checks the stick on the Mac, and the loader checks its own read on the
machine against the build's sums. The loader's lines on that machine are the
measurement.

---

## 7. What is not done

- **The kernel is not relocatable**, so its address is a fixed claim the
  loader can only check.
- **The disk on a stick is still refused over 32 MB** by `mkusb_image.py`,
  until the ThinkPad has booted a bigger one through this loader.
- **`/home` in a partition of its own** (`USB_HOME=partition`) has booted on
  the ThinkPad four times - `c70d9df`, `9af841c`, `895aa3f` and `b5ce4a4`,
  the last of which Diego used and called stable - so it is no longer an
  experiment, and the table above is what says so. The first of those had its
  desktop 20 seconds late; `895aa3f` measured where the time went. It is not
  the default yet, and its partition is held to the same 32 MB, though
  nothing reads it into memory.
- **Secure Boot** is not supported; the loader is unsigned, as GRUB was.
- **The screen is the firmware's current mode.** The loader does not choose
  one.
- **Why the first ThinkPad stick through this loader refused** is not known;
  §3 has both boots.
