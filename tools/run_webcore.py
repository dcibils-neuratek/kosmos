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

And W6c's: each page painted by WebCore through Skia into a Kosmos surface
of 800 by 600, saved by `gfx.encode_png`, brought back off the disk to
`build/webcore/pictures/` to be looked at, and held pixel for pixel to
GTK's expected picture of the same test - the same Skia draws both, and
with no text in them nothing a font engine decides can differ.

And W6c's text: the LayoutTests whose page is set in Ahem and needs nothing
loaded (`upstream.WEBKIT_TEXT_TESTS`), held to their trees as text - their
pictures are the font engine's, so only the fourteen above are compared.

Usage: run_webcore.py IMAGE [APPS]

The controls: `KOSMOS_WEBCORE_BREAK=page` moves one box of one page by a
pixel before the guest is given it, and that page's tree must disagree;
`=picture` changes one pixel of the first expected picture, and that
picture must; `=text` sets one text test's Ahem a pixel larger, and its
tree must; `=generic` names IBM Plex Sans beside `serif`, and serif must;
`=script` takes the one script out of a page that scrolls itself, and its
tree must; `=image` leaves the picture three pages load off the disk, and
their trees must - without the process dying.
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
WORK = scratch.directory("webcore")
FOLDER = os.path.join(WORK, "webcore")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

SUITE = "css2.1/20110323"
# Each test as its path under LayoutTests: the fourteen boxes held to trees
# and pictures, and the tests with text in Ahem held to trees.
PICTURED = [SUITE + "/abspos-containing-block-initial-%s.htm" % n
            for n in ("004a", "004b", "004c", "004d", "004e", "004f",
                      "005a", "005b", "005c", "005d", "009a", "009b", "009e", "009f")]
TESTS = PICTURED + upstream.WEBKIT_TEXT_TESTS + upstream.WEBKIT_SCRIPT_TESTS
# `KOSMOS_WEBCORE_ONLY=a,b` runs only the tests whose path holds one of
# those, for finding which page did something; the gate sets nothing.
if os.environ.get("KOSMOS_WEBCORE_ONLY"):
    _only = os.environ["KOSMOS_WEBCORE_ONLY"].split(",")
    TESTS = [t for t in TESTS if any(o in t for o in _only)]
    PICTURED = [t for t in PICTURED if t in TESTS]
FONTS = ["IBMPlexSans-Regular.ttf", "IBMPlexSerif-Regular.ttf", "IBMPlexMono-Regular.ttf"]
BROKEN = TESTS[0]
BROKEN_TEXT = "fast/text/soft-hyphen-3.html"
BROKEN_SCRIPT = "fast/block/positioning/rtl-fixed-positioning.html"

# The generic families, Kosmos's own check: a page in each, and in none,
# laid out exactly as the same page naming the font Kosmos gives that
# family - with its text measured, not empty.
SENTENCE = "Kosmos lays out a page in its own fonts."
GENERIC = [("serif", "IBM Plex Serif"), ("sans-serif", "IBM Plex Sans"),
           ("monospace", "IBM Plex Mono"), (None, "IBM Plex Serif")]


def generic_pages(broken):
    """Each pair as (generic page, named page, its html, the other's)."""
    pages = []
    for i, (generic, named) in enumerate(GENERIC):
        if broken == "generic" and i == 0:
            named = "IBM Plex Sans"             # the control: the wrong face
        style = lambda family: (' style="font-family: %s; font-size: 20px"' % family
                                if family else ' style="font-size: 20px"')
        pages.append(("generic-%d.html" % i, "named-%d.html" % i,
                      "<div%s>%s</div>" % (style(generic), SENTENCE),
                      "<div%s>%s</div>" % (style("'%s'" % named), SENTENCE)))
    return pages


def page_of(t):
    """The name a test's page has in the guest's folder: its file's."""
    return t.rsplit("/", 1)[1]


def stem(t):
    return t.rsplit(".", 1)[0]

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W6b's WebCore test (`tools/run_webcore.py`).
-- kosmos: image webcoretest.elf
local kit = use("webcoretest.elf")
local folder = "/Home/apps/webcore/webcore"
local pages = {%s}
for _, line in ipairs(kit.run(folder, pages)) do
  print(line)
end
-- W6c: each page painted into a surface the size of the view, and saved.
for _, page in ipairs(pages) do
  local surface = gfx.surface{ w = 800, h = 600 }
  for _, line in ipairs(kit.paint(folder, page, surface)) do
    print(line)
  end
  local ok, why = fs.write(folder .. "/pictures/" .. page .. ".png", gfx.encode_png(surface))
  print(ok and ("saved " .. page) or ("unsaved " .. page .. ": " .. tostring(why)))
end
print("webcoretest: done")
""" % ", ".join('"%s"' % n for n in [page_of(t) for t in TESTS]
               + [n for g in generic_pages(None) for n in g[:2]])


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
    # What the pages load beside them - an image, a sheet - at the path
    # they name it by, since every page sits in the one folder.
    for f in upstream.WEBKIT_TEST_FILES:
        to = os.path.join(FOLDER, "pages", "resources", f.split("resources/", 1)[1])
        os.makedirs(os.path.dirname(to), exist_ok=True)
        if broken != "image":
            shutil.copyfile(os.path.join(tests, "LayoutTests", f), to)
    expected = {}
    assert len({page_of(t) for t in TESTS}) == len(TESTS), "two tests share a file name"
    for t in TESTS:
        page = open(os.path.join(tests, "LayoutTests", t)).read()
        if broken == "page" and t == BROKEN:
            # The control: the root box a pixel to the right.
            assert "left:100px" in page
            page = page.replace("left:100px", "left:101px", 1)
        if broken == "text" and t == BROKEN_TEXT:
            # The control for text: Ahem a pixel larger, every glyph wider.
            assert "font-size: 25px" in page
            page = page.replace("font-size: 25px", "font-size: 26px", 1)
        if broken == "script" and t == BROKEN_SCRIPT:
            # The control for scripts: the page's one script taken out.
            assert "window.scrollBy(-100, 100);" in page
            page = page.replace("window.scrollBy(-100, 100);", "", 1)
        with open(os.path.join(FOLDER, "pages", page_of(t)), "w") as f:
            f.write(page)
        lines = open(os.path.join(tests, "LayoutTests", stem(t) + "-expected.txt")).read().rstrip("\n")
        expected[page_of(t)] = [ln.rstrip() for ln in lines.split("\n")]
    for generic, named, a, b in generic_pages(broken):
        for name, html in ((generic, a), (named, b)):
            with open(os.path.join(FOLDER, "pages", name), "w") as f:
                f.write(html)
    return expected


def generics_disagree(got):
    """The generic families' pages against the named ones: the names that
    differ, or whose text measured nothing."""
    failed = []
    for (generic, named), (gpage, npage, _, _) in zip(GENERIC, generic_pages(None)):
        a, b = got.get(gpage), got.get(npage)
        widths = [ln for ln in (a or []) if "text run" in ln and SENTENCE in ln]
        if a is None or b is None:
            print("FAIL %s: no tree" % (generic or "the default font"))
        elif a != b:
            print("FAIL %s: not laid out as '%s'" % (generic or "the default font", named))
            for x, y in zip(a, b):
                if x != y:
                    print("  %s\n  %s" % (x, y))
                    break
        elif not widths or " width 0:" in widths[0]:
            print("FAIL %s: its text has no width" % (generic or "the default font"))
        else:
            continue
        failed.append(generic or "default")
    return failed


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


def pictures_disagree(pictures, broken):
    """Each picture the guest painted against GTK's expected one, pixel for
    pixel: the names that differ, with the first pixels that do said. The
    control `picture` changes one pixel of the first expected picture."""
    tests = upstream.webkit_tests()
    failed = []
    for t in PICTURED:
        mine = os.path.join(pictures, page_of(t) + ".png")
        if not os.path.exists(mine):
            print("FAIL %s: no picture" % t)
            failed.append(t)
            continue
        theirs = os.path.join(tests, "LayoutTests", "platform", "gtk", stem(t) + "-expected.png")
        w, h, have = pngread.pixels(mine)
        ew, eh, want = pngread.pixels(theirs)
        if broken == "picture" and t == BROKEN:
            row = bytearray(want[300])
            row[1200] ^= 0xff
            want = want[:300] + [bytes(row)] + want[301:]
        if (w, h) != (ew, eh):
            print("FAIL %s: painted %dx%d, GTK's %dx%d" % (t, w, h, ew, eh))
            failed.append(t)
            continue
        wrong = [(x, y) for y in range(h) if have[y] != want[y]
                 for x in range(w) if have[y][x * 3:x * 3 + 3] != want[y][x * 3:x * 3 + 3]]
        if wrong:
            failed.append(t)
            print("FAIL %s: %d of %d pixels differ from GTK's" % (t, len(wrong), w * h))
            for x, y in wrong[:3]:
                print("  (%d,%d): GTK %s, Kosmos %s" % (x, y, want[y][x * 3:x * 3 + 3].hex(),
                                                       have[y][x * 3:x * 3 + 3].hex()))
    return failed


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
    os.makedirs(os.path.join(FOLDER, "pictures"), exist_ok=True)
    with open(os.path.join(FOLDER, "pictures", "README"), "w") as f:
        f.write("W6c: each page as WebCore painted it.\n")
    for here, _, names in sorted(os.walk(FOLDER)):
        for n in sorted(names):
            src = os.path.join(here, n)
            files.append((src, "/Home/apps/webcore/webcore/" + os.path.relpath(src, FOLDER)))
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
            said = [ln for ln in lines
                    if ln.startswith(("page ", "tree ", "webcoretest", "painted ", "saved ",
                                      "unsaved "))]
            done = "webcoretest: done" in said
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

    unsaved = [ln for ln in said if ln.startswith("unsaved ")]
    pictures = os.path.join(ROOT, "build", "webcore", "pictures")
    shutil.rmtree(pictures, ignore_errors=True)
    os.makedirs(pictures)
    for name in [ln[len("saved "):] for ln in said if ln.startswith("saved ")]:
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK,
                        "/Home/apps/webcore/webcore/pictures/%s.png" % name,
                        os.path.join(pictures, name + ".png")],
                       check=True, capture_output=True, cwd=ROOT)
    print("pictures: %d of %d painted, in %s%s" % (
        len(os.listdir(pictures)), len(expected) + 2 * len(GENERIC), pictures,
        "" if not unsaved else " - " + "; ".join(unsaved)))

    picture_failed = pictures_disagree(pictures, broken)

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
    generic_failed = generics_disagree(got)
    if generic_failed:
        print("FAIL: %d of %d generic families as Kosmos's fonts: %s"
              % (len(GENERIC) - len(generic_failed), len(GENERIC), ", ".join(generic_failed)))
        return 1
    if picture_failed:
        print("FAIL: %d of %d LayoutTests painted as GTK's port paints them: %s"
              % (len(PICTURED) - len(picture_failed), len(PICTURED), ", ".join(picture_failed)))
        return 1

    print("PASS: WebCore laid out %d of WebKit's LayoutTests on Kosmos, %d of them with text "
          "in Ahem, every render tree as WebKit expects on every port, and painted the %d "
          "without text through Skia pixel for pixel as GTK's port does; serif, sans-serif, "
          "monospace and the default font as IBM Plex's three; in %.1f s"
          % (len(expected), len(expected) - len(PICTURED), len(PICTURED), took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
