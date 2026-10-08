-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Splitting text into lines, and the argument shape three programs share.
--
--   local text = use("/Kosmos/Libraries/text.lua")
--
-- Small on purpose. `head`, `tail`, `wc` and `grep` all had the same two
--questions to solve - where do lines end, and how is `-n 3` spelled - and three
-- copies of an answer is how two of them end up disagreeing about a file
-- with no final newline.
--
-- And a word in capitals that keeps its accents (`text.upper`), which
-- Lua's own `upper` cannot do: it knows ASCII and leaves "ñ" as it is.
--
-- And two ways a number is written for a person to read, which programs and
-- windows had each written out again: its digits grouped in thousands, and
-- a share as a bar of characters.

local text = {}

--
-- A file as a list of lines.
--
-- **A trailing newline does not make an empty last line.** "a\nb\n" is two
-- lines and not three, which is what every counting tool means by it and
-- what `wc -l` reports. A file with no final newline still has its last
-- line, which is the other half of the same rule.
--
--
-- Capitals, in UTF-8: "Cordón" is "CORDÓN", not "CORDóN". ASCII, Latin-1's
-- letters, Latin Extended-A, Greek and Cyrillic - the scripts the system's
-- faces draw and a map's names are written in (Maps, 8 October, where
-- Montevideo's neighbourhoods came out half capitals). A character with no
-- capital, or in another script, is left as it is, and bytes that are not
-- UTF-8 are passed through rather than refused.
--
local function capital(c)
  if c >= 0x61 and c <= 0x7a then return c - 32 end
  if c < 0xe0 then return c end
  if c <= 0xfe then return c == 0xf7 and c or c - 32 end
  if c == 0xff then return 0x178 end

  if c >= 0x100 and c <= 0x17f then
    -- Pairs, the capital first, except the two runs where it is second.
    if (c >= 0x139 and c <= 0x148) or (c >= 0x179 and c <= 0x17e) then
      return c % 2 == 0 and c - 1 or c
    end

    if c == 0x131 or c == 0x138 or c == 0x149 or c == 0x17f then return c end

    return c % 2 == 1 and c - 1 or c
  end

  if c >= 0x3b1 and c <= 0x3c9 then return c == 0x3c2 and 0x3a3 or c - 32 end

  -- Greek's vowels with their accent, which sit apart from the rest.
  if c == 0x3ac then return 0x386 end
  if c >= 0x3ad and c <= 0x3af then return c - 37 end
  if c == 0x3cc then return 0x38c end
  if c == 0x3cd or c == 0x3ce then return c - 63 end
  if c >= 0x430 and c <= 0x44f then return c - 32 end
  if c >= 0x450 and c <= 0x45f then return c - 80 end

  return c
end

function text.upper(s)
  s = tostring(s)

  if not utf8.len(s) then return s:upper() end

  local out = {}

  for _, c in utf8.codes(s) do out[#out + 1] = utf8.char(capital(c)) end

  return table.concat(out)
end

function text.lines(body)
  local out = {}

  for line in tostring(body):gmatch("([^\n]*)\n?") do
    out[#out + 1] = line
  end

  -- `gmatch` with an optional newline yields one empty match past the end.
  if out[#out] == "" then out[#out] = nil end

  return out
end

--
-- `-n <count>` and a path, in either order, out of the one argument string.
--
-- A program here is handed `args` as a *string*, not a list, so every one of
-- them parses. This is that parse in one place, with the default the caller
-- names: `head` and `tail` both want ten and `grep` wants none of this.
--
-- Read with `files.words`, as every program reads its words, so a path
-- with a space in it is one when it is quoted (`testing.md` 18.414).
--
function text.count_and_path(args, fallback)
  local said = use("/Kosmos/Libraries/files.lua").words(args)
  local n, path = fallback, nil
  local i = 1

  while i <= #said do
    if said[i] == "-n" and tostring(said[i + 1]):match("^%d+$") then
      n, i = tonumber(said[i + 1]), i + 2
    else
      path = path or said[i]
      i = i + 1
    end
  end

  return n, path
end

--
-- **A whole number with its thousands marked**, as the drawings write a
-- count: 1,204 files, 2,007,961,344 bytes. Tracker, Info, Machine and
-- Solar System each grouped the digits themselves, two of them by
-- reversing the string. Rounded down to a whole one first; a sign kept.
--
function text.grouped(n)
  local whole = math.floor(tonumber(n) or 0)
  local sign, digits = tostring(whole):match("^(-?)(%d+)$")
  local more

  -- Past what an integer holds, or not a number at all: as Lua writes it.
  if not digits then return tostring(whole) end

  repeat
    digits, more = digits:gsub("^(%d+)(%d%d%d)", "%1,%2")
  until more == 0

  return sign .. digits
end

--
-- **A bar of characters for a share**, as a program at the prompt draws
-- one: `[||||......]`, `pct` of 100 filled across `width`. `monitor` and
-- `htop` each had this.
--
function text.meter(pct, width)
  local filled = math.max(0, math.min(width, (pct * width) // 100))

  return "[" .. ("|"):rep(filled) .. ("."):rep(width - filled) .. "]"
end

return text
