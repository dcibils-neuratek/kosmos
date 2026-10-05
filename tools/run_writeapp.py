#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Kosmos Write's window: **the page on the screen is the page that prints**
(`docs/write.md` W4, `testing.md` 18.382).

A document is made in the machine and opened in Write; the window is looked
at, zoomed, and exported to PDF with Control-E; the PDF is taken off the
machine's disk on this Mac, and macOS renders it (`sips`) at the size the
window showed the page. **With a disk**, as Write is used: without one,
`/Home` is held in memory, where a file could be no larger than 16 KB until
5 October 2026, and a PDF of five faces is not (`roadmap.md`, *In memory, no
size compiled in*). Then the two pictures are compared **line by line**: the
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


def sea_png(width=160, height=100):
    """A picture to put in (W5): sky over sea, as a PNG made here."""
    rows = []

    for y in range(height):
        sky = y < height * 2 // 5
        row = bytearray(b"\0")

        for x in range(width):
            row += bytes((90 + x // 4, 150 + y // 2, 230) if sky else (20, 70 + y // 3, 140))

        rows.append(bytes(row))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


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

    sea = os.path.join(work, "sea.png")
    sea_bytes = sea_png()

    with open(sea, "wb") as f:
        f.write(sea_bytes)

    kfs("put", disk, sea, "/Home/Pictures/sea.png")

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

            # The Document panel and the tools (W4d): Add Page, Letter, the
            # header's words, the left margin, View.
            def tool(key):
                m = re.search(r"writer: tool %s at (\d+),(\d+) (\d+)x(\d+)" % key,
                              guest.seen[seen_from:])
                return tuple(int(v) for v in m.groups()) if m else None

            def last_report(since):
                return said("writer: Untitled, ", since, 15)

            add = tool("addpage")
            mark = len(guest.seen)

            if add:
                press_at(add[0] + add[2] // 2, add[1] + 20)

            pages = last_report(mark)
            check(pages is not None and pages.startswith("2 pages"),
                  "Add Page did not make a second page: %r" % pages)

            tabs = control("tabs")

            if tabs:
                press_at(tabs[0] + tabs[2] * 3 // 4, tabs[1] + tabs[3] // 2)

            paper = control("paper")
            mark = len(guest.seen)

            if paper:
                press_at(paper[0] + 30, paper[1] + paper[3] // 2)
                listed = re.search(r"writer: menu paper at (\d+),(\d+)", guest.seen[mark:])

                if listed:
                    mark = len(guest.seen)
                    press_at(int(listed.group(1)) + 40, int(listed.group(2)) + 4 + 30 + 15)

            letter = last_report(mark)
            check(letter is not None and (" %dx%d" % (round(612 * 1.25), round(792 * 1.25))) in letter,
                  "choosing Letter did not make the page 612 by 792 points: %r" % letter)

            words = control("header_text")
            left = control("margin_left")

            if words and left:
                press_at(words[0] + 20, words[1] + words[3] // 2)
                press(*letters("Header words"), "ret")
                plus = left[0] + left[2] - 12
                press_at(plus, left[1] + left[3] // 2)
                press_at(plus, left[1] + left[3] // 2)
            else:
                check(False, "the Document panel did not say where its controls are")

            # The Document panel's switches (W4e): facing pages, hyphenation,
            # ligatures off, and Spanish.
            for key in ("facing", "hyphenation", "ligatures"):
                box = control(key)

                if box:
                    press_at(box[0] + 8, box[1] + box[3] // 2)
                else:
                    check(False, "the Document panel did not say where %s is" % key)

            language = control("language")
            mark = len(guest.seen)

            if language:
                press_at(language[0] + 30, language[1] + language[3] // 2)
                listed = re.search(r"writer: menu language at (\d+),(\d+)", guest.seen[mark:])

                if listed:
                    press_at(int(listed.group(1)) + 40, int(listed.group(2)) + 4 + 30 + 15)

            view = tool("view")
            mark = len(guest.seen)

            if view:
                press_at(view[0] + view[2] // 2, view[1] + 20)

            check(said("writer: thumbnails shown", mark, 10) is not None,
                  "View did not show the page thumbnails")

            # A numbered list (W4e): at the document's end, Numbers from the
            # Layout part, two items, Return twice to leave it, and a line.
            press("ctrl-end", "ret")

            if tabs:
                press_at(tabs[0] + tabs[2] // 4, tabs[1] + tabs[3] // 2)

            parts = control("parts")

            if parts:
                press_at(parts[0] + parts[2] // 2, parts[1] + parts[3] // 2)

            lists = control("list")
            mark = len(guest.seen)

            if lists:
                press_at(lists[0] + 30, lists[1] + lists[3] // 2)
                listed = re.search(r"writer: menu list at (\d+),(\d+)", guest.seen[mark:])

                if listed:
                    press_at(int(listed.group(1)) + 40,
                             int(listed.group(2)) + 4 + 2 * 30 + 15)       # Numbers
            else:
                check(False, "the Layout part did not say where its list chooser is")

            press(*letters("Item one"), "ret", *letters("Item two"), "ret", "ret",
                  *letters("After"))

            # A picture (W5): Media, the Open panel at /Home/Pictures with
            # the one picture there chosen, Return, and a caption.
            media = tool("media")
            mark = len(guest.seen)

            if media:
                press_at(media[0] + media[2] // 2, media[1] + 20)

            # The Open panel (`panel.lua`): its first entry is chosen as it
            # opens, and its Open button - 96 by 24, 12 from the right and
            # 62 from the foot - opens it.
            chooser = said("wm: window Choose a picture at ", mark, 30)
            where = re.match(r"(\d+),(\d+) (\d+)x(\d+)", chooser or "")

            if where:
                cx, cy, cw, ch = (int(v) for v in where.groups())
                time.sleep(1.0)
                guest.mouse_to(*R._to_tablet(cx + cw - 12 - 48, cy + ch - 62 + 12, sw, sh))
                time.sleep(0.3)
                guest.mouse_button(True)
                time.sleep(0.15)
                guest.mouse_button(False)
                time.sleep(0.5)

            put_in = said("writer: picture ", mark, 30)
            check(put_in is not None and put_in.startswith("pictures/1.png, 160x100 px"),
                  "Media did not put the picture in: %r" % put_in)
            press(*letters("A sea"))

            # A table (W5b): the Table tool, its cells typed into with Tab -
            # Shift-Tab back once, and a fourth row made by Tab in the last
            # cell - a column added from the Format panel's Table part, and
            # Return out of its last row onto the line after it.
            table_tool = tool("table")
            mark = len(guest.seen)

            if table_tool:
                press_at(table_tool[0] + table_tool[2] // 2, table_tool[1] + 20)

            made = said("writer: table 3 by 3 at paragraph ", mark, 30)
            check(made is not None, "the Table tool did not put a table in")

            for i, word in enumerate(["Planet", "Moons", "Kind", "Mars", "2", "rock",
                                      "Earth", "1", "rock", "Venus"]):
                if i:
                    press("tab")
                press(*letters(word))

            press("tab", "0", "shift-tab", "shift-1", "tab", "tab", *letters("rock"))
            columns = control("table_columns")
            mark = len(guest.seen)

            if columns:
                press_at(columns[0] + columns[2] - 12, columns[1] + columns[3] // 2)

            check(said("writer: table 4 by 4", mark, 15) is not None,
                  "the Table part's stepper did not add a column to the table "
                  "Tab had made four rows")
            press("ret", *letters("Done"))

            # A text box (W7a): the Text tool, two lines - Return breaks one
            # in a box - its fill chosen from the panel, Down out of it.
            text_tool = tool("textbox")
            mark = len(guest.seen)

            if text_tool:
                press_at(text_tool[0] + text_tool[2] // 2, text_tool[1] + 20)

            check(said("writer: text box at paragraph ", mark, 30) is not None,
                  "the Text tool did not put a text box in")
            press(*letters("Note"), "ret", *letters("two"))
            fill = control("box_fill")
            mark = len(guest.seen)

            if fill:
                press_at(fill[0] + 20, fill[1] + fill[3] // 2)
                listed = re.search(r"writer: menu fill at (\d+),(\d+)", guest.seen[mark:])

                if listed:
                    press_at(int(listed.group(1)) + 40, int(listed.group(2)) + 4 + 30 + 15)

            check(said("writer: text box 80 mm, bordered, #eef3fb", mark, 15) is not None,
                  "the Text box part did not fill the box with Mist")
            press("down", *letters("End"))

            # A shape (W7b): the Shape tool's list, its fifth, a star - and
            # on the screen a star: the drawing's blue at its middle and the
            # paper at its box's corner, which a rectangle would have
            # covered. Then wider from the Shape part, and Down past it.
            shape_tool = tool("shape")
            mark = len(guest.seen)

            if shape_tool:
                press_at(shape_tool[0] + shape_tool[2] // 2, shape_tool[1] + 20)
                listed = re.search(r"writer: menu shape at (\d+),(\d+)", guest.seen[mark:])

                if listed:
                    press_at(int(listed.group(1)) + 40, int(listed.group(2)) + 4 + 4 * 30 + 15)

            drawn = said("writer: shape star at paragraph ", mark, 30)
            box = re.search(r", (\d+)x(\d+) px at (-?\d+),(-?\d+)", drawn or "")
            check(box is not None, "the Shape tool did not put a star in: %r" % drawn)

            if box:
                bw, bh, bx, by = (int(v) for v in box.groups())
                time.sleep(1.0)
                sw2, sh2, shot = R.parse_ppm(guest.screendump())

                def on_screen(x, y):
                    i = 3 * ((wy + y) * sw2 + wx + x)
                    return shot[i], shot[i + 1], shot[i + 2]

                middle = on_screen(bx + bw // 2, by + bh // 2)
                corner = on_screen(bx + 3, by + 3)
                check(middle == (0x2a, 0x55, 0xc9) and corner == (0xff, 0xff, 0xff),
                      "the star on the screen is not blue at its middle on white paper "
                      "at its corner: %r and %r" % (middle, corner))

            wider = control("shape_width")
            mark = len(guest.seen)

            if wider:
                press_at(wider[0] + wider[2] - 12, wider[1] + wider[3] // 2)

            check(said("writer: shape star 45 by 30 mm, #2a55c9", mark, 15) is not None,
                  "the Shape part's stepper did not make the star wider")
            press("down", *letters("Fin"))

            # A chart (W7c): the Chart tool's first, columns - on the screen
            # in both series' colours - its data shown from the panel, a
            # number typed over, Bar chosen, the data hidden, Down past it.
            chart_tool = tool("chart")
            mark = len(guest.seen)

            if chart_tool:
                press_at(chart_tool[0] + chart_tool[2] // 2, chart_tool[1] + 20)
                listed = re.search(r"writer: menu chart at (\d+),(\d+)", guest.seen[mark:])

                if listed:
                    press_at(int(listed.group(1)) + 40, int(listed.group(2)) + 4 + 15)

            drawn = said("writer: chart column at paragraph ", mark, 30)
            box = re.search(r", (\d+)x(\d+) px at (-?\d+),(-?\d+)", drawn or "")
            check(box is not None, "the Chart tool did not put a chart in: %r" % drawn)

            if box:
                bw, bh, bx, by = (int(v) for v in box.groups())
                time.sleep(1.0)
                sw3, sh3, shot3 = R.parse_ppm(guest.screendump())
                inks = {}

                for yy in range(max(0, wy + by), min(sh3, wy + by + bh), 2):
                    for xx in range(max(0, wx + bx), min(sw3, wx + bx + bw), 2):
                        i = 3 * (yy * sw3 + xx)
                        key = (shot3[i], shot3[i + 1], shot3[i + 2])
                        inks[key] = inks.get(key, 0) + 1

                common = sorted(inks.items(), key=lambda kv: -kv[1])[:5]
                check(inks.get((0x2a, 0x55, 0xc9), 0) > 100 and inks.get((0xd3, 0x54, 0x00), 0) > 100,
                      "the chart on the screen is not columns in its two series' colours: "
                      "%r, the box's commonest %r" % (drawn, common))

            data = control("chart_data")
            mark = len(guest.seen)

            if data:
                press_at(data[0] + 8, data[1] + data[3] // 2)

            check(said("writer: chart data shown", mark, 15) is not None,
                  "Edit data did not show the chart's data")
            press("backspace", "backspace", *letters("40"))

            kinds = control("chart_kind")
            mark = len(guest.seen)

            if kinds:
                press_at(kinds[0] + 3 + (kinds[2] - 6) * 3 // 8, kinds[1] + kinds[3] // 2)

            check(said("writer: chart bar 70 mm, 4 categories, 2 series", mark, 15) is not None,
                  "the Chart part did not make the chart bars")

            if data:
                mark = len(guest.seen)
                press_at(data[0] + 8, data[1] + data[3] // 2)

            check(said("writer: chart data hidden", mark, 15) is not None,
                  "Edit data did not hide the chart's data again")
            press("down", *letters("Last"))

            # A comment (W7d): the word just typed selected, the Comment
            # tool, its words typed into the panel's field, Return.
            press("shift-home")
            comment_tool = tool("comment")
            mark = len(guest.seen)

            if comment_tool:
                press_at(comment_tool[0] + comment_tool[2] // 2, comment_tool[1] + 20)

            check(said('writer: comment 1 on "Last"', mark, 30) is not None,
                  "the Comment tool did not put a comment on the selected word")
            mark = len(guest.seen)
            press(*letters("Check this"), "ret")
            check(said('writer: comment 1 says "Check this"', mark, 15) is not None,
                  "the comment's words were not typed into its field")

            # Export's list, its third item: Word's DOCX (W6).
            export_tool = tool("export")
            mark = len(guest.seen)

            if export_tool:
                press_at(export_tool[0] + export_tool[2] // 2, export_tool[1] + 20)
                listed = re.search(r"writer: menu export at (\d+),(\d+)", guest.seen[mark:])

                if listed:
                    press_at(int(listed.group(1)) + 40, int(listed.group(2)) + 4 + 2 * 30 + 15)

            docx_said = said("writer: exported /Home/Untitled.docx, ", mark, 30)
            check(docx_said is not None and docx_said.startswith("16 paragraphs"),
                  "Export's list did not export Word's DOCX: %r" % docx_said)

            mark = len(guest.seen)
            press("ctrl-s")
            saved = said("writer: saved ", mark, 30)
            check(saved == "/Home/Untitled.write, 16 paragraphs",
                  "Control-S did not save the typed document: %r" % saved)

            # **Resized, as the browser is** (Diego, 5 October: "I need to be
            # able to resizse the window like we do with the browsser"): the
            # grip, the window's bottom right corner, dragged 300 left and
            # 100 up - Write takes 900x700 and lays its tools out to it, the
            # last one ending 12 in from the new right edge.
            mark = len(guest.seen)
            gx, gy = wx + 1200 - 6, wy + 800 - 6
            guest.mouse_to(*R._to_tablet(gx, gy, sw, sh))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)

            for step in range(1, 6):
                guest.mouse_to(*R._to_tablet(gx - 60 * step, gy - 20 * step, sw, sh))
                time.sleep(0.15)

            guest.mouse_button(False)
            resized = said("writer: resized to ", mark, 30)
            check(resized == "900x700, the tools ending at 888",
                  "the grip dragged 300 left and 100 up did not resize Write to "
                  "900x700 with its tools laid out to it: %r" % resized)

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

            with zipfile.ZipFile(typed_file) as z:
                kept = z.read("pictures/1.png") if "pictures/1.png" in z.namelist() else None

            check(kept == sea_bytes and 'name = "pictures/1.png"' in text,
                  "the .write file does not hold the picture as it came, or its paragraph")
            hello = re.search(r'\{[^{}]*text = "Hello world!"[^{}]*\}', text)
            check("columns = 4" in text and "header = true" in text,
                  "the table in the file is not four columns with a header row")
            check(re.search(r'box = \{[^{}]*fill = "#eef3fb"', text) is not None,
                  "the text box in the file is not filled with Mist")
            check(re.search(r'shape = \{[^{}]*kind = "star"[^{}]*width_mm = 45', text) is not None,
                  "the star in the file is not 45 mm wide")
            check(re.search(r'chart = \{[^{}]*kind = "bar"', text) is not None,
                  "the chart in the file is not bars")
            check(re.search(r'\{[^{}]*id = 1,[^{}]*text = "Check this"', text) is not None
                  and re.search(r'\{[^{}]*comment = 1,[^{}]*text = "Last"', text) is not None,
                  "the comment and the word it is about are not in the file")

            # Word's DOCX, read here: the table as Word's, its new column.
            word_file = os.path.join(work, "typed.docx")
            kfs("get", disk, "/Home/Untitled.docx", word_file)

            with zipfile.ZipFile(word_file) as z:
                word = z.read("word/document.xml").decode("utf-8")

            check(word.count("<w:tbl>") == 2 and word.count("<w:gridCol ") == 5
                  and 'r:id="rIdChart1"' in word and '<w:commentRangeStart w:id="1"/>' in word
                  and word.count("<w:tr>") == 5
                  and "Venus!" in word and "<w:tblHeader/>" in word,
                  "the DOCX does not hold the table, four by four with its header")
            heading = re.search(r'style = "Heading 1"', text)
        except (R.Failure, OSError, KeyError, zipfile.BadZipFile) as e:
            got, hello, heading = ["could not be read: %s" % e], None, None

        # The file's keys are sorted, so the body's words come before the
        # header's.
        check(got == ["Yes ", "Hello world!", "A Second text", "Item one",
                      "Item two", "After", "A sea", "Planet", "Moons", "Kind",
                      "Mars", "2", "rock", "Earth", "1", "rock", "Venus!", "0",
                      "rock", "Done", "Note\\ntwo", "End", "Fin",
                      "2025", "2026", "Spring", "40", "18", "Summer", "20", "26",
                      "Autumn", "15", "21", "Winter", "9", "14", "Last", "Check this",
                      "Header words"],
              "the typed document holds %r, not what the keys meant" % got)
        check(text.count('name = "Letter"') == 1 and 'text = "Header words"' in text
              and re.search(r"left = 27[,\n]", text) is not None
              and "page_break_before = true" in text,
              "the Document panel's paper, header words and margin, and Add "
              "Page's break, are not in the file")
        check("facing = true" in text and "hyphenation = true" in text
              and "ligatures = false" in text and 'language = "es"' in text,
              "the Document panel's switches and language are not in the file")
        check(text.count('list = "number"') == 2,
              "the two items are not a numbered list in the file, and the "
              "line after them out of it: %d" % text.count('list = "number"'))
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
