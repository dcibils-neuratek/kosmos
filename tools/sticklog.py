#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Kosmos partition of a stick, or of a stick's image, on standard output.

**Why.** `/home` on a stick is its Kosmos partition, which macOS does not
mount, and `tools/kfs.lua` reads a filesystem out of an image file. So this
finds that partition in the stick's GPT and copies its bytes out, and
`kfs.lua get` takes a file from the copy - `make stick-log`, through
`tools/sticklog.sh`, which picks the stick and runs this as root.

**It never writes what it reads.** SOURCE is opened once, for reading. A raw
device on macOS answers reads in whole sectors only, and every read here is
whole sectors.

The GPT is read as `tools/mkusb_image.py` writes it and as the disk server
reads it (`stick_home` in `user/init/init.lua`): the header at block 1 with
its signature, the entries where it says they are, and the first entry whose
type is Kosmos's. Neither CRC is checked, as the disk server does not check
them either, and a stick with two Kosmos partitions gives its first.

Exit 0 with the partition written; 2 when SOURCE has no GPT or no Kosmos
partition, ends inside the partition, or cannot be read.

Usage: sticklog.py SOURCE > PARTITION
"""

import struct
import sys

from mkusb_image import KOSMOS_TYPE_GUID, guid_bytes

SECTOR = 512
CHUNK = 2048 * SECTOR           # a megabyte, in whole sectors
ENTRIES_MOST = 1024 * 1024      # a partition table this large is not one


def fail(why):
    sys.stderr.write("sticklog: %s\n" % why)
    sys.exit(2)


def read_at(f, sector, count):
    f.seek(sector * SECTOR)
    return f.read(count * SECTOR)


def kosmos_partition(f):
    """The first and last block of the first Kosmos partition."""
    header = read_at(f, 1, 1)

    if len(header) < 92 or header[0:8] != b"EFI PART":
        fail("no GPT: block 1 does not begin with EFI PART")

    entries_at, count, size = struct.unpack_from("<QII", header, 72)

    if size < 128 or count == 0 or count * size > ENTRIES_MOST:
        fail("a GPT header naming %d entries of %d bytes, which is not a "
             "partition table" % (count, size))

    table = read_at(f, entries_at, (count * size + SECTOR - 1) // SECTOR)
    kosmos = guid_bytes(KOSMOS_TYPE_GUID)

    for i in range(count):
        entry = table[i * size:(i + 1) * size]

        # Its type at byte 0, and its first and last block at 32.
        if len(entry) == size and entry[0:16] == kosmos:
            first, last = struct.unpack_from("<QQ", entry, 32)

            if last >= first:
                return first, last

    fail("no Kosmos partition in the GPT")


def main():
    if len(sys.argv) != 2:
        fail("usage: sticklog.py SOURCE > PARTITION")

    try:
        f = open(sys.argv[1], "rb", buffering=0)
    except OSError as e:
        fail("cannot read %s: %s" % (sys.argv[1], e.strerror))

    with f:
        first, last = kosmos_partition(f)
        left = (last - first + 1) * SECTOR
        out = sys.stdout.buffer

        f.seek(first * SECTOR)

        while left > 0:
            piece = f.read(min(CHUNK, left))

            if not piece:
                fail("the source ended %d bytes short of its Kosmos "
                     "partition's end" % left)

            out.write(piece)
            left -= len(piece)

        out.flush()

    sys.stderr.write("sticklog: the Kosmos partition, blocks %d to %d, "
                     "%.1f MB\n" % (first, last,
                                    (last - first + 1) * SECTOR / 1e6))


if __name__ == "__main__":
    main()
