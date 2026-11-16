-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The USB sticks this machine holds, and what their first blocks say.
--
--   sticks           every stick: its size, its names, and its partitions
--
-- The smallest thing that uses the block protocol end to end (USB step 5d,
-- `usb.md` §7): a stick's size asked of the USB driver, then its GUID partition
-- table read through it - the header at block 1, held to the block it says it
-- is at, and the entries it points to. It only reads.

local blocks = use("/Kosmos/Libraries/blocks.lua")

local gpt = use("/Kosmos/Libraries/gpt.lua")

local function partitions(r, unit, info)
  local list, why = gpt.read(r, unit, info.block_size)

  if not list then
    print("  " .. why)
    return
  end

  for _, p in ipairs(list) do
    print(string.format("  partition %d: \"%s\", blocks %d to %d, type %s",
                        p.number, p.name, p.first, p.last, p.type))
  end

  if #list == 0 then
    print("  a GUID partition table with no partitions in it")
  end
end

local r, why = blocks.open()

if not r then
  print("sticks: " .. tostring(why))
  return
end

-- Every number the driver has given out: a stick that left is a gap, and
-- the numbers after it are not moved down (`blockproto.h`).
local named, err = blocks.units()

if not named then
  print("sticks: " .. tostring(err))
  r:close()
  return
end

local ready = 0

for unit = 0, named - 1 do
  local info = blocks.info(unit)

  if info then
    print(string.format("unit %d: %d blocks of %d bytes, \"%s\" \"%s\"",
                        unit, info.blocks, info.block_size, info.vendor,
                        info.product))
    partitions(r, unit, info)
    ready = ready + 1
  end
end

if ready == 0 then
  print("sticks: no USB stick is ready")
end

r:close()
