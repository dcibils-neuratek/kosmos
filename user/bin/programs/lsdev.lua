-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- lsdev: every device this machine has, driven or not (`docs/machine.md`
-- M2).
--
--   lsdev            by kind, as This Machine's first view
--   lsdev tree       by connection, as its second
--
-- Not `devices`, which the design named it: that word is the shell's
-- already, the list of nodes in /Devices, and the shell's own word wins.
--
-- `devices.lua`'s one list, the same one the window draws: each device's
-- group, its name, where it is, and whether anything drives it - "no
-- driver" with its class where nothing here does.

local devices = use("/Kosmos/Libraries/devices.lua")
local words = use("/Kosmos/Libraries/files.lua").words(args, 2)
local list, counts = devices.read()

local function state(d)
  if not d.driven then return "no driver" end

  return "driven" .. (d.driver and (": " .. d.driver) or "")
end

local function line(d, indent)
  local name = indent .. d.name

  print(("%-40s %-18s %s%s"):format(name, state(d), d.place or "",
        d.detail and ("  - " .. d.detail) or ""))
end

if words[1] == "tree" then
  local roots, children = devices.tree(list)

  local function walk(d, depth)
    line(d, ("  "):rep(depth))
    for _, c in ipairs(children[d.id] or {}) do walk(c, depth + 1) end
  end

  for _, d in ipairs(roots) do walk(d, 0) end
else
  local group

  for _, d in ipairs(list) do
    if d.group ~= group then
      group = d.group
      print(group)
    end

    line(d, "  ")
  end
end

print(("lsdev: %d devices, %d driven, %d with no driver"):format(counts.total,
      counts.driven, counts.undriven))
