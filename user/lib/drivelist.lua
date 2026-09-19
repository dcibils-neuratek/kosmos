-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Every drive this machine found, and the filesystems on each.
--
-- The Drives app's model (USB step 6e, `docs/drives.html`), apart from the
-- window so it can be asked at a prompt and held to what a test put on a
-- stick. Two sources, and nothing new asked of either:
--
--   the USB driver, `/dev/blocks`   each stick: what it says it is, how
--                                   many blocks, how long a block is
--   the drive server, `/drives`     each filesystem it found: its name,
--                                   type, size, how much is free, and which
--                                   stick and partition it is on
--
-- and the machine's own disk, `sys.disk()`, whose size is known and whose
-- partitions are not read yet: the drive server reads sticks, and the disk
-- is the kernel's. **What is not known is said, not guessed** - a stick's
-- partition table and the space no filesystem claims come from the drive
-- server later, and until then the space no volume accounts for is "not in
-- a filesystem Kosmos reads", which is true whatever is there.

local drivelist = {}

local have_blocks, blocks = pcall(use, "/lib/blocks.lua")

local function trimmed(s)
  return (tostring(s or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

--
-- A size the way a person reads one: gigabytes with a decimal past one, and
-- megabytes below. Powers of 1024, as `df` and Tracker count.
--
function drivelist.size(bytes)
  bytes = bytes or 0

  if bytes >= 1024 * 1024 * 1024 then
    return ("%.1f GB"):format(bytes / (1024 * 1024 * 1024))
  end

  if bytes >= 1024 * 1024 then
    return ("%d MB"):format(bytes // (1024 * 1024))
  end

  return ("%d KB"):format((bytes + 1023) // 1024)
end

--
-- The drives, sticks first by the number the USB driver gave them, then the
-- machine's own. Each is { kind, name, bytes, unit, volumes, unclaimed },
-- `volumes` in partition order, `unclaimed` what no volume accounts for.
--
function drivelist.drives()
  local out = {}
  local volumes = (fs.volumes and fs.volumes("/drives")) or {}
  local units = have_blocks and blocks.units() or 0

  for unit = 0, (units or 0) - 1 do
    local info = blocks.info(unit)

    if info and (info.blocks or 0) > 0 then
      local name = trimmed((info.vendor or "") .. " " .. (info.product or ""))
      local drive = {
        kind = "USB stick", unit = unit,
        name = name ~= "" and name or ("USB stick " .. unit),
        bytes = info.blocks * (info.block_size or 512),
        volumes = {},
      }
      local claimed = 0

      for _, v in ipairs(volumes) do
        if v.unit == unit then
          drive.volumes[#drive.volumes + 1] = v
          claimed = claimed + (v.bytes or 0)
        end
      end

      table.sort(drive.volumes, function(a, b)
        return (a.partition or 0) < (b.partition or 0)
      end)

      drive.unclaimed = math.max(0, drive.bytes - claimed)
      out[#out + 1] = drive
    end
  end

  local disk = sys.disk and sys.disk()

  if type(disk) == "table" and (disk.bytes or 0) > 0 then
    out[#out + 1] = { kind = "Internal drive", name = "Internal drive",
                      bytes = disk.bytes, volumes = {}, internal = true,
                      unclaimed = disk.bytes }
  end

  return out
end

-- Where a volume opens in Tracker.
function drivelist.path(volume)
  return "/drives/" .. volume.name
end

return drivelist
