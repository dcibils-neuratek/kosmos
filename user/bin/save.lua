-- Writes a file to the disk, and reads it back to prove it is there.
--
--   save notes.txt Hello from before the reboot
--
-- The smallest thing that demonstrates a filesystem: a name, some bytes,
-- and both still present after the machine is turned off and on. `ls /home`
-- lists what is there, `cat /home/<name>` prints one.

local name, rest = args:match("^%s*(%S+)%s*(.*)$")

if not name then
  print("save: save <name> <text>")
  return
end

local ok, err = fs.write("/home/" .. name, rest)

if not ok then
  print("save: " .. tostring(err))
  return
end

local back, rerr = fs.read("/home/" .. name)

if not back then
  print("save: written, but could not be read back: " .. tostring(rerr))
  return
end

local attrs = fs.getattr("/home/" .. name)

print(("saved %s: %d bytes, %d extent(s)")
      :format(name, attrs and attrs.size or #back,
              attrs and attrs.extents or 0))
print(("read back: %s"):format(back))
