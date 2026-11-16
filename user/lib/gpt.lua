-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A drive's GUID partition table, read through the block protocol.
--
--   local gpt = use("/Kosmos/Libraries/gpt.lua")
--   local list, why = gpt.read(reader, unit, block_size)
--
-- `reader` is what `blocks.open` returned, for sticks or for disks. The
-- header at block 1 is held to the block it says it is at, and the entries
-- it points to are read. Each partition comes back as
-- { number, name, first, last, type, id }: its place in the table from 1,
-- its name, its first and last blocks, its type GUID and its own unique
-- GUID - the one `opt/kosmos/home` names. It only reads.
--
-- `sticks` and `disks` both use it; it was `sticks`'s own until `disks`
-- wanted the same.

local blocks = use("/Kosmos/Libraries/blocks.lua")

local gpt = {}

local function u32(s, at) return (string.unpack("<I4", s, at)) end
local function u64(s, at) return (string.unpack("<I8", s, at)) end

-- A GUID as it is written out: its first three fields are little-endian.
function gpt.guid(bytes, at)
  local a, b, c = string.unpack("<I4I2I2", bytes, at)
  local rest = { bytes:byte(at + 8, at + 15) }

  return string.format("%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                       a, b, c, rest[1], rest[2], rest[3], rest[4], rest[5],
                       rest[6], rest[7], rest[8])
end

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

-- The types a person meets on a PC and a stick, by name.
gpt.TYPES = {
  ["C12A7328-F81F-11D2-BA4B-00A0C93EC93B"] = "EFI system",
  ["E3C9E316-0B5C-4DB8-817D-F92DF00215AE"] = "Microsoft reserved",
  ["EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"] = "Microsoft basic data",
  ["DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"] = "Windows recovery",
  ["8A9DC8A8-83CF-4F7F-962B-43157A68F14A"] = "Kosmos",
  ["0FC63DAF-8483-4772-8E79-3D69D8477DE4"] = "Linux filesystem",
}

-- The partitions, in table order, or nil and why.
function gpt.read(r, unit, block_size)
  local header, why = r:read(unit, 1, 1)

  if not header then
    return nil, "block 1: " .. tostring(why)
  end

  if header:sub(1, 8) ~= "EFI PART" or u64(header, 25) ~= 1 then
    return nil, "no GUID partition table at block 1"
  end

  local first_entry, count, size = u64(header, 73), u32(header, 81),
                                   u32(header, 85)
  local need = (count * size + block_size - 1) // block_size
  local most = blocks.TRANSFER_MOST // block_size

  if size < 128 or need == 0 or need > most then
    return nil, "a GUID partition table whose entries this does not read"
  end

  local entries, err = r:read(unit, first_entry, need)

  if not entries then
    return nil, "its partition entries: " .. tostring(err)
  end

  local list = {}

  for i = 0, count - 1 do
    local entry = entries:sub(i * size + 1, i * size + size)

    if #entry < size then break end

    if entry:sub(1, 16) ~= string.rep("\0", 16) then
      list[#list + 1] = { number = i + 1, name = name_of(entry),
                          first = u64(entry, 33), last = u64(entry, 41),
                          type = gpt.guid(entry, 1), id = gpt.guid(entry, 17) }
    end
  end

  return list
end

return gpt
