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
            #
            # A file the machine writes that is bigger than a message.
            #
            # `fs.write` used to *raise* on this - `value does not fit in a
            # message`, out of the serialiser, from a call whose failures
            # are otherwise return values. The namespace splits a long write
            # for `/data` and diskfs takes no offset to append at, so
            # everything above about two kilobytes could not be written to
            # the disk at all. It goes through a region now, the way
            # `files.copy` always did.
            #
            # Compared rather than counted: a length says the write happened
            # and an equality says it happened *correctly*, and a region
            # write that lands at the wrong offset gets the length right.
            #
            'local big = string.rep("kosmos-0123456789", 6000) '
            'local ok, err = fs.write("/home/books/big.bin", big) '
            'local back = fs.read("/home/books/big.bin") '
            'print("GUEST" .. "-BIG", ok, tostring(err), '
            '#(back or ""), back == big)',
            #
            # And the ceiling that is real, which must be a value and not an
            # exception. `/data` is a fixed pool - 16 KB a file - so this
            # cannot succeed; what it must not do is throw.
            #
            'local ok, err = fs.write("/data/big", string.rep("z", 150000)) '
            'print("GUEST" .. "-RAM", ok, tostring(err), '
            '#(fs.read("/data/big") or ""))',
            #
            # And more large writes than a thread has capability slots.
            #
            # A loop rather than a call, because this is the shape every
            # resource bug here has had: it works, and then it stops working
            # on the thirty-second try. Sending the value through a region
            # hands the server a capability, and diskfs kept every one - the
            # read path had paid that debt back since a PDF found it on its
            # fifteenth read, and the write path never had. Nothing noticed
            # while `files.copy` was the only caller.
            #
            # Forty because a thread gets thirty-two. If CAPS_PER_THREAD
            # ever grows, this number has to grow past it or the check
            # quietly stops checking anything.
            #
            'local piece = string.rep("z", 5000) '
            'local worked = 0 '
            'for i = 1, 40 do '
            '  local ok = fs.write("/home/books/many" .. i .. ".bin", piece) '
            '  if not ok then break end '
            '  worked = worked + 1 '
            'end '
            'print("GUEST" .. "-MANY", worked)',
        ], each=60)

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

        # ---- a file bigger than a message, written by the machine ----
        flat = out.replace("\t", " ")
        big_body = "kosmos-0123456789" * 6000

        if ("GUEST-BIG true nil %d true" % len(big_body)) not in flat:
            raise Failure(
                "the machine could not write and read back a file larger "
                f"than a message ({len(big_body)} bytes). `fs.write` used to "
                "raise here on the disk.\n" + out[-900:]
            )

        checks += 1

        # And it is really on the disk, not only in something's memory:
        # this computer takes it back out of the image.
        big_back = os.path.join(work, "big-from-the-machine.bin")
        kfs("get", disk, "/home/books/big.bin", big_back)

        with open(big_back) as f:
            if f.read() != big_body:
                raise Failure(
                    "the large file the machine wrote did not come back out "
                    "of the image byte for byte."
                )

        checks += 1

        # ---- and a ceiling that is a value rather than an exception ----
        if "GUEST-RAM false /data is full 16384" not in flat:
            raise Failure(
                "a write past what /data holds should come back as false "
                "and a sentence, not as a raise.\n" + out[-900:]
            )

        checks += 1

        # ---- more large writes than a thread has capability slots ----
        if "GUEST-MANY 40" not in flat:
            raise Failure(
                "forty large writes in a row did not all work, which is a "
                "server keeping the capability to a buffer it was lent. It "
                "used to stop at thirty-two.\n" + out[-900:]
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
