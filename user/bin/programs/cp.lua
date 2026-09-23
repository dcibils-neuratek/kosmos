-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- cp: copies a file.
--
--   cp notes.txt notes.bak      beside it
--   cp notes.txt /home/archive  into a directory
--
-- A directory as the destination means "into it", which is the one piece of
-- shell behaviour worth keeping: `cp a /home/archive` and
-- `cp a /home/archive/a` name the same result, and not having to type the
-- name twice is most of what a shell is for.
--
-- No `-r`, and the refusal is `files.copy`'s rather than this program's. A
-- recursive copy has to decide what a half-finished one leaves behind, and
-- that is the same question `mkdir -p` is refused for: the honest answers
-- are "keep what was made" or "undo it", and the second is a transaction.

local files = use("/lib/files.lua")

local a, b = args:match("^%s*(%S+)%s+(%S+)")

if not a or not b then
  print("cp: cp <from> <to>")
  return
end

local from = files.abs(a, cwd)
local to   = files.abs(b, cwd)

local there = fs.getattr(to)

if there and there.kind == "directory" then
  to = files.join(to, from:match("([^/]+)$") or from)
end

local ok, err = files.copy(from, to)

if not ok then
  print("cp: " .. tostring(err))
  return
end

print("copied to " .. to)
