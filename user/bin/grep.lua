-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- grep: the lines of a file that match.
--
--   grep deadline notes.txt
--   grep "^local" /bin/ls.lua
--
-- **The pattern is a Lua pattern, not a regular expression**, and that is
-- worth knowing before it surprises you: `.` matches any character as it
-- would elsewhere, but there is no `|`, no `+` after a group, and a literal
-- dot is `%.` rather than `\.`. It is the pattern language this whole system
-- is written in, and inventing a second one here so that the spelling
-- matched a different operating system would mean carrying a regex engine to
-- do it.
--
-- `find` is the other half of searching and looks at *attributes* - what a
-- file is, rather than what is in it.

local files = use("/lib/files.lua")
local text = use("/lib/text.lua")

local pattern, name = args:match("^%s*(%S+)%s+(%S+)")

if not pattern or not name then
  print("grep: grep <pattern> <path>")
  return
end

local path = files.abs(name, cwd)
local body, err = fs.read(path)

if body == nil then
  print("grep: " .. path .. ": " .. tostring(err))
  return
end

if type(body) ~= "string" then
  print("grep: " .. path .. " holds a " .. type(body) .. "; try `cat`")
  return
end

local found = 0

for i, line in ipairs(text.lines(body)) do
  -- Guarded, because a pattern is a thing somebody typed: an unfinished
  -- `%` or `[` raises out of `find` rather than returning nothing, and a
  -- program that dies on a typo is worse than one that says what was wrong.
  local ok, at = pcall(string.find, line, pattern)

  if not ok then
    print("grep: " .. tostring(at))
    return
  end

  if at then
    found = found + 1
    print(("%4d  %s"):format(i, line))
  end
end

if found == 0 then print("no lines match") end
