#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A stick's /home, made from a folder on this Mac.

    python3 tools/homeimage.py ~/Kosmos/home build/stick-home.img 512

**Diego, 19 September**: "from now on we need to make the drive image at
least 512mb as we are adding more content to it", "and i will be adding more
images, videos, etc to test in kosmos". So the stick's /home is no longer the
32 MB disk files were put on by hand: it is made fresh, at the size given,
from whatever is in the folder - `~/Kosmos/home` by default (the Makefile's
`HOME_DIR`) - with the folders inside it kept as folders.

**The folder is Diego's and never the repository's.** Game data, songs and
other people's photographs do not go in a public tree; the folder is where
they live, and this copies them only into an image under `build/`.

What is left out, and said: macOS's own litter - `.DS_Store` and the `._`
files it writes beside others on some drives - and a name with a `:` in it,
which `kfs.lua` reads as the line between this Mac's path and the guest's.
Everything else goes in, dot-files included: `.music` is Music's.

`Desktop` and `Deskbar` need not be in the folder: Tracker and the Deskbar
make them on a first boot that finds none (`deskbar.lua`).
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")

LITTER = {".DS_Store", ".localized"}


def pairs_from(folder):
    """Every file under `folder` as host:/home/path, and what was left out."""
    pairs, left_out = [], []

    for top, dirs, files in os.walk(folder):
        dirs.sort()
        rel = os.path.relpath(top, folder)
        guest_dir = "/home" if rel == "." else "/home/" + rel.replace(os.sep, "/")

        for name in sorted(files):
            host = os.path.join(top, name)

            if name in LITTER or name.startswith("._"):
                continue

            if ":" in name or ":" in host:
                left_out.append(host)
                continue

            pairs.append("%s:%s/%s" % (host, guest_dir, name))

    return pairs, left_out


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: homeimage.py <folder> <image> <megabytes>")

    folder, image, megabytes = sys.argv[1], sys.argv[2], sys.argv[3]

    if not os.path.isdir(folder):
        sys.exit("homeimage: no folder %s - make it, and put in it what the "
                 "stick's /home should hold" % folder)

    pairs, left_out = pairs_from(folder)

    for host in left_out:
        print("homeimage: left out %s - a `:` in its name" % host)

    os.makedirs(os.path.dirname(os.path.abspath(image)), exist_ok=True)
    made = subprocess.run([LUA, KFS, "create", image, megabytes] + pairs,
                          cwd=ROOT)

    return made.returncode


if __name__ == "__main__":
    sys.exit(main())
