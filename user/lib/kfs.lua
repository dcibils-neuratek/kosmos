-- The on-disk filesystem: its layout, and reading and writing the parts of
-- it that are structure rather than contents.
--
-- Borrowed rather than invented, deliberately and everywhere:
--
--   * **ext2's skeleton** - a superblock, a bitmap of free blocks, a table
--     of inodes, and directories stored as ordinary files. Without its block
--     groups, which exist to keep inodes near their data on a spinning disk
--     and buy nothing on an SD card.
--   * **extents** rather than ext2's indirect blocks. Fewer indirection
--     levels to get wrong, less metadata, and a whole file in one extent is
--     the normal case here.
--   * **BFS's semantics** - attributes belong to the file and are typed -
--     though the index over them is rebuilt at mount rather than stored.
--     design.md 8.3 says why: it is derivable, and derivable state that is
--     also stored is state that can disagree with itself, which on a
--     filesystem means a query returning a file that is not there.
--   * **ext3's journal**, which is not here yet. Its space is reserved from
--     the start so that adding it does not move everything else.
--
-- Everything is little-endian and every structure is packed with
-- `string.pack`, which is the reason this is Lua and readable rather than C
-- and a struct definition: the format string beside the field names *is*
-- the specification.

local kfs = {}

kfs.MAGIC        = 0x4b464f53          -- "KFOS", little endian
kfs.VERSION      = 1

kfs.BLOCK        = 4096                -- and the page size, which is not a
                                       -- coincidence: design.md 8.4 hands a
                                       -- large file over as mapped pages.
kfs.SECTOR       = 512
kfs.PER_BLOCK    = kfs.BLOCK // kfs.SECTOR

kfs.INODE_SIZE   = 128
kfs.EXTENTS      = 12                  -- what fits in an inode after its
                                       -- header, at 8 bytes each

kfs.ROOT_INODE   = 1                   -- 0 means "none", so the root is 1

-- Journal space, reserved and unused. Sized at a megabyte because a journal
-- holds one transaction's worth of blocks and a transaction here is a
-- handful; the number is round rather than derived, and it is reserved now
-- only so that turning it on later does not move the data blocks.
kfs.JOURNAL_BLOCKS = 256

kfs.KIND_FREE = 0
kfs.KIND_FILE = 1
kfs.KIND_DIR  = 2

--------------------------------------------------------------------------
-- The superblock. Block 0.
--
-- Every number that says where something is lives here, so that nothing
-- else has to compute a layout and two pieces of code cannot disagree about
-- one. `mkfs` decides them once; everybody else reads them.
--------------------------------------------------------------------------

local SUPER = "<I4I4I4I4I4I4I4I4I4I4I8"

local SUPER_FIELDS = {
  "magic", "version", "block_size", "blocks",
  "bitmap_at", "bitmap_blocks",
  "inodes_at", "inode_count",
  "journal_at", "data_at",
  "created",
}

function kfs.pack_super(sb)
  local values = {}

  for i, name in ipairs(SUPER_FIELDS) do
    values[i] = sb[name] or 0
  end

  local packed = string.pack(SUPER, table.unpack(values))

  -- Padded to a whole block, because that is the unit a disk is written in
  -- and a short write would leave whatever was there before behind it.
  return packed .. string.rep("\0", kfs.BLOCK - #packed)
end

function kfs.unpack_super(bytes)
  if #bytes < string.packsize(SUPER) then
    return nil, "the superblock is short"
  end

  local sb = {}
  local values = { string.unpack(SUPER, bytes) }

  for i, name in ipairs(SUPER_FIELDS) do
    sb[name] = values[i]
  end

  if sb.magic ~= kfs.MAGIC then
    return nil, "not a kosmos filesystem"
  end

  if sb.version ~= kfs.VERSION then
    return nil, ("version %d, and this understands %d")
                :format(sb.version, kfs.VERSION)
  end

  -- Checked rather than trusted. Every one of these is used as an offset
  -- into the disk, and a plausible wrong number is how a filesystem
  -- corrupts something that was fine.
  if sb.block_size ~= kfs.BLOCK then
    return nil, ("blocks of %d bytes, and this understands %d")
                :format(sb.block_size, kfs.BLOCK)
  end

  if sb.data_at <= sb.inodes_at or sb.inodes_at <= sb.bitmap_at
     or sb.bitmap_at == 0 or sb.data_at >= sb.blocks then
    return nil, "the superblock's layout does not make sense"
  end

  return sb
end

--------------------------------------------------------------------------
-- Inodes.
--
-- Fixed size so that inode n is at a computable place, which is the whole
-- reason ext2's table is a table. 128 bytes: 32 of header and 96 of
-- extents, which is twelve of them.
--------------------------------------------------------------------------

local INODE_HEAD = "<I4I4I8I8I4I4"     -- kind, links, size, mtime, attrs, n

function kfs.pack_inode(node)
  local out = string.pack(INODE_HEAD,
                          node.kind or kfs.KIND_FREE,
                          node.links or 0,
                          node.size or 0,
                          node.mtime or 0,
                          node.attrs or 0,
                          #(node.extents or {}))

  for _, e in ipairs(node.extents or {}) do
    out = out .. string.pack("<I4I4", e.start, e.count)
  end

  return out .. string.rep("\0", kfs.INODE_SIZE - #out)
end

function kfs.unpack_inode(bytes, at)
  at = at or 1

  local kind, links, size, mtime, attrs, n, next_at =
    string.unpack(INODE_HEAD, bytes, at)

  if n > kfs.EXTENTS then
    return nil, "an inode claims more extents than fit in one"
  end

  local extents = {}

  for _ = 1, n do
    local start, count
    start, count, next_at = string.unpack("<I4I4", bytes, next_at)
    extents[#extents + 1] = { start = start, count = count }
  end

  return { kind = kind, links = links, size = size, mtime = mtime,
           attrs = attrs, extents = extents }
end

--------------------------------------------------------------------------
-- The layout, computed in one place.
--
-- Given a disk of so many blocks, where everything goes. `mkfs` writes the
-- answer into the superblock and nothing recomputes it afterwards - but it
-- is a function rather than a comment so that `fsck` can ask the same
-- question and compare.
--------------------------------------------------------------------------

function kfs.layout(blocks, inode_count)
  -- One bit per block, rounded up to whole blocks.
  local bitmap_blocks = (blocks + kfs.BLOCK * 8 - 1) // (kfs.BLOCK * 8)
  local inode_blocks  = (inode_count * kfs.INODE_SIZE + kfs.BLOCK - 1)
                        // kfs.BLOCK

  local bitmap_at  = 1                        -- block 0 is the superblock
  local inodes_at  = bitmap_at + bitmap_blocks
  local journal_at = inodes_at + inode_blocks
  local data_at    = journal_at + kfs.JOURNAL_BLOCKS

  return {
    magic         = kfs.MAGIC,
    version       = kfs.VERSION,
    block_size    = kfs.BLOCK,
    blocks        = blocks,
    bitmap_at     = bitmap_at,
    bitmap_blocks = bitmap_blocks,
    inodes_at     = inodes_at,
    inode_count   = inode_count,
    journal_at    = journal_at,
    data_at       = data_at,
  }
end

--------------------------------------------------------------------------
-- Talking to the disk, in blocks rather than sectors.
--
-- Everything above this counts in filesystem blocks; the syscall counts in
-- sectors. One conversion, here, rather than the same multiplication
-- scattered over every caller - which is the rule `gfx.md` states about
-- pitch, one layer down and for the same reason.
--------------------------------------------------------------------------

function kfs.read_block(n)
  return sys.disk_read(n * kfs.PER_BLOCK, kfs.BLOCK)
end

function kfs.write_block(n, bytes)
  if #bytes > kfs.BLOCK then
    return nil, "a block write longer than a block"
  end

  -- Padded, so a caller may hand over just the part it cares about and the
  -- rest of the block is defined rather than whatever was there.
  if #bytes < kfs.BLOCK then
    bytes = bytes .. string.rep("\0", kfs.BLOCK - #bytes)
  end

  return sys.disk_write(n * kfs.PER_BLOCK, bytes)
end

--------------------------------------------------------------------------
-- Formatting.
--
-- Writes a superblock, an empty bitmap with the metadata marked used, and
-- an inode table holding one entry: an empty root directory.
--------------------------------------------------------------------------

function kfs.mkfs(sectors, now)
  local blocks = sectors // kfs.PER_BLOCK

  if blocks < 64 then
    return nil, "the disk is too small to hold a filesystem"
  end

  -- One inode per sixteen blocks, which is a guess in the same spirit as
  -- ext2's and has the same consequence: it is fixed at format time and
  -- running out of them is running out of files while blocks remain.
  local inode_count = math.max(64, blocks // 16)
  local sb = kfs.layout(blocks, inode_count)

  sb.created = now or 0

  -- The bitmap. Everything from block 0 up to the first data block is
  -- metadata and is used; the rest is free.
  --
  -- Written a block at a time rather than built as one string: the bitmap
  -- for a large disk is bigger than a process's heap, and building it whole
  -- would work on the disks tested and fail on a real one.
  local used = sb.data_at

  for i = 0, sb.bitmap_blocks - 1 do
    local first = i * kfs.BLOCK * 8          -- the block this byte 0 covers
    local bytes = {}

    for byte = 0, kfs.BLOCK - 1 do
      local base = first + byte * 8
      local v = 0

      for bit = 0, 7 do
        local block = base + bit

        if block < used or block >= sb.blocks then
          -- Past the end of the disk counts as used, so nothing ever
          -- allocates a block that is not there.
          v = v | (1 << bit)
        end
      end

      bytes[byte + 1] = string.char(v)
    end

    local ok, err = kfs.write_block(sb.bitmap_at + i, table.concat(bytes))
    if not ok then return nil, "writing the bitmap: " .. tostring(err) end
  end

  -- The inode table, empty but for the root.
  local per_block = kfs.BLOCK // kfs.INODE_SIZE
  local blank = kfs.pack_inode { kind = kfs.KIND_FREE }
  local inode_blocks = (inode_count * kfs.INODE_SIZE + kfs.BLOCK - 1)
                       // kfs.BLOCK

  for i = 0, inode_blocks - 1 do
    local block = {}

    for j = 0, per_block - 1 do
      local number = i * per_block + j

      if number == kfs.ROOT_INODE then
        -- The root: a directory with no entries yet, and two links,
        -- because it is its own parent.
        block[j + 1] = kfs.pack_inode { kind = kfs.KIND_DIR, links = 2,
                                        size = 0, mtime = sb.created,
                                        extents = {} }
      else
        block[j + 1] = blank
      end
    end

    local ok, err = kfs.write_block(sb.inodes_at + i, table.concat(block))
    if not ok then return nil, "writing the inodes: " .. tostring(err) end
  end

  -- The superblock last, and that ordering is the point rather than a
  -- detail: it is what makes the filesystem exist. A format interrupted
  -- before this leaves a disk that says it is not a filesystem, which is
  -- true. A format interrupted after it would leave one that claims to be
  -- and is not.
  local ok, err = kfs.write_block(0, kfs.pack_super(sb))
  if not ok then return nil, "writing the superblock: " .. tostring(err) end

  return sb
end

function kfs.mount()
  local bytes, err = kfs.read_block(0)

  if not bytes then
    return nil, "reading the superblock: " .. tostring(err)
  end

  return kfs.unpack_super(bytes)
end

return kfs
