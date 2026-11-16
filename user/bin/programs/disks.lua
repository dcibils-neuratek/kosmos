-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The drives inside this machine, and their partitions.
--
--   disks            every SATA disk: its size, and each partition's
--                    number, name, size, type, what it holds and its GUID
--
-- What `/Home` on a partition Windows made needs (`roadmap.md`, *SATA*):
-- the partition's own GUID, which `opt/kosmos/home` names, read here from
-- the drive rather than copied out of Windows. "holds" is read from the
-- partition's first block - NTFS's name, kfs's magic, or nothing but
-- zeroes - so the empty one Windows left is the one that says so. A
-- partition the disk server holds answers no read, and says that instead.
-- It only reads (`/Devices/disks` is read only to programs).

local blocks = use("/Kosmos/Libraries/blocks.lua")
local gpt = use("/Kosmos/Libraries/gpt.lua")

local function size_of(n)
  if n >= 1024 ^ 3 then return string.format("%.1f GB", n / 1024 ^ 3) end
  return string.format("%.0f MB", n / 1024 ^ 2)
end

-- What a partition's first block says it holds.
local function holds(r, unit, p)
  local first, why = r:read(unit, p.first, 1)

  if not first then
    return tostring(why):find("disk server holds", 1, true) and "Kosmos, in use"
           or ("not read: " .. tostring(why))
  end

  if first:sub(4, 11) == "NTFS    " then return "NTFS" end
  if first:sub(1, 4) == "SOFK" then return "Kosmos (kfs)" end
  if first:sub(4, 8) == "MSDOS" or first:sub(83, 87) == "FAT32" then return "FAT" end
  if first == string.rep("\0", #first) then return "nothing - empty" end
  return "something else"
end

local r, why = blocks.open(blocks.DISKS)

if not r then
  print("disks: " .. tostring(why))
  return
end

local named, err = blocks.units(blocks.DISKS)

if not named then
  print("disks: " .. tostring(err))
  r:close()
  return
end

local ready = 0

for unit = 0, named - 1 do
  local info = blocks.info(unit, blocks.DISKS)

  if info then
    print(string.format("disk %d: %s, \"%s\" \"%s\"", unit,
                        size_of(info.blocks * info.block_size), info.vendor, info.product))
    local list, how = gpt.read(r, unit, info.block_size)

    if not list then
      print("  " .. how)
    else
      for _, p in ipairs(list) do
        print(string.format("  %d. %-24s %10s  %-20s holds %s", p.number,
                            p.name ~= "" and ('"' .. p.name .. '"') or "(no name)",
                            size_of((p.last - p.first + 1) * info.block_size),
                            gpt.TYPES[p.type] or p.type, holds(r, unit, p)))
        print("     GUID " .. p.id)
      end

      if #list == 0 then
        print("  a GUID partition table with no partitions in it")
      end
    end

    ready = ready + 1
  end
end

if ready == 0 then
  print("disks: no SATA disk is ready")
end

r:close()
