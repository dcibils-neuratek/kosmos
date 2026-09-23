-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- touch: makes an empty file.
--
--   touch notes.txt
--
-- **Only that.** On a Unix it also moves a timestamp forward, and this one
-- does not, because a file here has no modification time to move - `stat`
-- will tell you what a file does have. Saying so is better than quietly
-- doing half of what the name promises somewhere else.
--
-- An existing file is left exactly as it is rather than emptied, which is
-- the one thing `touch` must never do.

local files = use("/lib/files.lua")

local name = args:match("^%s*(%S+)")

if not name then
  print("touch: touch <path>")
  return
end

local path = files.abs(name, cwd)

if fs.getattr(path) then
  print(path .. " is already there")
  return
end

local ok, err = fs.write(path, "")

if not ok then
  print("touch: " .. tostring(err))
  return
end

print("made " .. path)
