-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- rm: removes files, and directories with -r.
--
--   rm notes.txt            one file
--   rm a.txt b.txt c.txt    several
--   rm -r /home/archive     a directory and everything under it
--
-- **The filesystem refuses a directory that is not empty**, and that refusal
-- is worth keeping rather than working around: it is the one thing standing
-- between a mistyped path and a subtree. So `-r` is not a flag the server
-- understands - it is this program agreeing to do the walk, depth first,
-- and every step of it is a delete the filesystem would have allowed on its
-- own.
--
-- **It stops at the first failure.** Carrying on would mean a summary
-- nobody reads and a half-deleted tree nobody can describe; the reason one
-- delete failed is almost always the reason the next twenty will.

local files = use("/lib/files.lua")

local recursive = false
local list = {}

for word in args:gmatch("%S+") do
  if word == "-r" or word == "-rf" then
    recursive = true
  else
    list[#list + 1] = word
  end
end

if #list == 0 then
  print("rm: rm [-r] <path>...")
  return
end

local gone = 0

-- Depth first, because a directory can only go once it is empty - which is
-- the filesystem's rule and the reason this walk exists at all.
local function remove(path)
  local attrs = fs.getattr(path)

  if not attrs then return nil, "no such file" end

  if attrs.kind == "directory" then
    if not recursive then
      return nil, "is a directory; use -r"
    end

    for _, e in ipairs(fs.list(path) or {}) do
      local ok, why = remove(files.join(path, e))

      if not ok then return nil, why end
    end
  end

  local ok, why = fs.send(path, { type = "delete" })

  if not ok then return nil, tostring(why) end

  gone = gone + 1
  return true
end

for _, name in ipairs(list) do
  local path = files.abs(name, cwd)
  local ok, why = remove(path)

  if not ok then
    print(("rm: %s: %s"):format(path, why))
    if gone > 0 then print(("removed %d before that"):format(gone)) end
    return
  end
end

print(("removed %d"):format(gone))
