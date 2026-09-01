-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Everything whose attributes match.
--
--   find kind=note                  everywhere that can answer
--   find /home kind=note            just there
--   find kind=person city=Montevideo
--
-- The filesystem answers out of an index, so this costs the size of the
-- answer and not the size of the filesystem. That is the whole point of
-- attributes being indexed and it is the number bench/ measures.
--
-- **With no path it asks every mount**, and that is the design showing
-- through rather than a convenience. A query is a question in the same
-- protocol every server speaks, so "search everything" is a loop over the
-- mount table and not a feature anybody had to build. Servers that cannot
-- answer one - the program store, the device server - say so and are
-- skipped, which is also just the protocol working.
--
-- It used to ask `/data` and only `/data`, which was the ramfs. Once the
-- disk could answer queries as well, a hard-coded mount meant the files a
-- person actually keeps were the ones `find` could not see.

local where = {}
local n = 0
local root = nil

for word in tostring(args or ""):gmatch("%S+") do
  local name, value = word:match("^([^=]+)=(.*)$")

  if name then
    where[name] = tonumber(value) or value
    n = n + 1
  elseif word:sub(1, 1) == "/" and not root and n == 0 then
    root = word
  else
    print("usage: find [path] name=value [name=value ...]")
    return
  end
end

if n == 0 then
  print("usage: find name=value [name=value ...]")
  return
end

local function search(path)
  local paths, err = fs.query(path, where)

  return paths, err
end

local found  = {}
local asked  = 0
local refused = {}

if root then
  local paths, err = search(root)

  if not paths then
    print("find: " .. tostring(err))
    return
  end

  asked = 1
  for _, p in ipairs(paths) do found[#found + 1] = p end
else
  for _, mount in ipairs(fs.mounts()) do
    local paths, err = search(mount)

    if paths then
      asked = asked + 1
      for _, p in ipairs(paths) do found[#found + 1] = p end
    else
      refused[#refused + 1] = mount .. " (" .. tostring(err) .. ")"
    end
  end
end

table.sort(found)

if #found == 0 then
  print("nothing matches")

  -- Said out loud, because "nothing matches" from a search that could not
  -- ask anywhere is a different fact from "nothing matches" - and looks
  -- identical.
  if asked == 0 then
    print("  nothing that was mounted could answer a query")
  end

  return
end

for _, path in ipairs(found) do
  print("  " .. path)
end
