#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The SATA driver's first step: a disk behind AHCI found, and read only.

`docs/roadmap.md`, *SATA (AHCI)*: the M700's drive is behind an Intel SATA
controller, and Windows is on it. q35's own ICH9 AHCI controller is given a
64 MB disk laid out as one of those is - a protective MBR, a GPT, and a
Microsoft basic data partition beginning with an NTFS boot sector - and:

  - the driver finds the controller (8086:2922) and its ports;
  - the disk on port 0 answers IDENTIFY: QEMU's model, 64 MB in 131072
    sectors of 512;
  - its first sectors read: a protective MBR and a GPT;
  - one disk found, said, and the driver ending with 0;
  - and afterwards the image is the same bytes it was - nothing written.

Usage: run_ahci.py IMAGE
"""

import hashlib
import os
import struct
import sys
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import scratch                                              # noqa: E402

CHECKS = 6
SECTOR = 512
SECTORS = 131072                       # 64 MB
BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"     # Microsoft basic data


def guid(text):
    return uuid.UUID(text).bytes_le


def windows_like(path):
    """A protective MBR, a GPT at both ends, and one basic data partition
    from 1 MB to the end, holding an NTFS boot sector and bytes after it."""
    entries = bytearray(128 * 128)
    first, last = 2048, SECTORS - 34
    entries[0:128] = (guid(BASIC_DATA) + uuid.uuid4().bytes_le
                      + struct.pack("<QQQ", first, last, 0)
                      + "Basic data partition".encode("utf-16-le").ljust(72, b"\0"))
    entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF

    def header(mine, other, table):
        h = bytearray(struct.pack("<8sIII4sQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, b"\0\0\0\0",
                                  mine, other, 34, SECTORS - 34, uuid.uuid4().bytes_le,
                                  table, 128, 128, entries_crc))
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b"\0")

    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, SECTORS - 1)
    mbr[510:512] = b"\x55\xAA"

    ntfs = bytearray(SECTOR)
    ntfs[0:3] = b"\xEB\x52\x90"
    ntfs[3:11] = b"NTFS    "
    ntfs[510:512] = b"\x55\xAA"

    with open(path, "wb") as f:
        f.truncate(SECTORS * SECTOR)
        f.seek(0)
        f.write(mbr)
        f.write(header(1, SECTORS - 1, 2))
        f.write(entries)
        f.seek(first * SECTOR)
        f.write(ntfs)
        f.write(os.urandom(256 * 1024))          # something of the partition's own
        f.seek((SECTORS - 33) * SECTOR)
        f.write(entries)
        f.write(header(SECTORS - 1, 1, SECTORS - 33))


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    disk = scratch.path("ahci-disk.img")
    windows_like(disk)
    before = digest(disk)
    fails = []

    def check(ok, complaint):
        if not ok:
            fails.append(complaint)

    # On q35's own AHCI controller, at 00:1f.2: its first port.
    R.extra_args(image, ["-drive", "file=%s,format=raw,if=none,id=sata" % disk,
                         "-device", "ide-hd,drive=sata,bus=ide.0"])
    guest = R.Guest(image, 120)

    try:
        found = guest.wait_for_line("ahci: ", "the SATA driver to say what it found")
        check(found.startswith("8086:2922, AHCI 1."),
              "the controller was not q35's ICH9 in AHCI mode: %r" % found)
        disk_line = guest.wait_for_line("ahci: port 0, ", "the disk on port 0")
        check(disk_line.strip() == "QEMU HARDDISK, 64 MB in 131072 sectors of 512",
              "IDENTIFY did not say a 64 MB QEMU disk: %r" % disk_line)
        layout = guest.wait_for_line("ahci: port 0 holds", "the disk's first sectors")
        check(layout.strip() == "a GPT, its first sector a protective MBR",
              "the disk's GPT and protective MBR were not read: %r" % layout)
        count = guest.wait_for_line("ahci: 1 disk", "how many disks")
        check(count.strip() == "found", "the driver did not say one disk found: %r" % count)
        # And ended, cleanly: a role that returns runs init's chunk again,
        # which took the screen from the shell on 13 October.
        ended = guest.wait_for_line("(ahci) ended, code ", "the driver to end")
        check(ended.strip() == "0", "the driver did not end with 0: %r" % ended)
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    check(digest(disk) == before, "the disk was written to: its bytes changed")

    if fails:
        print("FAIL: %d of %d checks on the SATA driver:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if "ahci" in l or "SATA" in l:
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on the SATA driver (q35's ICH9 found in AHCI mode; a 64 MB disk's "
          "IDENTIFY; its protective MBR and GPT read; one disk said; the driver ending with 0; the disk not written)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
