-- Makes a directory.
--
--   mkdir /home/notes
--
-- One component at a time and no `-p`: making a whole path at once has to
-- decide what to do when it fails halfway, and the honest answers are
-- "leave the ones it made" or "undo them", both of which are a transaction
-- and neither of which belongs here before there is a journal.

local path = args:match("^%s*(%S+)")

if not path then
  print("mkdir: mkdir <path>")
  return
end

local ok, err = fs.send(path, { type = "mkdir" })

if not ok then
  print("mkdir: " .. tostring(err))
  return
end

print("made " .. path)
