#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Kosmos Write inside the machine, held to what this Mac reads for itself.

`docs/write.md`. The host tests (`test_writedoc.lua`, `test_pageset.lua`)
hold the arithmetic against a measure they can do sums with; this holds the
machine's half against an outside reader:

  * **W2, the faces.** `gfx.typefaces()` names every TrueType face the image
    carries, and the names, weights and slants must be what the fonts in
    `assets/fonts/` say - read here by a reader of this file's own. A
    face's advance for a string and its vertical metrics, in the font's
    units, must be this reader's sums from `cmap` and `hmtx`.
  * **A paragraph set with them** must break where this Mac breaks it,
    greedily, with the same widths - a second implementation of the one
    thing a page must agree with a PDF about.
  * **A hundred pages set**, and how long that took, to the counter.

The reader below is enough of TrueType for that and no more: the table
directory, `head`, `hhea`, `OS/2`, `name`, `cmap` formats 4 and 12, `hmtx`.
"""

import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_disk
import scratch


HOST_LUA = "build/host/lua"
TOOL = "tools/kfs.lua"
FONTS = "assets/fonts"

# A string with an accent, a dash and a character no Latin face has, so a
# missing glyph is counted too.
PROBE = "Hello, Kosmos Write - café 東"


class Failure(Exception):
    pass


def kfs(*args):
    done = subprocess.run([HOST_LUA, TOOL, *args], capture_output=True,
                          text=True)

    if done.returncode != 0:
        raise Failure(f"kfs.lua {' '.join(args)} failed:\n"
                      + done.stdout + done.stderr)

    return done.stdout


class Font:
    """A TrueType file, read for its names and its measures."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()

        count = struct.unpack(">H", self.data[4:6])[0]
        self.tables = {}

        for i in range(count):
            tag, _, offset, length = struct.unpack(
                ">4sIII", self.data[12 + 16 * i:28 + 16 * i])
            self.tables[tag.decode("latin-1")] = (offset, length)

        head = self.table("head")
        self.units = struct.unpack(">H", head[18:20])[0]

        hhea = self.table("hhea")
        self.ascent, self.descent, self.gap = struct.unpack(">hhh", hhea[4:10])
        self.hmetrics = struct.unpack(">H", hhea[34:36])[0]

        os2 = self.table("OS/2")
        self.weight = struct.unpack(">H", os2[4:6])[0]
        self.italic = bool(struct.unpack(">H", os2[62:64])[0] & 1)

        self.family = self.name(16) or self.name(1)
        self.cmap = self.read_cmap()

        hmtx = self.table("hmtx")
        self.advances = [struct.unpack(">H", hmtx[4 * i:4 * i + 2])[0]
                         for i in range(self.hmetrics)]

    def table(self, tag):
        offset, length = self.tables[tag]
        return self.data[offset:offset + length]

    def name(self, wanted):
        t = self.table("name")
        count, strings = struct.unpack(">HH", t[2:6])

        for i in range(count):
            platform, encoding, language, nid, length, offset = struct.unpack(
                ">HHHHHH", t[6 + 12 * i:18 + 12 * i])

            if (platform, encoding, language, nid) == (3, 1, 0x409, wanted):
                return t[strings + offset:strings + offset + length] \
                    .decode("utf-16-be")

        return None

    def read_cmap(self):
        t = self.table("cmap")
        count = struct.unpack(">H", t[2:4])[0]
        best = None

        for i in range(count):
            platform, encoding, offset = struct.unpack(
                ">HHI", t[4 + 8 * i:12 + 8 * i])
            fmt = struct.unpack(">H", t[offset:offset + 2])[0]

            if (platform, encoding, fmt) == (3, 10, 12):
                best = (offset, 12)
                break

            if (platform, encoding, fmt) == (3, 1, 4) and best is None:
                best = (offset, 4)

        offset, fmt = best
        out = {}

        if fmt == 12:
            groups = struct.unpack(">I", t[offset + 12:offset + 16])[0]

            for g in range(groups):
                start, end, glyph = struct.unpack(
                    ">III", t[offset + 16 + 12 * g:offset + 28 + 12 * g])

                for c in range(start, end + 1):
                    out[c] = glyph + c - start
        else:
            segs = struct.unpack(">H", t[offset + 6:offset + 8])[0] // 2
            ends = offset + 14
            starts = ends + 2 * segs + 2
            deltas = starts + 2 * segs
            ranges = deltas + 2 * segs

            for s in range(segs):
                end = struct.unpack(">H", t[ends + 2 * s:ends + 2 * s + 2])[0]
                start = struct.unpack(">H", t[starts + 2 * s:starts + 2 * s + 2])[0]
                delta = struct.unpack(">h", t[deltas + 2 * s:deltas + 2 * s + 2])[0]
                ro = struct.unpack(">H", t[ranges + 2 * s:ranges + 2 * s + 2])[0]

                for c in range(start, end + 1):
                    if c == 0xffff:
                        continue

                    if ro == 0:
                        glyph = (c + delta) & 0xffff
                    else:
                        at = ranges + 2 * s + ro + 2 * (c - start)
                        glyph = struct.unpack(">H", t[at:at + 2])[0]

                        if glyph:
                            glyph = (glyph + delta) & 0xffff

                    if glyph:
                        out[c] = glyph

        return out

    def advance(self, text):
        """The text's advance in font units, and how many had no glyph."""
        total, missing = 0, 0

        for ch in text:
            glyph = self.cmap.get(ord(ch), 0)
            missing += glyph == 0
            total += self.advances[min(glyph, self.hmetrics - 1)]

        return total, missing


# The paragraph both sides set: Body, IBM Plex Serif at 11 points, across
# A4's column with 25 mm margins.
PARAGRAPH = ("Kosmos Write sets a paragraph once, for the screen and the PDF "
             "alike, and this paragraph is set twice: once inside the machine "
             "with the faces the image carries, and once on this Mac with "
             "the same fonts read by a reader of its own. Where each line "
             "breaks has to be the same on both sides, word for word, or the "
             "page on the screen and the page that prints are two different "
             "pages. Café, naïve and résumé are here "
             "for the accents.")

COLUMN_PT = (210 - 2 * 25) * 72 / 25.4


def greedy(font, size_pt, text, column):
    """Lines broken at spaces, as many words as fit: the Mac's own."""
    scale = size_pt / font.units
    lines, line = [], ""

    for word in text.split(" "):
        trial = word if not line else line + " " + word

        if line and font.advance(trial)[0] * scale > column:
            lines.append(line)
            line = word
        else:
            line = trial

    lines.append(line)
    return lines


PROGRAM = '''
local faces = use("/Kosmos/Libraries/faces.lua")
local writedoc = use("/Kosmos/Libraries/writedoc.lua")
local pageset = use("/Kosmos/Libraries/pageset.lua")

for _, f in ipairs(gfx.typefaces()) do
  print("FACE", f.file, f.family, f.weight, f.italic)
end

local probe = %(probe)s

for _, file in ipairs{ "IBMPlexSans-Regular.ttf", "IBMPlexSerif-Bold.ttf",
                       "IBMPlexSans-SemiBold.ttf", "Oswald-Regular.ttf" } do
  local face = gfx.typeface(file)
  print("ADVANCE", file, face:advance(probe))
  print("METRICS", file, face:metrics())
end

print("NOFACE", gfx.typeface("Nope.ttf"))

local catalogue = faces.catalogue(gfx.typefaces())
local measure = faces.measure(catalogue, gfx.typeface)

local doc = writedoc.new()
doc.body = { { style = "Body", runs = { { text = %(paragraph)s } } } }

local set = pageset.set(writedoc.check(doc), measure)

for i, line in ipairs(set.pages[1].lines) do
  local parts = {}
  for k, pc in ipairs(line.pieces) do parts[k] = pc.text end
  print("LINE", i, table.concat(parts))
end

local body = {}
local words = ("the quick brown fox jumps over the lazy dog "):rep(7)

for i = 1, 1000 do
  body[i] = { style = i %% 25 == 1 and "Heading 1" or "Body",
              runs = { { text = words:sub(1, 300) } } }
end

local book = writedoc.check{ format = "kosmos-write", version = 1, body = body }
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local t0 = sys.ticks()
local pages = pageset.set(book, measure).pages
print("SET", #pages, (sys.ticks() - t0) * 1000 // hz)
'''


def lua_string(s):
    """A Lua string literal: printable ASCII as it is, every other byte of
    the UTF-8 as a decimal escape."""
    out = []

    for b in s.encode("utf-8"):
        if 32 <= b < 127 and b not in (34, 92):
            out.append(chr(b))
        else:
            out.append("\\%03d" % b)

    return '"' + "".join(out) + '"'


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0
    work = scratch.directory()
    disk = os.path.join(work, "write.img")

    try:
        program = os.path.join(work, "wsuite.lua")

        with open(program, "w") as f:
            f.write(PROGRAM % {"probe": lua_string(PROBE),
                               "paragraph": lua_string(PARAGRAPH)})

        kfs("create", disk, "32")
        kfs("put", disk, program, "/Home/wsuite.lua")

        out = run_disk.boot(image, disk, ["run /Home/wsuite.lua"], each=120)
        flat = out.replace("\t", " ")

        def said(marker):
            return [l[len(marker) + 1:].strip() for l in flat.splitlines()
                    if l.startswith(marker + " ")]

        # ---- every face, named as the font names itself ----
        fonts = {name: Font(os.path.join(FONTS, name))
                 for name in sorted(os.listdir(FONTS))
                 if name.endswith(".ttf")}
        seen = {}

        for line in said("FACE"):
            m = re.match(r"(\S+) (.+) (\d+) (true|false)$", line)

            if m:
                seen[m.group(1)] = (m.group(2), int(m.group(3)),
                                    m.group(4) == "true")

        if set(seen) != set(fonts):
            raise Failure("gfx.typefaces() lists %s; the image carries %s"
                          % (sorted(seen), sorted(fonts)))

        for name, font in fonts.items():
            if seen[name] != (font.family, font.weight, font.italic):
                raise Failure("%s is %r inside the machine and %r by the font"
                              % (name, seen[name],
                                 (font.family, font.weight, font.italic)))

        checks += 1

        # ---- a face's measures, in its own units ----
        advances = {l.split(" ")[0]: l.split(" ")[1:] for l in said("ADVANCE")}
        metrics = {l.split(" ")[0]: l.split(" ")[1:] for l in said("METRICS")}

        for name in ("IBMPlexSans-Regular.ttf", "IBMPlexSerif-Bold.ttf",
                     "IBMPlexSans-SemiBold.ttf", "Oswald-Regular.ttf"):
            font = fonts[name]
            want = [str(v) for v in font.advance(PROBE)]

            if advances.get(name) != want:
                raise Failure("%s's advance for %r is %r inside the machine "
                              "and %r here" % (name, PROBE, advances.get(name),
                                               want))

            want = [str(v) for v in (font.units, font.ascent, font.descent,
                                     font.gap)]

            if metrics.get(name) != want:
                raise Failure("%s's metrics are %r inside the machine and %r "
                              "here" % (name, metrics.get(name), want))

        checks += 1

        if not said("NOFACE") or not said("NOFACE")[-1].startswith(
                "nil no face called Nope.ttf"):
            raise Failure("a face the image does not carry was not refused "
                          "with why: %r" % said("NOFACE"))

        checks += 1

        # ---- a paragraph broken where this Mac breaks it ----
        mine = greedy(fonts["IBMPlexSerif-Regular.ttf"], 11, PARAGRAPH,
                      COLUMN_PT)
        theirs = [l.split(" ", 1)[1] if " " in l else ""
                  for l in said("LINE")]

        if theirs != mine:
            raise Failure("the machine broke the paragraph into\n  %s\nand "
                          "this Mac into\n  %s"
                          % ("\n  ".join(theirs), "\n  ".join(mine)))

        checks += 1

        # ---- a hundred pages, to the counter ----
        got = said("SET")

        if not got or not re.match(r"\d+ \d+$", got[-1]) \
                or int(got[-1].split()[0]) < 40:
            raise Failure("a thousand paragraphs were not set: %r\n%s"
                          % (got, out[-900:]))

        pages, ms = (int(v) for v in got[-1].split())
        checks += 1

        print(f"PASS: {checks} checks on Kosmos Write inside the machine "
              f"({len(fonts)} faces as the fonts name them, a paragraph of "
              f"{len(mine)} lines broken as this Mac breaks it, a hundred "
              f"pages - {pages} - set in {ms} ms under emulation).")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
