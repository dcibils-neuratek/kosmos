-- ls: what is under a path.
--
--   ls            where you are
--   ls /bin       somewhere else
--
-- A listing is what the server said plus whatever is mounted below the
-- path, and only the namespace knows the second half - which is what makes
-- `/` a directory at all. Nothing is mounted there, so no server answers
-- for it, and it is a directory made entirely of mount points.

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

for _, entry in ipairs(entries) do
  local child = (path == "/" and "/" or path .. "/") .. entry
  local attrs = fs.getattr(child)
  local size = attrs and attrs.size

  print(("  %-16s %s"):format(entry, size and (size .. " bytes") or ""))
end
