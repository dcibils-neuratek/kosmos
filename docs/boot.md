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

3. **Reads the kernel into two copies and takes a fingerprint of every
   page** (FNV-1a), then checks that every buffer it has allocated lies
   outside the kernel's range.
4. **Reads the disk** into memory below 4 GB, because a module's addresses
   are 32 bits, and fingerprints it.
5. **Reads `\boot\kosmos.cmdline`**, keeping only the characters
   `mkusb_image.py` allows, and appends four words of its own.
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

The four words the loader adds to the command line are
`kosmos-boot/before=`, `/after=`, `/lost=` - five digits each, pages repaired
before ExitBootServices, after it, and pages that could not be - and
`kosmos-boot/disk=same`, `diff` or `none`. `kernel/main.c` reads them with
`hal_boot_option` and prints the line above; a boot through anything else has
no such words and no such line.

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

**On the ThinkPad, 13 September.** Two sticks, the same kernel and disk:

| stick | disk | on the ThinkPad |
| ----- | ---- | --------------- |
| 0.10.59 as committed | 32 MB | a black panel, no text; a key press went back to the firmware's Boot Menu |
| the same kernel with 0.10.60's loader, which draws its own lines | 32 MB | booted to the desktop |

The black panel was a refusal: only `refuse()` waits for a key, and what it
printed went through a console that machine does not show. **Why it refused
is not known** - its reason never reached a screen, and the second loader
differs only in what makes it seen. On the boot that worked, Kosmos's own USB
driver needed a second Address Device for that stick, so a read error is one
candidate; the loader growing by 5 KB is another. A refusal from here on says
which.

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

---

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
  instructions: the kernel draws its boot log from stage 2, so a black panel
  after that line is before `hal_fb_early`.
- `the disk: diff` in the kernel's line is the disk changing after the loader
  read it.

---

## 6. How it is tested

- **`tools/test_efiboot.c`**, in `make test`: the header parser on synthetic
  images and on the build's own `kosmos.bin`, the map conversion on an
  unsorted, fragmented map with oversized descriptors, and an information
  structure built by `mbi.c` and read back with the kernel's own `mb2_find` and
  `mb2_framebuffer_from`. `testing.md` §18.42 has the controls.
- **`tools/run_uefi.py`**, in `make test`: the stick `mkusb_image.py` makes,
  with a 4 MB disk `kfs.lua` made, booted under OVMF on an xHCI controller as
  a USB drive - the loader's lines, the kernel's line about what it was
  handed, `the disk: same` in it (`none` for a stick without one), the kernel
  at 16 MB, nothing of the firmware's under it, and the screen, ACPI, SMBIOS
  and the other processors through that path. **And a refusal**: a second
  stick, whose kernel is zeros, must be refused on the serial line and drawn
  on in the lower half of the screen by the loader itself.

**What QEMU cannot show**: the ThinkPad's own map at the moment the loader
runs, whether its firmware writes into memory it has handed out, and the
console-control switch - under OVMF the loader never has to make it, so that
call has run only on the ThinkPad, if at all. The loader's lines on that
machine are the measurement.

---

## 7. What is not done

- **The kernel is not relocatable**, so its address is a fixed claim the
  loader can only check.
- **The disk on a stick is still refused over 32 MB** by `mkusb_image.py`,
  until the ThinkPad has booted a bigger one through this loader.
- **Secure Boot** is not supported; the loader is unsigned, as GRUB was.
- **The screen is the firmware's current mode.** The loader does not choose
  one.
- **Why the first ThinkPad stick through this loader refused** is not known;
  §3 has both boots.
