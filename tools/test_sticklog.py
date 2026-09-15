#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`make stick-log` without the stick.

The real thing reads a stick's raw device as root, which a test cannot own.
Everything after that read is the same code on a file, so this builds a
stick's image the way `mkusb_image.py` builds one - an ESP, and a kfs disk
holding a log in the Kosmos partition after it - and asks for the log back as
`tools/sticklog.sh` does: `sticklog.py` copies the partition out, and
`kfs.lua get` takes the file from the copy.

And the ways it must refuse, each by name: no GPT, a GPT with no Kosmos
partition, and a source that ends inside the partition - which is what a
stick pulled out halfway through the read gives.

Usage: test_sticklog.py [LUA]
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mkusb_image  # noqa: E402

SECTOR = mkusb_image.SECTOR


def main():
    lua = sys.argv[1] if len(sys.argv) > 1 else "build/host/lua"
    work = tempfile.mkdtemp(prefix="kosmos-sticklog-")
    checks = 0
    fails = []

    def check(ok, complaint):
        nonlocal checks

        if ok:
            checks += 1
        else:
            fails.append(complaint)

    copy = os.path.join(work, "partition.img")

    def sticklog(source):
        """Its exit status, the bytes it wrote, and what it said."""
        with open(copy, "wb") as out:
            done = subprocess.run([sys.executable,
                                   os.path.join(HERE, "sticklog.py"), source],
                                  stdout=out, stderr=subprocess.PIPE)

        with open(copy, "rb") as f:
            return (done.returncode, f.read(),
                    done.stderr.decode("utf-8", "replace").strip())

    def kfs(*words):
        return subprocess.run([lua, os.path.join(HERE, "kfs.lua")] + list(words),
                              capture_output=True)

    def digest(path):
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()

    try:
        # About what the ring holds when full, in lines shaped like its own.
        text = "".join("[%d.%03d] line %d of the boot log\n"
                       % (i // 1000, i % 1000, i) for i in range(1, 8000))
        log = os.path.join(work, "log.txt")

        with open(log, "w") as f:
            f.write(text)

        home = os.path.join(work, "home.img")
        esp = os.path.join(work, "esp.img")
        stick = os.path.join(work, "stick.img")

        made = kfs("create", home, "4").returncode == 0 \
            and kfs("put", home, log, "/home/log.txt").returncode == 0

        if not made:
            print("FAIL: kfs.lua could not make a disk with a log on it")
            return 1

        with open(esp, "wb") as f:
            f.truncate(1024 * 1024)

        mkusb_image.write_gpt(stick, esp, os.path.getsize(esp), home=home,
                              home_guid="5B1C5C9A-8E11-4F3D-9C1A-6F1D2E3A4B5C")

        # ---- the partition, whole, and the stick untouched ----
        before = digest(stick)
        code, partition, said = sticklog(stick)

        with open(home, "rb") as f:
            check(code == 0 and partition == f.read(),
                  "the Kosmos partition copied out is not the kfs disk put in "
                  "it (exit %d, %d bytes): %s" % (code, len(partition), said))

        check(digest(stick) == before, "reading the stick changed it")

        got = os.path.join(work, "got.txt")
        taken = kfs("get", copy, "/home/log.txt", got)
        back = None

        if taken.returncode == 0 and os.path.exists(got):
            with open(got) as f:
                back = f.read()

        check(back == text,
              "the log taken from the copy is not the log put on the stick: "
              "%s" % (taken.stderr.decode("utf-8", "replace").strip()
                      or "%d bytes of %d" % (len(back or ""), len(text))))

        # ---- no GPT: a disk of zeros ----
        blank = os.path.join(work, "blank.img")

        with open(blank, "wb") as f:
            f.truncate(8 * 1024 * 1024)

        code, partition, said = sticklog(blank)
        check(code == 2 and not partition and "no GPT" in said,
              "a disk with no GPT was not refused as one (exit %d, %d bytes): "
              "%s" % (code, len(partition), said))

        # ---- a GPT with no Kosmos partition: its type's first byte changed ----
        other = os.path.join(work, "other.img")
        shutil.copyfile(stick, other)

        with open(other, "r+b") as f:
            f.seek(2 * SECTOR + 128)
            byte = f.read(1)[0]
            f.seek(2 * SECTOR + 128)
            f.write(bytes([byte ^ 0xFF]))

        code, partition, said = sticklog(other)
        check(code == 2 and not partition and "no Kosmos partition" in said,
              "a GPT whose only other partition is not Kosmos's was not "
              "refused (exit %d, %d bytes): %s" % (code, len(partition), said))

        # ---- a source that ends inside the partition ----
        short = os.path.join(work, "short.img")
        shutil.copyfile(stick, short)

        with open(short, "r+b") as f:
            f.truncate(os.path.getsize(stick) // 2)

        code, partition, said = sticklog(short)
        check(code == 2 and "short" in said,
              "a source ending inside the Kosmos partition was not refused "
              "(exit %d, %d bytes): %s" % (code, len(partition), said))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if fails:
        print("FAIL: %d of %d checks on reading a file off a stick's Kosmos "
              "partition:" % (len(fails), checks + len(fails)))

        for complaint in fails:
            print("  " + complaint)

        return 1

    print("PASS: %d checks on reading a file off a stick's Kosmos partition "
          "(copied whole, the stick untouched, the log back from the copy, "
          "and no GPT, no Kosmos partition and a short source refused)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
