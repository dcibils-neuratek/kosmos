-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Writes a file to the disk, and reads it back to prove it is there.
--
--   save notes.txt Hello from before the reboot
--
-- The smallest thing that demonstrates a filesystem: a name, some bytes,
-- and both still present after the machine is turned off and on. `ls /Home`
-- lists what is there, `cat /Home/<name>` prints one.

local said, rest = use("/Kosmos/Libraries/files.lua").words(args, 1)
local name = said[1]

if not name then
  print("save: save <name> <text>")
  return
end

local ok, err = fs.write("/Home/" .. name, rest)

if not ok then
  print("save: " .. tostring(err))
  return
end

local back, rerr = fs.read("/Home/" .. name)

if not back then
  print("save: written, but could not be read back: " .. tostring(rerr))
  return
end

local attrs = fs.getattr("/Home/" .. name)

print(("saved %s: %d bytes, %d extent(s)")
      :format(name, attrs and attrs.size or #back,
              attrs and attrs.extents or 0))
print(("read back: %s"):format(back))
