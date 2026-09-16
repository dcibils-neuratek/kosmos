#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Two FAT volumes on one stick, made by mtools (USB step 6b).

**Somebody else's reading of the format.** `tools/test_fat.py` already holds
`fat_decode.c` to volumes mtools wrote, for the reason `usb.md` gives: the
reader is ours and the drives it will read are not. This is the same idea one
layer up - the drive server walking volumes this project did not lay out.

**Two partitions, and that is the point rather than thoroughness.** The first
version of this made one, and a stride bug survived a green test because of
it: the server answered a listing with 104-byte volume records and the
namespace decoded them as 80-byte entries, so the *first* name was right - a
name is the first 64 bytes of both - and everything after it read into the
middle of the previous record. One volume is exactly the case that cannot
tell the two apart. Two differ at the second name, which is where the fault
begins.

They differ in kind as well as in label, so one fixture covers FAT32's
cluster chain and FAT16's fixed root directory - two quite different walks
through `fat_decode.c`.

  1. `PHOTOS`, FAT32, one sector a cluster, 48 MB
  2. `BACKUP`, FAT16, four sectors a cluster, 12 MB

The files are chosen for what they exercise rather than to be interesting:
a short name, a long name of 3000 bytes (a chain of six clusters on the
FAT32 volume, so a read has to follow the table rather than run off the
first cluster), and one directory down.
"""

import os
import shutil
import struct
import subprocess
import tempfile

SECTOR = 512

# (label, mformat arguments, first sector, sectors, MBR type byte)
#
# 0x0C is FAT32 with LBA addressing and 0x0E is FAT16 with it, which is what
# anything modern writes (`drives_decode.h`). Both are offered to the boot
# sector anyway - a type byte is one byte somebody may have written by hand,
# and the boot sector is what decides.
VOLUMES = [
    ("PHOTOS", ["-F", "-c", "1"], 2048, 98304, 0x0C),
    ("BACKUP", ["-c", "4"], 100352, 24576, 0x0E),
]

SECTORS = 131072                # 64 MB: both partitions and room to spare

# (path on the volume, bytes) - read back from here, never from the volume.
FILES = {
    "PHOTOS": [
        ("hello.txt", b"Kosmos reads a drive.\n"),
        ("A Long File Name.txt", b"x" * 3000),
        ("Italy/roma.txt", b"roma\n"),
    ],
    "BACKUP": [
        ("notes.txt", b"the second volume\n"),
    ],
}


def available():
    """Whether mtools is here. The caller prints the SKIP, as test_fat does."""
    return (shutil.which("mformat") is not None
            and shutil.which("mcopy") is not None
            and shutil.which("mmd") is not None)


def build(path):
    """Writes the image at `path`. Returns its size in blocks."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")

    def run(argv):
        r = subprocess.run(argv, capture_output=True, text=True, env=env)

        if r.returncode != 0:
            raise SystemExit("fatstick: %s failed: %s"
                             % (" ".join(argv), r.stderr.strip()))

    with open(path, "wb") as f:
        f.truncate(SECTORS * SECTOR)

    for label, args, first, sectors, _type in VOLUMES:
        at = "%s@@%d" % (path, first * SECTOR)

        run(["mformat", "-i", at] + args
            + ["-v", label, "-T", str(sectors), "::"])

        work = tempfile.mkdtemp(prefix="kosmos-fatstick-")
        made = set()

        for name, data in FILES.get(label, []):
            local = os.path.join(work, os.path.basename(name))

            with open(local, "wb") as f:
                f.write(data)

            folder = os.path.dirname(name)

            if folder and folder not in made:
                run(["mmd", "-i", at, "::" + folder])
                made.add(folder)

            run(["mcopy", "-i", at, local, "::" + name])

    # The partition table last, so mformat cannot overwrite it: each entry's
    # type at +4, its first sector at +8 and its count at +12, with the
    # signature at 510 (`drives_decode.h`, and `tools/test_fat.py` writes to
    # the same layout).
    table = bytearray(SECTOR)

    for slot, (_label, _args, first, sectors, type_byte) in enumerate(VOLUMES):
        at = 446 + slot * 16
        table[at + 4] = type_byte
        table[at + 8:at + 16] = struct.pack("<II", first, sectors)

    table[510:512] = b"\x55\xAA"

    with open(path, "r+b") as f:
        f.write(table)

    return SECTORS


if __name__ == "__main__":
    import sys

    if not available():
        print("SKIP: no mtools. `brew install mtools`.")
        raise SystemExit(0)

    out = sys.argv[1] if len(sys.argv) > 1 else "build/host/fat32-stick.img"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    build(out)
    print("wrote %s: %s" % (out, ", ".join("%s (%s)" % (v[0], "FAT32"
                                                        if v[4] == 0x0C
                                                        else "FAT16")
                                            for v in VOLUMES)))
