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
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402
import scratch                                               # noqa: E402
from run_editor import letters                               # noqa: E402

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
        # Ink is text: the caret - the drawing's accent, 0x2a55c9 - stands at
        # the title's start and is not.
        caret_colour = (0x2a, 0x55, 0xc9)
        screen = bands(pw, visible,
                       lambda x, y: (sum(rgb(left + x, top + y)) < 3 * 150
                                     and rgb(left + x, top + y) != caret_colour))

        check(len(screen) >= 6, "the page on the screen has %d lines of ink"
              % len(screen))

        # ---- zoom, by the bar's buttons: + and - are typed ----
        def click(x, y):
            guest.mouse_to(*R._to_tablet(wx + x, wy + y, sw, sh))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)
            time.sleep(0.3)

        # The Zoom tool's list, where the window says it is, and its fifth
        # item, 150%.
        def where(line):
            m = re.match(r"(\d+),(\d+) (\d+)x(\d+)", line or "")
            return tuple(int(v) for v in m.groups()) if m else None

        tool = where(said("writer: tool zoom at ", 0, 10))

        def zoom_to(item):
            mark = len(guest.seen)
            click(tool[0] + tool[2] // 2, tool[1] + 20)
            box = where(said("writer: menu zoom at ", mark, 10))

            if box is None:
                return None

            mark = len(guest.seen)
            click(box[0] + 40, box[1] + 4 + (item - 1) * 30 + 15)
            return said("writer: t.write, ", mark, 20)

        zoomed = zoom_to(5) if tool else None
        check(zoomed is not None and " at 150%, " in zoomed
              and ("%dx" % round(595.276 * 1.5)) in zoomed,
              "the Zoom tool did not zoom to 150%%: %r" % zoomed)

        if tool:
            zoom_to(4)

        # ---- Export PDF ----
        mark = len(guest.seen)
        guest.sendkey("ctrl-e")
        exported = said("writer: exported ", mark, 60)
        check(exported is not None and exported.startswith("/Home/t.pdf, 1 pages"),
              "Control-E did not export the PDF: %r - the guest said:\n%s"
              % (exported, guest.seen[mark:][-600:]))

        # ---- the PDF opened in Kosmos's own PDF viewer, through the PDF
        # Kit's door: every glyph of its page drawn, no face missing ----
        # The desktop put away first, as `run_editor.py` does: typed lines
        # reach the shell only when it is at its prompt.
        mark = len(guest.seen)
        guest.proc.stdin.write(R.STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 20

        while time.monotonic() < deadline and R.PROMPT not in guest.seen[mark:]:
            guest._read_available()
            time.sleep(0.2)

        mark = len(guest.seen)
        guest.type("wm pdfview:/Home/t.pdf")
        viewed = said("pdfview: page 1 of 1, ", mark, 90)
        m = re.match(r"(\d+) glyphs, (\d+) faces missing", viewed or "")
        check(m is not None and int(m.group(1)) > 100 and m.group(2) == "0",
              "the PDF viewer did not draw the exported page: %r" % viewed)

        # ---- typing (W4b): a new document, by the keyboard and a click ----
        mark = len(guest.seen)
        guest.proc.stdin.write(R.STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 20

        while time.monotonic() < deadline and R.PROMPT not in guest.seen[mark:]:
            guest._read_available()
            time.sleep(0.2)

        mark = len(guest.seen)
        new_window_from = mark
        guest.type("wm writer")
        fresh = said("writer: Untitled, 1 page at 125%, page 1 at ", mark, 90)
        opened = said("wm: window Untitled - Kosmos Write at ", mark, 30)

        if fresh is None or opened is None:
            check(False, "Write did not open a new document: %r" % fresh)
        else:
            wx, wy = (int(v) for v in re.match(r"(\d+),(\d+)", opened).groups())
            fx, fy = (int(v) for v in re.match(r"(\d+),(\d+)", fresh).groups())
            time.sleep(1.5)

            def press(*names):
                for n in names:
                    guest.sendkey(n)
                    time.sleep(0.05)

            press(*letters("Hello wordl"), "backspace", "backspace", *letters("ld"))
            press("ret", *letters("Second line"))
            press("home", *letters("A "))
            press("ctrl-z", "ctrl-y")
            press("end", *(["shift-left"] * 4), *letters("text"))
            press("up", "end", *letters("!"))

            # A click left of the first line's text, inside the margin: the
            # caret at its start. The line's baseline is 25 mm and an ascent
            # down the page, about 103 pixels at 125%.
            time.sleep(0.5)
            guest.mouse_to(*R._to_tablet(wx + fx + 60, wy + fy + 98, sw, sh))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)
            time.sleep(0.3)
            press(*letters("Yes "))

            # Formatting (W4c): "Hello world!" selected, made bold by
            # Control-B and italic by the panel, and its paragraph made a
            # Heading 1 from the style's list.
            press("shift-end", "ctrl-b")
            seen_from = new_window_from

            def control(key):
                m = re.search(r"writer: control %s at (\d+),(\d+) (\d+)x(\d+)" % key,
                              guest.seen[seen_from:])
                return tuple(int(v) for v in m.groups()) if m else None

            def press_at(x, y):
                guest.mouse_to(*R._to_tablet(wx + x, wy + y, sw, sh))
                time.sleep(0.3)
                guest.mouse_button(True)
                time.sleep(0.15)
                guest.mouse_button(False)
                time.sleep(0.4)

            marks = control("marks")
            style_box = control("style")

            if marks and style_box:
                # Italic is the second of four.
                press_at(marks[0] + 3 + (marks[2] - 6) * 3 // 8, marks[1] + marks[3] // 2)
                mark = len(guest.seen)
                press_at(style_box[0] + 30, style_box[1] + style_box[3] // 2)
                styles = re.search(r"writer: menu style at (\d+),(\d+) (\d+)x(\d+)",
                                   guest.seen[mark:])

                if styles:
                    sx, sy = int(styles.group(1)), int(styles.group(2))
                    press_at(sx + 40, sy + 4 + 2 * 30 + 15)      # Heading 1
            else:
                check(False, "the Format panel did not say where its controls are")

            mark = len(guest.seen)
            press("ctrl-s")
            saved = said("writer: saved ", mark, 30)
            check(saved == "/Home/Untitled.write, 2 paragraphs",
                  "Control-S did not save the typed document: %r" % saved)

            time.sleep(1)

            if os.environ.get("KEEP_TYPED"):
                tw, th, typed_px = R.parse_ppm(guest.screendump())

                with open(os.environ["KEEP_TYPED"], "wb") as f:
                    f.write(png(tw, th, typed_px))

        # ---- the PDF, off the disk ----
        guest.close()
        pdf = os.path.join(work, "t.pdf")
        kfs("get", disk, "/Home/t.pdf", pdf)

        with open(pdf, "rb") as f:
            data = f.read()

        check(data.startswith(b"%PDF-1.4") and str(len(data)) in (exported or ""),
              "the PDF on the disk is %d bytes and the window said %r"
              % (len(data), exported))

        # The typed document, read here: what the keys and the click meant.
        typed_file = os.path.join(work, "typed.write")

        try:
            kfs("get", disk, "/Home/Untitled.write", typed_file)

            with zipfile.ZipFile(typed_file) as z:
                text = z.read("document").decode("utf-8")

            got = re.findall(r'text = "([^"]*)"', text)
            hello = re.search(r'\{[^{}]*text = "Hello world!"[^{}]*\}', text)
            heading = re.search(r'style = "Heading 1"', text)
        except (R.Failure, OSError, KeyError, zipfile.BadZipFile) as e:
            got, hello, heading = ["could not be read: %s" % e], None, None

        check(got == ["Yes ", "Hello world!", "A Second text"],
              "the typed document holds %r, not what the keys meant" % got)
        check(hello is not None and 'weight = "Bold"' in hello.group(0)
              and "italic = true" in hello.group(0) and heading is not None,
              "the Format panel did not make the words bold and italic and "
              "the paragraph a Heading 1: %r" % (hello.group(0) if hello else text[:400]))

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
