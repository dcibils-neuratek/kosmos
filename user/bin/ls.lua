-- ls: what is under a path.
--
--   ls            where you are
--   ls /bin       somewhere else
--
-- A listing is what the server said plus whatever is mounted below the
-- path, and only the namespace knows the second half - which is what makes
-- `/` a directory at all. Nothing is mounted there, so no server answers
-- for it, and it is a directory made entirely of mount points.

local types = use("/lib/filetypes.lua")

local name = args:match("^%s*(%S+)")
local path = name and (name:sub(1, 1) == "/" and name
                       or ((cwd == "/" and "/" or cwd .. "/") .. name))
                  or cwd

local entries, err = fs.list(path)

if not entries then
  print("ls: " .. path .. ": " .. tostring(err))
  return
end

if #entries == 0 then
  print("(empty)")
  return
end

-- Built whole, then printed once.
--
-- Printing a line at a time is a round trip per line: `print` reaches the
-- console server over IPC, and when the console is a Terminal window that
-- window has to wake to take it. A listing of thirty files was thirty of
-- those, which is what made `ls` arrive one line at a time slowly enough
-- to watch.
--
-- The `getattr` per entry is still a round trip each, and that one is
-- inherent to the protocol as it stands: `list` returns names, and the size
-- and kind live on the node. Making `list` able to answer with attributes
-- is the real fix and belongs in the protocol rather than here.
local out = {}

for _, entry in ipairs(entries) do
  local child = (path == "/" and "/" or path .. "/") .. entry
  local attrs = fs.getattr(child)

  -- A directory says so rather than reporting the size of the entries it
  -- happens to hold. That number is true and it is not what anybody asking
  -- means, which is the definition of a misleading answer.
  -- Size and kind in their own columns, the same two Tracker shows and
  -- from the same table - `/lib/filetypes.lua`. Two programs answering
  -- "what is this" differently is the thing that table exists to stop.
  local size, kind

  if attrs and attrs.kind == "directory" then
    size, kind = "--", "folder"
  else
    size = attrs and attrs.size and tostring(attrs.size) or ""
    kind = types.kind_of(entry) or "file"
  end

  out[#out + 1] = ("  %-18s %8s  %s"):format(entry, size, kind)
end

print(table.concat(out, "\n"))
