#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**`/Sandboxes` added to a stick, with `/Home` left as it is**
(`docs/linux.md` L1).

Diego, 14 October 2026: "what if instead /home we crate a new /Sandboxes
drive with virtual machibes disks?", "that pollutes the home?". Guests'
disks go on a partition of their own, served by a second disk server, so a
guest that fills its disk fills its own drive and never `/Home`.

A stick is written with its image, and the image's GPT ends where the image
does - about two gigabytes into a 128 GB Kingston, with the rest of the
stick in no partition at all. Writing a new image would take `/Home` with
it, so this does what `sgdisk -e -n` would, without the root a Mac's
partitioning tools want for a file and with nothing above the table touched:

  1. the table moved to the stick's real end - the backup entries and
     header at its last sectors, and the last usable sector with them;
  2. a Kosmos partition, `KOSMOS SANDBOXES`, after the last one there, its
     first megabyte zeroed - a blank partition, which the disk server
     formats the first time it is asked;
  3. the word `opt/kosmos/sandboxes=<its GUID>` added to the stick's
     `\\boot\\kosmos.cmdline`, which the loader hands the kernel, so a
     machine started from the stick has the drive.

**Run twice, it changes nothing the second time** and says the GUID again:
a stick that has the partition already is answered, not given a second.

Usage:
  sandboxes_drive.py <stick or image> [--mb N] [--no-cmdline]

On a Mac the stick is `/dev/rdiskN`, unmounted, and needs `sudo`; `diskutil
list` says which N. `--mb` is the drive's size, 32768 by default (Diego, 14
October: 32 GB).
"""

import fcntl
import os
import stat
import struct
import subprocess
import sys
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mkusb_image                                          # noqa: E402

SECTOR = 512
ALIGN = 2048                    # a megabyte, as every partition tool aligns
LABEL = "KOSMOS SANDBOXES"
WORD = "opt/kosmos/sandboxes="

# macOS's <sys/disk.h>: a raw disk's block count and size.
DKIOCGETBLOCKSIZE = 0x40046418
DKIOCGETBLOCKCOUNT = 0x40086419


class Refused(Exception):
    pass


def sectors_of(fd, path):
    """How many 512-byte sectors the stick or the file holds."""
    if stat.S_ISREG(os.fstat(fd).st_mode):
        return os.fstat(fd).st_size // SECTOR

    try:
        size = struct.unpack("<I", fcntl.ioctl(fd, DKIOCGETBLOCKSIZE, b"\0" * 4))[0]
        count = struct.unpack("<Q", fcntl.ioctl(fd, DKIOCGETBLOCKCOUNT, b"\0" * 8))[0]
    except OSError:
        end = os.lseek(fd, 0, os.SEEK_END)
        return end // SECTOR

    if size != SECTOR:
        raise Refused("%s has %d-byte sectors, and Kosmos's sticks have 512"
                      % (path, size))

    return count


def read_at(fd, lba, count):
    os.lseek(fd, lba * SECTOR, os.SEEK_SET)
    data = b""

    while len(data) < count * SECTOR:
        piece = os.read(fd, count * SECTOR - len(data))

        if not piece:
            raise Refused("the stick ended at sector %d" % (lba + len(data) // SECTOR))

        data += piece

    return data


def write_at(fd, lba, data):
    os.lseek(fd, lba * SECTOR, os.SEEK_SET)
    os.write(fd, data)


def header_fields(h):
    if h[0:8] != b"EFI PART":
        raise Refused("there is no GPT at sector 1 - not a Kosmos stick")

    size = struct.unpack_from("<I", h, 12)[0]
    want = struct.unpack_from("<I", h, 16)[0]
    check = bytearray(h[:size])
    check[16:20] = b"\0\0\0\0"

    if zlib.crc32(bytes(check)) & 0xFFFFFFFF != want:
        raise Refused("the GPT's header does not hold to its CRC; nothing was written")

    mine, other, first, last = struct.unpack_from("<QQQQ", h, 24)
    entries_at, count, entry_size, entries_crc = struct.unpack_from("<QIII", h, 72)

    return {"size": size, "mine": mine, "other": other, "first": first,
            "last": last, "disk": h[56:72], "entries_at": entries_at,
            "count": count, "entry_size": entry_size, "entries_crc": entries_crc}


def make_header(f, mine, other, last, entries_at, entries_crc):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    h[8:12] = struct.pack("<I", 0x00010000)
    h[12:16] = struct.pack("<I", 92)
    h[24:56] = struct.pack("<QQQQ", mine, other, f["first"], last)
    h[56:72] = f["disk"]
    h[72:92] = struct.pack("<QIII", entries_at, f["count"], f["entry_size"],
                           entries_crc)
    h[16:20] = struct.pack("<I", zlib.crc32(bytes(h)) & 0xFFFFFFFF)

    return bytes(h) + b"\0" * (SECTOR - 92)


def guid_text(raw):
    return str(uuid.UUID(bytes_le=bytes(raw))).upper()


def plan(fd, path, megabytes):
    """What the stick holds and what would be written: (GUID, writes, said),
    where `writes` is empty when the partition is there already."""
    total = sectors_of(fd, path)
    h = read_at(fd, 1, 1)
    f = header_fields(h)

    if f["entry_size"] != 128:
        raise Refused("GPT entries of %d bytes; Kosmos writes 128" % f["entry_size"])

    entry_sectors = (f["count"] * 128 + SECTOR - 1) // SECTOR
    entries = bytearray(read_at(fd, f["entries_at"], entry_sectors)[:f["count"] * 128])

    if zlib.crc32(bytes(entries)) & 0xFFFFFFFF != f["entries_crc"]:
        raise Refused("the GPT's entries do not hold to their CRC; nothing was written")

    kosmos = mkusb_image.guid_bytes(mkusb_image.KOSMOS_TYPE_GUID)
    used, free_slot = [], None

    for i in range(f["count"]):
        e = entries[i * 128:(i + 1) * 128]

        if e[0:16] == b"\0" * 16:
            if free_slot is None:
                free_slot = i
            continue

        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16-le", "replace").rstrip("\0")
        used.append((first, last, name, e))

        if e[0:16] == kosmos and name == LABEL:
            return guid_text(e[16:32]), [], ("%s has a /Sandboxes partition already, "
                                             "sectors %d to %d" % (path, first, last))

    if free_slot is None:
        raise Refused("every entry of the GPT is used")

    end = max(last for _, last, _, _ in used) if used else f["first"] - 1
    first = (end + 1 + ALIGN - 1) // ALIGN * ALIGN
    last_usable = total - 34
    wanted = megabytes * 1024 * 1024 // SECTOR
    last = first + wanted - 1

    if last > last_usable:
        raise Refused("%s has %d MB after its last partition, and the drive is to "
                      "be %d MB; give a smaller --mb"
                      % (path, max(0, last_usable - first + 1) * SECTOR // 1048576,
                         megabytes))

    own = uuid.uuid4()
    e = bytearray(128)
    e[0:16] = kosmos
    e[16:32] = own.bytes_le
    e[32:48] = struct.pack("<QQ", first, last)
    label = LABEL.encode("utf-16-le")
    e[56:128] = label + b"\0" * (72 - len(label))
    entries[free_slot * 128:(free_slot + 1) * 128] = e

    crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
    padded = bytes(entries) + b"\0" * (entry_sectors * SECTOR - len(entries))
    backup_entries = total - 1 - entry_sectors

    mbr = bytearray(read_at(fd, 0, 1))
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0x00, 0x00, 0x02, 0x00,
                               0xEE, 0xFF, 0xFF, 0xFF,
                               1, min(total - 1, 0xFFFFFFFF))

    # The order is the safe one: the new partition's first megabyte (blank,
    # and over the old backup table if it sits there), the backup table at
    # the real end, and the primary last - until it is written the stick
    # reads exactly as it did.
    writes = [
        (first, b"\0" * (ALIGN * SECTOR)),
        (backup_entries, padded),
        (total - 1, make_header(f, total - 1, 1, last_usable, backup_entries, crc)),
        (f["entries_at"], padded),
        (1, make_header(f, 1, total - 1, last_usable, f["entries_at"], crc)),
        (0, bytes(mbr)),
    ]

    return str(own).upper(), writes, ("a /Sandboxes partition of %d MB at sectors "
                                      "%d to %d of %s, the table moved to its "
                                      "end at sector %d"
                                      % (megabytes, first, last, path, total - 1))


def esp_offset(fd):
    """Where the stick's EFI partition starts, in bytes."""
    f = header_fields(read_at(fd, 1, 1))
    entries = read_at(fd, f["entries_at"], 32)
    esp = mkusb_image.guid_bytes(mkusb_image.ESP_TYPE_GUID)

    for i in range(f["count"]):
        e = entries[i * 128:(i + 1) * 128]

        if e[0:16] == esp:
            return struct.unpack_from("<Q", e, 32)[0] * SECTOR

    raise Refused("the stick has no EFI partition to put the word in")


def new_line(had, guid):
    """The command line with the word, the others kept; None when it has it."""
    words = [w for w in had if not w.startswith(WORD)] + [WORD + guid]

    if words == had:
        return None

    line = " ".join(words)

    if len(line) > 384:
        raise Refused("the command line would be %d characters, past the 384 "
                      "the loader reads" % len(line))

    return line


def put_word_mounted(path, guid):
    """On a Mac's disk: its EFI partition mounted, and the file edited there.
    `mtools` cannot open a whole disk macOS holds ("Resource busy" - Diego's
    stick, 14 October 2026)."""
    esp = path.replace("/dev/rdisk", "/dev/disk") + "s1"
    mounted = subprocess.run(["diskutil", "mount", esp], capture_output=True, text=True)

    if mounted.returncode != 0:
        raise Refused("its EFI partition would not mount: %s" % mounted.stderr.strip())

    point = None

    for line in subprocess.run(["diskutil", "info", esp], capture_output=True,
                               text=True).stdout.splitlines():
        if line.strip().startswith("Mount Point:"):
            point = line.split(":", 1)[1].strip()

    try:
        at = os.path.join(point or "", "boot", "kosmos.cmdline")

        if not point or not os.path.exists(at):
            raise Refused("the stick has no boot/kosmos.cmdline; nothing written")

        had = open(at).read().split()
        line = new_line(had, guid)

        if line is not None:
            with open(at + ".new", "w") as out:
                out.write(line + "\n")

            os.replace(at + ".new", at)

        return line or " ".join(had)
    finally:
        subprocess.run(["diskutil", "unmount", esp], capture_output=True)


def put_word(path, offset, guid):
    """`opt/kosmos/sandboxes=<guid>` on the stick's command line, once.

    **The line is read before it is written, and a line that cannot be read
    is not written**: a failed read taken for an empty line would have put
    back the new word alone, and the rest - `/Home`'s own word among them -
    lost."""
    if path.startswith("/dev/") and sys.platform == "darwin":
        return put_word_mounted(path, guid)

    target = "%s@@%d" % (path, offset)
    have = subprocess.run(["mtype", "-i", target, "::/boot/kosmos.cmdline"],
                          capture_output=True, text=True)

    if have.returncode != 0:
        raise Refused("the stick's command line could not be read (%s); nothing written"
                      % have.stderr.strip().splitlines()[0] if have.stderr.strip() else "?")

    had = have.stdout.split()
    line = new_line(had, guid)

    # Said already: not written again, so a second run changes no byte.
    if line is None:
        return " ".join(had)

    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"),
                       "kosmos-cmdline-%d" % os.getpid())

    with open(tmp, "w") as out:
        out.write(line + "\n")

    put = subprocess.run(["mcopy", "-o", "-i", target, tmp, "::/boot/kosmos.cmdline"],
                         capture_output=True, text=True)
    os.remove(tmp)

    if put.returncode != 0:
        raise Refused("mcopy could not write the command line: %s" % put.stderr.strip())

    return line


def main(argv):
    args = argv[1:]
    megabytes, cmdline = 32768, True

    if "--no-cmdline" in args:
        args.remove("--no-cmdline")
        cmdline = False

    if "--mb" in args:
        at = args.index("--mb")
        megabytes = int(args[at + 1])
        del args[at:at + 2]

    if len(args) != 1:
        sys.exit(__doc__)

    path = args[0]

    try:
        fd = os.open(path, os.O_RDWR)
    except OSError as e:
        sys.exit("sandboxes_drive: cannot open %s: %s%s"
                 % (path, e.strerror, " (sudo, and the stick unmounted)"
                    if path.startswith("/dev/") else ""))

    try:
        guid, writes, said = plan(fd, path, megabytes)

        for lba, data in writes:
            write_at(fd, lba, data)

        if writes:
            os.fsync(fd)

        offset = esp_offset(fd) if cmdline else None
    except Refused as why:
        os.close(fd)
        sys.exit("sandboxes_drive: %s" % why)

    os.close(fd)
    print(said)
    print("its GUID: %s" % guid)

    if cmdline:
        try:
            print("the stick's command line: %s" % put_word(path, offset, guid))
        except Refused as why:
            sys.exit("sandboxes_drive: %s" % why)

    print("a machine started with %s%s has /Sandboxes" % (WORD, guid))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
