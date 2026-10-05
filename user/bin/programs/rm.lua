-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- rm: removes files, and directories with -r.
--
--   rm notes.txt            one file
--   rm a.txt b.txt c.txt    several
--   rm -r /Home/archive     a directory and everything under it
--
-- **The filesystem refuses a directory that is not empty**, and that refusal
-- is worth keeping rather than working around: it is the one thing standing
-- between a mistyped path and a subtree. So `-r` is not a flag the server
-- understands - it is this program agreeing to the walk, depth first, and
-- every step of it is a delete the filesystem would have allowed on its
-- own.
--
-- **It stops at the first failure.** Carrying on would mean a summary
-- nobody reads and a half-deleted tree nobody can describe; the reason one
-- delete failed is almost always the reason the next twenty will.

local files = use("/Kosmos/Libraries/files.lua")

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

-- The walk is `files.remove`'s - depth first, because a directory can only
-- go once it is empty, which is the filesystem's rule - the one the Trash
-- is emptied with. What is this program's is that it is asked for: a
-- directory without `-r` is refused here, before anything goes.
for _, name in ipairs(list) do
  local path = files.abs(name, cwd)
  local attrs = fs.getattr(path)
  local ok, said, before

  if not attrs then
    ok, said, before = nil, "no such file", 0
  elseif attrs.kind == "directory" and not recursive then
    ok, said, before = nil, "is a directory; use -r", 0
  else
    ok, said, before = files.remove(path)
  end

  if not ok then
    gone = gone + before
    print(("rm: %s: %s"):format(path, said))
    if gone > 0 then print(("removed %d before that"):format(gone)) end
    return
  end

  gone = gone + said
end

print(("removed %d"):format(gone))
