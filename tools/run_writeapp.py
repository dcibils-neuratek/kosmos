#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Kosmos Write's window: **the page on the screen is the page that prints**
(`docs/write.md` W4, `testing.md` 18.382).

A document is made in the machine and opened in Write; the window is looked
at, zoomed, and exported to PDF with Control-E; the PDF is taken off the
machine's disk on this Mac, and macOS renders it (`sips`) at the size the
window showed the page. **With a disk**, as Write is used: without one,
`/Home` is held in memory, where a file may be no larger than 16 KB, and a
PDF of five faces is not (`roadmap.md`, *In memory, no size compiled in*). Then the two pictures are compared **line by line**: the
same lines of text, each starting, ending and standing where the other's
does, to a couple of pixels - which is what one setting for the screen and
the PDF promises, and what a screen set with a screen font's rounded widths
would break within a line.

Usage: run_writeapp.py IMAGE
"""

import os
import re
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402
import scratch                                               # noqa: E402

HOST_LUA = "build/host/lua"

# The document, made at the prompt: a title, a justified paragraph long
# enough to wrap several times, a heading, and a line in two looks.
MAKE = ('local wd = use("/Kosmos/Libraries/writedoc.lua") local d = wd.new() '
        'd.body = { { style = "Title", runs = { { text = "Screen and paper" } } }, '
        '{ style = "Body", align = "justify", runs = { { text = ("The page on the '
        'screen is the page that prints, line for line. "):rep(5) } } }, '
        '{ style = "Heading 1", runs = { { text = "Faces" } } }, '
        '{ style = "Body", runs = { { text = "Italic, ", italic = true }, '
        '{ text = "bold", weight = "Bold" }, { text = " and plain." } } } } '
        'print("made" .. ":", wd.save("/Home/t.write", d))')



def kfs(*args):
    done = subprocess.run([HOST_LUA, "tools/kfs.lua", *args],
                          capture_output=True, text=True)

    if done.returncode != 0:
        raise R.Failure("kfs.lua %s failed:\n%s%s"
                        % (" ".join(args), done.stdout, done.stderr))


def with_disk(image, disk):
    """The display harness's machine, with `disk` as its drive."""
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    device = "virtio-blk-pci" if board == "X86_ARGS" else "virtio-blk-device"

    setattr(R, board, saved + [
        "-drive", "file=%s,format=raw,if=none,id=disk" % disk,
        "-device", device + ",drive=disk",
    ])

    try:
        return R.Guest(image, 120)
    finally:
        setattr(R, board, saved)


def png(width, height, rgb):
    """RGB bytes as a PNG: for `KEEP_SHOT`, a picture a person can open."""
    rows = b"".join(b"\0" + rgb[3 * width * y:3 * width * (y + 1)]
                    for y in range(height))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def bands(width, height, inked):
    """The lines of text in a picture: runs of rows with ink, each as its
    top, bottom, leftmost and rightmost inked pixel."""
    out, current = [], None

    for y in range(height):
        xs = [x for x in range(width) if inked(x, y)]

        if xs:
            if current is None:
                current = [y, y, xs[0], xs[-1]]
            else:
                current[1] = y
                current[2] = min(current[2], xs[0])
                current[3] = max(current[3], xs[-1])
        elif current is not None:
            out.append(tuple(current))
            current = None

    if current is not None:
        out.append(tuple(current))

    return out


def main():
    image = sys.argv[1]
    work = scratch.directory("w")
    disk = os.path.join(work, "write.img")
    maker = os.path.join(work, "mk.lua")

    with open(maker, "w") as f:
        f.write(MAKE)

    kfs("create", disk, "32")
    kfs("put", disk, maker, "/Home/mk.lua")

    guest = with_disk(image, disk)
    failed = []
    checks = 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def said(text, since, seconds=30):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            at = guest.seen.find(text, since)

            if at >= 0 and "\n" in guest.seen[at + len(text):]:
                return guest.seen[at + len(text):].split("\n", 1)[0].strip()

            time.sleep(0.1)

        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        # A program, since `use` is a program's and not the prompt's.
        mark = len(guest.seen)
        guest.type("run /Home/mk.lua")

        if said("made:", mark) != "true":
            print("FAIL: the document was not made at the prompt:\n"
                  + guest.seen[mark:][-800:])
            return 1

        # ---- the window, and the page in it ----
        mark = len(guest.seen)
        guest.type("wm writer:/Home/t.write")
        opened = said("wm: window t.write - Kosmos Write at ", mark, 90)
        shown = said("writer: t.write, ", mark, 90)

        if opened is None or shown is None:
            print("FAIL: Write did not open t.write.\n--- the guest said ---\n"
                  + guest.seen[mark:][-1500:])
            return 1

        wx, wy, ww, wh = (int(v) for v in
                          re.match(r"(\d+),(\d+) (\d+)x(\d+)", opened).groups())
        m = re.match(r"(\d+) pages? at (\d+)%, page 1 at (\d+),(\d+) (\d+)x(\d+)",
                     shown)
        pages, percent, px_, py_, pw, ph = (int(v) for v in m.groups())

        check(pages == 1 and percent == 125 and pw == round(595.276 * 1.25),
              "Write did not show one A4 page at 125%%: %r" % shown)

        time.sleep(2)
        sw, sh, pixels = R.parse_ppm(guest.screendump())

        # `KEEP_SHOT=file.png` keeps the screen, to be looked at by a person.
        if os.environ.get("KEEP_SHOT"):
            with open(os.environ["KEEP_SHOT"], "wb") as f:
                f.write(png(sw, sh, pixels))
        left, top = wx + px_, wy + py_

        def rgb(x, y):
            i = 3 * (y * sw + x)
            return pixels[i], pixels[i + 1], pixels[i + 2]

        # The page's rows the window shows: it is taller than the view.
        visible = min(ph, wy + wh - top, sh - top)

        # White paper at its corners - the bottom ones where the window
        # stops showing it.
        corners = [rgb(left + 2, top + 2), rgb(left + pw - 3, top + 2),
                   rgb(left + 2, top + visible - 4),
                   rgb(left + pw - 3, top + visible - 4)]

        check(all(c == (255, 255, 255) for c in corners),
              "the page is not white paper at its corners: %r" % corners)
        screen = bands(pw, visible, lambda x, y: sum(rgb(left + x, top + y)) < 3 * 150)

        check(len(screen) >= 6, "the page on the screen has %d lines of ink"
              % len(screen))

        # ---- zoom ----
        mark = len(guest.seen)
        guest.sendkey("equal")
        zoomed = said("writer: t.write, ", mark, 20)
        check(zoomed is not None and " at 150%, " in zoomed
              and ("%dx" % round(595.276 * 1.5)) in zoomed,
              "+ did not zoom to 150%%: %r" % zoomed)

        mark = len(guest.seen)
        guest.sendkey("minus")
        said("writer: t.write, ", mark, 20)

        # ---- Export PDF ----
        mark = len(guest.seen)
        guest.sendkey("ctrl-e")
        exported = said("writer: exported ", mark, 60)
        check(exported is not None and exported.startswith("/Home/t.pdf, 1 pages"),
              "Control-E did not export the PDF: %r - the guest said:\n%s"
              % (exported, guest.seen[mark:][-600:]))

        # ---- the PDF, off the disk ----
        guest.close()
        pdf = os.path.join(work, "t.pdf")
        kfs("get", disk, "/Home/t.pdf", pdf)

        with open(pdf, "rb") as f:
            data = f.read()

        check(data.startswith(b"%PDF-1.4") and str(len(data)) in (exported or ""),
              "the PDF on the disk is %d bytes and the window said %r"
              % (len(data), exported))

        picture = os.path.join(work, "t.bmp")

        # ---- macOS draws the PDF at the window's size, and the lines are
        # held to each other ----
        done = subprocess.run(["sips", "-s", "format", "bmp",
                               "--resampleHeightWidth", str(ph), str(pw), pdf,
                               "--out", picture], capture_output=True, text=True)

        if done.returncode != 0:
            failed.append("macOS would not render the exported PDF: "
                          + done.stdout + done.stderr)
            raise R.Failure("no picture")

        with open(picture, "rb") as f:
            bmp = f.read()

        offset = struct.unpack("<I", bmp[10:14])[0]
        bw, bh = struct.unpack("<ii", bmp[18:26])
        rows_down = bh < 0
        bh = abs(bh)

        def alpha(x, y):
            row = y if rows_down else bh - 1 - y
            return bmp[offset + 4 * (row * bw + x) + 3]

        paper = bands(bw, min(bh, visible), lambda x, y: alpha(x, y) > 110)

        check(bw == pw and bh == ph, "macOS drew the page %dx%d, not %dx%d"
              % (bw, bh, pw, ph))

        if len(paper) != len(screen):
            check(False, "the screen shows %d lines and the PDF %d:\n  screen %r"
                  "\n  paper  %r" % (len(screen), len(paper), screen, paper))
        else:
            worst = 0

            for a, b in zip(screen, paper):
                worst = max(worst, *(abs(p - q) for p, q in zip(a, b)))

            check(worst <= 3, "a line on the screen is %d pixels from the "
                  "PDF's:\n  screen %r\n  paper  %r" % (worst, screen, paper))

        print("  %d lines on the screen, each within %s pixels of the PDF's"
              % (len(screen), "a few" if failed else "3"))
    except R.Failure as e:
        failed.append(str(e))
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on Kosmos Write's window:\n  %s"
              % (len(failed), max(checks, len(failed)), "\n  ".join(failed)))
        return 1

    print("PASS: %d checks on Kosmos Write's window (the page on the screen "
          "is the PDF's, line by line)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
