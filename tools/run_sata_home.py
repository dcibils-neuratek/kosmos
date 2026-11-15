#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`/Home` on a SATA disk, in a partition Windows made (SATA A3).

`docs/roadmap.md`, *SATA (AHCI)*, and Diego's choice of 15 November 2026:
Windows shrinks its own partition and makes another with "Do not format
this volume"; Kosmos is told that partition's unique GUID on the boot line
(`opt/kosmos/home`), finds it on `/Devices/disks`, formats it only when a
person types `mkfs --yes`, and never writes a byte outside it - not
Windows' partition, not the partition table.

So a 128 MB disk on q35's AHCI controller laid out as such a machine's is -
a protective MBR, a GPT at both ends, Windows' basic data partition holding
an NTFS boot sector and bytes of its own, and a second basic data partition
for Kosmos whose first block is zeros (what Windows may well leave; a blank
USB partition is formatted unasked, and this one must not be) - and two
boots with that partition's GUID named:

  - the first: the disk server finds the partition on SATA disk 0, by its
    GUID though its type is Windows', and says so; it leaves the partition
    unformatted; `mkfs --yes` formats it; `save` writes a file;
  - a program reading a block inside the partition through `/Devices/disks`
    is refused - the disk server's alone - and the table's block 1 is
    answered;
  - the second boot finds the file;
  - and every byte outside the Kosmos partition is what it was before.

Usage: run_sata_home.py IMAGE
"""

import hashlib
import os
import re
import struct
import sys
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_x86                                              # noqa: E402
import scratch                                              # noqa: E402

CHECKS = 8
SECTOR = 512
SECTORS = 262144                        # 128 MB
BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"     # Microsoft basic data
WINDOWS = (2048, 65535)
HOME = (65536, SECTORS - 34)
HOME_GUID = "5A3E2C1B-7D44-4E8F-9A01-C0FFEE5A7A3E"


def guid(text):
    return uuid.UUID(text).bytes_le


def windows_with_home(path):
    entries = bytearray(128 * 128)

    def entry(i, own, span, name):
        entries[i * 128:(i + 1) * 128] = (
            guid(BASIC_DATA) + guid(own) + struct.pack("<QQQ", span[0], span[1], 0)
            + name.encode("utf-16-le").ljust(72, b"\0"))

    entry(0, str(uuid.uuid4()), WINDOWS, "Basic data partition")
    entry(1, HOME_GUID, HOME, "")
    entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF

    def header(mine, other, table):
        h = bytearray(struct.pack("<8sIII4sQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0,
                                  b"\0\0\0\0", mine, other, 34, SECTORS - 34,
                                  uuid.uuid4().bytes_le, table, 128, 128, entries_crc))
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b"\0")

    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1,
                               SECTORS - 1)
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
        f.seek(WINDOWS[0] * SECTOR)
        f.write(ntfs)
        f.write(os.urandom(1024 * 1024))         # Windows' own files
        f.seek(WINDOWS[1] * SECTOR)
        f.write(os.urandom(SECTOR))              # its last sector, beside ours
        # Kosmos's partition: a first block of zeros, then what was there.
        f.seek(HOME[0] * SECTOR + 4096)
        f.write(os.urandom(256 * 1024))
        f.seek((SECTORS - 33) * SECTOR)
        f.write(entries)
        f.write(header(SECTORS - 1, 1, SECTORS - 33))


def outside(path):
    """A digest of every byte that is not the Kosmos partition's."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read(HOME[0] * SECTOR))
        f.seek((HOME[1] + 1) * SECTOR)
        h.update(f.read())
    return h.hexdigest()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    disk = scratch.path("sata-home.img")
    windows_with_home(disk)
    before = outside(disk)
    fails = []

    def check(ok, complaint):
        if not ok:
            fails.append(complaint)

    extra = ("-drive", "file=%s,format=raw,if=none,id=sata" % disk,
             "-device", "ide-hd,drive=sata,bus=ide.0",
             "-fw_cfg", "name=opt/kosmos/home,string=" + HOME_GUID.lower())

    guard = ('fs.write("/Temporary/guard.lua", [[local b = '
             'use("/Kosmos/Libraries/blocks.lua") local r = b.open(b.DISKS) '
             'local x, why = r:read(0, %d, 1) print("inside:", x and #x, why) '
             'local c = r:read(0, 1, 1) print("outside:", c and #c) r:close()]])'
             % (HOME[0] + 64))

    # Formatted on the first boot, while `/Home` is in memory because there
    # was nothing on the disk when the machine started; on it from the
    # second, which saves; and the third reads the file back.
    one = run_x86.boot(image, None, 150.0,
                       typed=("diskinfo", "mkfs --yes", "diskinfo", guard,
                              "/Temporary/guard.lua"),
                       extra=extra, after="ahci: serving")
    two = run_x86.boot(image, None, 150.0,
                       typed=("diskinfo", "save notes.txt kept on SATA"),
                       extra=extra, after="ahci: serving")
    three = run_x86.boot(image, None, 150.0,
                         typed=("diskinfo", "cat /Home/notes.txt"),
                         extra=extra, after="ahci: serving")

    if one is None or two is None or three is None:
        check(False, "the machine would not boot with /Home named on a SATA disk")
        one, two, three = one or "", two or "", three or ""

    where = ("on the Kosmos partition on SATA disk 0, blocks %d to %d" % HOME)
    sectors = "disk: %d sectors of 512 bytes" % (HOME[1] - HOME[0] + 1)

    def shown(out):
        return "\n    ".join(l.strip() for l in out.splitlines()
                              if "disk:" in l or "filesystem:" in l or "Kosmos partition" in l
                              or "Formatted" in l or "saved" in l or "kept on" in l
                              or "mkfs" in l or "left alone" in l or "inside:" in l
                              or "outside:" in l)

    check(where in one and sectors in one and where in two and where in three,
          "diskinfo did not say /Home is partition blocks %d to %d on SATA disk 0, "
          "named by its GUID, on all three boots:\n    %s\n    %s"
          % (HOME[0], HOME[1], shown(one), shown(two)))

    first = re.split(r"^(?:kosmos> )?mkfs --yes\s*$", one, maxsplit=1, flags=re.M)[0]
    check("filesystem: none" in first and "filesystem: version" not in first,
          "the partition was formatted before `mkfs --yes` asked - a blank first "
          "block on a drive inside the machine is not a partition with nothing to "
          "lose:\n    %s" % shown(one))

    check("holds no Kosmos filesystem" in one,
          "the disk server did not say the partition holds no Kosmos filesystem "
          "and `mkfs --yes` makes one:\n    %s" % shown(one))

    check("Formatted." in one and "filesystem: version" in re.split(r"^(?:kosmos> )?mkfs --yes\s*$", one, maxsplit=1, flags=re.M)[-1]
          and "filesystem: version" in two,
          "`mkfs --yes` did not format the partition, or the next boot did not "
          "find the filesystem it made:\n    %s\n    %s" % (shown(one), shown(two)))

    check("saved notes.txt" in two and "in memory" not in two,
          "the second boot did not save a file to /Home on the SATA disk:\n    %s"
          % shown(two))

    check(re.search(r"inside:\tnil\tthat block is a drive's a disk server holds", one)
          is not None and re.search(r"outside:\t512", one) is not None,
          "a program's read of block %d, inside /Home's partition, was not refused "
          "on /Devices/disks, or block 1 outside it not answered:\n    %s"
          % (HOME[0] + 64, shown(one)))

    check("kept on SATA" in three.split("cat /Home/notes.txt")[-1],
          "the third boot did not find the file the second saved:\n    %s" % shown(three))

    check(outside(disk) == before,
          "a byte outside the Kosmos partition changed: Windows' partition or the "
          "partition table was written")

    if fails:
        kept = os.path.join("build", "gate", "sata-home.log")
        with open(kept, "w") as f:
            f.write(one + "\n==== the second boot ====\n" + two
                    + "\n==== the third boot ====\n" + three)
        print("FAIL: %d of %d checks on /Home on a SATA disk (the boots in %s):"
              % (len(fails), CHECKS, kept))
        for f in fails:
            print("  " + f)
        for l in (one + two + three).splitlines():
            if "ahci" in l or "diskfs" in l or "home:" in l:
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on /Home on a SATA disk (the partition Windows made found "
          "by its GUID on SATA disk 0; left unformatted until `mkfs --yes`; formatted "
          "and a file saved; a read inside it refused on /Devices/disks; the file "
          "there on the next boot; every byte outside it unchanged)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
