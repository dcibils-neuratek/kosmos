#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W6b's WebCore on Kosmos (`docs/w6.md`).

`webcoretest.elf` - WebCore built as WebKit's Kosmos port, with no window
and no GPU (the EGL Kit's no display), and `user/kits/webcoretest/` -
loads WebKit's own LayoutTests into a page each and says their render
trees, which are held line for line to the tests' `-expected.txt`, as
WebKit's `run-webkit-tests` holds every port to them (Diego's decision 2A).

The tests are CSS 2.1's of the initial containing block, the fourteen of
that suite whose expected tree is one file for every port - boxes placed,
sized, bordered and coloured with no text in them - fetched from WebKit's
git at the release's commit (`upstream.WEBKIT_TESTS`). The fonts the
harness is given are the Skia test's, and Ahem, LayoutTests' own.

Usage: run_webcore.py IMAGE [APPS]

The control: `KOSMOS_WEBCORE_BREAK=page` moves one box of one page by a
pixel before the guest is given it, and that page's tree must disagree.
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
import upstream                                              # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("webcore")
FOLDER = os.path.join(WORK, "webcore")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

SUITE = "LayoutTests/css2.1/20110323"
TESTS = ["abspos-containing-block-initial-" + n
         for n in ("004a", "004b", "004c", "004d", "004e", "004f",
                   "005a", "005b", "005c", "005d", "009a", "009b", "009e", "009f")]
FONTS = ["IBMPlexSans-Regular.ttf", "IBMPlexSerif-Regular.ttf", "IBMPlexMono-Regular.ttf"]
BROKEN = TESTS[0]

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W6b's WebCore test (`tools/run_webcore.py`).
-- kosmos: image webcoretest.elf
local kit = use("webcoretest.elf")
for _, line in ipairs(kit.run("/Home/apps/webcore/webcore", {%s})) do
  print(line)
end
""" % ", ".join('"%s.htm"' % t for t in TESTS)


def gather(broken):
    """The pages and the fonts, under the folder; the expected trees."""
    tests = upstream.webkit_tests()
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(os.path.join(FOLDER, "pages"))
    os.makedirs(os.path.join(FOLDER, "fonts"))
    for f in FONTS:
        shutil.copyfile(os.path.join(ROOT, "assets", "fonts", f),
                        os.path.join(FOLDER, "fonts", f))
    shutil.copyfile(os.path.join(tests, "LayoutTests", "resources", "Ahem.ttf"),
                    os.path.join(FOLDER, "fonts", "Ahem.ttf"))
    expected = {}
    for t in TESTS:
        page = open(os.path.join(tests, SUITE, t + ".htm")).read()
        if broken == "page" and t == BROKEN:
            # The control: the root box a pixel to the right.
            assert "left:100px" in page
            page = page.replace("left:100px", "left:101px", 1)
        with open(os.path.join(FOLDER, "pages", t + ".htm"), "w") as f:
            f.write(page)
        lines = open(os.path.join(tests, SUITE, t + "-expected.txt")).read().rstrip("\n")
        expected[t + ".htm"] = [ln.rstrip() for ln in lines.split("\n")]
    return expected


def trees(said):
    """The guest's lines, as a tree for each page."""
    found, current = {}, None
    for ln in said:
        if ln.startswith("page "):
            current = ln[5:]
            found[current] = []
        elif ln.startswith("tree ") and current is not None:
            found[current].append(ln[5:].rstrip())
    for name in found:
        while found[name] and not found[name][-1]:
            found[name].pop()
    return found


def main():
    built = os.path.join(APPS, "webcoretest.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1
    broken = os.environ.get("KOSMOS_WEBCORE_BREAK")
    expected = gather(broken)

    image = os.path.join(WORK, "webcoretest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "webcoretest.lua"), "w") as f:
        f.write(PROGRAM)

    files = [(os.path.join(WORK, n), "/Home/apps/webcore/" + n)
             for n in ("webcoretest.lua", "webcoretest.elf")]
    for sub in ("pages", "fonts"):
        for n in sorted(os.listdir(os.path.join(FOLDER, sub))):
            files.append((os.path.join(FOLDER, sub, n), "/Home/apps/webcore/webcore/%s/%s" % (sub, n)))
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 300)
    said = []
    done = False
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/webcore/webcoretest.lua")
        while time.monotonic() < started + 240 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines if ln.startswith(("page ", "tree ", "webcoretest"))]
            done = any(ln.startswith("webcoretest: ") for ln in said)
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_WEBCORE_SERIAL"):
        with open(os.environ["KOSMOS_WEBCORE_SERIAL"], "w") as f:
            f.write(guest.seen)

    if not done:
        print("FAIL: the WebCore test did not finish\n--- the guest's last words ---\n"
              + guest.seen[mark:][-2000:])
        return 1

    got = trees(said)
    failed = []
    for name, want in expected.items():
        have = got.get(name)
        if have != want:
            failed.append(name)
            print("FAIL %s" % name)
            if have is None:
                print("  no tree")
                continue
            for i in range(max(len(want), len(have))):
                w = want[i] if i < len(want) else "(nothing)"
                h = have[i] if i < len(have) else "(nothing)"
                if w != h:
                    print("  line %d\n    expected: %s\n    Kosmos:   %s" % (i + 1, w, h))
    if failed:
        print("FAIL: %d of %d LayoutTests laid out as WebKit expects: %s"
              % (len(expected) - len(failed), len(expected), ", ".join(failed)))
        return 1

    print("PASS: WebCore laid out %d of CSS 2.1's LayoutTests on Kosmos, every render tree "
          "as WebKit expects on every port, in %.1f s" % (len(expected), took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
