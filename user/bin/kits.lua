-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What kits this machine has, and what is in them.
--
--   kits              the list
--   kits pdf          what one holds
--
-- A kit is a library that happens to be written in C: the things a program
-- will want that have no business being interpreted - a decompressor, a
-- scanner, a codec. They are reached with `use("/kits/name")`, exactly as a
-- Lua library is reached with `use("/lib/name.lua")`, because which language
-- something is written in is not a fact its caller should have to know.
--
-- The name is BeOS's and so is the idea: Interface Kit, Storage Kit, Media
-- Kit, Translation Kit. `docs/beos.md` explains what this system takes from
-- there and what it leaves.

local which = args[1]

if not which then
  local names = sys.kit_names()

  print(("%d kit(s):"):format(#names))

  for _, name in ipairs(names) do
    local kit = sys.kit(name)
    local n = 0
    for _ in pairs(kit or {}) do n = n + 1 end
    print(("  /kits/%-12s %d entries"):format(name, n))
  end

  print("")
  print("kits <name> to see one.  use(\"/kits/<name>\") to use it.")
  return
end

local kit, why = sys.kit(which)

if not kit then
  print("kits: " .. tostring(why))
  return
end

local names = {}
for name in pairs(kit) do names[#names + 1] = name end
table.sort(names)

print(("/kits/%s"):format(which))

for _, name in ipairs(names) do
  print(("  %-16s %s"):format(name, type(kit[name])))
end
