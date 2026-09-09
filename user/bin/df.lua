-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- df: how much is in each mount, and how much room is left.
--
--   df
--
-- **Measured by asking, not by a table of what the mounts are.** Every
-- mount this process holds gets walked and its sizes added up, so a mount
-- added tomorrow appears here without this program being edited - the same
-- reason Tracker builds its sidebar from `fs.mounts()`.
--
-- What "free" means differs by mount and only one of them can answer it.
-- The disk keeps a superblock with a block count and a free count, and
-- that is a real number. `/ramfs` is a fixed pool of nodes decided at
-- compile time. `/bin` and `/lib` are in the image and cannot grow at all.
-- Rather than invent a total for each, this prints what each one is able
-- to say and leaves the rest blank, which is the honest shape.
--
-- `diskinfo` is the long answer about the disk alone, including the
-- geometry and where the journal sits.

local files = use("/lib/files.lua")

local MAX_DEPTH = 12

local function measure(path, depth)
  local attrs = fs.getattr(path)

  if not attrs then return 0, 0 end

  if attrs.kind ~= "directory" then
    return 1, tonumber(attrs.size) or 0
  end

  if depth >= MAX_DEPTH then return 1, 0 end

  local n, bytes = 1, 0

  for _, entry in ipairs(fs.list(path) or {}) do
    local sub_n, sub_bytes = measure(files.join(path, entry), depth + 1)

    n = n + sub_n
    bytes = bytes + sub_bytes
  end

  return n, bytes
end

local all = fs.mounts()
local storage = {}

--
-- Which mounts are storage at all, decided by asking each one to list.
--
-- **Nothing below may touch a mount that is not in here**, and that is not
-- tidiness. `/net` speaks a fixed protocol, and a `read` aimed at it goes
-- through the generic path - a Lua table sent to a server that expects a
-- struct, which is the exact mistake `ns.send` refuses by hand. A listing
-- is the cheap question that tells the two apart.
--
for _, prefix in ipairs(all) do
  local nested = false

  for _, other in ipairs(all) do
    if other ~= prefix and prefix:sub(1, #other + 1) == other .. "/" then
      nested = true
      break
    end
  end

  if not nested and fs.list(prefix) then
    storage[#storage + 1] = prefix
  end
end

print(("%-14s %8s %10s"):format("mount", "entries", "bytes"))

for _, prefix in ipairs(storage) do
  local n, bytes = measure(prefix, 0)

  print(("%-14s %8d %10s"):format(prefix, n - 1, files.size(bytes)))
end

-- And the one mount that keeps a real total. Read through whichever disk
-- mount answers, because /home, /system and /user are three views of one
-- filesystem.
local sb

for _, prefix in ipairs(storage) do
  local maybe = fs.read(prefix .. "/.super")

  if type(maybe) == "table" and maybe.present then
    sb = maybe
    break
  end
end

print("")

if sb and sb.formatted then
  local used = sb.blocks - sb.free_blocks

  print(("disk: %s used of %s, %d blocks free of %d")
        :format(files.size(used * sb.block_size),
                files.size(sb.blocks * sb.block_size),
                sb.free_blocks, sb.blocks))
else
  print("disk: none formatted; /home is in memory and will not survive")
end
