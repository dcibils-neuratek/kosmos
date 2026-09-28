#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""What the build installs into /Home/Apps, as `host:/Home/path` pairs.

    python3 tools/installed.py aarch64 ~/Kosmos/home
    python3 tools/installed.py x86_64  ~/Kosmos/home

Each folder in `user/installed/` is an application (`docs/elf.md` step 5):
its Lua files, the image its program names in its header (`-- kosmos: image
doom.elf`) - taken from `make apps`, stripped, into `build/installed/` -
and the data it plays, copied from the top of a person's home folder when
it is there and not already in the application's folder: Doom's WAD,
Quake's pak. A stick's /Home (`homeimage.py`) and `make install-apps` both
ask here, so the list is one list.

The folder is never written to: data is only ever read out of it.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INSTALLED = os.path.join(ROOT, "user", "installed")

# What each application plays, where a person's home keeps it at the top,
# and where it goes in the application's folder. Part of the game, as the
# WAD is (Diego, 27 September); a ROM is a person's own and stays in
# /Home/roms.
DATA = {
    "Doom": [("doom1.wad", "doom1.wad")],
    "Quake": [("id1/pak0.pak", "id1/pak0.pak")],
}


def image_of(program):
    """The image a program's header names, or None."""
    with open(program, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("--"):
                break

            got = re.match(r"--\s*kosmos:\s*image\s+(\S+)", line)

            if got:
                return got.group(1)

    return None


def pairs(arch, home, apps=None):
    """The pairs, with the images from `apps` - the lean userland's by
    default; the gate's suites give the test userland's, which it builds."""
    apps = apps or os.path.join(ROOT, "build", "user-x86_64" if arch == "x86_64" else "user",
                                "apps")
    staged = os.path.join(ROOT, "build", "installed", arch)
    objcopy = ("x86_64-elf-" if arch == "x86_64" else "aarch64-none-elf-") + "objcopy"
    out = []

    for name in sorted(os.listdir(INSTALLED)):
        folder = os.path.join(INSTALLED, name)

        if not os.path.isdir(folder):
            continue

        guest = "/Home/Apps/" + name

        for file in sorted(os.listdir(folder)):
            if file.endswith(".lua"):
                out.append("%s:%s/%s" % (os.path.join(folder, file), guest, file))

        program = os.path.join(folder, name.lower() + ".lua")
        image = os.path.exists(program) and image_of(program)

        if image:
            built = os.path.join(apps, image)

            if not os.path.exists(built):
                sys.exit("installed: no %s - `make apps` (and ARCH=x86_64) builds it" % built)

            os.makedirs(staged, exist_ok=True)
            stripped = os.path.join(staged, image)
            subprocess.run([objcopy, "--strip-debug", built, stripped], check=True)
            out.append("%s:%s/%s" % (stripped, guest, image))

        for there, here in DATA.get(name, []):
            source = os.path.join(home, there) if home else ""
            own = os.path.join(home, "Apps", name, here) if home else ""

            if source and os.path.isfile(source) and not os.path.isfile(own):
                out.append("%s:%s/%s" % (source, guest, here))

    return out


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: installed.py <aarch64|x86_64> [home folder]")

    for pair in pairs(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None):
        print(pair)


if __name__ == "__main__":
    main()
