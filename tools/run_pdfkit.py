#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The PDF Kit's three additions for the reader (`docs/pdfview.md` step 1).

A PDF made here by `tools/mkpdf.py`, with a ToUnicode map, an outline of
two chapters on two pages, and a JPEG drawn on page 1 at a known place -
the four solid quadrants of `assets/images/test-quads.jpg`. In the guest,
through the kit's door alone:

  - `pdf.text`: page 1's text holds its sentences, in reading order, and
    each run says where it is;
  - `pdf.outline`: the two chapters, titled, on pages 1 and 2;
  - `pdf.render`: the picture is on the page, each quadrant its colour
    where the page puts it.

And the same three on a PDF made without each addition, as controls: no
map is no text (the codes are glyph ids, not characters), no outline
is no entries, no picture is paper where the picture would be.

Usage: run_pdfkit.py IMAGE
"""

import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402
import run_servers as S                                     # noqa: E402

SENTENCES = ["The quick brown fox jumps over the lazy dog.",
             "Pack my box with five dozen liquor jugs."]
LATER = "Sphinx of black quartz, judge my vow."

# Page 1: a chapter and two sentences, then blank lines to the end of the
# page - the picture sits 400 to 600 points up, where lines 10 to 23 would
# be - so the second chapter begins page 2.
TEXT = ("# The first chapter\n" + "\n".join(SENTENCES) + "\n" + "\n" * 43
        + "# The second chapter\n" + LATER + "\n")

# Where each quadrant's middle lands at scale 1, in points from the top:
# the picture is 200 square with its corner at (72, 400) from the bottom.
QUADS = [("red",   122, 242, 0xd02828),
         ("green", 222, 242, 0x28b43c),
         ("blue",  122, 342, 0x3246c8),
         ("white", 222, 342, 0xf0f0f0)]

PAPER = 0x808080          # neither white nor any quadrant: paper is paper

PROBE = r'''
local pdf = use("/Kosmos/Libraries/pdf.lua")
local path = args
local ok, doc = pcall(pdf.open, path)
if not ok then print("PDFKIT open failed " .. tostring(doc)) return end

local p1 = doc:page(1)
local ok1, t = pcall(pdf.text, doc, p1)
if not ok1 then print("PDFKIT text failed " .. tostring(t)) else
  print("PDFKIT runs " .. #t.runs)
  local r = t.runs[1]
  if r then print(("PDFKIT run1 %d %d %d %d"):format(r.x // 1, r.y // 1, r.w // 1, r.size // 1)) end
  print("PDFKIT text<<" .. t.text:gsub("\n", "|") .. ">>")
end

local ok2, out = pcall(pdf.outline, doc)
if not ok2 then print("PDFKIT outline failed " .. tostring(out)) else
  print("PDFKIT entries " .. #out)
  for _, e in ipairs(out) do print(("PDFKIT entry %s|%d|%d"):format(e.title, e.page or 0, e.depth or 0)) end
end

local s = gfx.surface{ w = 612, h = 792 }
s:fill(0, 0, 612, 792, 0xff808080)
local ok3, why = pcall(pdf.render, doc, p1, s, 1, 0xff000000)
if not ok3 then print("PDFKIT render failed " .. tostring(why)) end
print("PDFKIT pictures " .. tostring(doc.pictures_drawn or 0))
for _, at in ipairs({ {122, 242}, {222, 242}, {122, 342}, {222, 342} }) do
  print(("PDFKIT pixel %d %d %06x"):format(at[1], at[2], s:get(at[1], at[2]) & 0xffffff))
end
s:free()
print("PDFKIT done")
'''


def make(work, name, flags):
    text = os.path.join(work, "kit.txt")
    with open(text, "w") as f:
        f.write(TEXT)
    out = os.path.join(work, name)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpdf.py"), text, out] + flags,
                   check=True, capture_output=True, cwd=ROOT)
    with open(out, "rb") as f:
        return f.read()


def near(got, want, by=24):
    return all(abs(((got >> s) & 0xff) - ((want >> s) & 0xff)) <= by for s in (16, 8, 0))


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    work = scratch.directory("pdfkit")

    full = make(work, "full.pdf", ["--tounicode", "--outline",
                                   "--jpeg", "assets/images/test-quads.jpg"])
    bare = make(work, "bare.pdf", [])

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    checks, fails, said = 0, [], {}

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(complaint)

    def probe(path):
        out = session.run("/Temporary/probe.lua " + path).decode(errors="replace")
        said[path] = out
        return out

    try:
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(PROBE.encode(), "/Temporary/probe.lua")
        session.put(full, "/Temporary/full.pdf")
        session.put(bare, "/Temporary/bare.pdf")

        out = probe("/Temporary/full.pdf")
        check("PDFKIT done" in out, "the probe did not finish on the full PDF")

        # The text, in reading order, each sentence whole.
        m = re.search(r"PDFKIT text<<(.*?)>>", out)
        text = m.group(1) if m else ""
        for s in SENTENCES:
            check(s in text, "page 1's text does not hold %r: %r" % (s, text[:300]))
        check(text.find(SENTENCES[0]) < text.find(SENTENCES[1]),
              "page 1's sentences are out of order: %r" % text[:300])
        check("The first chapter" in text and LATER not in text,
              "page 1's text is not page 1's: %r" % text[:300])

        # And where: the first run is the heading, at the top left of the page.
        m = re.search(r"PDFKIT run1 (-?\d+) (-?\d+) (\d+) (\d+)", out)
        check(m is not None and 30 <= int(m.group(1)) <= 100 and 0 <= int(m.group(2)) <= 120
              and int(m.group(3)) > 50 and int(m.group(4)) >= 8,
              "the first run is not at the top left: %r" % (m and m.group(0)))

        # The outline.
        entries = re.findall(r"PDFKIT entry ([^|\r\n]*)\|(\d+)\|(\d+)", out)
        check(entries == [("The first chapter", "1", "0"), ("The second chapter", "2", "0")],
              "the outline is %r" % entries)

        # The picture, each quadrant where the page puts it.
        check(re.search(r"PDFKIT pictures 1\b", out) is not None, "no picture was drawn")
        pixels = re.findall(r"PDFKIT pixel (\d+) (\d+) ([0-9a-f]{6})", out)
        for (name, x, y, want), got in zip(QUADS, pixels or [None] * 4):
            check(got is not None and near(int(got[2], 16), want),
                  "the picture's %s quadrant at (%d, %d) is %s" % (name, x, y, got and got[2]))

        # The controls: the same document without each addition.
        out = probe("/Temporary/bare.pdf")
        check("PDFKIT done" in out, "the probe did not finish on the bare PDF")
        m = re.search(r"PDFKIT text<<(.*?)>>", out)
        bare_text = m.group(1) if m else ""
        # Identity-H codes are glyph ids, not characters: without the map
        # the page says nothing rather than something wrong.
        check(bare_text == "",
              "without a ToUnicode map there was text - the map is not what finds it: %r"
              % bare_text[:200])
        check(re.search(r"PDFKIT entries 0\b", out) is not None,
              "without an outline there were entries")
        pixels = re.findall(r"PDFKIT pixel \d+ \d+ ([0-9a-f]{6})", out)
        check(len(pixels) == 4 and all(int(p, 16) == PAPER for p in pixels),
              "without a picture the page is not paper there: %r" % pixels)
    except Exception as e:                  # noqa: BLE001 - said below
        check(False, "the steps stopped: %s: %s" % (type(e).__name__, str(e)[:500]))
    finally:
        transcript = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on the PDF Kit's text, outline and pictures:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        for k, v in said.items():
            print("--- %s ---\n%s" % (k, v[-2000:]))
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    print("PASS: %d checks on the PDF Kit (a page's text in reading order with where it is, "
          "the outline's two chapters and their pages, a JPEG drawn where the page puts it; "
          "and without a map, an outline or a picture, none of them)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
