-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- tree: what is under a path, and under that.
--
--   tree            where you are
--   tree /ramfs     somewhere else
--
-- `ls` shows one level and this shows all of them, which on a machine whose
-- root is a list of mounts is the fastest way to see what there actually is.
--
-- **Depth is capped**, because a namespace is not guaranteed to be a tree:
-- one disk is mounted at three places here, and a mount inside itself would
-- walk for as long as you let it. Twelve is deeper than anything real and
-- shallow enough to end.

local files = use("/lib/files.lua")

local MAX_DEPTH = 12

local name = args:match("^%s*(%S+)")
local root = files.abs(name, cwd)

local dirs, shown = 0, 0

local function walk(path, prefix, depth)
  local entries, err = fs.list(path)

  if not entries then
    print(prefix .. "(" .. tostring(err) .. ")")
    return
  end

  table.sort(entries)

  for i, entry in ipairs(entries) do
    local last = (i == #entries)
    local full = files.join(path, entry)
    local attrs = fs.getattr(full)
    local folder = attrs and attrs.kind == "directory"

    print(prefix .. (last and "`-- " or "|-- ") .. entry
          .. (folder and "/" or ""))

    shown = shown + 1

    if folder then
      dirs = dirs + 1

      if depth < MAX_DEPTH then
        walk(full, prefix .. (last and "    " or "|   "), depth + 1)
      else
        print(prefix .. (last and "    " or "|   ") .. "...")
      end
    end
  end
end

print(root)
walk(root, "", 1)
print(("%d directories, %d entries"):format(dirs, shown))
