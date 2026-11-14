#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W7a's WebKit2 on Kosmos (`docs/w7.md`).

`webkit2test.elf` - WebKit2's three processes, built as WebKit's Kosmos
port, and `user/kits/webkit2test/` - is a UI process with no window. It
starts a web process and a network process, which are the same image
started again in their roles, loads a page in them, and paints the picture
the web process sends it into a surface of 800 by 600. The pages are W6c's
boxes, and each picture is held pixel for pixel to GTK's expected one, as
`x86-webcore` holds a page WebCore painted in one process: the same Skia
paints both, so the processes and the connection between them must change
nothing.

Then the same page with the web process ended as soon as its navigation
commits: the UI process must be told, say so, load the page again in a new
web process, and that must paint the same picture.

Usage: run_webkit2.py IMAGE [APPS]

The control: `KOSMOS_WEBKIT2_BREAK=picture` changes one pixel of the first
expected picture, and that picture must disagree.
"""

import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402
import pngread                                               # noqa: E402
import upstream                                              # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("webkit2")
FOLDER = os.path.join(WORK, "webkit2")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

SUITE = "css2.1/20110323"
PAGES = [SUITE + "/abspos-containing-block-initial-%s.htm" % n for n in ("004a", "009f")]
# Each load as (its picture's name, the page, the mode).
LOADS = [(t.rsplit("/", 1)[1], t, "load") for t in PAGES] + \
        [("killed-" + PAGES[0].rsplit("/", 1)[1], PAGES[0], "kill")]

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W7a's WebKit2 test (`tools/run_webkit2.py`).
-- kosmos: image webkit2test.elf
local kit = use("webkit2test.elf")
local folder = "/Home/apps/webkit2/webkit2"
for _, load in ipairs({%s}) do
  local name, mode = load[1], load[2]
  local html = assert(fs.read(folder .. "/pages/" .. name:gsub("^killed%%-", "")))
  local surface = gfx.surface{ w = 800, h = 600 }
  print("load " .. name)
  for _, line in ipairs(kit.load(html, surface, 120000, mode)) do
    print("said " .. line)
  end
  local ok, why = fs.write(folder .. "/pictures/" .. name .. ".png", gfx.encode_png(surface))
  print(ok and ("saved " .. name) or ("unsaved " .. name .. ": " .. tostring(why)))
end
print("webkit2test: done")
""" % ", ".join('{"%s", "%s"}' % (name, mode) for name, _, mode in LOADS)


def stem(t):
    return t.rsplit(".", 1)[0]


def said_for(said):
    """The guest's lines, as what each load said."""
    found, current = {}, None
    for ln in said:
        if ln.startswith("load "):
            current = ln[5:]
            found[current] = []
        elif ln.startswith("said ") and current is not None:
            found[current].append(ln[5:])
    return found


def picture_disagrees(mine, page, broken, first):
    """A picture against GTK's expected one: why it differs, or None."""
    theirs = os.path.join(upstream.webkit_tests(), "LayoutTests", "platform", "gtk",
                          stem(page) + "-expected.png")
    if not os.path.exists(mine):
        return "no picture"
    w, h, have = pngread.pixels(mine)
    ew, eh, want = pngread.pixels(theirs)
    if broken == "picture" and first:
        row = bytearray(want[300])
        row[1200] ^= 0xff
        want = want[:300] + [bytes(row)] + want[301:]
    if (w, h) != (ew, eh):
        return "painted %dx%d, GTK's %dx%d" % (w, h, ew, eh)
    wrong = [(x, y) for y in range(h) if have[y] != want[y]
             for x in range(w) if have[y][x * 3:x * 3 + 3] != want[y][x * 3:x * 3 + 3]]
    if wrong:
        x, y = wrong[0]
        return "%d of %d pixels differ from GTK's, the first (%d,%d): GTK %s, Kosmos %s" % (
            len(wrong), w * h, x, y, want[y][x * 3:x * 3 + 3].hex(), have[y][x * 3:x * 3 + 3].hex())
    return None


def main():
    built = os.path.join(APPS, "webkit2test.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make ARCH=x86_64 apps` builds it" % built)
        return 1
    broken = os.environ.get("KOSMOS_WEBKIT2_BREAK")
    tests = upstream.webkit_tests()
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(os.path.join(FOLDER, "pages"))
    os.makedirs(os.path.join(FOLDER, "pictures"))
    for t in PAGES:
        shutil.copyfile(os.path.join(tests, "LayoutTests", t),
                        os.path.join(FOLDER, "pages", t.rsplit("/", 1)[1]))
    with open(os.path.join(FOLDER, "pictures", "README"), "w") as f:
        f.write("W7a: each page as a web process painted it.\n")

    image = os.path.join(WORK, "webkit2test.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "webkit2test.lua"), "w") as f:
        f.write(PROGRAM)
    files = [(os.path.join(WORK, n), "/Home/apps/webkit2/" + n)
             for n in ("webkit2test.lua", "webkit2test.elf")]
    for here, _, names in sorted(os.walk(FOLDER)):
        for n in sorted(names):
            src = os.path.join(here, n)
            files.append((src, "/Home/apps/webkit2/webkit2/" + os.path.relpath(src, FOLDER)))
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 400)
    said = []
    done = False
    mark = 0
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/webkit2/webkit2test.lua")
        while time.monotonic() < started + 360 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines
                    if ln.startswith(("load ", "said ", "webkit2test", "saved ", "unsaved "))]
            done = "webkit2test: done" in said
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_WEBKIT2_SERIAL"):
        with open(os.environ["KOSMOS_WEBKIT2_SERIAL"], "w") as f:
            f.write(guest.seen)

    if not done:
        print("FAIL: the WebKit2 test did not finish\n--- the guest's last words ---\n"
              + guest.seen[mark:][-3000:])
        return 1

    pictures = os.path.join(ROOT, "build", "webkit2", "pictures")
    shutil.rmtree(pictures, ignore_errors=True)
    os.makedirs(pictures)
    for name in [ln[len("saved "):] for ln in said if ln.startswith("saved ")]:
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK,
                        "/Home/apps/webkit2/webkit2/pictures/" + name + ".png",
                        os.path.join(pictures, name + ".png")],
                       check=True, capture_output=True, cwd=ROOT)

    told = said_for(said)
    failed = []
    for i, (name, page, mode) in enumerate(LOADS):
        lines = told.get(name, [])
        print("%s (%s): %s" % (name, mode, "; ".join(lines) or "nothing said"))
        if "painted" not in lines:
            failed.append("%s: not painted" % name)
            continue
        if mode == "kill":
            story = ["ending the web process", "the web process ended", "loading again", "painted"]
            at = [lines.index(s) if s in lines else -1 for s in story]
            if -1 in at or at != sorted(at):
                failed.append("%s: the ended web process was not said and loaded again" % name)
                continue
            pids = [ln.split()[-1] for ln in lines if ln.startswith("committed in ")]
            if len(pids) < 2 or pids[0] == pids[-1]:
                failed.append("%s: loaded again in the same process (%s)" % (name, ", ".join(pids)))
                continue
        why = picture_disagrees(os.path.join(pictures, name + ".png"), page, broken, i == 0)
        if why:
            failed.append("%s: %s" % (name, why))
    for f in failed:
        print("FAIL " + f)
    if failed:
        print("FAIL: %d of %d loads through WebKit2's processes as expected"
              % (len(LOADS) - len(failed), len(LOADS)))
        return 1
    print("PASS: WebKit2 on Kosmos - a UI process started a web process and a network process, "
          "and %d pages were loaded in them and painted pixel for pixel as GTK's port paints "
          "them; a web process ended halfway was said, and the page loaded again in a new one "
          "and painted the same; in %.1f s" % (len(PAGES), took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
