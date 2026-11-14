#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**A saved site's page, painted by WebCore on Kosmos** - not a test.

Diego, 13 November 2026, asked to see the browser render a real site.
Until W7 there is no window and no network loader, so the nearest true
thing is a page saved on this Mac with its style sheets and pictures, every
reference made a local file (`page.html` and `files/` in a folder), laid
out and painted by `webcoretest.elf` in the guest exactly as
`x86-webcore`'s pages are, and the picture brought back.

Usage: run_site.py IMAGE APPS SITE_FOLDER OUT.png [WIDTH HEIGHT]

A site's bytes are its authors' and never go in the repository: the folder
and the picture live under `build/` or a scratch directory.
"""

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE, APPS, SITE, OUT = sys.argv[1:5]
WIDTH, HEIGHT = (int(sys.argv[5]), int(sys.argv[6])) if len(sys.argv) > 6 else (1280, 1600)
X86 = "x86_64" in IMAGE
WORK = scratch.directory("site")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
DIR = "/Home/apps/webcore/webcore"
FONTS = ["IBMPlexSans-Regular.ttf", "IBMPlexSerif-Regular.ttf", "IBMPlexMono-Regular.ttf"]

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A saved site's page, painted (tools/run_site.py).
-- kosmos: image webcoretest.elf
local kit = use("webcoretest.elf")
local surface = gfx.surface{ w = %d, h = %d }
for _, line in ipairs(kit.paint("%s", "page.html", surface)) do
  print(line)
end
local ok, why = fs.write("%s/pictures/page.png", gfx.encode_png(surface))
print(ok and "site: saved" or ("site: unsaved " .. tostring(why)))
""" % (WIDTH, HEIGHT, DIR, DIR)


def main():
    built = os.path.join(APPS, "webcoretest.elf")
    image = os.path.join(WORK, "webcoretest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "site.lua"), "w") as f:
        f.write(PROGRAM)
    with open(os.path.join(WORK, "README"), "w") as f:
        f.write("The page as WebCore painted it.\n")

    files = [(os.path.join(WORK, "site.lua"), "/Home/apps/webcore/site.lua"),
             (image, "/Home/apps/webcore/webcoretest.elf"),
             (os.path.join(WORK, "README"), DIR + "/pictures/README")]
    files += [(os.path.join(ROOT, "assets", "fonts", f), DIR + "/fonts/" + f) for f in FONTS]
    for here, _, names in sorted(os.walk(SITE)):
        for n in sorted(names):
            src = os.path.join(here, n)
            files.append((src, DIR + "/pages/" + os.path.relpath(src, SITE)))
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    # **A real site wants a real machine's memory.** The suites' guests have
    # 512 MB, and WebCore's image is 148 MB of it before a page: The Verge,
    # once its WOFF2 fonts were decoded as well as its 92 pictures, ran
    # WebKit's allocator dry (`testing.md` 18.802). Two gigabytes is less
    # than any machine Kosmos runs on; `KOSMOS_SITE_MEMORY` says otherwise.
    memory = os.environ.get("KOSMOS_SITE_MEMORY", "2G")
    for args in (R.X86_ARGS, R.QEMU_ARGS):
        args[args.index("-m") + 1] = memory

    guest = R.Guest(IMAGE, 900)
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/webcore/site.lua")
        until = time.monotonic() + 800
        while time.monotonic() < until:
            time.sleep(0.5)
            guest._read_available()
            tail = guest.seen[mark:]
            if "site: saved" in tail or "site: unsaved" in tail or "ended, code" in tail:
                break
    finally:
        guest.close()

    said = [ln for ln in guest.seen[mark:].replace("\r", "").split("\n")
            if ln.startswith(("site:", "webcoretest", "painted", "process"))]
    print("\n".join(said))
    print("site: %.0f s in the guest, under TCG" % (time.monotonic() - started))
    if not any(ln == "site: saved" for ln in said):
        print("FAIL: no picture\n" + guest.seen[mark:][-1500:])
        return 1
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK,
                    DIR + "/pictures/page.png", OUT], check=True, cwd=ROOT)
    print("site: %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
