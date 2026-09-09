-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- mv: moves or renames a file or a directory.
--
--   mv notes.txt kept.txt       renamed where it is
--   mv notes.txt /home/archive  moved into a directory
--
-- Rename and move are one operation, as they are on any filesystem worth
-- the name: the entry moves from one directory to another and the data does
-- not move at all. `files.move` does the work and refuses the two cases only
-- it can see - a directory into itself, and a destination that already
-- exists - because the filesystem does not know which answer is wanted and
-- guessing is how a move becomes a delete.

local files = use("/lib/files.lua")

local a, b = args:match("^%s*(%S+)%s+(%S+)")

if not a or not b then
  print("mv: mv <from> <to>")
  return
end

local from = files.abs(a, cwd)
local to   = files.abs(b, cwd)

local there = fs.getattr(to)

if there and there.kind == "directory" then
  to = files.join(to, from:match("([^/]+)$") or from)
end

local ok, err = files.move(from, to)

if not ok then
  print("mv: " .. tostring(err))
  return
end

print("moved to " .. to)
