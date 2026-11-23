#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""What the build installs into /Home/Apps, /Home/System/Apps,
/Home/System/WebKit, /Home/System/LLM, /Home/Fonts and /Home/Developer, as `host:/Home/path`
pairs.

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
# What only one architecture builds: JetSki stands on WebCore, which is
# built for x86-64 alone while AArch64's suites wait (`CLAUDE.md`).
ONLY = {"JetSki": "x86_64", "Quake3": "x86_64"}

APPS = "/Home/Apps"

# **Kosmos's own applications, on the disk** (Diego, 15 November 2026:
# "/Home/System, seen as /Kosmos"): too big for the image the machine boots
# from, so kept in /Home/System/Apps, and seen by every program, read only,
# as /Kosmos/Apps/<name> (`ns.SYSTEM_APPS` in `init.lua`). Their folder
# carries its `package.txt` - this build's, from `packages.py` - so a
# netboot can tell a copy from another build and bring this one
# (`systemapps.lua`).
SYSTEM = {"JetSki", "Video"}
SYSTEM_APPS = "/Home/System/Apps"

# **Kosmos's WebKit, on the disk** (Diego, 15 November 2026: "One WebKit
# image", in "/Kosmos/WebKit"; `docs/mailweb.md` W1): the image JetSki and
# Mail both run in, `webkit.elf`, and Web Inspector's pages, in a folder of
# their own, seen read only as /Kosmos/WebKit. A program's header names it
# `-- kosmos: image webkit` - a system image, `IMAGES.SYSTEM` in `init.lua` -
# so it is not looked for beside the program. A `system` package of its own.
WEBKIT = "/Home/System/WebKit"

# **The media family's image** (`docs/media.md` M6, Diego, 23 November
# 2026: a system image beside WebKit's): `media.elf`, the Media Kit on
# FFmpeg built whole, in a folder of its own seen read only as /Kosmos/Media.
# Video and `listen` name it `-- kosmos: image media`; a `system` package.
MEDIA = "/Home/System/Media"
SYSTEM_IMAGES = {"webkit": "webkit.elf", "media": "media.elf"}

# **The LLM server on Vulkan** (`docs/xavier.md` X12, V4): the server's two
# images with a Vulkan driver linked in - lavapipe's and Intel's - in a
# folder of their own, which init reads when it starts the server: Intel's on
# a machine with an Intel GPU, lavapipe's elsewhere, and the system's own
# server, on the processor, when neither is there. A `system` package.
LLM = "/Home/System/LLM"
LLM_IMAGES = ["llm-vk.elf", "llm-anv.elf"]

DATA = {
    "Doom": [("doom1.wad", "doom1.wad")],
    "Quake": [("id1/pak0.pak", "id1/pak0.pak")],
    # Quake III's: the full game's nine paks - its own and the 1.32 point
    # release's - and the demo's one, each taken if it is there.
    "Quake3": [("baseq3/pak%d.pk3" % n, "baseq3/pak%d.pk3" % n) for n in range(9)]
              + [("demoq3/pak0.pk3", "demoq3/pak0.pk3")],
}

# What an application carries from the tree's own vendored assets, beside
# its program: Flight's aircraft (`assets/models/cessna172`, CC BY 4.0, and
# `assets/models/b747`, CC BY 3.0).
ASSETS = {
    "Flight": [("assets/models/cessna172/cessna_172.glb", "cessna_172.glb"),
               ("assets/models/b747/boeing_747.glb", "boeing_747.glb")],
}


def inspector():
    """WebKit's Web Inspector, its front end's pages (`tools/inspector.py`)."""
    import inspector as front_end

    return [(host, "Inspector/" + inside) for host, inside in front_end.files()]


# What the build makes for an application's folder besides its image.
def airports():
    """Flight's airports (`docs/flight.md` F8), made by `tools/airports.py`
    from OurAirports when it is missing or older than the script."""
    made = os.path.join(ROOT, "build", "airports", "airports.tsv")
    script = os.path.join(ROOT, "tools", "airports.py")

    if not os.path.exists(made) or os.path.getmtime(made) < os.path.getmtime(script):
        os.makedirs(os.path.dirname(made), exist_ok=True)
        subprocess.run([sys.executable, script, made], check=True, stdout=sys.stderr)
    return [(made, "airports.tsv")]


def aircraft():
    """Flight's aircraft (`docs/flight.md` F12): every file in its
    `Aircraft` folder, as `Aircraft/<name>.lua`. One list, which this file,
    Flight's suite and the gallery's /Home all take."""
    folder = os.path.join(INSTALLED, "Flight", "Aircraft")
    return [(os.path.join(folder, f), "Aircraft/" + f)
            for f in sorted(os.listdir(folder)) if f.endswith(".lua")]


def programs(name):
    """An application's own Lua, every `.lua` at the top of its folder, as
    (here, inside): its program and what it loads beside it - Flight's
    `model.lua` (F10b). The install, Flight's suite and the gallery take it."""
    folder = os.path.join(INSTALLED, name)
    return [(os.path.join(folder, f), f) for f in sorted(os.listdir(folder)) if f.endswith(".lua")]


def flight_made():
    return airports() + aircraft()


MADE = {"Flight": flight_made}


def stripped(image, apps, staged, objcopy):
    """`image` from `apps`, stripped of its debugging sections into `staged`."""
    built = os.path.join(apps, image)

    if not os.path.exists(built):
        sys.exit("installed: no %s - `make apps` (and ARCH=x86_64) builds it" % built)

    os.makedirs(staged, exist_ok=True)
    out = os.path.join(staged, image)
    subprocess.run([objcopy, "--strip-debug", built, out], check=True)
    return out


def webkit(arch, apps, staged, objcopy):
    """/Home/System/WebKit's pairs: the image, Web Inspector, and the folder's
    `package.txt`. x86-64 only, as WebCore is (`ONLY`)."""
    if arch != "x86_64":
        return []

    import packages

    mine = [(stripped("webkit.elf", apps, staged, objcopy), "webkit.elf")] + inspector()
    manifest = os.path.join(staged, "WebKit.package.txt")

    with open(manifest, "w") as f:
        f.write(packages.manifest(packages.webkit_facts(), mine))

    mine.append((manifest, "package.txt"))
    return ["%s:%s/%s" % (here, WEBKIT, inside) for here, inside in mine]


def media(arch, apps, staged, objcopy):
    """/Home/System/Media's pairs: the image and the folder's `package.txt`.
    Left out where `make apps` did not link it."""
    import packages

    if not os.path.exists(os.path.join(apps, "media.elf")):
        return []

    mine = [(stripped("media.elf", apps, staged, objcopy), "media.elf")]
    manifest = os.path.join(staged, "Media.package.txt")

    with open(manifest, "w") as f:
        f.write(packages.manifest(packages.media_facts(), mine))

    mine.append((manifest, "package.txt"))
    return ["%s:%s/%s" % (here, MEDIA, inside) for here, inside in mine]


def film_pairs(arch, apps):
    """What a suite that plays a film puts on its disk (`docs/media.md` M6c):
    the Video app's folder and `media.elf`, the image it and `listen` play
    in, as the install would. Empty of the image where `make apps` did not
    link it, which the suite names."""
    staged = os.path.join(ROOT, "build", "installed", arch)
    objcopy = ("x86_64-elf-" if arch == "x86_64" else "aarch64-elf-") + "objcopy"
    guest = SYSTEM_APPS + "/Video"
    mine = ["%s:%s/%s" % (here, guest, inside) for here, inside in programs("Video")]
    return mine + media(arch, apps, staged, objcopy)


def llm(arch, apps, staged, objcopy):
    """/Home/System/LLM's pairs: the images and the folder's `package.txt`.
    x86-64 only, as the Vulkan drivers are."""
    if arch != "x86_64":
        return []

    import packages

    mine = [(stripped(image, apps, staged, objcopy), image) for image in LLM_IMAGES]
    manifest = os.path.join(staged, "LLM.package.txt")

    with open(manifest, "w") as f:
        f.write(packages.manifest(packages.llm_facts(), mine))

    mine.append((manifest, "package.txt"))
    return ["%s:%s/%s" % (here, LLM, inside) for here, inside in mine]


def images_of(program):
    """The images a program's header names: its plain one first, then any
    a GPU has (`-- kosmos: image on intel-gpu k3dvk-anv.elf`, which init
    chooses on a machine with that GPU - `IMAGES.named`)."""
    plain, gpus = [], []
    with open(program, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("--"):
                break

            gpu = re.match(r"--\s*kosmos:\s*image\s+on\s+\S+\s+(\S+)", line)
            got = re.match(r"--\s*kosmos:\s*image\s+(\S+)", line)

            if gpu:
                gpus.append(gpu.group(1))
            elif got and not plain:
                plain.append(got.group(1))

    return plain + gpus


def pairs(arch, home, apps=None):
    """The pairs, with the images from `apps` - the lean userland's by
    default; the gate's suites give the test userland's, which it builds."""
    apps = apps or os.path.join(ROOT, "build", "user-x86_64" if arch == "x86_64" else "user",
                                "apps")
    staged = os.path.join(ROOT, "build", "installed", arch)
    objcopy = ("x86_64-elf-" if arch == "x86_64" else "aarch64-elf-") + "objcopy"
    out = []

    for name in sorted(os.listdir(INSTALLED)):
        folder = os.path.join(INSTALLED, name)

        if not os.path.isdir(folder):
            continue

        if ONLY.get(name, arch) != arch:
            continue

        guest = (SYSTEM_APPS if name in SYSTEM else APPS) + "/" + name
        mine = []                                   # (here, inside), this folder's

        mine += programs(name)

        program = os.path.join(folder, name.lower() + ".lua")
        images = images_of(program) if os.path.exists(program) else []

        # A system image is not beside its program: it is WebKit's folder's.
        # A GPU's image is built for the architectures whose drivers exist -
        # anv for x86-64 - and left out where it is not, which init answers
        # with the plain one.
        for n, image in enumerate(images):
            if image in SYSTEM_IMAGES:
                continue
            if n > 0 and not os.path.exists(os.path.join(apps, image)):
                continue
            mine.append((stripped(image, apps, staged, objcopy), image))

        if name in MADE:
            mine.extend(MADE[name]())

        for there, here in ASSETS.get(name, []):
            mine.append((os.path.join(ROOT, there), here))

        for there, here in DATA.get(name, []):
            source = os.path.join(home, there) if home else ""
            own = os.path.join(home, "Apps", name, here) if home else ""

            if source and os.path.isfile(source) and not os.path.isfile(own):
                mine.append((source, here))

        if name in SYSTEM:
            import packages

            os.makedirs(staged, exist_ok=True)
            manifest = os.path.join(staged, name + ".package.txt")

            with open(manifest, "w") as f:
                f.write(packages.manifest(packages.app_facts(name, guest), mine))

            mine.append((manifest, "package.txt"))

        out.extend("%s:%s/%s" % (here, guest, inside) for here, inside in mine)

    return (out + webkit(arch, apps, staged, objcopy) + media(arch, apps, staged, objcopy)
            + llm(arch, apps, staged, objcopy))


def fonts():
    """The faces Kosmos keeps on the disk rather than in its image, into
    /Home/Fonts with their licence: Japanese, Korean and Chinese
    (`tools/fetch_fonts.py`, which fetches them the first time). Apart from
    `pairs`, which the game suites also ask for their own disks and which
    would carry 22 MB more for nothing."""
    import fetch_fonts

    fetch_fonts.main()

    return ["%s:/Home/Fonts/%s" % (os.path.join(fetch_fonts.OUT, name), name)
            for name, _, _, _ in fetch_fonts.FILES]


def developer(arch, folder=None):
    """What TinyCC builds an image from, into /Home/Developer (`docs/tinycc.md`,
    step C2, Diego's decision 1): the lean userland's runtime, libgcc, the
    header slot and the headers, as `make apps` leaves them in `developer/`.
    The runtime carries its build's protocol stamp, which the C Kit holds a
    build to; so this is installed from the same build as the system."""
    folder = folder or os.path.join(ROOT, "build", "user-x86_64" if arch == "x86_64" else "user",
                                    "developer")

    if not os.path.isfile(os.path.join(folder, ".made")):
        sys.exit("installed: no %s - `make apps` (and ARCH=x86_64) makes it" % folder)

    out = []

    for here, _, files in os.walk(folder):
        for name in sorted(files):
            if name.startswith("."):
                continue

            path = os.path.join(here, name)
            out.append("%s:/Home/Developer/%s" % (path, os.path.relpath(path, folder)))

    return sorted(out)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: installed.py <aarch64|x86_64> [home folder]")

    for pair in (pairs(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
                 + fonts() + developer(sys.argv[1])):
        print(pair)


if __name__ == "__main__":
    main()
