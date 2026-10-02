#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Faces off the disk, loaded the first time a character needs one.

Japanese, Korean and Chinese live in `/Home/Fonts` rather than in the image
(Diego, 1 October: "go with the fonts on disk, loaded when needed";
`roadmap.md` 6zz j5, `testing.md` 18.341). The machine boots with a disk
carrying two of the four - Korean and Japanese - and a program on it that
measures and draws, wrapping the loader the runtime gave it so it can count
what was asked:

  - text with nothing beyond Latin loads nothing;
  - Hangul loads the Korean face, kana the Japanese, and the Han ideographs
    are drawn from the Japanese one already in memory - each a real glyph,
    as wide as its face says, and not `?`;
  - told the page is Simplified Chinese, Han asks for the Simplified face
    and then the Traditional one, which this disk does not have, once
    each - and draws from the Japanese meanwhile, and asks nothing more;
  - four hundred different syllables measure as four hundred glyphs: the
    table a face keeps them in grows, where it stopped at a hundred and
    twenty-eight and drew the rest `?`;
  - a character drawn inks pixels a `?` does not;
  - bytes that are not a face are refused by name.

Usage: run_fonts.py IMAGE
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import fetch_fonts                                           # noqa: E402
import run_disk                                              # noqa: E402
import scratch                                               # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")

PROBE = r'''-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
local asked = {}
local base = gfx.font_loader()
print("LOADER", base ~= nil)
gfx.font_loader(function(file)
  asked[file] = (asked[file] or 0) + 1
  return base(file)
end)

local function asks()
  local t = {}
  for k, v in pairs(asked) do t[#t + 1] = k:match("^IBMPlexSans(%u%u)") .. "=" .. v end
  table.sort(t)
  return table.concat(t, ",")
end

local function loaded()
  local t = {}
  for _, f in ipairs(gfx.fallbacks()) do t[#t + 1] = f:match("^IBMPlexSans(%u%u)") or f end
  return table.concat(t, ",")
end

print("USE", gfx.use_font("ibmplexsans", 18))
local q = gfx.measure("?")
print("ASCII", gfx.measure("Kosmos"), "[" .. loaded() .. "]", "[" .. asks() .. "]")
print("HANGUL", gfx.measure("한국어"), gfx.measure("한"), q, loaded())
print("KANA", gfx.measure("ひらがな"), gfx.measure("ひ"), loaded())
print("HAN", gfx.measure("日本語"), gfx.measure("日"), loaded())
print("PREFER", gfx.font_prefer("zh-CN"), gfx.measure("中文"), gfx.measure("中"), asks())
print("AGAIN", gfx.measure("中文字"), asks())
gfx.font_prefer("ja")

local many = {}
for i = 0, 399 do many[#many + 1] = utf8.char(0xAC00 + i * 7) end
print("MANY", gfx.measure(table.concat(many)), gfx.measure("가") * 400)

local function ink(str)
  local s = gfx.surface{ w = 48, h = 32 }
  local n = 0
  s:fill(0, 0, 48, 32, 0xff000000)
  s:text(4, 4, str, 0xffffffff)
  for y = 0, 31 do
    for x = 0, 47 do
      if s:get(x, y) ~= 0xff000000 then n = n + 1 end
    end
  end
  s:free()
  return n
end

print("INK", ink("한"), ink("日"), ink("?"))
print("NOTFONT", gfx.font_fallback("bogus.ttf", sys.memory_map(sys.memory(1)), 4096))
print("DONE", asks())
'''


def field(out, tag):
    """The tab-separated values after `tag` on its line, or None."""
    for line in out.splitlines():
        if line.startswith(tag + "\t"):
            return line.split("\t")[1:]
    return None


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("fonts")

    fetch_fonts.main()

    probe = os.path.join(work, "fontprobe.lua")
    with open(probe, "w", encoding="utf-8") as f:
        f.write(PROBE)

    pairs = [probe + ":/Home/fontprobe.lua"]

    for name in ("IBMPlexSansKR-Regular.ttf", "IBMPlexSansJP-Regular.ttf"):
        pairs.append(os.path.join(fetch_fonts.OUT, name) + ":/Home/Fonts/" + name)

    disk = os.path.join(work, "fonts.img")
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, "32", *pairs],
                   check=True, capture_output=True, cwd=ROOT)

    out = run_disk.boot(image, disk, ["run /Home/fontprobe.lua"],
                        boot_timeout=120, each=120)

    checks = failures = 0

    def check(ok, what):
        nonlocal checks, failures
        checks += 1
        if not ok:
            failures += 1
            print(f"not ok {checks} - {what}")

    def num(tag, i):
        got = field(out, tag)
        try:
            return int(got[i])
        except (TypeError, ValueError, IndexError):
            return None

    check(field(out, "LOADER") == ["true"],
          "the runtime gave the program a loader: %r" % field(out, "LOADER"))

    ascii_ = field(out, "ASCII")
    check(ascii_ is not None and ascii_[1:] == ["[]", "[]"],
          f"Latin loads nothing and asks for nothing: {ascii_!r}")

    q = num("HANGUL", 2)
    han1 = num("HANGUL", 1)
    check(han1 is not None and q is not None and han1 != q
          and num("HANGUL", 0) == 3 * han1 and field(out, "HANGUL")[3] == "KR",
          f"Hangul from the Korean face, three syllables three glyphs: {field(out, 'HANGUL')!r}")

    kana = field(out, "KANA")
    check(kana is not None and num("KANA", 0) == 4 * num("KANA", 1)
          and num("KANA", 1) != q and kana[2] == "KR,JP",
          f"kana from the Japanese face: {kana!r}")

    han = field(out, "HAN")
    check(han is not None and num("HAN", 0) == 3 * num("HAN", 1)
          and num("HAN", 1) != q and han[2] == "KR,JP",
          f"Han from the Japanese face already loaded, nothing more read: {han!r}")

    prefer = field(out, "PREFER")
    check(prefer is not None and prefer[0] == "true"
          and num("PREFER", 1) == 2 * num("PREFER", 2) and num("PREFER", 2) == num("HAN", 1)
          and prefer[3] == "JP=1,KR=1,SC=1,TC=1",
          "told Simplified Chinese, Han asks for SC and then TC - neither on this "
          f"disk - once each, and draws from JP: {prefer!r}")

    again = field(out, "AGAIN")
    check(again is not None and again[1] == "JP=1,KR=1,SC=1,TC=1",
          f"a face that is not there is not asked for again: {again!r}")

    check(num("MANY", 0) is not None and num("MANY", 0) == num("MANY", 1)
          and num("MANY", 0) > 400 * q,
          "four hundred different syllables, four hundred glyphs - the table "
          f"grows past 128: {field(out, 'MANY')!r}, a ? is {q}")

    ink = field(out, "INK")
    check(ink is not None and num("INK", 0) > num("INK", 2) and num("INK", 1) > num("INK", 2)
          and num("INK", 2) > 0,
          f"drawn, a syllable and an ideograph ink more than a ?: {ink!r}")

    check(field(out, "NOTFONT") == ["nil", "not a TrueType face"],
          f"bytes that are not a face are refused: {field(out, 'NOTFONT')!r}")

    check("fonts: IBMPlexSansKR-Regular.ttf, 2533776 bytes, loaded" in out
          and "fonts: IBMPlexSansJP-Regular.ttf, 5707624 bytes, loaded" in out,
          "the runtime says what it loaded")

    if failures:
        print(out[-2500:])
        print(f"FAIL: {failures} of {checks} checks on faces off the disk "
              f"({run_disk.machine(image)}).")
        return 1

    print(f"PASS: {checks} checks on faces off the disk ({run_disk.machine(image)}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
