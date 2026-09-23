-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- du: how much is under a path.
--
--   du              where you are
--   du /home        somewhere else
--
-- One line per directory directly inside it, then the total - which is the
-- shape of the question people actually ask a `du`: not "how big is this
-- file" but "which of these is the big one".
--
-- **The size comes from the attribute a write maintains**, so a node that
-- has never been written as text has no size and counts as nothing. That is
-- the same number `ls` shows, and a `du` that disagreed with `ls` about the
-- same file would be worse than one that admits it only counts what it can
-- see.

local files = use("/lib/files.lua")

local MAX_DEPTH = 12

local name = args:match("^%s*(%S+)")
local root = files.abs(name, cwd)

local function total(path, depth)
  local attrs = fs.getattr(path)

  if not attrs then return 0 end

  if attrs.kind ~= "directory" then
    return tonumber(attrs.size) or 0
  end

  if depth >= MAX_DEPTH then return 0 end

  local sum = 0

  for _, entry in ipairs(fs.list(path) or {}) do
    sum = sum + total(files.join(path, entry), depth + 1)
  end

  return sum
end

local entries, err = fs.list(root)

if not entries then
  print("du: " .. root .. ": " .. tostring(err))
  return
end

table.sort(entries)

for _, entry in ipairs(entries) do
  local full = files.join(root, entry)
  local attrs = fs.getattr(full)

  if attrs and attrs.kind == "directory" then
    print(("%10s  %s/"):format(files.size(total(full, 1)), entry))
  end
end

print(("%10s  %s"):format(files.size(total(root, 0)), root))
