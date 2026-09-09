-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- tail: the last lines of a file.
--
--   tail notes.txt
--   tail -n 3 notes.txt
--
-- The whole file is read to find its end, and that is honest rather than
-- clever: a server here answers with a value, so there is no seeking to the
-- end of one. On a file too big for that, the read fails and says so, which
-- is better than a `tail` that quietly showed the wrong lines.

local files = use("/lib/files.lua")
local text = use("/lib/text.lua")

local n, name = text.count_and_path(args, 10)

if not name then
  print("tail: tail [-n <count>] <path>")
  return
end

local path = files.abs(name, cwd)
local body, err = fs.read(path)

if body == nil then
  print("tail: " .. path .. ": " .. tostring(err))
  return
end

if type(body) ~= "string" then
  print("tail: " .. path .. " holds a " .. type(body) .. "; try `cat`")
  return
end

local lines = text.lines(body)

for i = math.max(1, #lines - n + 1), #lines do print(lines[i]) end
