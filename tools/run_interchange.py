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

import io
import os
import random
import re
import shutil
import zipfile
import subprocess
import sys
import scratch

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

    work = scratch.directory()
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
        kfs("put", disk, put_me, "/Home/books/manual.txt")

        listing = kfs("ls", disk, "/Home/books")

        if "manual.txt" not in listing:
            raise Failure("the file is not in the image after `put`:\n"
                          + listing)

        checks += 1

        #
        # **Zips, both ways** (`roadmap.md` 6v): a folder for the machine to
        # compress - a file that deflates, one that does not, and an empty
        # one - and two zips made here by Python's own `zipfile` for it to
        # open, the second naming a place outside the folder it would be
        # opened into. And the program that does it, put as a file.
        #
        zip_text = b"hello kosmos " * 200
        zip_noise = random.Random(7).randbytes(3000)
        zip_files = {"a.txt": zip_text, "sub/b.bin": zip_noise,
                     "empty.txt": b""}

        for name, data in zip_files.items():
            path = os.path.join(work, "zt-" + name.replace("/", "-"))

            with open(path, "wb") as f:
                f.write(data)

            kfs("put", disk, path, "/Home/zt/" + name)

        def made_by_python(entries):
            b = io.BytesIO()

            with zipfile.ZipFile(b, "w") as z:
                for name, data, how in entries:
                    z.writestr(zipfile.ZipInfo(name) if how is None
                               else name, data,
                               compress_type=how or zipfile.ZIP_STORED)

            return b.getvalue()

        for name, data in (
            ("py.zip", made_by_python([
                ("x/", b"", None),
                ("x/one.txt", b"one " * 100, zipfile.ZIP_DEFLATED),
                ("x/two.txt", b"two", zipfile.ZIP_STORED)])),
            ("evil.zip", made_by_python([
                ("../evil.txt", b"out of bounds", zipfile.ZIP_STORED)])),
            ("ztest.lua", b'''
local zip = use("/Kosmos/Libraries/zip.lua")

local ok, why = zip.write{ paths = { "/Home/zt" }, to = "/Home/zt.zip" }
print("ZIP-WRITE", ok, why)

ok, why = zip.extract{ from = "/Home/zt.zip", into = "/Home/zt2" }
print("ZIP-BACK", ok, why,
      fs.read("/Home/zt2/a.txt") == fs.read("/Home/zt/a.txt"),
      fs.read("/Home/zt2/sub/b.bin") == fs.read("/Home/zt/sub/b.bin"),
      (fs.getattr("/Home/zt2/empty.txt") or {}).size)

ok, why = zip.extract{ from = "/Home/py.zip", into = "/Home/pyx" }
print("ZIP-PY", ok, why, fs.read("/Home/pyx/one.txt") == string.rep("one ", 100),
      fs.read("/Home/pyx/two.txt"))

ok, why = zip.extract{ from = "/Home/evil.zip", into = "/Home/evilx" }
print("ZIP-SLIP", ok, fs.getattr("/Home/evil.txt") == nil,
      fs.getattr("/Home/evilx") == nil, tostring(why):find("outside") ~= nil)
'''),
        ):
            path = os.path.join(work, name)

            with open(path, "wb") as f:
                f.write(data)

            kfs("put", disk, path, "/Home/" + name)

        #
        # How much room is left, as this computer counts it.
        #
        # The machine's `df` must say the same number, and it did not: the
        # disk server answered `blocks - data_at`, every block past the
        # metadata, so a disk the host tool had filled with fourteen
        # megabytes said thirty of thirty-two were free on the ThinkPad.
        # Asked first, before anything below writes to the image.
        #
        host_free = re.search(r"(\d+) blocks free of (\d+)", kfs("df", disk))


        # ---- the machine reads what this computer wrote ----
        out = run_disk.boot(image, disk, [
            "df",
            'local v = fs.read("/Home/books/manual.txt") '
            'print("GUEST" .. "-READ", v and #v or -1)',
            'fs.write("/Home/books/reply.txt", '
            '"written inside the machine, read outside it")',
            #
            # A file the machine writes that is bigger than a message.
            #
            # `fs.write` used to *raise* on this - `value does not fit in a
            # message`, out of the serialiser, from a call whose failures
            # are otherwise return values. The namespace splits a long write
            # for `/Temporary` and diskfs takes no offset to append at, so
            # everything above about two kilobytes could not be written to
            # the disk at all. It goes through a region now, the way
            # `files.copy` always did.
            #
            # Compared rather than counted: a length says the write happened
            # and an equality says it happened *correctly*, and a region
            # write that lands at the wrong offset gets the length right.
            #
            'local big = string.rep("kosmos-0123456789", 6000) '
            'local ok, err = fs.write("/Home/books/big.bin", big) '
            'local back = fs.read("/Home/books/big.bin") '
            'print("GUEST" .. "-BIG", ok, tostring(err), '
            '#(back or ""), back == big)',
            #
            # And the ceiling that is real, which must be a value and not an
            # exception. `/Temporary` is a fixed pool - 16 KB a file - so this
            # cannot succeed; what it must not do is throw.
            #
            'local ok, err = fs.write("/Temporary/big", string.rep("z", 150000)) '
            'print("GUEST" .. "-RAM", ok, tostring(err), '
            '#(fs.read("/Temporary/big") or ""))',
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
            '  local ok = fs.write("/Home/books/many" .. i .. ".bin", piece) '
            '  if not ok then break end '
            '  worked = worked + 1 '
            'end '
            'print("GUEST" .. "-MANY", worked)',
            # The zip library, and the two programs at the prompt.
            "run /Home/ztest.lua",
            "zip /Home/prog.zip /Home/zt",
            "unzip /Home/prog.zip",
            'print("GUEST" .. "-UNZIPPED", fs.read("/Home/prog/a.txt") == '
            'fs.read("/Home/zt/a.txt"))',
        ], each=60)

        if ("GUEST-READ %d" % len(body)) not in out.replace("\t", " "):
            raise Failure(
                "the machine could not read a file this computer wrote "
                f"into the image (expected {len(body)} bytes).\n"
                + out[-900:]
            )

        checks += 1

        # ---- the same free space, counted on both sides ----
        guest_free = re.search(r"(\d+) blocks free of (\d+)", out)

        if (host_free is None or guest_free is None
                or guest_free.groups() != host_free.groups()):
            raise Failure(
                "the machine's `df` and this computer's `kfs.lua df` disagree "
                "about the same image: "
                f"{guest_free.group(0) if guest_free else 'nothing from df'} "
                f"against {host_free.group(0) if host_free else 'nothing'}.\n"
                + out[-900:]
            )

        checks += 1

        # ---- and this computer reads what the machine wrote ----
        kfs("get", disk, "/Home/books/reply.txt", got_back)

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
        kfs("get", disk, "/Home/books/big.bin", big_back)

        with open(big_back) as f:
            if f.read() != big_body:
                raise Failure(
                    "the large file the machine wrote did not come back out "
                    "of the image byte for byte."
                )

        checks += 1

        # ---- and a ceiling that is a value rather than an exception ----
        if "GUEST-RAM false /Temporary is full 16384" not in flat:
            raise Failure(
                "a write past what /Temporary holds should come back as false "
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

        # ---- zips, made there and read here, made here and read there ----
        for marker, want, what in [
            ("ZIP-WRITE", "true nil", "the machine did not make a zip of a folder"),
            ("ZIP-BACK", "true nil true true 0",
             "a zip the machine made did not open back into the same files - "
             "one that deflates, one that does not, and an empty one"),
            ("ZIP-PY", "true nil true two",
             "a zip Python made, one file deflated and one stored, did not "
             "open on the machine"),
            ("ZIP-SLIP", "nil true true true",
             "a zip naming ../evil.txt was opened, or wrote outside its "
             "folder, or made the folder, or did not say why"),
        ]:
            lines = [l for l in flat.splitlines() if l.startswith(marker + " ")]

            if not lines or lines[-1][len(marker) + 1:].strip() != want:
                raise Failure(f"{what}: wanted {want!r}, got "
                              + (repr(lines[-1]) if lines else out[-900:]))

            checks += 1

        zipped = os.path.join(work, "zt.zip")
        kfs("get", disk, "/Home/zt.zip", zipped)

        with zipfile.ZipFile(zipped) as z:
            names = z.namelist()
            wanted = ["zt/", "zt/a.txt", "zt/empty.txt", "zt/sub/",
                      "zt/sub/b.bin"]

            if names != wanted:
                raise Failure("the machine's zip, read by Python, holds %r - "
                              "wanted %r" % (names, wanted))

            bad = z.testzip()

            if bad is not None:
                raise Failure("Python finds %s's CRC wrong in the machine's "
                              "zip" % bad)

            for name, data in zip_files.items():
                if z.read("zt/" + name) != data:
                    raise Failure("zt/%s read back from the machine's zip by "
                                  "Python is not the file" % name)

            if (z.getinfo("zt/a.txt").compress_type != zipfile.ZIP_DEFLATED
                    or z.getinfo("zt/sub/b.bin").compress_type
                    != zipfile.ZIP_STORED):
                raise Failure("the machine's zip did not deflate the text and "
                              "store the noise, which does not get smaller")

        checks += 1

        if ("zip: made /Home/prog.zip" not in flat
                or "unzip: opened into /Home/prog" not in flat
                or "GUEST-UNZIPPED true" not in flat):
            raise Failure("`zip` and `unzip` at the prompt did not make and "
                          "open an archive:\n" + out[-900:])

        checks += 1

        # And removing it from here really removes it.
        kfs("rm", disk, "/Home/books/manual.txt")

        if "manual.txt" in kfs("ls", disk, "/Home/books"):
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
