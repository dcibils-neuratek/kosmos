#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`homeimage.py`, on a folder made here: nested folders kept, a dot-file
kept, macOS's litter and a name with a colon left out, the size asked for,
and what the build installs put in beside the folder's - winning over a file
the folder has at the same path, and saying so."""

import os
import shutil
import subprocess
import sys
import scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LUA = os.path.join(ROOT, "build", "host", "lua")

# The colon said, the size, three names in /Home, the nested file, two
# names kept out of /Home, the `._` file kept out, and the bytes back; then
# the build's file said to win, its bytes back rather than the folder's, and
# the build's other file there.
CHECKS = 13


def main():
    work = scratch.directory("homeimage")
    folder = os.path.join(work, "home")
    image = os.path.join(work, "home.img")
    fails = []

    try:
        os.makedirs(os.path.join(folder, "roms", "snes"))
        os.makedirs(os.path.join(folder, "Apps", "Doom"))
        files = {
            "song.mp3": b"not a song" * 100,
            ".music": b"/Home",
            "roms/snes/game one.sfc": bytes(range(256)) * 128,
            ".DS_Store": b"litter",
            "roms/._game one.sfc": b"litter",
            "a:b.txt": b"a colon",
            "Apps/Doom/doom.lua": b"-- the folder's, older",
        }

        for rel, data in files.items():
            with open(os.path.join(folder, rel), "wb") as out:
                out.write(data)

        built = {"doom.lua": b"-- the build's", "doom.elf": b"\x7fELF" * 64}

        for name, data in built.items():
            with open(os.path.join(work, name), "wb") as out:
                out.write(data)

        made = subprocess.run([sys.executable,
                               os.path.join(HERE, "homeimage.py"),
                               folder, image, "64"]
                              + ["%s:/Home/Apps/Doom/%s" % (os.path.join(work, n), n)
                                 for n in built],
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

        top, snes = ls("/Home"), ls("/Home/roms/snes")

        for name in ("song.mp3", ".music", "roms"):
            if name not in top:
                fails.append("/Home has no %s:\n%s" % (name, top))

        if "game one.sfc" not in snes:
            fails.append("the nested folder lost its file:\n" + snes)

        for name in (".DS_Store", "a:b.txt"):
            if name in top:
                fails.append("/Home holds %s, which should be left out" % name)

        if "._game" in ls("/Home/roms"):
            fails.append("macOS's ._ file went in")

        back = os.path.join(work, "back.sfc")
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", image,
                        "/Home/roms/snes/game one.sfc", back],
                       capture_output=True, check=False)

        if not os.path.exists(back) or \
                open(back, "rb").read() != files["roms/snes/game one.sfc"]:
            fails.append("the nested file did not come back byte for byte")

        if "/Home/Apps/Doom/doom.lua is the build's" not in made.stdout:
            fails.append("the build's doom.lua was not said to win over the "
                         "folder's:\n" + made.stdout)

        doom = os.path.join(work, "back.lua")
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", image,
                        "/Home/Apps/Doom/doom.lua", doom],
                       capture_output=True, check=False)

        if not os.path.exists(doom) or open(doom, "rb").read() != built["doom.lua"]:
            fails.append("/Home/Apps/Doom/doom.lua is not the build's")

        if "doom.elf" not in ls("/Home/Apps/Doom"):
            fails.append("the build's doom.elf is not in /Home/Apps/Doom")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if fails:
        print("FAIL: %d of %d checks on homeimage.py:" % (len(fails), CHECKS))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on homeimage.py (folders kept, a dot-file kept, "
          "macOS's litter and a name with a colon left out, the size asked "
          "for, a file back byte for byte, and what the build installs put "
          "in, over the folder's at the same path)." % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
