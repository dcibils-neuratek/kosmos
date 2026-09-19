#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`homeimage.py`, on a folder made here: nested folders kept, a dot-file
kept, macOS's litter and a name with a colon left out, and the size asked
for."""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LUA = os.path.join(ROOT, "build", "host", "lua")

# The colon said, the size, three names in /home, the nested file, two
# names kept out of /home, the `._` file kept out, and the bytes back.
CHECKS = 10


def main():
    work = tempfile.mkdtemp(prefix="kosmos-homeimage-")
    folder = os.path.join(work, "home")
    image = os.path.join(work, "home.img")
    fails = []

    try:
        os.makedirs(os.path.join(folder, "roms", "snes"))
        files = {
            "song.mp3": b"not a song" * 100,
            ".music": b"/home",
            "roms/snes/game one.sfc": bytes(range(256)) * 128,
            ".DS_Store": b"litter",
            "roms/._game one.sfc": b"litter",
            "a:b.txt": b"a colon",
        }

        for rel, data in files.items():
            with open(os.path.join(folder, rel), "wb") as out:
                out.write(data)

        made = subprocess.run([sys.executable,
                               os.path.join(HERE, "homeimage.py"),
                               folder, image, "64"],
                              capture_output=True, text=True)

        if made.returncode != 0:
            print("FAIL: homeimage.py would not make the image:\n"
                  + made.stdout + made.stderr)
            return 1

        if "left out" not in made.stdout or "a:b.txt" not in made.stdout:
            fails.append("the name with a colon was not said to be left out")

        if os.path.getsize(image) != 64 * 1024 * 1024:
            fails.append("the image is %d bytes, not 64 MB"
                         % os.path.getsize(image))

        def ls(path):
            return subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "ls",
                                   image, path],
                                  capture_output=True, text=True).stdout

        top, snes = ls("/home"), ls("/home/roms/snes")

        for name in ("song.mp3", ".music", "roms"):
            if name not in top:
                fails.append("/home has no %s:\n%s" % (name, top))

        if "game one.sfc" not in snes:
            fails.append("the nested folder lost its file:\n" + snes)

        for name in (".DS_Store", "a:b.txt"):
            if name in top:
                fails.append("/home holds %s, which should be left out" % name)

        if "._game" in ls("/home/roms"):
            fails.append("macOS's ._ file went in")

        back = os.path.join(work, "back.sfc")
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", image,
                        "/home/roms/snes/game one.sfc", back],
                       capture_output=True, check=False)

        if not os.path.exists(back) or \
                open(back, "rb").read() != files["roms/snes/game one.sfc"]:
            fails.append("the nested file did not come back byte for byte")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if fails:
        print("FAIL: %d of %d checks on homeimage.py:" % (len(fails), CHECKS))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on homeimage.py (folders kept, a dot-file kept, "
          "macOS's litter and a name with a colon left out, the size asked "
          "for, and a file back byte for byte)." % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
