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
  * **W3, the PDF.** A document of three pages written as a PDF inside the
    machine and read three ways: by Kosmos's own reader there - every page
    drawn, no face refused - and here, by this file, which takes it apart
    object by object (every offset in its table where it says, each piece
    of text where the setting placed it, each face's widths the font's own
    and its program the font's own bytes), and by macOS, which renders it
    (`sips`) without having heard of Kosmos.

The reader below is enough of TrueType for that and no more: the table
directory, `head`, `hhea`, `OS/2`, `name`, `cmap` formats 4 and 12, `hmtx`.
"""

import os
import re
import zlib
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
        self.ps_name = re.sub(r"[^A-Za-z0-9_-]", "", self.name(6) or "")
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

# W3's document: a title with an em dash, and a run with accents, a euro,
# curly quotes and one character no Latin face has.
TITLE = "Kosmos Write \u2014 PDF"
ACCENTS = " - caf\u00e9, \u20ac5, \u201cquoted\u201d, \u6771"


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

-- A PDF's font size is its em: a face drawn by the PDF reader's rasteriser
-- at 100 px must stand 102 px above its baseline (IBM Plex's ascent is 1025
-- units of 1000), not 78 (sized so that ascent and descent make 100).
do
  local at, len = gfx.typeface("IBMPlexSerif-Regular.ttf"):program()
  print("DOCFONT", gfx.docfont(at, len, len, 100):metrics().ascent)
end

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

-- W3: a document of every look, written as a PDF.
local pdfwrite = use("/Kosmos/Libraries/pdfwrite.lua")
local long = ("Paragraphs fill the page and then the next, and every line "
              .. "of them stands where the setting put it. "):rep(6)
local body = {
  { style = "Title", runs = { { text = %(title)s } } },
  { style = "Subtitle", runs = { { text = "Three pages, every look" } } },
  { style = "Heading 1", runs = { { text = "Looks" } } },
  { style = "Body", align = "justify", runs = {
      { text = "Plain, " }, { text = "italic", italic = true }, { text = ", " },
      { text = "bold", weight = "Bold" }, { text = ", " },
      { text = "underlined", underline = true }, { text = ", " },
      { text = "struck", strike = true }, { text = ", " },
      { text = "red", colour = "#c0392b" }, { text = %(accents)s },
      { text = " " .. long } } },
}

for i = 1, 14 do
  body[#body + 1] = { style = i %% 5 == 0 and "Heading 2" or "Body",
                      runs = { { text = i %% 5 == 0 and ("Part " .. i) or long } } }
end

local letter = writedoc.check{ format = "kosmos-write", version = 1, body = body }
local placed = pageset.set(letter, measure)
local ok, notes = use("/Kosmos/Libraries/pdf.lua").write("/Home/w.pdf",
  placed, measure, { title = "Kosmos Write" })

print("PDF", ok, type(notes) == "table" and notes.pages, type(notes) == "table" and notes.fonts,
      type(notes) == "table" and notes.missing, type(notes) == "table" and notes.bytes or notes)

for _, f in ipairs(placed.looks) do
  local e = measure.face_of(f)
  print("LOOKFACE", e.file)
end

local nonspace = 0

for p, page in ipairs(placed.pages) do
  local function piece(pc, baseline)
    if pc.text ~= "" then
      nonspace = nonspace + utf8.len((pc.text:gsub("[ %%c]", "")))
      -- In brackets, so a space at either end survives the console.
      print("PIECE", p, pdfwrite.num(pc.x_pt),
            pdfwrite.num(page.height_pt - baseline), "[" .. pc.text .. "]")
    end
  end

  for _, line in ipairs(page.lines) do
    for _, pc in ipairs(line.pieces) do piece(pc, line.baseline_pt) end
  end

  if page.footer then piece(page.footer.piece, page.footer.baseline_pt) end
end

-- And read back by Kosmos's own reader, through the PDF Kit's door: every
-- page drawn, no face refused.
local pdf = use("/Kosmos/Libraries/pdf.lua")
local opened, doc = pcall(pdf.open, "/Home/w.pdf")
local why = not opened and doc or nil
doc = opened and doc or nil
print("READ", doc ~= nil and #doc.pages or tostring(why))

local drawn_all, refused = 0, 0

if doc then
  local paper = gfx.surface{ w = 600, h = 850 }

  for i = 1, #doc.pages do
    local page = doc:page(i)
    local okr, drawn, _, missing = pcall(pdf.render, doc, page, paper, 1,
                                         0xff000000)
    print("RENDER", i, okr, drawn, missing)
    drawn_all = drawn_all + (okr and drawn or 0)
    refused = refused + (okr and missing or 1)
  end
end

print("DRAWN", drawn_all, nonspace, refused)
'''


def pdf_objects(data):
    """Every object of a PDF by its number, through its own cross-reference
    table - which is the check that the table is right: each offset must be
    where that object starts."""
    at = int(re.search(rb"startxref\s+(\d+)\s+%%EOF\s*$", data).group(1))

    if not data[at:].startswith(b"xref"):
        raise Failure("startxref does not point at the cross-reference table")

    head = re.match(rb"xref\n0 (\d+)\n", data[at:])
    count = int(head.group(1))
    rows = data[at + head.end():at + head.end() + 20 * count]
    objects = {}

    for n in range(1, count):
        offset = int(rows[20 * n:20 * n + 10])

        if not data[offset:].startswith(b"%d 0 obj" % n):
            raise Failure("the table says object %d is at %d, and it is not"
                          % (n, offset))

        objects[n] = data[offset:data.index(b"endobj", offset)]

    return objects


def stream_of(obj):
    """An object's stream, inflated when it says it is deflated."""
    start = obj.index(b"stream\n") + 7
    length = int(re.search(rb"/Length (\d+)", obj).group(1))
    raw = obj[start:start + length]

    if b"/FlateDecode" in obj[:start]:
        return zlib.decompress(raw)

    return raw


def ref(obj, key):
    """The object number a dictionary's `key` refers to."""
    return int(re.search(rb"/" + key + rb" \[?(\d+) 0 R", obj).group(1))


def ttf_tables(data):
    """A TrueType file's tables: tag -> (offset, length, checksum)."""
    count = struct.unpack(">H", data[4:6])[0]
    out = {}

    for i in range(count):
        tag = data[12 + 16 * i:16 + 16 * i].decode("latin-1")
        checksum, offset, length = struct.unpack(
            ">III", data[16 + 16 * i:28 + 16 * i])
        out[tag] = (offset, length, checksum)

    return out


def ttf_sum(data):
    data = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xffffffff


def outlines(data, tables):
    """Each glyph's outline bytes, through `loca`."""
    head_at = tables["head"][0]
    long_loca = struct.unpack(">h", data[head_at + 50:head_at + 52])[0] != 0
    count = struct.unpack(">H", data[tables["maxp"][0] + 4:
                                     tables["maxp"][0] + 6])[0]
    loca, glyf = tables["loca"][0], tables["glyf"][0]

    if long_loca:
        offsets = struct.unpack(">%dI" % (count + 1), data[loca:loca + 4 * (count + 1)])
    else:
        offsets = [2 * v for v in struct.unpack(">%dH" % (count + 1),
                                                data[loca:loca + 2 * (count + 1)])]

    return [data[glyf + offsets[g]:glyf + offsets[g + 1]] for g in range(count)]


def subset_problem(sub, original, shown):
    """What is wrong with `sub` as a subset of `original` showing `shown`,
    or None: the glyphs shown kept as they were, nothing else but the few a
    composite needs, the widths the same, every checksum right, and the
    tables a PDF reader never reads gone."""
    tables, whole = ttf_tables(sub), ttf_tables(original)

    for tag in ("cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"):
        if tag not in tables:
            return "it has no %s" % tag

    for tag in ("GPOS", "GSUB", "name", "DSIG"):
        if tag in tables:
            return "it still has %s" % tag

    for tag, (offset, length, checksum) in tables.items():
        body = bytearray(sub[offset:offset + length])

        if tag == "head":
            body[8:12] = b"\0\0\0\0"

        if ttf_sum(bytes(body)) != checksum:
            return "its %s's checksum is wrong" % tag

    zeroed = bytearray(sub)
    head = tables["head"][0]
    adjustment = struct.unpack(">I", sub[head + 8:head + 12])[0]
    zeroed[head + 8:head + 12] = b"\0\0\0\0"

    if (0xB1B0AFBA - ttf_sum(bytes(zeroed))) & 0xffffffff != adjustment:
        return "its checkSumAdjustment is wrong"

    if sub[head + 18:head + 20] != original[whole["head"][0] + 18:
                                            whole["head"][0] + 20]:
        return "its units to the em changed"

    o, l, _ = whole["hmtx"]
    so, sl, _ = tables["hmtx"]

    if sub[so:so + sl] != original[o:o + l]:
        return "its widths (hmtx) changed"

    mine, theirs = outlines(sub, tables), outlines(original, whole)

    if len(mine) != len(theirs):
        return "it has %d glyphs, and the font %d" % (len(mine), len(theirs))

    kept = 0

    for g, outline in enumerate(mine):
        if outline:
            kept += 1

            if outline[:len(theirs[g])] != theirs[g]:
                return "glyph %d's outline is not the font's" % g

        if (g in shown or g == 0) and len(outline) < len(theirs[g]):
            return "glyph %d is shown and its outline was dropped" % g

    if kept > 3 * (len(shown) + 1):
        return "it keeps %d outlines for %d glyphs shown" % (kept, len(shown))

    return None


def pdf_checks(said, out, fonts, disk, work):
    """W3: the PDF the machine wrote, read three ways."""
    checks = 0
    got = said("PDF")

    if not got or not got[-1].startswith("true "):
        raise Failure("the machine did not write a PDF: %r\n%s"
                      % (got, out[-900:]))

    _, pages, nfonts, missing, size = got[-1].split()
    pages, nfonts, missing = int(pages), int(nfonts), int(missing)

    if pages < 3 or missing != 1:
        raise Failure("the PDF has %d pages and %d characters its faces lack; "
                      "wanted three or more and one (the Japanese character)"
                      % (pages, missing))

    checks += 1

    # ---- Kosmos's own reader, inside the machine ----
    drawn = said("DRAWN")

    if said("READ") != [str(pages)] or not drawn:
        raise Failure("Kosmos's reader did not open the PDF it wrote: %r"
                      % said("READ"))

    drawn_all, nonspace, refused = (int(v) for v in drawn[-1].split())

    if refused != 0 or drawn_all != nonspace:
        raise Failure("Kosmos's reader drew %d glyphs of %d, with %d faces "
                      "refused: %r" % (drawn_all, nonspace, refused,
                                       said("RENDER")))

    checks += 1

    # ---- this file's reading of it ----
    path = os.path.join(work, "w.pdf")
    kfs("get", disk, "/Home/w.pdf", path)

    with open(path, "rb") as f:
        data = f.read()

    # `KEEP_PDF=file` keeps a copy, to be looked at by a person.
    if os.environ.get("KEEP_PDF"):
        shutil.copyfile(path, os.environ["KEEP_PDF"])

    if str(len(data)) != size or not data.startswith(b"%PDF-1.4\n"):
        raise Failure("the PDF taken off the disk is %d bytes, and the "
                      "machine said %s" % (len(data), size))

    objects = pdf_objects(data)
    checks += 1

    page_objs = [o for n, o in sorted(objects.items()) if b"/Type /Page " in o]

    if len(page_objs) != pages or any(
            b"/MediaBox [0 0 595.276 841.89]" not in o for o in page_objs):
        raise Failure("the PDF's pages are not %d A4 pages" % pages)

    # Each face: which file it is, its glyphs' widths, what each glyph is.
    by_ps = {fonts[f].ps_name: f for f in set(said("LOOKFACE"))}
    faces = {}

    for name, number in re.findall(rb"/F(\d+) (\d+) 0 R", page_objs[0]):
        top = objects[int(number)]

        if b"/Subtype /Type0" not in top or b"/Encoding /Identity-H" not in top:
            raise Failure("face F%s is not a Type 0 font in Identity-H"
                          % name.decode())

        tagged = re.search(rb"/BaseFont /(\S+)", top).group(1).decode()

        # A subset's name: six capitals, a plus, and the face's own.
        if not re.match(r"[A-Z]{6}\+", tagged):
            raise Failure("%s is not named as a subset is" % tagged)

        base = tagged[7:]

        if base not in by_ps:
            raise Failure("the PDF embeds %s, which is none of %r"
                          % (base, sorted(by_ps)))

        file = by_ps[base]
        font = fonts[file]
        cid = objects[ref(top, b"DescendantFonts")]
        cmap = stream_of(objects[ref(top, b"ToUnicode")]).decode("latin-1")
        chars = "".join(re.findall(r"beginbfchar\n(.*?)endbfchar", cmap,
                                   re.S))
        unicode = {int(g, 16): bytes.fromhex(u).decode("utf-16-be")
                   for g, u in re.findall(r"<([0-9A-F]{4})> <([0-9A-F]+)>",
                                          chars)}

        # The widths: each glyph's, the font's own advance.
        w = re.search(rb"/W \[(.*)\] /CIDToGIDMap", cid).group(1).decode()

        for first, run in re.findall(r"(\d+) \[([^\]]*)\]", w):
            for k, value in enumerate(run.split()):
                glyph = int(first) + k
                want = font.advances[min(glyph, font.hmetrics - 1)] * 1000 \
                    / font.units

                if value != ("%.3f" % want).rstrip("0").rstrip("."):
                    raise Failure("%s's width for glyph %d is %s in the PDF "
                                  "and %.3f in the font" % (base, glyph,
                                                            value, want))

        # The program, held to the font once the glyphs shown are known.
        program = objects[ref(objects[ref(cid, b"FontDescriptor")],
                              b"FontFile2")]
        subset = stream_of(program)

        if b"/Length1 %d" % len(subset) not in program:
            raise Failure("%s's program does not say its own length" % base)

        # Every glyph it maps is the glyph its font gives that character.
        for glyph, ch in unicode.items():
            if font.cmap.get(ord(ch[0])) != glyph:
                raise Failure("%s's ToUnicode says glyph %d is %r, and the "
                              "font does not" % (base, glyph, ch))

        faces[name.decode()] = (font, unicode, subset, file, base, set())

    if len(faces) != nfonts or nfonts != len(by_ps):
        raise Failure("the PDF has %d faces, the machine said %d and used %r"
                      % (len(faces), nfonts, sorted(by_ps)))

    checks += 1

    # Each piece of text, read back through its face's ToUnicode, where and
    # as the setting placed it - a character the face lacks is its missing
    # glyph, read as U+FFFD.
    shown, wanted = [], []

    for page_no, o in enumerate(page_objs, 1):
        ops = stream_of(objects[ref(o, b"Contents")]).decode("latin-1")

        for m in re.finditer(r"BT /F(\d+) [\d.]+ Tf [\d. ]+ rg (-?[\d.]+) "
                             r"(-?[\d.]+) Td (<[0-9A-F]*> Tj|\[[^\]]*\] TJ) ET",
                             ops):
            font, unicode, _, _, _, used = faces[m.group(1)]
            glyphs = "".join(re.findall(r"<([0-9A-F]*)>", m.group(4)))
            used.update(int(glyphs[i:i + 4], 16)
                        for i in range(0, len(glyphs), 4))
            text = "".join(unicode.get(int(glyphs[i:i + 4], 16), "�")
                           for i in range(0, len(glyphs), 4))
            shown.append((page_no, m.group(2), m.group(3), text, font))

    pieces = said("PIECE")

    if len(shown) != len(pieces):
        raise Failure("the PDF shows %d pieces of text and the setting "
                      "placed %d" % (len(shown), len(pieces)))

    for (page_no, x, y, text, font), piece in zip(shown, pieces):
        p, px, py, ptext = piece.split(" ", 3)
        ptext = ptext[1:-1]
        ptext = "".join(ch if ord(ch) in font.cmap else "�"
                        for ch in ptext)

        if (str(page_no), x, y, text) != (p, px, py, ptext):
            raise Failure("the PDF shows %r on page %d at %s %s, and the "
                          "setting placed %r on page %s at %s %s"
                          % (text, page_no, x, y, ptext, p, px, py))

    checks += 1

    # Each face's subset: what the pages show, and little else.
    for font, _, subset, file, base, used in faces.values():
        with open(os.path.join(FONTS, file), "rb") as f:
            problem = subset_problem(subset, f.read(), used)

        if problem:
            raise Failure("%s's subset of %s: %s" % (base, file, problem))

    if len(data) > 200 * 1024:
        raise Failure("the PDF is %d KB; its faces are not subsets"
                      % (len(data) // 1024))

    checks += 1

    # ---- and macOS's own renderer ----
    picture = os.path.join(work, "w.bmp")
    done = subprocess.run(["sips", "-s", "format", "bmp", path, "--out",
                           picture], capture_output=True, text=True)

    if done.returncode != 0 or not os.path.exists(picture):
        raise Failure("macOS would not render the PDF: " + done.stdout
                      + done.stderr)

    with open(picture, "rb") as f:
        bmp = f.read()

    offset = struct.unpack("<I", bmp[10:14])[0]
    width, height = struct.unpack("<ii", bmp[18:26])
    inked = sum(1 for i in range(offset + 3, offset + 4 * width * abs(height), 4)
                if bmp[i] > 128)

    if inked < 5000:
        raise Failure("macOS rendered the first page with %d inked pixels "
                      "of %d" % (inked, width * abs(height)))

    checks += 1
    return checks


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
                               "paragraph": lua_string(PARAGRAPH),
                               "title": lua_string(TITLE),
                               "accents": lua_string(ACCENTS)})

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

        # ---- the PDF reader's rasteriser sizes a face by its em ----
        if said("DOCFONT") != ["102"]:
            raise Failure("gfx.docfont draws IBM Plex Serif at 100 px with "
                          "an ascent of %r; a PDF's size is the em, so 102"
                          % said("DOCFONT"))

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

        checks += pdf_checks(said, out, fonts, disk, work)

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
