#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Files on and off the disk from the development machine.

Kosmos does not use FAT32, for the reasons design.md gives - no
attributes, no journal, no way to say what a file is. The cost of that
trade is that a Mac cannot mount the image and drop a file onto it, and
this is the test that the answer works.

Both directions, because both are needed and they fail differently:

  * `kfs.lua put` writes a file into the image, and the machine must be
    able to read it. That is how a book, a font or a WAD gets in.
  * the machine writes a file, and `kfs.lua get` must read it back out.
    That is how anything made inside gets to a real computer.

What makes it trustworthy is that both sides run the *same* `kfs.lua`. A
host tool that understood the format separately would be a second
implementation, and this test would be checking that two copies of the
same idea still agree - which they would, right up until one changed.
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_disk


HOST_LUA = "build/host/lua"
TOOL = "tools/kfs.lua"


class Failure(Exception):
    pass


def kfs(*args):
    done = subprocess.run([HOST_LUA, TOOL, *args], capture_output=True,
                          text=True)

    if done.returncode != 0:
        raise Failure(f"kfs.lua {' '.join(args)} failed:\n"
                      + done.stdout + done.stderr)

    return done.stdout


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    work = tempfile.mkdtemp()
    disk = os.path.join(work, "interchange.img")
    put_me = os.path.join(work, "from-the-mac.txt")
    got_back = os.path.join(work, "from-the-machine.txt")

    # Long enough to span several blocks, so this is not only testing a
    # file that fits in one.
    body = "".join("line %04d of a file written outside the machine\n" % i
                   for i in range(400))

    with open(put_me, "w") as f:
        f.write(body)

    try:
        kfs("create", disk, "32")
        kfs("put", disk, put_me, "/home/books/manual.txt")

        listing = kfs("ls", disk, "/home/books")

        if "manual.txt" not in listing:
            raise Failure("the file is not in the image after `put`:\n"
                          + listing)

        checks += 1

        # ---- the machine reads what this computer wrote ----
        out = run_disk.boot(image, disk, [
            'local v = fs.read("/home/books/manual.txt") '
            'print("GUEST" .. "-READ", v and #v or -1)',
            'fs.write("/home/books/reply.txt", '
            '"written inside the machine, read outside it")',
        ], each=40)

        if ("GUEST-READ %d" % len(body)) not in out.replace("\t", " "):
            raise Failure(
                "the machine could not read a file this computer wrote "
                f"into the image (expected {len(body)} bytes).\n"
                + out[-900:]
            )

        checks += 1

        # ---- and this computer reads what the machine wrote ----
        kfs("get", disk, "/home/books/reply.txt", got_back)

        with open(got_back) as f:
            back = f.read()

        if back != "written inside the machine, read outside it":
            raise Failure(
                "a file the machine wrote did not come back out of the "
                f"image correctly. Got: {back!r}"
            )

        checks += 1

        # And removing it from here really removes it.
        kfs("rm", disk, "/home/books/manual.txt")

        if "manual.txt" in kfs("ls", disk, "/home/books"):
            raise Failure("`rm` did not remove the file from the image.")

        checks += 1

        print(f"PASS: {checks} checks moving files between this computer "
              "and the machine.")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
