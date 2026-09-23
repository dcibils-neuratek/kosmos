-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- head: the first lines of a file.
--
--   head notes.txt
--   head -n 3 notes.txt
--
-- Ten by default, which is the number every other system chose and there is
-- no reason to disagree with them about it.
--
-- A read here returns a *value*, not a stream of bytes - `/ramfs` gives back
-- the table that was written - so a thing with no lines in it says so rather
-- than being turned into text nobody wrote. `cat` prints those.

local files = use("/lib/files.lua")
local text = use("/lib/text.lua")

local n, name = text.count_and_path(args, 10)

if not name then
  print("head: head [-n <count>] <path>")
  return
end

local path = files.abs(name, cwd)
local body, err = fs.read(path)

if body == nil then
  print("head: " .. path .. ": " .. tostring(err))
  return
end

if type(body) ~= "string" then
  print("head: " .. path .. " holds a " .. type(body) .. "; try `cat`")
  return
end

local lines = text.lines(body)

for i = 1, math.min(n, #lines) do print(lines[i]) end
