#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Builds the USB image the ThinkPad boots: GPT, one ESP, GRUB, Kosmos.

**This replaced a `grub-mkrescue` ISO, and the reason is a real failure on
real firmware rather than a preference.**

That ISO booted perfectly under OVMF and got this far on the machine:

    error: file '/boot/grub/x86_64-efi/boot.mod' not found.
    Entering rescue mode...

Which is good news wearing a bad hat: the firmware found the stick, read
its partition table, launched `BOOTX64.EFI`, and GRUB *ran*. What GRUB
could not do was load its own modules - because `grub-mkrescue` puts them
only inside the El Torito FAT image and leaves nothing at
`/boot/grub/x86_64-efi` on the ISO9660 filesystem beside it. Under QEMU
GRUB's idea of its root resolved to the FAT image and the modules were
there; on this firmware it resolved somewhere else and they were not.

So nothing here depends on which filesystem GRUB thinks it booted from:

  - **the modules are built into `BOOTX64.EFI`**, so there is no loading to
    fail. `grub-mkimage` takes them as arguments and links them in;
  - **there is one filesystem, FAT**, which is what a UEFI firmware is
    required to be able to read - no ISO9660 in the picture at all;
  - `grub.cfg` and the kernel sit on that same partition, at the prefix the
    image was built with.

The result is a plain GPT disk with a single EFI System Partition, which is
the arrangement every UEFI machine is specified to boot and the one this
machine has already proved it can read.

**And a disk image beside the kernel, when there is one.** `--disk PATH`
copies a kfs image onto the same partition and tells GRUB to load it as a
module; `hal/pc/memdisk.c` then presents that memory as the machine's disk,
which is how a ThinkPad this kernel cannot yet read a USB stick on gets its
game data. The partition grows to hold it.

Usage: mkusb_image.py KERNEL OUT [--disk IMAGE] [name=value ...]
"""

import re
import os
import struct
import subprocess
import sys
import zlib

ESP_MB = 64

# The partition type every UEFI firmware looks for, and the one this image
# has exactly one of. UEFI 2.10, table 5.7.
ESP_TYPE_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

SECTOR = 512

#
# **Only what GRUB needs in order to read the partition it was loaded
# from.** Everything else is loaded from that partition at run time, which
# is not laziness - it is what works.
#
# The first version linked twenty-two modules in, including `efi_gop`, and
# GRUB then would not set a video mode at all: Kosmos came up with `none
# attached` on a machine whose firmware had a perfectly good panel.
# Swapping in `grub-mkrescue`'s own 200 KB binary, into this same image,
# fixed it immediately - so it was the core, not the layout, and a
# minimal core that `insmod`s the rest is the arrangement known to work.
#
# What must be built in is exactly the bootstrap: a module that fails to
# load cannot be the module that loads modules. `part_gpt` and `fat` to
# find and read this partition, `normal` and `configfile` to run
# `grub.cfg`, and `search` for when `$root` disappoints.
#
CORE_MODULES = [
    "part_gpt", "part_msdos", "fat", "normal", "configfile",
    "search", "search_fs_uuid", "search_label",
]

#
# `insmod` rather than built in, for the reason above - and `efi_gop`
# rather than `all_video`, which picks GRUB's own bochs driver under QEMU
# and hands over 800x600 at 24 bits per pixel, a depth `loader_fb.c`
# refuses because everything above `struct fb` treats a pixel as one
# 32-bit word. A laptop has no bochs; asking the firmware directly gives
# the panel its own mode.
#
GRUB_CFG = """set timeout=3
set default=0

menuentry "Kosmos" {
    insmod efi_gop
    insmod multiboot2
    multiboot2 /boot/kosmos.bin@ARGS@
@MODULE@    boot
}
"""


def guid_bytes(text):
    """A GUID as it is stored: first three fields little-endian, rest not."""
    a, b, c, d, e = text.split("-")

    return (struct.pack("<IHH", int(a, 16), int(b, 16), int(c, 16))
            + bytes.fromhex(d) + bytes.fromhex(e))


def run(argv, **kw):
    result = subprocess.run(argv, capture_output=True, text=True, **kw)

    if result.returncode != 0:
        sys.exit("mkusb_image: %s failed:\n%s%s"
                 % (argv[0], result.stdout, result.stderr))

    return result.stdout


def build_efi(grub_dir, out):
    """GRUB, with every module it needs linked in rather than alongside."""
    run(["x86_64-elf-grub-mkimage",
         "-O", "x86_64-efi",
         "-d", grub_dir,
         # Where `grub.cfg` is, on the partition this image lives on.
         "-p", "/boot/grub",
         "-o", out] + CORE_MODULES)


def build_esp(kernel, efi, out, grub_dir, args="", disk=None):
    """A FAT filesystem holding the loader, its configuration and Kosmos."""
    size = ESP_MB * 1024 * 1024

    # The disk on top of what the rest needs, rounded up to a megabyte, and
    # eight more for FAT's own tables at that size.
    if disk:
        size += ((os.path.getsize(disk) + 0xFFFFF) // 0x100000 + 8) * 1024 * 1024

    with open(out, "wb") as f:
        f.truncate(size)

    # FAT32, one sector per cluster group chosen by mformat, and a label
    # so `search --label` has something to find if `$root` ever goes wrong.
    run(["mformat", "-i", out, "-F", "-v", "KOSMOS", "-T", str(size // SECTOR),
         "::"])

    for d in ("::/EFI", "::/EFI/BOOT", "::/boot", "::/boot/grub"):
        run(["mmd", "-i", out, d])

    cfg = out + ".cfg"

    with open(cfg, "w") as f:
        f.write(GRUB_CFG.replace("@ARGS@", (" " + args) if args else "")
                        .replace("@MODULE@",
                                 "    module2 /boot/disk.img\n" if disk else ""))

    run(["mcopy", "-i", out, efi, "::/EFI/BOOT/BOOTX64.EFI"])
    run(["mcopy", "-i", out, cfg, "::/boot/grub/grub.cfg"])
    run(["mcopy", "-i", out, kernel, "::/boot/kosmos.bin"])

    if disk:
        run(["mcopy", "-i", out, disk, "::/boot/disk.img"])

    #
    # **And the module directory as well, beside the ones built in.**
    #
    # The built-in set is what has to work before anything can be read at
    # all - a module that fails to load cannot be the thing that loads
    # modules. This is the rest, and it exists because guessing the set was
    # wrong once: with `efi_gop` linked in and the directory absent, GRUB
    # would not set a video mode at all and Kosmos came up with `none
    # attached` on a machine whose firmware had a perfectly good panel.
    # `grub-mkrescue`'s image could load whatever it turned out to need;
    # this one can now too.
    #
    # A few megabytes on a stick, against a class of failure that only
    # appears on hardware.
    #
    run(["mmd", "-i", out, "::/boot/grub/x86_64-efi"])

    mods = sorted(f for f in os.listdir(grub_dir)
                  if f.endswith(".mod") or f.endswith(".lst"))

    # In batches, because a command line has a limit and there are 268.
    for i in range(0, len(mods), 40):
        batch = [os.path.join(grub_dir, m) for m in mods[i:i + 40]]
        run(["mcopy", "-i", out] + batch + ["::/boot/grub/x86_64-efi/"])

    os.remove(cfg)

    return size


def write_gpt(path, esp, esp_size):
    """A protective MBR, a GPT at both ends, and one partition between them.

    Written here rather than shelled out to, because the tools that do this
    on a Mac want to be root and this does not need to be: the image is a
    file until somebody deliberately copies it to a device.
    """
    first_usable = 34
    esp_sectors = esp_size // SECTOR
    last = first_usable + esp_sectors - 1
    total = last + 34            # room for the backup table at the end

    with open(path, "wb") as f:
        f.truncate(total * SECTOR)

        # A protective MBR: one partition of type 0xEE covering everything,
        # so a tool that only understands MBR sees the disk as in use
        # rather than as empty.
        mbr = bytearray(SECTOR)
        mbr[446:462] = struct.pack("<BBBBBBBBII", 0x00, 0x00, 0x02, 0x00,
                                   0xEE, 0xFF, 0xFF, 0xFF,
                                   1, min(total - 1, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xAA"
        f.write(mbr)

        entries = bytearray(128 * 128)
        entries[0:16] = guid_bytes(ESP_TYPE_GUID)
        entries[16:32] = os.urandom(16)             # this partition's own id
        entries[32:40] = struct.pack("<Q", first_usable)
        entries[40:48] = struct.pack("<Q", last)
        entries[48:56] = struct.pack("<Q", 0)
        # **Padded to the full 72 bytes on purpose.** Assigning a shorter
        # value into a longer slice of a `bytearray` *resizes* it, which
        # left this table ten bytes short and its checksum computed over
        # the wrong buffer - and firmware that verifies the CRC then
        # ignores the disk, which is a stick the machine will not boot with
        # nothing on screen to say why.
        name = "KOSMOS".encode("utf-16-le")
        entries[56:128] = name + b"\x00" * (72 - len(name))

        entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
        disk_guid = os.urandom(16)

        def header(my_lba, other_lba, entries_lba):
            h = bytearray(92)
            h[0:8] = b"EFI PART"
            h[8:12] = struct.pack("<I", 0x00010000)
            h[12:16] = struct.pack("<I", 92)
            h[20:24] = struct.pack("<I", 0)          # crc, filled below
            h[24:32] = struct.pack("<Q", my_lba)
            h[32:40] = struct.pack("<Q", other_lba)
            h[40:48] = struct.pack("<Q", first_usable)
            h[48:56] = struct.pack("<Q", last)
            h[56:72] = disk_guid
            h[72:80] = struct.pack("<Q", entries_lba)
            h[80:84] = struct.pack("<I", 128)
            h[84:88] = struct.pack("<I", 128)
            h[88:92] = struct.pack("<I", entries_crc)
            h[16:20] = struct.pack("<I", zlib.crc32(bytes(h)) & 0xFFFFFFFF)

            return bytes(h) + b"\x00" * (SECTOR - 92)

        f.seek(1 * SECTOR)
        f.write(header(1, total - 1, 2))

        f.seek(2 * SECTOR)
        f.write(bytes(entries))

        f.seek(first_usable * SECTOR)

        with open(esp, "rb") as src:
            f.write(src.read())

        # And the same table again at the far end, which the specification
        # requires and some firmware checks.
        f.seek((total - 33) * SECTOR)
        f.write(bytes(entries))

        f.seek((total - 1) * SECTOR)
        f.write(header(total - 1, 1, total - 33))

    return total * SECTOR


def main():
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.bin"
    out = sys.argv[2] if len(sys.argv) > 2 else "build/x86_64/kosmos-usb.img"

    # Words for the kernel's command line, after the path on GRUB's
    # `multiboot2` line - `opt/kosmos/smp=1` and the like, which is how a
    # machine with no fw_cfg is given a boot option at all. Checked, because
    # they are written into a GRUB script, where a quote or a semicolon would
    # make it a different script.
    words = sys.argv[3:]
    disk = None

    if len(words) >= 2 and words[0] == "--disk":
        disk, words = words[1], words[2:]

        if not os.path.isfile(disk):
            sys.exit("mkusb_image: no disk image at %s" % disk)

    args = " ".join(words)

    if args and not re.fullmatch(r"[A-Za-z0-9_./=,:-]+( [A-Za-z0-9_./=,:-]+)*", args):
        sys.exit("mkusb_image: %r is not a list of name=value words" % args)

    if not os.path.exists(kernel):
        sys.exit("mkusb_image: no %s. Run `make x86-build`." % kernel)

    prefix = run(["brew", "--prefix", "x86_64-elf-grub"]).strip()
    grub_dir = os.path.join(prefix, "lib/x86_64-elf/grub/x86_64-efi")

    if not os.path.isdir(grub_dir):
        sys.exit("mkusb_image: no GRUB modules at %s" % grub_dir)

    work = out + ".parts"
    os.makedirs(work, exist_ok=True)

    efi = os.path.join(work, "BOOTX64.EFI")
    esp = os.path.join(work, "esp.img")

    build_efi(grub_dir, efi)
    esp_size = build_esp(kernel, efi, esp, grub_dir, args, disk)
    total = write_gpt(out, esp, esp_size)

    print("%s  %.1f MB  (GRUB %.0f KB with %d modules built in)%s%s"
          % (out, total / 1e6, os.path.getsize(efi) / 1024, len(CORE_MODULES),
             "; the kernel is told: " + args if args else "",
             "; with %s as its disk, %.1f MB" % (disk, os.path.getsize(disk) / 1e6)
             if disk else ""))


if __name__ == "__main__":
    sys.exit(main())
