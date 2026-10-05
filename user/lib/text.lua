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
function text.count_and_path(args, fallback)
  local n = tonumber(tostring(args or ""):match("%-n%s+(%d+)")) or fallback
  local rest = tostring(args or ""):gsub("%-n%s+%d+", "")

  return n, rest:match("^%s*(%S+)")
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
