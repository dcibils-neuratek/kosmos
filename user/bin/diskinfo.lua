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
print(("  journal at block %d, reserved and unused"):format(sb.journal_at))
print(("  data    at block %d, %s blocks free of %d")
      :format(sb.data_at,
              sb.free_blocks and tostring(sb.free_blocks)
                or ("an unreadable number of (" .. tostring(sb.free_why) .. ")"),
              sb.blocks))
