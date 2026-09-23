-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Makes a directory.
--
--   mkdir /home/notes
--
-- One component at a time and no `-p`: making a whole path at once has to
-- decide what to do when it fails halfway, and the honest answers are
-- "leave the ones it made" or "undo them", both of which are a transaction
-- and neither of which belongs here before there is a journal.

local files = use("/lib/files.lua")

local name = args:match("^%s*(%S+)")

if not name then
  print("mkdir: mkdir <path>")
  return
end

-- Against where you are, which it did not do: `cd /ramfs` then `mkdir box`
-- asked for `box` and was told there is no such path, because a name with
-- no slash in it is not a path at all until somebody says where from.
local path = files.abs(name, cwd)

local ok, err = fs.send(path, { type = "mkdir" })

if not ok then
  print("mkdir: " .. tostring(err))
  return
end

print("made " .. path)
