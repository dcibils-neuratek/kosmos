-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Splitting text into lines, and the argument shape three programs share.
--
--   local text = use("/lib/text.lua")
--
-- Small on purpose. `head`, `tail`, `wc` and `grep` all had the same two
--questions to solve - where do lines end, and how is `-n 3` spelled - and three
-- copies of an answer is how two of them end up disagreeing about a file
-- with no final newline.

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

return text
