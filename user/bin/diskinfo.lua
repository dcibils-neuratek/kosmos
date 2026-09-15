-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What is on the disk.
--
-- Reads `/disk/super`, which is served by the one process holding the block
-- device. This program has no access to sectors at all - it asks, like
-- everything else, and would get the same answer through the same path if
-- the disk were on another machine.
--
-- Its real job is to be what proves a format survived a reboot: run
-- `mkfs --yes`, restart the machine, run this.

local sb, err = fs.read("/home/.super")

if not sb then
  print("diskinfo: " .. tostring(err))
  return
end

if not sb.present then
  print("disk: none attached (" .. tostring(sb.why) .. ")")
  return
end

print(("disk: %d sectors of %d bytes, %d MB")
      :format(sb.sectors, sb.sector_size, sb.bytes // (1024 * 1024)))

-- Only for `/home` on a USB stick (`usb.md` §7, 5e): the kernel's disk has
-- no partition to name, and nothing to refuse a flush.
if sb.where then
  print("  on " .. sb.where)
end

--
-- **What finding the stick took**, in the log's own seconds, so it reads
-- beside the driver's lines in `log`: `sys.info().log_origin` is the counter
-- at the log's zero. On the ThinkPad the driver had the stick ready at 4.963 s
-- and the prompt came at 22, and this says whether the disk server spent the
-- difference looking (`usb.md` §7).
--
if sb.search and sb.search.looks then
  local info = sys.info() or {}
  local hz, origin = info.counter_hz or 0, info.log_origin or 0

  local function at(counter)
    if hz == 0 or not counter then return "an unknown time" end

    return ("%.2f s"):format((counter - origin) / hz)
  end

  print(("  found at %s, by look %d; the first look was at %s")
        :format(at(sb.search.found), sb.search.looks, at(sb.search.first)))

  local stops = {}

  for step, n in pairs(sb.search.stops or {}) do
    stops[#stops + 1] = { step = step, n = n }
  end

  table.sort(stops, function(a, b)
    if a.n ~= b.n then return a.n > b.n end

    return a.step < b.step
  end)

  for _, s in ipairs(stops) do
    print(("    %d look(s) before it found %s"):format(s.n, s.step))
  end
end

if sb.flush_why then
  print("  its cache: not written out when asked, so a commit is only as "
        .. "safe as the stick (" .. sb.flush_why .. ")")
end

if not sb.formatted then
  print("filesystem: none (" .. tostring(sb.why) .. ")")
  print("")
  print("`mkfs --yes` lays one down. It erases everything.")
  return
end

print(("filesystem: version %d, %d blocks of %d bytes")
      :format(sb.version, sb.blocks, sb.block_size))
print(("  bitmap  at block %d, %d block(s)")
      :format(sb.bitmap_at, sb.bitmap_blocks))
print(("  inodes  at block %d, %d of them")
      :format(sb.inodes_at, sb.inode_count))
print(("  journal at block %d, every write goes through it"):format(sb.journal_at))
print(("  data    at block %d, %s blocks free of %d")
      :format(sb.data_at,
              sb.free_blocks and tostring(sb.free_blocks)
                or ("an unreadable number of (" .. tostring(sb.free_why) .. ")"),
              sb.blocks))
