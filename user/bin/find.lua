-- Everything whose attributes match.
--
--   find kind=note
--   find kind=person city=Montevideo
--
-- The filesystem answers out of an index, so this costs the size of the
-- answer and not the size of the filesystem. That is the whole point of
-- attributes being indexed and it is the number bench/ measures.

local where = {}
local n = 0

for pair in tostring(args or ""):gmatch("%S+") do
  local name, value = pair:match("^([^=]+)=(.*)$")

  if not name then
    print("usage: find name=value [name=value ...]")
    return
  end

  where[name] = tonumber(value) or value
  n = n + 1
end

if n == 0 then
  print("usage: find name=value [name=value ...]")
  return
end

local paths, err = fs.query("/data", where)

if not paths then
  print("find: " .. tostring(err))
  return
end

if #paths == 0 then
  print("nothing matches")
  return
end

for _, path in ipairs(paths) do
  print("  " .. path)
end
