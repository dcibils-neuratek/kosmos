#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The packages this build offers, made for a Kosmos machine to install.

    python3 tools/packages.py [home folder]      into build/packages

Diego, 12 October 2026, of pushing Doom, Quake, the SNES and the developer
files to the M700 after every netboot: "perhaps add a program in kosmos that
requests those from the mac as needed?", "where we show all available
packages", "and we can install them on demand", "from the server (which is
the mac now)"; and of what a package is, "a zip file containing an app with
a manifest on what the app is? its files and where are they located?"
(`docs/packages.md`). `tools/packages_serve.py` serves the folder, and
Kosmos's `pkg` and `install` ask it.

**A package is a zip of its folder with `package.txt` inside**: what it is,
the version and the protocols it was built for, where it installs, the
powers it needs, what it depends on, and every file with its size and
SHA-256 - so the machine can refuse a package for other protocols, or a file
that did not arrive whole, before anything installed is touched.

**Written from the application's header, never by hand**: `-- kosmos:
name`, `icon`, `needs`, `kind`, `about` and `depends` are what the
application already says of itself, so the manifest cannot disagree with it.
The developer files have no header, and their facts are here.

**The index**, `index.txt`, is every manifest without its files, a package a
paragraph, so the list is one small request.

**The format** is a line a fact - a key, spaces, the value - which another
machine reads without a parser of any language. A file's line is `file`,
its size, its SHA-256 and its path last, since a path may have spaces in it.
"""

import glob
import hashlib
import os
import re
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import installed as I                                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "packages")

# **A sandbox's image** (`docs/sandboxes.md` S1): Alpine Linux's kernel and
# an initramfs of its userland, made by `tools/alpine.py` from Alpine's
# published files and installed where `sandbox` and the Sandboxes window
# find images.
ALPINE = {
    "name": "alpine", "title": "Alpine Linux", "kind": "sandbox",
    "about": "Alpine Linux 3.24.2 for a sandbox on a PC: Linux 6.18 and Alpine's userland",
    "icon": "", "needs": "", "depends": "",
}

DEVELOPER = {
    "name": "developer", "title": "Developer files", "kind": "developer",
    "about": "what New Project builds a C program with: the runtime and the headers",
    "icon": "", "needs": "", "depends": "",
}


def stamp():
    """`sys.protostamp` of this tree: what `tools/protostamp.py` computes,
    over the same headers the Makefile gives it."""
    headers = sorted(glob.glob(os.path.join(ROOT, "user/include/*proto.h"))
                     + glob.glob(os.path.join(ROOT, "user/include/*ring.h"))
                     + [os.path.join(ROOT, "kernel/syscall.h")], key=os.path.basename)
    digest = hashlib.sha256()

    for path in headers:
        digest.update(os.path.basename(path).encode() + b"\0")

        with open(path, "rb") as f:
            digest.update(f.read())

    return digest.hexdigest()[:16]


def build_name():
    try:
        return subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT,
                              capture_output=True, text=True).stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def version():
    with open(os.path.join(ROOT, "VERSION")) as f:
        return f.read().strip()


def header(path):
    """The `-- kosmos:` words of an application's opening comment."""
    words = {}

    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("--"):
                break

            got = re.match(r"--\s*kosmos:\s*(\w+)\s*(.*)$", line.rstrip("\n"))

            if got:
                key, value = got.group(1), got.group(2).strip()
                words[key] = (words[key] + " " + value).strip() if key in words else value

    return words


def sha256(path):
    digest = hashlib.sha256()

    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)

    return digest.hexdigest()


def facts_text(facts):
    keys = ("name", "title", "kind", "about", "version", "build", "protocols",
            "into", "icon", "needs", "depends", "bytes")
    return "".join("%-11s %s\n" % (k, facts.get(k, "")) for k in keys)


def make(facts, folder, pairs):
    """One package: `pairs` of (here, path inside), and its manifest, zipped
    under `folder/` so the folder is the zip's one top folder."""
    files = sorted(pairs, key=lambda p: p[1])
    lines = ["file        %d %s %s\n" % (os.path.getsize(here), sha256(here), inside)
             for here, inside in files]
    facts["bytes"] = sum(os.path.getsize(here) for here, _ in files)
    manifest = "# Made by tools/packages.py; a line a fact.\n" + facts_text(facts) + "".join(lines)
    path = os.path.join(OUT, facts["name"] + ".zip")

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(folder + "/package.txt", manifest)

        for here, inside in files:
            z.write(here, folder + "/" + inside)

    facts["zip"] = os.path.getsize(path)
    return facts


def main():
    home = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Kosmos/home")
    home = home if os.path.isdir(home) else None
    os.makedirs(OUT, exist_ok=True)

    common = {"version": version(), "build": build_name(), "protocols": stamp()}
    apps = {}

    for pair in I.pairs("x86_64", home):
        here, guest = pair.split(":", 1)
        parts = guest.split("/")                    # "", "Home", "Apps", "Doom", ...

        if len(parts) > 4 and parts[1] == "Home" and parts[2] == "Apps":
            apps.setdefault(parts[3], []).append((here, "/".join(parts[4:])))

    made = []

    for folder in sorted(apps):
        program = next((os.path.join(I.INSTALLED, folder, f)
                        for f in sorted(os.listdir(os.path.join(I.INSTALLED, folder)))
                        if f.endswith(".lua")), None)
        words = header(program) if program else {}
        facts = dict(common, name=folder.lower(), title=words.get("name", folder),
                     kind=words.get("kind", "application"), about=words.get("about", ""),
                     into="/Home/Apps/" + folder, icon=words.get("icon", ""),
                     needs=words.get("needs", ""), depends=words.get("depends", ""))
        made.append(make(facts, folder, apps[folder]))

    dev = [(p.split(":", 1)[0], p.split(":", 1)[1][len("/Home/Developer/"):])
           for p in I.developer("x86_64")]
    made.append(make(dict(common, into="/Home/Developer", **DEVELOPER), "Developer", dev))

    # For a PC, as every package here is: the M700 runs it (`docs/sandboxes.md` X3).
    alpine = os.path.join(ROOT, "build", "sandboxes", "alpine-x86_64")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "alpine.py"), "--arch", "x86_64",
                    alpine], check=True, stdout=subprocess.DEVNULL)
    made.append(make(dict(common, into="/Home/Sandboxes/Images/alpine", **ALPINE), "alpine",
                     [(os.path.join(alpine, f), f) for f in ("Image", "initramfs", "modloop")]))

    with open(os.path.join(OUT, "index.txt"), "w") as f:
        f.write("# The packages, a paragraph each; tools/packages.py.\n")

        for facts in made:
            f.write("\n" + facts_text(facts) + "%-11s %s\n" % ("zip", facts["zip"]))

    for facts in made:
        print("packages: %-10s %8d KB  %s" % (facts["name"], facts["zip"] // 1024, facts["into"]))

    print("packages: %d in %s, %s %s, protocols %s"
          % (len(made), OUT, common["version"], common["build"], common["protocols"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
