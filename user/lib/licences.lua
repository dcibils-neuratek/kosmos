-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- LICENSE, as the list it is: the project's own terms, then every vendored
-- component with where its licence lives.
--
-- **LICENSE is the only record, and this is how it is read.** The About
-- window reads the image's own copy - `sys.asset("LICENSE")` - rather than
-- carrying a list of its own, and `tools/test_licences.lua` reads the same
-- file on the build machine and checks that every vendored tree is named in
-- it. So the window cannot disagree with the file, and a library vendored
-- without an entry fails `make test` instead of going unmentioned, which is
-- how musl's maths and NetSurf's five libraries went unlisted.
--
-- The shape LICENSE keeps below its `---` rule:
--
--   two spaces     an entry: the component, who made it, its licence
--   six spaces     that entry's details, one line or several
--   the margin     a note between entries
--
-- A blank line ends whichever of those was open. Any other indentation is
-- reported as stray rather than guessed at, so a line that would have fallen
-- out of the window fails the test instead.

local M = {}

function M.parse(text)
  local found = { items = {}, entries = {}, stray = {} }
  local below = false
  local open = nil
  local number = 0

  for line in (text .. "\n"):gmatch("([^\n]*)\n") do
    number = number + 1
    line = (line:gsub("\r$", ""))

    if not below then
      -- Above the rule: the project's own licence, named on its first line,
      -- and the line that says whose it is.
      if line == "---" then
        below = true
      elseif not found.licence and line:match("%S") then
        found.licence = line:match("^%s*(.-)%s*$")
      elseif not found.holder and line:match("^Copyright") then
        found.holder = line
      end
    elseif not line:match("%S") then
      open = nil
    else
      local detail = line:match("^      (%S.*)$")
      local entry = line:match("^  (%S.*)$")
      local note = line:match("^(%S.*)$")

      if detail and open and open.title then
        open.details[#open.details + 1] = detail
      elseif entry then
        open = { title = entry, details = {} }
        found.items[#found.items + 1] = open
        found.entries[#found.entries + 1] = open
      elseif note then
        if open and open.note then
          open.note[#open.note + 1] = note
        else
          open = { note = { note } }
          found.items[#found.items + 1] = open
        end
      else
        found.stray[#found.stray + 1] = ("line %d: %s"):format(number, line)
      end
    end
  end

  return found
end

return M
