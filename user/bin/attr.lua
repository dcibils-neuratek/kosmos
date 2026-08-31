-- Attributes: what a node is, as opposed to what is in it.
--
--   attr /data/notes.txt                  show them
--   attr /data/notes.txt kind=note        set one
--   attr /data/ada  kind=person email=ada@example.org
--
-- A node with attributes and no content is a perfectly good node - the BeOS
-- People file, which is a named entity with an address and a phone number
-- and nothing inside it. `write` is not involved.

local path, rest = tostring(args or ""):match("^%s*(%S+)%s*(.*)$")

if not path then
  print("usage: attr <path> [name=value ...]")
  return
end

if rest == "" then
  local attrs, err = fs.getattr(path)

  if not attrs then
    print("attr: " .. tostring(err))
    return
  end

  local names = {}
  for name in pairs(attrs) do names[#names + 1] = name end
  table.sort(names)

  if #names == 0 then
    print(path .. " has no attributes")
    return
  end

  for _, name in ipairs(names) do
    print(("  %-12s %s"):format(name, tostring(attrs[name])))
  end

  return
end

local set = {}

for pair in rest:gmatch("%S+") do
  local name, value = pair:match("^([^=]+)=(.*)$")

  if not name then
    print("attr: not a name=value pair: " .. pair)
    return
  end

  -- A number stays a number. The index keys by tostring either way, but
  -- what comes back out of getattr should be what went in.
  set[name] = tonumber(value) or value
end

local ok, err = fs.setattr(path, set)

if not ok then
  print("attr: " .. tostring(err))
  return
end

for name, value in pairs(set) do
  print(("  %-12s %s"):format(name, tostring(value)))
end
