-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- lsusb: every device on every USB port, driven or not (`docs/machine.md` M1).
--
--   lsusb
--
-- The USB driver's own list, through `usb.lua`: its controller's PCI
-- place and port, its speed, its vendor and product, the kind the driver
-- made of it - and "no driver" with the class it reported where nothing
-- here reads it.

local usb = use("/Kosmos/Libraries/usb.lua")
local list, why = usb.devices()

if not list then
  print("lsusb: " .. tostring(why))
  return
end

if #list == 0 then
  print("lsusb: nothing plugged in")
  return
end

for _, d in ipairs(list) do
  local pci = ("%02x:%02x.%x"):format(d.pci >> 8, (d.pci >> 3) & 0x1f, d.pci & 7)

  print(("%s port %-2d  %04x:%04x  %-11s  %-16s %s%s"):format(
        pci, d.port, d.vendor, d.product, usb.SPEEDS[d.speed] or "-",
        d.driven and ("driven: " .. d.kind) or "no driver",
        d.name ~= "" and d.name or "(no name)",
        d.driven and "" or ("  - class %02x/%02x/%02x"):format(d.class, d.subclass, d.protocol)))
end

print(("lsusb: %d devices, %d driven"):format(#list, (function()
  local n = 0
  for _, d in ipairs(list) do if d.driven then n = n + 1 end end
  return n
end)()))
