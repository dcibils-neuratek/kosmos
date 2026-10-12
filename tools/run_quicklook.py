#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Quick Look in Tracker (`docs/quicklook.html`, Q1): Space shows a file.

A desktop, `/Home/Looks` holding a red picture 64 by 48 made here and a text
file, Tracker opened on it, the picture chosen, and the keys:

  - Space: the panel, saying a picture, PNG, 64 x 48;
  - on the screen, the picture's red - about 64 by 48 of it, so a picture
    smaller than the panel is shown at its own size, never enlarged;
  - Down: the text file, shown as its facts, and Tracker's selection with it;
  - Up: the picture again;
  - Down twice: a PDF of three pages of words made here - shown as its
    pages, the first's paper and its words on the screen, and the wheel
    bringing the second into view (Q2);
  - Down once more: a WAV made here, shown as a song of 0:03 and playing as
    soon as it is shown - heard, in what QEMU's sound card wrote - and Up
    stopping it (Q3);
  - Escape: closed, and the red gone from the screen;
  - Space and Return: closed, and the picture opened in Photo.

Usage: run_quicklook.py IMAGE
"""

import os
import random
import re
import math
import struct
import wave
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                             # noqa: E402
import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

CHECKS = 12
PIC_W, PIC_H = 64, 48
RED = (220, 30, 40)

MAKE = ('local files = use("/Kosmos/Libraries/files.lua")\n'
        'print("MADE", files.make_folder("/Home/Looks"))\n')

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = "tracker",'
          ' args = "/Home/Looks" })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')


def png(w, h, rgb):
    """A picture of one colour, as PNG: made here, nobody's."""
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def pdf_pages(work):
    """A PDF of three pages of words, through `tools/mkpdf.py`, which embeds
    a font: the PDF Kit draws text from an embedded face and nothing it
    only names, and draws no shapes yet - so a page of coloured squares, the
    first version of this, was blank paper in Quick Look and in the PDF
    viewer alike. Made here, nobody's."""
    text = os.path.join(work, "pages.txt")
    out = os.path.join(work, "pages.pdf")
    line = "Quick Look shows a document as its pages, one under another."    # one line: 46 a page
    with open(text, "w") as f:
        f.write("\n".join(line for _ in range(46 * 3)))
    subprocess.run([sys.executable, os.path.join(HERE, "mkpdf.py"), text, out],
                   check=True, capture_output=True, cwd=os.path.dirname(HERE))
    with open(out, "rb") as f:
        return f.read()


PAPER = (0xf4, 0xf1, 0xea)    # preview.lua's


def count(px, width, height, rgb, near=12):
    n = 0
    for i in range(0, width * height * 3, 3):
        if abs(px[i] - rgb[0]) < near and abs(px[i + 1] - rgb[1]) < near and abs(px[i + 2] - rgb[2]) < near:
            n += 1
    return n


def tone(path, seconds=3, rate=44100):
    """A tone of 440 Hz, stereo, sixteen bits: made here, nobody's."""
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(rate * seconds):
            v = int(12000 * math.sin(2 * math.pi * 440 * i / rate))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))
    with open(path, "rb") as f:
        return f.read()


def loudest_since(path, mark):
    """The loudest sample QEMU has written to the recording since `mark`."""
    with open(path, "rb") as f:
        data = f.read()[max(mark, 44):]
    n = len(data) // 2
    return max((abs(v) for v in struct.unpack("<%dh" % n, data[:n * 2])), default=0)


def dark(px, width, height):
    """How many pixels are dark - a word's stems, at a size where few are
    the ink's colour exactly."""
    n = 0
    for i in range(0, width * height * 3, 3):
        if px[i] < 110 and px[i + 1] < 110 and px[i + 2] < 110:
            n += 1
    return n


def reds(px, width, height):
    """How many pixels on the screen are the picture's red."""
    n = 0
    for i in range(0, width * height * 3, 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if abs(r - RED[0]) < 12 and abs(g - RED[1]) < 12 and abs(b - RED[2]) < 12:
            n += 1
    return n


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    work = scratch.directory("quicklook")
    heard = os.path.join(work, "heard.wav")
    # What the machine plays, written to a file (as `run_film.py` hears it).
    R.extra_args(image, ["-audiodev", "wav,id=snd0,path=%s" % heard,
                         "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd0"])
    guest = S.boot(image, telnet, web)
    fails = []
    said = {}

    def line(pattern, mark, seconds=15):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    def check(ok, what):
        if not ok:
            fails.append(what)

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(MAKE.encode(), "/Temporary/make.lua")
        session.run("/Temporary/make.lua")
        time.sleep(1)
        session.put(png(PIC_W, PIC_H, RED), "/Home/Looks/a-red.png")
        session.put(b"A note, made up for the test.\n", "/Home/Looks/b-notes.txt")
        session.put(pdf_pages(work), "/Home/Looks/c-pages.pdf")
        session.put(tone(os.path.join(work, "tone.wav")), "/Home/Looks/d-tone.wav")
        session.put(LAUNCH.encode(), "/Temporary/launch.lua")
        time.sleep(1)
        mark = len(guest.seen)
        session.run("/Temporary/launch.lua")
        rows = line(r"tracker: rows of /Home/Looks at (\d+),(\d+), (\d+) apart", mark, 30)
        if not rows:
            raise RuntimeError("Tracker never showed /Home/Looks")
        time.sleep(1.5)
        width, height, _ = R.parse_ppm(guest.screendump())

        # The picture chosen, by one click on its row.
        x, y = int(rows.group(1)), int(rows.group(2))
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.1)
        guest.mouse_button(True)
        time.sleep(0.05)
        guest.mouse_button(False)
        time.sleep(1)
        # Away from the panel's middle, so the pointer is not counted.
        guest.mouse_to(*R._to_tablet(5, height - 5, width, height))

        # Space: the panel, with the picture.
        mark = len(guest.seen)
        guest.sendkey("spc")
        shown = line(r"tracker: quick look (\S+) - (\w+), ([^;]*);", mark)
        said["picture"] = shown and shown.group(0)
        check(shown and shown.group(1) == "a-red.png" and shown.group(2) == "picture"
              and "Picture" in shown.group(3) and "64 x 48" in shown.group(3),
              "Space did not show a-red.png as a picture, 64 x 48: %r" % said["picture"])
        # Waited for, not a number of seconds: the picture's red, and how long
        # it took to come after the panel said it.
        start, n = time.time(), 0
        while time.time() - start < 10:
            _, _, px = R.parse_ppm(guest.screendump())
            n = reds(px, width, height)
            # The picture, not the first red pixel: four stray ones once
            # ended the wait before it was painted.
            if n >= PIC_W * PIC_H * 0.8:
                break
            time.sleep(0.25)
        said["red"], said["red after"] = n, time.time() - start
        ink_before = dark(px, width, height)
        check(PIC_W * PIC_H * 0.8 <= n <= PIC_W * PIC_H * 1.2,
              "the picture was not on the screen at its own size: %d red pixels, %d expected"
              % (n, PIC_W * PIC_H))

        # Down: the text file, as its facts.
        mark = len(guest.seen)
        guest.sendkey("down")
        shown = line(r"tracker: quick look (\S+) - (\w+), ([^;]*);", mark)
        check(shown and shown.group(1) == "b-notes.txt" and shown.group(2) == "facts",
              "Down did not show b-notes.txt as its facts: %r" % (shown and shown.group(0)))

        # Up: the picture again.
        mark = len(guest.seen)
        guest.sendkey("up")
        shown = line(r"tracker: quick look (\S+) - (\w+)", mark)
        check(shown and shown.group(1) == "a-red.png" and shown.group(2) == "picture",
              "Up did not come back to the picture: %r" % (shown and shown.group(0)))

        # Down twice: the PDF, as its pages.
        mark = len(guest.seen)
        guest.sendkey("down")
        guest.sendkey("down")
        shown = line(r"tracker: quick look c-pages.pdf - (\w+), ([^;]*);", mark)
        said["pdf"] = shown and shown.group(0)
        check(shown and shown.group(1) == "pdf" and "3 pages" in shown.group(2),
              "Down to the PDF did not show it as its pages, 3 of them: %r" % said["pdf"])
        # The first page: its paper, and words on it - more of the ink's
        # colour on the screen than there was before the PDF was shown.
        start, paper, ink = time.time(), 0, 0
        while time.time() - start < 15:
            _, _, px = R.parse_ppm(guest.screendump())
            paper, ink = count(px, width, height, PAPER, 4), dark(px, width, height)
            if paper > 30000 and ink - ink_before > 1500:
                break
            time.sleep(0.5)
        said["paper"], said["ink"] = paper, ink - ink_before
        check(paper > 30000 and ink - ink_before > 1500,
              "the PDF's first page was not drawn: %d paper pixels, %d more of the ink"
              % (paper, ink - ink_before))
        # The wheel over the pages: the panel's middle, which Tracker says in
        # its window's points, and where the window is.
        at = re.findall(r"wm: window Tracker at (\d+),(\d+)", guest.seen)
        middle = re.findall(r"quick look c-pages.pdf .*middle at (\d+),(\d+)", guest.seen)
        if at and middle:
            mx, my = int(at[-1][0]) + int(middle[-1][0]), int(at[-1][1]) + int(middle[-1][1])
        else:
            mx, my = width // 2, height // 2
        guest.mouse_to(*R._to_tablet(mx, my, width, height))
        time.sleep(0.3)
        mark = len(guest.seen)
        # A notch is 60 points and a page at the panel's width nearly a
        # thousand: twenty to be past the first.
        for _ in range(20):
            guest.mouse_button(True, "wheel-down")
            time.sleep(0.05)
            guest.mouse_button(False, "wheel-down")
            time.sleep(0.15)
        paged = line(r"tracker: quick look page 2 of 3", mark)
        time.sleep(1)
        _, _, px = R.parse_ppm(guest.screendump())
        said["ink 2"] = dark(px, width, height) - ink_before
        check(paged and said["ink 2"] > 1500,
              "the wheel did not bring the PDF's second page into view: %r, %d more of the ink"
              % (paged and paged.group(0), said["ink 2"]))
        guest.mouse_to(*R._to_tablet(5, height - 5, width, height))

        # Down: the song (Q3) - shown as one, three seconds long, playing as
        # soon as it is shown, and heard: the recording holds the tone.
        heard_at = os.path.getsize(heard) if os.path.exists(heard) else 44
        mark = len(guest.seen)
        guest.sendkey("down")
        shown = line(r"tracker: quick look d-tone.wav - (\w+), ([^;]*);", mark)
        said["song"] = shown and shown.group(0)
        check(shown and shown.group(1) == "song" and "0:03" in shown.group(2),
              "Down to the WAV did not show it as a song of 0:03: %r" % said["song"])
        sounding = line(r"tracker: quick look sounding d-tone.wav at ([12]) s", mark, 15)
        time.sleep(0.5)
        said["loudest"] = loudest_since(heard, heard_at) if os.path.exists(heard) else 0
        if os.environ.get("QL_SHOT"):         # a picture of the song's view, to look at
            open(os.environ["QL_SHOT"], "wb").write(guest.screendump())
        check(sounding and said["loudest"] > 2000,
              "the song did not play when shown: %r, loudest sample %d"
              % (sounding and sounding.group(0), said["loudest"]))

        # Up: another file, and the song stops - no more of it said.
        mark = len(guest.seen)
        guest.sendkey("up")
        line(r"tracker: quick look c-pages.pdf", mark)
        time.sleep(2)
        guest._read_available()
        said["stopped"] = guest.seen[mark:].count("sounding d-tone.wav")
        check(said["stopped"] <= 1, "the song went on playing after the arrows moved on: "
              "%d more seconds of it" % said["stopped"])

        mark = len(guest.seen)
        guest.sendkey("up")
        guest.sendkey("up")
        line(r"tracker: quick look a-red.png", mark)

        # Escape: closed, and the red gone.
        mark = len(guest.seen)
        guest.sendkey("esc")
        closed = line(r"tracker: quick look closed", mark)
        time.sleep(1.5)
        _, _, px = R.parse_ppm(guest.screendump())
        check(closed and reds(px, width, height) < 50,
              "Escape did not close the panel and take the picture away")

        # Space, then Return: closed, and the picture opened in Photo.
        mark = len(guest.seen)
        guest.sendkey("spc")
        line(r"tracker: quick look a-red.png", mark)
        mark = len(guest.seen)
        guest.sendkey("ret")
        check(line(r"tracker: quick look closed", mark) and line(r"wm: window Photo", mark, 20),
              "Return did not close the panel and open the picture in Photo")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on Quick Look:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if l.startswith("tracker:") or "Photo" in l or "error" in l.lower():
                print("  | " + l.strip()[:180])
        return 1

    print("PASS: %d checks on Quick Look (Space showing a PNG 64 x 48 as a picture, %d red "
          "pixels on the screen %.1f s after - its own size, not enlarged; Down a text file as its facts; "
          "Up the picture again; a PDF of three pages shown as its pages, %d paper pixels and "
          "%d of ink, the wheel to the second, %d of ink; Escape closing it; Return opening "
          "the picture in Photo; a WAV shown as a song, heard at %d, and stopped by Up)"
          % (CHECKS, said.get("red", 0), said.get("red after", 0), said.get("paper", 0),
             said.get("ink", 0), said.get("ink 2", 0), said.get("loudest", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
