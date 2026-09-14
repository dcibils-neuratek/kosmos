#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Whether a stick holds the image written to it, and where it does not.

**Nothing between the build and the kernel ever asked.** `tools/mkusb.sh`
wrote the image with `dd` and ejected; the loader fingerprints what it reads
off the stick, which says memory kept those bytes and nothing about whether
they are the build's; and the kernel's canary knows the build's sums for the
userland image only. A stick that gives back bytes nobody wrote was
undetectable from one end of the chain to the other - and it is a cause that
fits the ThinkPad's boots since 11 September, which followed the build and the
disk size and never once happened under QEMU, which reads the image file and
not the stick (`docs/boot.md`).

So `mkusb.sh` reads the stick back after writing it and pipes it here, and
this compares it with the image sector by sector and names every difference:
the protective MBR, the GPT, the FAT's own sectors, or a file on the EFI
System Partition and the offset in it - and for the kernel, the page, the
address it is loaded at and the ELF section it belongs to, when given the
ELF. One line saying which part of the kernel a stick corrupted is worth more
than any number of boots of it.

**Two kinds of difference, because macOS mounts the stick the moment `dd`
lets go of it**, and a mount writes: a free cluster taken for `.fseventsd`,
a directory entry in a slot that was empty, the FSInfo sector's free count,
the dirty bits in the second FAT entry. None of that is anything the firmware
reads to find a file. So a difference is **bookkeeping** only when it is
exactly that - an entry of a cluster the image left free, a directory slot
the image left empty, a last-access date, FSInfo - and **damage** everywhere
else: any byte of a file, any FAT entry of a cluster a file uses, any
directory entry the image wrote, the boot sector, the GPT.

**READBACK is the stick's raw device**, which `mkusb.sh` hands it as root,
so nothing is copied onto a disk that may be nearly full and there is no pipe
between the stick and the verdict - the first version had `dd` piped in, and
its SIGPIPE refused a stick this had just called perfect. `-` reads standard
input instead, which is how `test_stickcheck.py` streams its faults in. Either
way only as much is read as the image has.

Exit 0 when every sector matches; 3 when only bookkeeping differs; 1 for
damage, or when the stick gave back fewer bytes than the image has; 2 when it
could not be asked.

Usage: stickcheck.py IMAGE DEVICE|READBACK|- [KERNEL.ELF]
"""

import struct
import subprocess
import sys

SECTOR = 512
CHUNK = 4 * 1024 * 1024

# Where the x86-64 kernel is loaded, from `boot/x86_64/kosmos.ld`, so an offset
# in `\boot\kosmos.bin` is an address.
KERNEL_LOAD = 0x01000000

# Differences listed; the rest are counted.
SHOWN = 40

DAMAGE, BOOKKEEPING = "damage", "bookkeeping"


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Layout:
    """What each byte of an image is: the GPT's first partition read as the
    FAT32 filesystem `mkusb_image.py` makes, and every file's clusters."""

    def __init__(self, path):
        self.f = open(path, "rb")
        header = self.at(SECTOR, 92)

        if header[:8] != b"EFI PART":
            raise ValueError("%s has no GPT" % path)

        entries = struct.unpack_from("<Q", header, 72)[0]
        first, last = struct.unpack_from("<QQ", self.at(entries * SECTOR, 128),
                                         32)
        self.esp = first * SECTOR
        self.esp_end = (last + 1) * SECTOR

        boot = self.at(self.esp, SECTOR)
        bps, spc, reserved, fats = u16(boot, 11), boot[13], u16(boot, 14), boot[16]
        spf = u32(boot, 36)

        self.fsinfo = {u16(boot, 48), u16(boot, 48) + u16(boot, 50)}
        self.fat_start = self.esp + reserved * bps
        self.fat_bytes = spf * bps
        self.data = self.esp + (reserved + fats * spf) * bps
        self.cluster = spc * bps
        self.fat = self.at(self.fat_start, self.fat_bytes)
        self.regions = [(0, SECTOR, "the protective MBR", None),
                        (SECTOR, self.esp, "the GPT", None),
                        (self.esp, self.fat_start,
                         "the ESP's boot sector and reserved sectors", None),
                        (self.fat_start, self.data, "the FATs", None)]
        self.walk(u32(boot, 44), "")
        self.regions.sort()

    def at(self, offset, n):
        self.f.seek(offset)
        return self.f.read(n)

    def chain(self, first):
        clusters, c = [], first

        while 2 <= c < 0x0FFFFFF8 and len(clusters) < len(self.fat) // 4:
            clusters.append(c)
            c = u32(self.fat, c * 4) & 0x0FFFFFFF

        return clusters

    def offset(self, cluster):
        return self.data + (cluster - 2) * self.cluster

    def walk(self, first, path):
        long_name = []

        for c in self.chain(first):
            block = self.at(self.offset(c), self.cluster)
            self.regions.append((self.offset(c), self.offset(c) + self.cluster,
                                 "the directory %s" % (path or "/"), None))

            for o in range(0, self.cluster, 32):
                e = block[o:o + 32]

                if e[0] == 0:
                    return

                if e[0] == 0xE5:
                    long_name = []
                    continue

                if e[11] == 0x0F:
                    text = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le",
                                                                  "replace")
                    long_name.insert(0, text)
                    continue

                if long_name:
                    name = "".join(long_name).split("\x00")[0]
                else:
                    stem = e[0:8].decode("ascii", "replace").rstrip()
                    ext = e[8:11].decode("ascii", "replace").rstrip()
                    name = stem + ("." + ext if ext else "")

                long_name = []

                if name in (".", "..") or e[11] & 0x08:
                    continue

                start, size = (u16(e, 20) << 16) | u16(e, 26), u32(e, 28)

                if e[11] & 0x10:
                    self.walk(start, path + "/" + name)
                    continue

                done = 0

                for fc in self.chain(start):
                    if done >= size:
                        break

                    self.regions.append((self.offset(fc),
                                         self.offset(fc) + self.cluster,
                                         path + "/" + name, done))
                    done += self.cluster

    def where(self, offset):
        for start, end, label, base in self.regions:
            if start <= offset < end:
                return label, None if base is None else base + offset - start

        if offset >= self.esp_end:
            return "past the ESP, where the backup GPT is", None

        return "free space in the ESP", None

    def kind(self, offset, want, got):
        """Damage or bookkeeping, for one sector that differs."""
        label, in_file = self.where(offset)

        if in_file is not None:
            return DAMAGE

        if label == "free space in the ESP":
            return BOOKKEEPING

        if label == "the ESP's boot sector and reserved sectors":
            sector = (offset - self.esp) // SECTOR
            return BOOKKEEPING if sector in self.fsinfo else DAMAGE

        if label == "the FATs":
            base = (offset - self.fat_start) % self.fat_bytes

            for j in range(0, SECTOR, 4):
                if want[j:j + 4] == got[j:j + 4]:
                    continue

                cluster = (base + j) // 4
                entry = u32(want, j) & 0x0FFFFFFF

                # FAT[1] carries the clean-shutdown and no-error bits.
                if cluster == 1:
                    continue

                if cluster < 2 or entry != 0:
                    return DAMAGE

            return BOOKKEEPING

        if label.startswith("the directory"):
            for j in range(0, SECTOR, 32):
                a, b = want[j:j + 32], got[j:j + 32]

                if a == b or a[0] in (0x00, 0xE5):
                    continue

                # A short entry's last-access date, which reading touches.
                if a[11] != 0x0F and a[:18] == b[:18] and a[20:] == b[20:]:
                    continue

                return DAMAGE

            return BOOKKEEPING

        return DAMAGE


class Kernel:
    """Sections and the userland image's extent, out of the kernel's ELF."""

    def __init__(self, path):
        elf = open(path, "rb").read()
        shoff = struct.unpack_from("<Q", elf, 0x28)[0]
        size, count, names = struct.unpack_from("<HHH", elf, 0x3A)
        strings = struct.unpack_from("<Q", elf, shoff + names * size + 24)[0]
        self.sections = []

        for i in range(count):
            s = shoff + i * size
            name = u32(elf, s)
            address, _, length = struct.unpack_from("<QQQ", elf, s + 16)
            text = elf[strings + name:elf.index(b"\x00", strings + name)]

            if address:
                self.sections.append((address, address + length,
                                      text.decode()))

        listing = subprocess.run(["x86_64-elf-nm", path], capture_output=True,
                                 text=True).stdout
        symbols = {}

        for line in listing.splitlines():
            parts = line.split()

            if len(parts) == 3:
                symbols[parts[2]] = int(parts[0], 16)

        # The image's bytes end where its length variable begins; `bin2c.py`
        # emits them in that order.
        self.userland = (symbols.get("init_image"), symbols.get("init_image_len"))

    def describe(self, address):
        text = ""

        for lo, hi, name in self.sections:
            if lo <= address < hi:
                text += " in %s" % name

        lo, hi = self.userland

        if lo is not None and hi is not None and lo <= address < hi:
            text += " (the userland image)"

        return text


def describe(layout, kernel, offset):
    label, in_file = layout.where(offset)

    if in_file is None:
        return label

    text = "%s, byte %#x of it" % (label, in_file)

    if label.lower().endswith("/kosmos.bin"):
        address = KERNEL_LOAD + in_file
        text += ", kernel page %d at %#010x" % (in_file // 4096, address)

        if kernel is not None:
            text += kernel.describe(address)

    return text


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip().splitlines()[-1])
        return 2

    try:
        layout = Layout(sys.argv[1])
    except (OSError, ValueError) as e:
        print("stickcheck: %s" % e)
        return 2

    kernel = Kernel(sys.argv[3]) if len(sys.argv) > 3 else None
    image = open(sys.argv[1], "rb")

    #
    # A device is read unbuffered, so every read is exactly the size asked
    # for - a whole number of sectors, which is all a raw disk will answer.
    #
    try:
        back = (sys.stdin.buffer if sys.argv[2] == "-"
                else open(sys.argv[2], "rb", buffering=0))
    except OSError as e:
        print("stickcheck: cannot read %s: %s" % (sys.argv[2], e))
        return 2

    image.seek(0, 2)
    size = image.tell()
    image.seek(0)

    ranges = []                 # [start, end, kind, first, wanted, held]
    received = 0
    position = 0

    while position < size:
        want = image.read(CHUNK)
        got = b""

        while len(got) < len(want):
            more = back.read(len(want) - len(got))

            if not more:
                break

            got += more

        received += len(got)
        got += b"\x00" * (len(want) - len(got))

        if want != got:
            for s in range(0, len(want), SECTOR):
                a, b = want[s:s + SECTOR], got[s:s + SECTOR]

                if a == b:
                    continue

                o = position + s
                kind = layout.kind(o, a, b)

                if ranges and ranges[-1][1] == o and ranges[-1][2] == kind:
                    ranges[-1][1] = o + SECTOR
                else:
                    first = next(i for i in range(len(a)) if a[i] != b[i])
                    ranges.append([o, o + SECTOR, kind, o + first,
                                   a[first:first + 16], b[first:first + 16]])

        position += len(want)

    damage = [r for r in ranges if r[2] == DAMAGE]
    bookkeeping = [r for r in ranges if r[2] == BOOKKEEPING]

    if received < size:
        print("stickcheck: the stick gave back %d of the image's %d bytes, so "
              "the rest could not be checked" % (received, size))

    if not ranges:
        if received < size:
            return 1

        print("stickcheck: the stick holds the image, every one of its %d "
              "sectors as written" % (size // SECTOR))
        return 0

    if damage:
        print("stickcheck: THE STICK DOES NOT HOLD THE IMAGE - %d sectors of "
              "it differ where the firmware reads:"
              % (sum(r[1] - r[0] for r in damage) // SECTOR))
    else:
        print("stickcheck: every file, FAT entry and directory entry the image "
              "wrote is on the stick; %d sectors of filesystem bookkeeping "
              "differ, which is what mounting the stick writes:"
              % (sum(r[1] - r[0] for r in bookkeeping) // SECTOR))

    for start, end, kind, first, wanted, held in (damage + bookkeeping)[:SHOWN]:
        print("  %-11s %#010x..%#010x %6d sectors  %s"
              % (kind, start, end, (end - start) // SECTOR,
                 describe(layout, kernel, start)))

        if end - start > SECTOR:
            print("  %36s to %s" % ("", describe(layout, kernel, end - 1)))

        print("  %36s at %#x the image has %s, the stick %s"
              % ("", first, wanted.hex(), held.hex()))

    if len(ranges) > SHOWN:
        print("  ... and %d more" % (len(ranges) - SHOWN))

    return 1 if damage or received < size else 3


if __name__ == "__main__":
    sys.exit(main())
