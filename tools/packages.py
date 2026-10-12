#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The packages this build offers, made for a Kosmos machine to install.

    python3 tools/packages.py [home folder]      into build/packages

Diego, 12 October 2026, of pushing Doom, Quake, the SNES and the developer
files to the M700 after every netboot: "perhaps add a program in kosmos that
requests those from the mac as needed?", "where we show all available
packages", "and we can install them on demand", "from the server (which is
the mac now)". So each installable thing is a **package** - one zip of its
folder - and an index says what there is; `tools/packages_serve.py` serves
the folder, and Kosmos's `install` asks it (`roadmap.md`, Kosmos App Store).

**What a package is**: an application in `user/installed/` - its Lua, the
image `make apps` linked for it, and the data it plays from the home folder
when it is there, as `installed.py` lists them for a stick - or the
developer files New Project builds with. Built from this tree, so a package
speaks this build's protocols, which is the whole reason they had to be sent
again after every change.

**The index**, `index.txt`, a line a package, tab-separated: name, what it
is called, where it installs, its size in bytes, the build it was made from,
and a sentence. Text rather than a table in Lua's own syntax, because the
reader is another machine and text is what it can check.
"""

import os
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import installed as I                                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "packages")

ABOUT = {
    "Doom": "id Software's Doom, the shareware episode, at the screen's size",
    "Quake": "id Software's Quake, the shareware episode",
    "SNES": "a Super Nintendo, for the ROMs in /Home/roms/snes",
    "Developer": "what New Project builds a C program with: the runtime and the headers",
}


def build_name():
    try:
        return subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT,
                              capture_output=True, text=True).stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def version():
    with open(os.path.join(ROOT, "VERSION")) as f:
        return f.read().strip()


def make(name, folder, pairs):
    """One package: `pairs` of (here, path under `folder`), zipped under
    `name/` so the folder is the zip's one top folder."""
    path = os.path.join(OUT, name.lower() + ".zip")

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for here, inside in sorted(pairs, key=lambda p: p[1]):
            z.write(here, name + "/" + inside)

    return path


def main():
    home = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Kosmos/home")
    home = home if os.path.isdir(home) else None
    os.makedirs(OUT, exist_ok=True)

    apps = {}

    for pair in I.pairs("x86_64", home):
        here, guest = pair.split(":", 1)
        parts = guest.split("/")                    # "", "Home", "Apps", "Doom", ...

        if len(parts) > 4 and parts[1] == "Home" and parts[2] == "Apps":
            apps.setdefault(parts[3], []).append((here, "/".join(parts[4:])))

    dev = [(p.split(":", 1)[0], p.split(":", 1)[1][len("/Home/Developer/"):])
           for p in I.developer("x86_64")]

    stamp = "%s %s" % (version(), build_name())
    lines = []

    for name in sorted(apps):
        path = make(name, "/Home/Apps/" + name, apps[name])
        lines.append((name.lower(), name, "/Home/Apps/" + name, os.path.getsize(path),
                      stamp, ABOUT.get(name, "an application")))

    path = make("Developer", "/Home/Developer", dev)
    lines.append(("developer", "Developer files", "/Home/Developer", os.path.getsize(path),
                  stamp, ABOUT["Developer"]))

    with open(os.path.join(OUT, "index.txt"), "w") as f:
        for line in lines:
            f.write("\t".join(str(v) for v in line) + "\n")

    for line in lines:
        print("packages: %-10s %8d KB  %s" % (line[0], line[3] // 1024, line[2]))

    print("packages: %d in %s, from %s" % (len(lines), OUT, stamp))
    return 0


if __name__ == "__main__":
    sys.exit(main())
