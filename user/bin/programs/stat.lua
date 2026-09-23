-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- stat: what a node is, in one answer.
--
--   stat notes.txt
--   stat /bin
--
-- `getattr` is one round trip and returns everything the server is willing
-- to say, so this is that reply laid out rather than a series of questions.
--
-- **`attr` is the other half and they are not the same command.** That one
-- is for the attributes *you* put on a file - `kind=note`, `author=diego` -
-- and can set them. This one is for what the filesystem knows on its own:
-- whether it is a directory, how many bytes, and where they sit on the
-- disk. Both come back in the same table, which is why the split is worth
-- naming: everything below `attributes` here was written by somebody.

local files = use("/lib/files.lua")

local name = args:match("^%s*(%S+)")

if not name then
  print("stat: stat <path>")
  return
end

local path = files.abs(name, cwd)
local attrs, err = fs.getattr(path)

if not attrs then
  print("stat: " .. path .. ": " .. tostring(err))
  return
end

print(path)
print(("  kind      %s"):format(tostring(attrs.kind or "file")))

if attrs.size then
  print(("  size      %s (%d bytes)")
        :format(files.size(tonumber(attrs.size) or 0),
                tonumber(attrs.size) or 0))
end

-- Where the bytes are, which only a disk has an answer for. A file in
-- memory has no extents and says nothing rather than saying zero.
if attrs.extents then
  print(("  extents   %s"):format(tostring(attrs.extents)))
end

local rest = {}

for k, v in pairs(attrs) do
  if k ~= "kind" and k ~= "size" and k ~= "extents" then
    rest[#rest + 1] = ("    %s = %s"):format(tostring(k), tostring(v))
  end
end

table.sort(rest)

if #rest > 0 then
  print("  attributes")
  for _, line in ipairs(rest) do print(line) end
end
