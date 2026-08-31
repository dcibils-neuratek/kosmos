-- cat: read a thing and print it.
--
--   cat /data/notes
--   cat /dev/cpu
--   cat /bin/hello.lua
--
-- The name is Linux's and the behaviour is not quite: what a server
-- returns here is a *value*, not a stream of bytes. `cat /data/sensor`
-- gives back the table that was written, with its numbers still numbers,
-- because `design.md` §1 makes the protocol between servers the data model
-- of the language. So this prints a table as a table and a string as a
-- string, and neither has been through a text encoding on the way.

local name = args:match("^%s*(%S+)")

-- Relative to where the caller was, which arrives with the request. The
-- working directory is the shell's idea; a server is always told a whole
-- path and knows nothing about it.
local path = name and (name:sub(1, 1) == "/" and name
                       or ((cwd == "/" and "/" or cwd .. "/") .. name))

if not path then
  print("usage: cat <path>")
  print("  try /data/notes, /dev/cpu, or /bin/hello.lua")
  return
end

-- Printed rather than returned, so that nested tables read as nested.
local function show(value, indent)
  indent = indent or ""

  if type(value) ~= "table" then
    return tostring(value)
  end

  local keys = {}
  for k in pairs(value) do keys[#keys + 1] = k end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)

  local parts = {}
  for _, k in ipairs(keys) do
    parts[#parts + 1] = ("%s  %s = %s"):format(indent, tostring(k),
                                               show(value[k], indent .. "  "))
  end

  return "{\n" .. table.concat(parts, "\n") .. "\n" .. indent .. "}"
end

local value, err = fs.read(path)

if value == nil then
  print("cat: " .. path .. ": " .. tostring(err))
  return
end

print(show(value))
