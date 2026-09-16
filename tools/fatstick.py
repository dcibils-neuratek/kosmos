#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A FAT32 stick with real files on it, made by mtools (USB step 6b).

**Somebody else's reading of the format.** `tools/test_fat.py` already holds
`fat_decode.c` to volumes mtools wrote, for the reason `usb.md` gives: the
reader is ours and the drives it will read are not. This is the same idea one
layer up - the drive server walking a volume this project did not lay out.

The layout is what a camera or a Windows box writes: an MBR at sector 0 with
one partition of type 0x0C beginning at sector 2048, and FAT32 inside it. The
files are chosen for what they exercise rather than to be interesting:

  - `hello.txt`, a short name and one cluster;
  - `A Long File Name.txt`, which needs long-name entries gathered from their
    pieces, and 3000 bytes, which at one sector a cluster is a chain of six -
    so a read has to follow the table rather than run off the first cluster;
  - `Italy/roma.txt`, one directory down, so a path is resolved rather than
    a root directory scanned.

A label of `PHOTOS`, because the naming rule Diego settled on 16 September
says an unlabelled volume is `Untitled` - and a test whose volume has no
label could not tell the two apart.
"""

import os
import shutil
import struct
import subprocess
import tempfile

SECTOR = 512
FIRST = 2048                    # where a partition conventionally starts
SECTORS = 131072                # 64 MB, which is FAT32's smallest comfortable

LABEL = "PHOTOS"

# (path on the volume, bytes) - read back from here, never from the volume.
FILES = [
    ("hello.txt", b"Kosmos reads a drive.\n"),
    ("A Long File Name.txt", b"x" * 3000),
    ("Italy/roma.txt", b"roma\n"),
]


def available():
    """Whether mtools is here. The caller prints the SKIP, as test_fat does."""
    return shutil.which("mformat") is not None and shutil.which("mcopy") is not None


def build(path):
    """Writes the image at `path`. Returns its size in blocks."""
    at = "%s@@%d" % (path, FIRST * SECTOR)
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")

    def run(argv):
        r = subprocess.run(argv, capture_output=True, text=True, env=env)

        if r.returncode != 0:
            raise SystemExit("fatstick: %s failed: %s"
                             % (" ".join(argv), r.stderr.strip()))

    with open(path, "wb") as f:
        f.truncate(SECTORS * SECTOR)

    # One sector a cluster, so 3000 bytes is a chain of six rather than one
    # cluster large enough to hide a reader that never follows the table.
    run(["mformat", "-i", at, "-F", "-c", "1", "-v", LABEL,
         "-T", str(SECTORS - FIRST), "::"])

    # The partition table, written after mformat so it cannot overwrite it:
    # type 0x0C is FAT32 with LBA addressing, which is what anything modern
    # writes (`drives_decode.h`).
    table = bytearray(SECTOR)
    table[446 + 4] = 0x0C
    table[446 + 8:446 + 16] = struct.pack("<II", FIRST, SECTORS - FIRST)
    table[510:512] = b"\x55\xAA"

    with open(path, "r+b") as f:
        f.write(table)

    work = tempfile.mkdtemp(prefix="kosmos-fatstick-")
    made = set()

    for name, data in FILES:
        local = os.path.join(work, os.path.basename(name))

        with open(local, "wb") as f:
            f.write(data)

        folder = os.path.dirname(name)

        if folder and folder not in made:
            run(["mmd", "-i", at, "::" + folder])
            made.add(folder)

        run(["mcopy", "-i", at, local, "::" + name])

    return SECTORS


if __name__ == "__main__":
    import sys

    if not available():
        print("SKIP: no mtools. `brew install mtools`.")
        raise SystemExit(0)

    out = sys.argv[1] if len(sys.argv) > 1 else "build/host/fat32-stick.img"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    build(out)
    print("wrote %s: FAT32 labelled %s, %d files" % (out, LABEL, len(FILES)))
