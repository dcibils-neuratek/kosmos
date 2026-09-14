-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The USB sticks this machine holds, and what their first blocks say.
--
--   sticks           every stick: its size, its names, and its partitions
--
-- The smallest thing that uses the block protocol end to end (USB step 5d,
-- `usb.md` §7): a stick's size asked of the USB driver, then its GUID partition
-- table read through it - the header at block 1, held to the block it says it
-- is at, and the entries it points to. It only reads.

local blocks = use("/lib/blocks.lua")

local function u32(s, at) return (string.unpack("<I4", s, at)) end
local function u64(s, at) return (string.unpack("<I8", s, at)) end

-- A partition's name: UTF-16LE in seventy-two bytes, to the first zero.
local function name_of(entry)
  local out = {}

  for at = 57, 127, 2 do
    local c = string.unpack("<I2", entry, at)

    if c == 0 then break end

    out[#out + 1] = (c >= 32 and c < 127) and string.char(c) or "?"
  end

  return table.concat(out)
end

-- A GUID as it is written out: its first three fields are little-endian.
local function guid(entry, at)
  local a, b, c = string.unpack("<I4I2I2", entry, at)
  local rest = { entry:byte(at + 8, at + 15) }

  return string.format("%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                       a, b, c, rest[1], rest[2], rest[3], rest[4], rest[5],
                       rest[6], rest[7], rest[8])
end

local function partitions(r, unit, info)
  local header, why = r:read(unit, 1, 1)

  if not header then
    print("  block 1: " .. tostring(why))
    return
  end

  if header:sub(1, 8) ~= "EFI PART" or u64(header, 25) ~= 1 then
    print("  no GUID partition table at block 1")
    return
  end

  local first_entry, count, size = u64(header, 73), u32(header, 81),
                                   u32(header, 85)
  local need = (count * size + info.block_size - 1) // info.block_size
  local most = blocks.TRANSFER_MOST // info.block_size

  if size < 128 or need == 0 or need > most then
    print("  a GUID partition table whose entries this does not read")
    return
  end

  local entries, err = r:read(unit, first_entry, need)

  if not entries then
    print("  its partition entries: " .. tostring(err))
    return
  end

  local shown = 0

  for i = 0, count - 1 do
    local entry = entries:sub(i * size + 1, i * size + size)

    if #entry < size then break end

    if entry:sub(1, 16) ~= string.rep("\0", 16) then
      print(string.format("  partition %d: \"%s\", blocks %d to %d, type %s",
                          i + 1, name_of(entry), u64(entry, 33),
                          u64(entry, 41), guid(entry, 1)))
      shown = shown + 1
    end
  end

  if shown == 0 then
    print("  a GUID partition table with no partitions in it")
  end
end

local r, why = blocks.open()

if not r then
  print("sticks: " .. tostring(why))
  return
end

local unit = 0

while true do
  local info = blocks.info(unit)

  if not info then break end

  print(string.format("unit %d: %d blocks of %d bytes, \"%s\" \"%s\"", unit,
                      info.blocks, info.block_size, info.vendor, info.product))
  partitions(r, unit, info)
  unit = unit + 1
end

if unit == 0 then
  print("sticks: no USB stick is ready")
end

r:close()
