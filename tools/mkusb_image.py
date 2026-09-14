#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Builds the USB image the ThinkPad boots: GPT, one ESP, Kosmos's own loader,
and Kosmos.

**The loader is `boot/efi/`, and it replaced GRUB on 13 September 2026.** An
image GRUB loaded on the ThinkPad arrived with bytes already changed, in some
layouts and not others, and GRUB reported nothing; under OVMF the same GRUB
had been loading the kernel over memory the firmware keeps. `docs/boot.md`
has the account. The loader claims the kernel's range from the firmware,
refuses on the screen what it cannot claim, checks the kernel byte for byte
before and after the firmware lets go, and says so.

**GRUB taught this file two things that stay true without it:**

  - **one filesystem, FAT**, which is what a UEFI firmware is required to be
    able to read. A `grub-mkrescue` ISO once booted under OVMF and dropped to
    `grub rescue>` on the machine, because its modules were only inside an El
    Torito image the firmware did not choose;
  - **a plain GPT disk with a single EFI System Partition**, the arrangement
    every UEFI machine is specified to boot and the one this machine has
    proved it reads.

So the stick holds `\\EFI\\BOOT\\BOOTX64.EFI` (the loader), `\\boot\\kosmos.bin`
(the kernel), `\\boot\\kosmos.cmdline` when there are words for the kernel's
command line, and `\\boot\\disk.img` when there is a disk. That is everything
the loader reads.

**And a disk image beside the kernel, when there is one.** `--disk PATH`
copies a kfs image onto the partition; the loader reads it into memory and
hands it over as a module, and `hal/pc/memdisk.c` presents that memory as the
machine's disk - which is how a ThinkPad Kosmos cannot yet read a USB stick
on gets its game data.

Usage: mkusb_image.py KERNEL OUT --loader BOOTX64.EFI [--disk IMAGE]
                      [name=value ...]
"""

import re
import os
import struct
import subprocess
import sys
import zlib

#
# **192, and the number is FAT32's rather than ours.**
#
# This was 64, which is ample for a 10 MB kernel and a loader - and produced
# a filesystem the ThinkPad's firmware would list and refuse to boot.
#
# FAT32 is *defined* as having at least 65,525 clusters; a volume that
# cannot reach that is not FAT32 no matter what the boot sector says. At
# 64 MB the only way to get there is 512-byte clusters, which is what
# `mformat` duly chose - legal, rare, and unloved by UEFI firmware FAT
# drivers. Every stick that ever booted this machine happened to carry a
# disk image too, which pushed the partition past 100 MB and let mformat
# use 1 KB clusters. The no-disk image was never bootable on hardware and
# nobody noticed, because under QEMU it works: an emulator's FAT driver
# does not care.
#
# 192 MB reaches 65,525 clusters comfortably at 1 KB each, which is the
# shape of the images known to work. It costs nothing but space on a stick
# that is gigabytes.
#
ESP_MB = 192

#
# **The disk image is 32 MB or less, and a bigger one is refused - for now.**
#
# Measured on the ThinkPad with GRUB, not derived: GRUB carried the disk into
# memory, and on that machine the size decided whether the kernel survived.
# 0.10.48 with a 64 MB disk stopped after GRUB's last line, twice, and the
# same build with a 32 MB disk booted; then 0.10.55 with a 32 MB disk stopped
# too. Size was a knob that moved where things landed, not a cause
# (`docs/boot.md`).
#
# **Kept until the ThinkPad has booted a bigger disk through Kosmos's own
# loader**, because a rule that came from a machine should go the same way.
# The loader places the disk wherever the firmware gives it memory and checks
# the kernel after, so there is no longer a reason the size should matter -
# and "no reason" is a prediction until the machine agrees.
#
STICK_DISK_MAX_MB = 32

# The partition type every UEFI firmware looks for, and the one this image
# has exactly one of. UEFI 2.10, table 5.7.
ESP_TYPE_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

SECTOR = 512


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


def page_sums(path):
    """The sums file the loader holds a read of `path` to (`boot/efi/sums.h`):
    "KOSMSUMS", the file's size, the page size, and FNV-1a over each 4096
    bytes of it.

    A page of zeros is summed once, because most of a disk image is zeros and
    this is Python: a 32 MB disk would otherwise be thirty-three million turns
    of the loop below.
    """
    with open(path, "rb") as handle:
        data = handle.read()

    def fnv(chunk):
        h = 0xcbf29ce484222325

        for b in chunk:
            h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff

        return h

    zeros = bytes(4096)
    zero_sum = fnv(zeros)
    out = bytearray(b"KOSMSUMS")
    out += struct.pack("<QQ", len(data), 4096)

    for at in range(0, len(data), 4096):
        chunk = data[at:at + 4096]
        out += struct.pack("<Q", zero_sum if chunk == zeros else fnv(chunk))

    return bytes(out)


def put_sums(image, path, name):
    """`path`'s sums, onto the stick as `name`."""
    sums = image + ".sums"

    with open(sums, "wb") as handle:
        handle.write(page_sums(path))

    run(["mcopy", "-i", image, sums, name])
    os.remove(sums)


def build_esp(kernel, loader, out, args="", disk=None):
    """A FAT filesystem holding the loader, the kernel and what it reads."""
    size = ESP_MB * 1024 * 1024

    # The disk on top of what the rest needs, rounded up to a megabyte, and
    # eight more for FAT's own tables at that size.
    if disk:
        size += ((os.path.getsize(disk) + 0xFFFFF) // 0x100000 + 8) * 1024 * 1024

    with open(out, "wb") as f:
        f.truncate(size)

    # FAT32 with a label, and `-c 2` - a kilobyte a cluster, said rather than
    # left to mformat, which maximises the cluster *count* and so picks the
    # smallest cluster that will do. See ESP_MB above for what that cost.
    run(["mformat", "-i", out, "-F", "-c", "2", "-v", "KOSMOS",
         "-T", str(size // SECTOR), "::"])

    for d in ("::/EFI", "::/EFI/BOOT", "::/boot"):
        run(["mmd", "-i", out, d])

    run(["mcopy", "-i", out, loader, "::/EFI/BOOT/BOOTX64.EFI"])
    run(["mcopy", "-i", out, kernel, "::/boot/kosmos.bin"])

    # And the build's sums of it, which the loader holds its read to: a stick
    # that hands back other bytes is refused on the screen, with the page.
    put_sums(out, kernel, "::/boot/kosmos.sums")

    # The kernel's command line, a file the loader reads rather than a line
    # in a GRUB script. The words were checked in main().
    if args:
        cmdline = out + ".cmdline"

        with open(cmdline, "w") as f:
            f.write(args + "\n")

        run(["mcopy", "-i", out, cmdline, "::/boot/kosmos.cmdline"])
        os.remove(cmdline)

    if disk:
        run(["mcopy", "-i", out, disk, "::/boot/disk.img"])
        put_sums(out, disk, "::/boot/disk.sums")

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

    # Words for the kernel's command line - `opt/kosmos/smp=1` and the like,
    # which is how a machine with no fw_cfg is given a boot option at all.
    # Checked, because the loader passes on only these characters and a word
    # it would cut short should be refused here, where somebody can read why.
    words = sys.argv[3:]
    loader = None
    disk = None

    while len(words) >= 2 and words[0] in ("--loader", "--disk"):
        if words[0] == "--loader":
            loader = words[1]
        else:
            disk = words[1]

        words = words[2:]

    if loader is None or not os.path.isfile(loader):
        sys.exit("mkusb_image: no loader at %s. Run `make %s`."
                 % (loader, "build/x86_64/BOOTX64.EFI"))

    if disk is not None:
        if not os.path.isfile(disk):
            sys.exit("mkusb_image: no disk image at %s" % disk)

        if os.path.getsize(disk) > STICK_DISK_MAX_MB * 1024 * 1024:
            sys.exit("mkusb_image: %s is %.0f MB, and the ThinkPad has not yet "
                     "booted a disk over %d MB (docs/boot.md). Make one that "
                     "size:\n  build/host/lua tools/kfs.lua create %s %d "
                     "host-file:/home/name ..."
                     % (disk, os.path.getsize(disk) / 1048576.0,
                        STICK_DISK_MAX_MB, disk, STICK_DISK_MAX_MB))

    args = " ".join(words)

    if args and not re.fullmatch(r"[A-Za-z0-9_./=,:-]+( [A-Za-z0-9_./=,:-]+)*", args):
        sys.exit("mkusb_image: %r is not a list of name=value words" % args)

    if len(args) > 200:
        sys.exit("mkusb_image: the loader reads 200 characters of the command "
                 "line, and these words are %d" % len(args))

    if not os.path.exists(kernel):
        sys.exit("mkusb_image: no %s. Run `make x86-build`." % kernel)

    work = out + ".parts"
    os.makedirs(work, exist_ok=True)

    esp = os.path.join(work, "esp.img")
    esp_size = build_esp(kernel, loader, esp, args, disk)
    total = write_gpt(out, esp, esp_size)

    print("%s  %.1f MB  (Kosmos's loader %.0f KB)%s%s"
          % (out, total / 1e6, os.path.getsize(loader) / 1024,
             "; the kernel is told: " + args if args else "",
             "; with %s as its disk, %.1f MB" % (disk, os.path.getsize(disk) / 1e6)
             if disk else ""))


if __name__ == "__main__":
    sys.exit(main())
