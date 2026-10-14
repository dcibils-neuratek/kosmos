#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`tools/sandboxes_drive.py` on a stick's image, held to what a stick needs
(`docs/linux.md` L1).

A stick made as `mkusb_image.write_gpt` makes one - an EFI partition with a
command line in it, `/Home` after it full of a pattern - in a file then made
four times longer, as the image is on a 128 GB Kingston: the table ends
where the image did. The tool has to:

  - move the table to the file's real end, both copies agreeing and each
    holding to its CRC;
  - add one Kosmos partition, `KOSMOS SANDBOXES`, after `/Home`, aligned,
    the size asked for, its first megabyte zeros;
  - leave the EFI partition's entry, `/Home`'s entry and every byte of
    `/Home` as they were;
  - put `opt/kosmos/sandboxes=<its GUID>` on the command line, the words
    already there kept;
  - change nothing on a second run, and say the same GUID;

and **refuse, writing nothing**, a drive larger than the room there is and
a table whose header does not hold to its CRC - the controls, which show
this test sees a stick left alone.

Usage: test_sandboxes_drive.py
"""

import os
import struct
import subprocess
import sys
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mkusb_image                                          # noqa: E402
import scratch                                              # noqa: E402

TOOL = os.path.join(HERE, "sandboxes_drive.py")
SECTOR = 512
passed, failed = 0, 0


def check(ok, what):
    global passed, failed

    if ok:
        passed += 1
    else:
        failed += 1
        print("FAIL: " + what)


def run(*args):
    return subprocess.run([sys.executable, TOOL] + list(args),
                          capture_output=True, text=True)


def stick(path, megabytes):
    """A stick image as `mkusb_image` makes one, in a file `megabytes` long."""
    work = path + ".parts"
    os.makedirs(work, exist_ok=True)
    esp, home = os.path.join(work, "esp.img"), os.path.join(work, "home.img")
    cmdline = os.path.join(work, "cmdline")

    with open(esp, "wb") as f:
        f.truncate(8 * 1024 * 1024)

    subprocess.run(["mformat", "-i", esp, "-F", "::"], check=True,
                   capture_output=True)
    subprocess.run(["mmd", "-i", esp, "::/boot"], check=True, capture_output=True)

    with open(cmdline, "w") as f:
        f.write("opt/kosmos/boot=wm opt/kosmos/home=X\n")

    subprocess.run(["mcopy", "-i", esp, cmdline, "::/boot/kosmos.cmdline"],
                   check=True, capture_output=True)

    with open(home, "wb") as f:
        f.write(bytes(range(256)) * (16 * 1024 * 1024 // 256))

    mkusb_image.write_gpt(path, esp, os.path.getsize(esp), home,
                          str(uuid.uuid4()).upper())

    with open(path, "r+b") as f:
        f.truncate(megabytes * 1024 * 1024)


def table(path):
    """Both headers, both entry arrays, and whether each holds to its CRC."""
    with open(path, "rb") as f:
        data = f.read()

    total = len(data) // SECTOR

    def header(lba):
        h = data[lba * SECTOR:lba * SECTOR + 92]
        crc = struct.unpack_from("<I", h, 16)[0]
        z = bytearray(h)
        z[16:20] = b"\0\0\0\0"
        mine, other, first, last = struct.unpack_from("<QQQQ", h, 24)
        at, count, size, ecrc = struct.unpack_from("<QIII", h, 72)
        entries = data[at * SECTOR:at * SECTOR + count * size]

        return {"ok": h[:8] == b"EFI PART" and zlib.crc32(bytes(z)) & 0xFFFFFFFF == crc
                and zlib.crc32(entries) & 0xFFFFFFFF == ecrc,
                "mine": mine, "other": other, "last": last, "entries": entries}

    primary = header(1)

    return data, total, primary, header(primary["other"])


def parts(entries):
    out = []

    for i in range(len(entries) // 128):
        e = entries[i * 128:(i + 1) * 128]

        if e[:16] != b"\0" * 16:
            first, last = struct.unpack_from("<QQ", e, 32)
            out.append({"type": e[:16], "own": str(uuid.UUID(bytes_le=e[16:32])).upper(),
                        "first": first, "last": last, "raw": e,
                        "name": e[56:].decode("utf-16-le").rstrip("\0")})

    return out


def main():
    path = scratch.path("sandboxes-drive.img")
    stick(path, 128)
    before, total, primary, _ = table(path)
    old = parts(primary["entries"])

    # The controls first: a drive too large, and a table whose header does
    # not hold - each refused, and the stick byte for byte as it was.
    big = run(path, "--mb", "200")
    check(big.returncode != 0 and "give a smaller --mb" in big.stderr
          and open(path, "rb").read() == before,
          "a drive larger than the room after /Home was not refused with the "
          "stick untouched: %r" % big.stderr)

    with open(path, "r+b") as f:
        f.seek(SECTOR + 40)
        f.write(b"\xff")

    broken = open(path, "rb").read()
    bad = run(path, "--mb", "64")
    check(bad.returncode != 0 and "CRC" in bad.stderr
          and open(path, "rb").read() == broken,
          "a table whose header does not hold to its CRC was not refused with "
          "nothing written: %r" % bad.stderr)

    with open(path, "r+b") as f:
        f.seek(SECTOR + 40)
        f.write(before[SECTOR + 40:SECTOR + 41])

    one = run(path, "--mb", "64")
    check(one.returncode == 0, "the tool failed on a stick with room: %r" % one.stderr)

    after, total, primary, backup = table(path)
    now = parts(primary["entries"])
    guid = next((p["own"] for p in now if p["name"] == "KOSMOS SANDBOXES"), None)

    check(primary["ok"] and backup["ok"] and primary["other"] == total - 1
          and backup["mine"] == total - 1 and backup["other"] == 1
          and primary["last"] == total - 34 == backup["last"]
          and primary["entries"] == backup["entries"],
          "the table was not moved whole to the stick's real end - both copies "
          "agreeing, each holding to its CRC, the last usable sector %d"
          % (total - 34))

    home = next(p for p in old if p["name"] == "KOSMOS HOME")
    new = [p for p in now if p["name"] == "KOSMOS SANDBOXES"]

    check(len(new) == 1 and len(now) == len(old) + 1
          and new[0]["type"] == mkusb_image.guid_bytes(mkusb_image.KOSMOS_TYPE_GUID)
          and new[0]["first"] > home["last"] and new[0]["first"] % 2048 == 0
          and new[0]["last"] - new[0]["first"] + 1 == 64 * 2048,
          "there was not one Kosmos partition, KOSMOS SANDBOXES, after /Home, "
          "aligned and 64 MB: %r" % [(p["name"], p["first"], p["last"]) for p in now])

    check(all(p["raw"] in [q["raw"] for q in now] for p in old)
          and after[home["first"] * SECTOR:(home["last"] + 1) * SECTOR]
          == before[home["first"] * SECTOR:(home["last"] + 1) * SECTOR],
          "the EFI partition's or /Home's entry, or a byte of /Home, changed")

    check(new and after[new[0]["first"] * SECTOR:(new[0]["first"] + 2048) * SECTOR]
          == b"\0" * (2048 * SECTOR),
          "the new partition's first megabyte was not zeros - the disk server "
          "would not take it for blank and format it")

    esp = next(p for p in now if p["type"] == mkusb_image.guid_bytes(mkusb_image.ESP_TYPE_GUID))
    line = subprocess.run(["mtype", "-i", "%s@@%d" % (path, esp["first"] * SECTOR),
                           "::/boot/kosmos.cmdline"], capture_output=True, text=True).stdout

    check(guid and line.split() == ["opt/kosmos/boot=wm", "opt/kosmos/home=X",
                                    "opt/kosmos/sandboxes=" + guid]
          and ("its GUID: %s" % guid) in one.stdout,
          "the command line did not gain opt/kosmos/sandboxes=%s with its "
          "words kept: %r" % (guid, line))

    two = run(path, "--mb", "64")
    check(two.returncode == 0 and "already" in two.stdout
          and ("its GUID: %s" % guid) in two.stdout
          and table(path)[0] == after,
          "a second run was not a stick left as it was, the same GUID said: %r"
          % (two.stdout + two.stderr))

    print("%s: %d checks on sandboxes_drive.py" % ("PASS" if not failed else "FAIL",
                                                   passed + failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
