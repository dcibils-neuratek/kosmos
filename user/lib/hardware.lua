-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What the machine is, in words: its name, and its network hardware.
--
--   local hardware = use("/lib/hardware.lua")
--
--   hardware.name(sys.info())      "LENOVO 20W000T9US ThinkPad T14 Gen 2i"
--   hardware.network(sys.bus())    network controllers, driven and not
--
-- **Two rows were wrong on the ThinkPad, and both were a label standing in
-- for a reading.** `neofetch` said `Host  QEMU q35 x86-64`, the Makefile's
-- platform string, and About said it beside "Platform:". And it said
-- `Network  virtio-net at 0.0.0.0` on a machine with no virtio-net, because
-- any network stack that answered was called virtio-net and its address is
-- four zero bytes until somebody configures one. Three programs printed the
-- first and three named the second, so the answer is here, once.
--
-- Nothing here is invented. A name is what the firmware wrote, as the kernel
-- handed it over; a card is a device the bus enumeration found, named by its
-- numbers. When there is nothing to read these return nothing, and the
-- caller says what it has instead.

local hardware = {}

--------------------------------------------------------------------------
-- The machine's name.
--------------------------------------------------------------------------

--
-- The firmware's manufacturer, product and version, trimmed and joined in
-- that order - or nil when it gave none, and `info.machine_source` says why.
--
-- **Placeholders are shown as written.** A board nobody finished says "To
-- Be Filled By O.E.M." in its tables, and Linux's neofetch keeps a list of
-- strings like that to delete. This does not: a list would be this file
-- deciding which of the firmware's words count, and a machine that calls
-- itself that is saying something true about itself.
--
function hardware.name(info)
  local words = {}

  for _, field in ipairs { "machine_vendor", "machine_product",
                           "machine_version" } do
    local text = info and info[field]

    text = (type(text) == "string") and text:match("^%s*(.-)%s*$") or ""

    if text ~= "" then words[#words + 1] = text end
  end

  if #words == 0 then return nil end

  return table.concat(words, " ")
end

--------------------------------------------------------------------------
-- Names for the numbers on the bus.
--
-- `machine` had these for its bus listing and they moved here, so a card is
-- called the same thing in every program that mentions one. The kernel
-- decodes none of these numbers, for `hal_bus_scan`'s reason: turning
-- 0x1af4:0x1041 into "virtio-net" is a table, and a table in a driver is a
-- driver deciding how somebody else prints.
--------------------------------------------------------------------------

hardware.VENDORS = {
  [0x1af4] = "Red Hat / virtio",
  [0x8086] = "Intel",
  [0x1b36] = "Red Hat / QEMU",
}

-- PCI class codes, high byte, and only the ones a machine here can show.
hardware.CLASSES = {
  [0x01] = "storage controller",
  [0x02] = "network controller",
  [0x03] = "display controller",
  [0x04] = "multimedia device",
  [0x06] = "bridge",
  [0x09] = "input device",
  [0x0c] = "serial bus controller",
}

-- virtio device types, for a board whose bus reports the type directly
-- rather than a vendor and a device. `class` is zero there, which is how
-- the two shapes are told apart.
hardware.VIRTIO = {
  [1] = "virtio-net", [2] = "virtio-blk", [3] = "virtio-console",
  [16] = "virtio-gpu", [18] = "virtio-input", [19] = "virtio-vsock",
  [25] = "virtio-sound",
}

-- Where a device is, in its board's own terms: a PCI address on bus 0, which
-- is the one a PC's scan walks, or the device-tree window it was found in.
function hardware.place(d)
  if d.class == 0 then return "window " .. tostring(d.where) end

  return ("%02x:%02x.%d"):format(0, d.where >> 3, d.where & 7)
end

function hardware.vendor(d)
  return hardware.VENDORS[d.vendor] or ("vendor 0x%04x"):format(d.vendor)
end

--------------------------------------------------------------------------
-- The network controllers.
--------------------------------------------------------------------------

--
-- Two lists, driven and not driven, each entry `{ name, place }`.
--
-- A device is a network controller when its PCI class says so, 02h, or on a
-- board whose bus is device-tree windows when its virtio type is 1. `name`
-- is "virtio-net" for virtio's, and the vendor with both numbers otherwise -
-- `Intel 8086:15fc` - because this system has no table of Intel's device ids
-- and a guess at a model would be the kind of row this file exists to stop.
--
-- **The undriven list is the point.** A laptop's Ethernet is found on the
-- bus whether or not anything drives it, and "no card" beside it is absence
-- and silence read alike - `hal_bus_scan`'s comment is the long version.
--
function hardware.network(bus)
  local driven, undriven = {}, {}

  for _, d in ipairs(bus or {}) do
    local name

    if d.class == 0 then
      if d.device == 1 then name = hardware.VIRTIO[1] end
    elseif (d.class >> 16) == 0x02 then
      name = (d.vendor == 0x1af4) and hardware.VIRTIO[1]
             or ("%s %04x:%04x"):format(hardware.vendor(d), d.vendor, d.device)
    end

    if name then
      local list = d.claimed and driven or undriven

      list[#list + 1] = { name = name, place = hardware.place(d) }
    end
  end

  return driven, undriven
end

return hardware
