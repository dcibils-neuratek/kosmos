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
-- The block bitmap.
--
-- One bit per block, set when the block is in use. Read and written a block
-- at a time rather than held in memory: the bitmap for a large disk is
-- bigger than a process's heap, and a filesystem that only works on small
-- disks is a filesystem that fails the first time it matters.
--------------------------------------------------------------------------

local function bitmap_position(sb, block)
  local bit_index  = block
  local byte_index = bit_index // 8
  local within     = bit_index % 8

  return sb.bitmap_at + byte_index // kfs.BLOCK,
         byte_index % kfs.BLOCK,
         within
end

function kfs.block_used(sb, block)
  local at, byte, bit = bitmap_position(sb, block)
  local bytes = kfs.read_block(at)

  if not bytes then return nil, "reading the bitmap" end

  return (bytes:byte(byte + 1) & (1 << bit)) ~= 0
end

local function bitmap_set(sb, block, used)
  local at, byte, bit = bitmap_position(sb, block)
  local bytes = kfs.read_block(at)

  if not bytes then return nil, "reading the bitmap" end

  local v = bytes:byte(byte + 1)

  if used then v = v | (1 << bit) else v = v & ~(1 << bit) end

  -- Rebuilt around the one byte that changed. A whole block for one bit is
  -- what a journal exists to make cheap; without one, correctness first.
  local out = bytes:sub(1, byte) .. string.char(v & 0xff)
              .. bytes:sub(byte + 2)

  return kfs.write_block(at, out)
end

-- The first free block, or nil. A linear scan from the first data block,
-- byte at a time so that a full byte is skipped in one test - which is the
-- difference between scanning a 64 MB disk in thousands of steps and in
-- hundreds of thousands.
function kfs.alloc_block(sb)
  for at = 0, sb.bitmap_blocks - 1 do
    local bytes = kfs.read_block(sb.bitmap_at + at)

    if not bytes then return nil, "reading the bitmap" end

    for byte = 0, kfs.BLOCK - 1 do
      local v = bytes:byte(byte + 1)

      if v ~= 0xff then
        for bit = 0, 7 do
          if (v & (1 << bit)) == 0 then
            local block = (at * kfs.BLOCK + byte) * 8 + bit

            if block >= sb.blocks then
              return nil, "the disk is full"
            end

            local ok, err = bitmap_set(sb, block, true)
            if not ok then return nil, err end

            return block
          end
        end
      end
    end
  end

  return nil, "the disk is full"
end

function kfs.free_block(sb, block)
  return bitmap_set(sb, block, false)
end

--------------------------------------------------------------------------
-- The inode table.
--------------------------------------------------------------------------

local function inode_position(sb, number)
  local per_block = kfs.BLOCK // kfs.INODE_SIZE

  return sb.inodes_at + number // per_block,
         (number % per_block) * kfs.INODE_SIZE
end

function kfs.read_inode(sb, number)
  if number < 0 or number >= sb.inode_count then
    return nil, "no such inode"
  end

  local at, offset = inode_position(sb, number)
  local bytes = kfs.read_block(at)

  if not bytes then return nil, "reading an inode" end

  return kfs.unpack_inode(bytes, offset + 1)
end

function kfs.write_inode(sb, number, node)
  if number < 0 or number >= sb.inode_count then
    return nil, "no such inode"
  end

  local at, offset = inode_position(sb, number)
  local bytes = kfs.read_block(at)

  if not bytes then return nil, "reading an inode" end

  local packed = kfs.pack_inode(node)
  local out = bytes:sub(1, offset) .. packed
              .. bytes:sub(offset + kfs.INODE_SIZE + 1)

  return kfs.write_block(at, out)
end

function kfs.alloc_inode(sb)
  -- From 2: 0 means "none" and 1 is the root, both fixed at format time.
  for n = 2, sb.inode_count - 1 do
    local node = kfs.read_inode(sb, n)

    if node and node.kind == kfs.KIND_FREE then
      return n
    end
  end

  return nil, "no inodes left"
end

--------------------------------------------------------------------------
-- File contents.
--
-- An extent is a run of consecutive blocks. Allocating one at a time and
-- extending the last extent when the block happens to follow it means a
-- file written in one go on a fresh disk is a single extent, and a
-- fragmented one costs an entry per fragment - up to twelve, after which
-- the file cannot grow. That limit is real and is checked rather than
-- silently truncating.
--------------------------------------------------------------------------

function kfs.read_file(sb, node)
  local parts = {}
  local left = node.size

  for _, e in ipairs(node.extents) do
    for i = 0, e.count - 1 do
      if left <= 0 then break end

      local bytes = kfs.read_block(e.start + i)
      if not bytes then return nil, "reading a file" end

      parts[#parts + 1] = (left < kfs.BLOCK) and bytes:sub(1, left) or bytes
      left = left - kfs.BLOCK
    end
  end

  return table.concat(parts)
end

local function release(sb, node)
  for _, e in ipairs(node.extents) do
    for i = 0, e.count - 1 do
      kfs.free_block(sb, e.start + i)
    end
  end

  node.extents = {}
  node.size = 0
end

function kfs.write_file(sb, number, node, data)
  -- Rewritten whole rather than in place. Overwriting a file with a shorter
  -- one has to release the blocks it no longer needs, and the version that
  -- kept them was a leak that only showed up as a disk filling with nothing
  -- on it.
  release(sb, node)

  local blocks = (#data + kfs.BLOCK - 1) // kfs.BLOCK

  for i = 0, blocks - 1 do
    local block, err = kfs.alloc_block(sb)

    if not block then
      release(sb, node)
      return nil, err
    end

    local chunk = data:sub(i * kfs.BLOCK + 1, (i + 1) * kfs.BLOCK)
    local ok, werr = kfs.write_block(block, chunk)

    if not ok then
      release(sb, node)
      return nil, werr
    end

    local last = node.extents[#node.extents]

    if last and last.start + last.count == block then
      last.count = last.count + 1        -- it follows: extend, do not add
    elseif #node.extents >= kfs.EXTENTS then
      release(sb, node)
      return nil, "the file is too fragmented for " .. kfs.EXTENTS
                  .. " extents"
    else
      node.extents[#node.extents + 1] = { start = block, count = 1 }
    end
  end

  node.size = #data

  return kfs.write_inode(sb, number, node)
end

--------------------------------------------------------------------------
-- Directories.
--
-- A directory is an ordinary file whose contents are entries, which is
-- ext2's arrangement and the reason a directory needs no special case
-- anywhere else: it is read, written and allocated by the code above.
--
-- One entry is an inode number, a name length and the name. No padding and
-- no alignment: the whole thing is parsed sequentially and nothing seeks
-- into the middle of it.
--
-- **Only the root, at this milestone.** Nested directories need a path walk
-- and a `mkdir`, and adding them is the next step rather than this one. The
-- format already carries what they need - a directory inode is a kind, not
-- a special place - so nothing here has to change to allow them.
--------------------------------------------------------------------------

local ENTRY = "<I4s1"          -- inode, then the name with a length byte

function kfs.read_dir(sb, node)
  local bytes, err = kfs.read_file(sb, node)

  if not bytes then return nil, err end

  local entries, at = {}, 1

  while at <= #bytes do
    local inode, name
    local ok, result = pcall(function()
      inode, name, at = string.unpack(ENTRY, bytes, at)
    end)

    if not ok then
      return nil, "a directory entry is malformed"
    end

    if inode ~= 0 then
      entries[#entries + 1] = { inode = inode, name = name }
    end
  end

  return entries
end

function kfs.write_dir(sb, number, node, entries)
  local parts = {}

  for _, e in ipairs(entries) do
    if #e.name > 255 then
      return nil, "a name longer than 255 bytes"
    end

    parts[#parts + 1] = string.pack(ENTRY, e.inode, e.name)
  end

  return kfs.write_file(sb, number, node, table.concat(parts))
end

--------------------------------------------------------------------------
-- Paths.
--
-- A directory is an inode with a different `kind`, and its contents are the
-- same entries the root has always held - so walking a path is the same
-- lookup repeated, and nothing about the format had to change to allow it.
-- That was the point of storing a directory as an ordinary file.
--
-- No `.` or `..`. A path is resolved from the root every time, so there is
-- nothing for them to be relative to down here; the shell has a working
-- directory and resolves it before asking. Adding them to the *format*
-- would mean two entries in every directory whose only job is to be
-- believed, and fsck would then have to check they still are.
--------------------------------------------------------------------------

local function split(path)
  local parts = {}

  for part in tostring(path or ""):gmatch("[^/]+") do
    if part == "." or part == ".." then
      return nil, "a path may not contain . or .."
    end

    parts[#parts + 1] = part
  end

  return parts
end

-- One name inside one directory.
local function entry_in(sb, dir_node, name)
  local entries, err = kfs.read_dir(sb, dir_node)

  if not entries then return nil, err end

  for _, e in ipairs(entries) do
    if e.name == name then return e.inode end
  end

  return nil, "no such file"
end

-- Walks `count` components and returns where it arrived.
local function walk(sb, parts, count)
  local number = kfs.ROOT_INODE
  local node, err = kfs.read_inode(sb, number)

  if not node then return nil, err end

  for i = 1, count do
    if node.kind ~= kfs.KIND_DIR then
      return nil, parts[i - 1] .. " is not a directory"
    end

    local found, ferr = entry_in(sb, node, parts[i])

    if not found then return nil, ferr end

    number = found
    node, err = kfs.read_inode(sb, number)

    if not node then return nil, err end
  end

  return number, node
end

--------------------------------------------------------------------------
-- Attributes.
--
-- BFS's idea and BFS's semantics: an attribute belongs to the file, it is
-- typed, and it is not part of the contents. A picture's caption travels
-- with the picture, and reading the picture does not read the caption.
--
-- **One block, and the inode points at it.** Not an extent list, because
-- these are small typed values - a title, a rating, a kind - and four
-- kilobytes of them on one file is already more than anything here has
-- wanted. If that stops being true the field is still just a block number,
-- and growing it into a list is a change to these two functions and to
-- nothing else.
--
-- **Serialised with `sys.pack`**, which is the serialiser an IPC message
-- already uses and the one a table stored in a file already uses. A
-- filesystem inventing a second way to write a Lua table down would be two
-- formats to keep in agreement, and they would drift.
--
-- **`attrs = 0` means there are none.** Zero is safe as "none" because
-- block 0 is the superblock and can never be an attribute block. Every new
-- inode and everything `mkfs` writes already leaves it at zero, which is
-- why this could be added without touching the format.
--
-- What is *not* stored here: kind, size, modification time, how many
-- extents. Those are in the inode, they are facts about the file, and a
-- second copy of a fact is a copy that can disagree. It is the same
-- argument design.md 8.3 makes for not storing the index.
--------------------------------------------------------------------------

function kfs.read_attrs(sb, node)
  if not node or node.attrs == 0 then return {} end

  local bytes, err = kfs.read_block(node.attrs)

  if not bytes then return nil, err end

  -- A length in front, because the block is padded with NULs out to 4096
  -- and the serialiser cannot tell where the value it wrote ended.
  local n = string.unpack("<I4", bytes)

  if n == 0 or n > kfs.BLOCK - 4 then
    return nil, "this is not an attribute block"
  end

  local value, perr = sys.unpack(bytes:sub(5, 4 + n))

  if type(value) ~= "table" then
    return nil, "the attributes did not unpack: " .. tostring(perr)
  end

  return value
end

function kfs.write_attrs(sb, number, node, attrs)
  -- Nothing left to say about this file, so the block goes back. A
  -- filesystem that keeps an empty block per attribute somebody set once
  -- and then cleared is a filesystem that leaks, slowly, in a way nobody
  -- notices until it is out of space.
  if next(attrs) == nil then
    if node.attrs ~= 0 then
      kfs.free_block(sb, node.attrs)
      node.attrs = 0
      return kfs.write_inode(sb, number, node)
    end

    return true
  end

  local packed, perr = sys.pack(attrs)

  if not packed then return nil, tostring(perr) end

  if #packed > kfs.BLOCK - 4 then
    return nil, "more attributes than fit in a block"
  end

  local block = node.attrs

  if block == 0 then
    local got, aerr = kfs.alloc_block(sb)

    if not got then return nil, aerr end

    block = got
  end

  -- The block before the inode, and the order matters. Interrupted between
  -- the two, this has written a block nothing points at: a leak, which
  -- `fsck` can find and which harms nobody meanwhile. The other order
  -- leaves an inode pointing at a block that was never written, which is a
  -- file whose attributes are whatever used to be there. Mirror of the
  -- ordering `unlink` uses, and for the same reason.
  local ok, werr = kfs.write_block(block,
                                   string.pack("<I4", #packed) .. packed)

  if not ok then
    if node.attrs == 0 then kfs.free_block(sb, block) end

    return nil, werr
  end

  if node.attrs ~= block then
    node.attrs = block
    return kfs.write_inode(sb, number, node)
  end

  return true
end

-- What is at this path.
function kfs.find(sb, path)
  local parts, err = split(path)

  if not parts then return nil, err end

  return walk(sb, parts, #parts)
end

-- The directory that would hold it, and the name it would have there.
function kfs.parent_of(sb, path)
  local parts, err = split(path)

  if not parts then return nil, err end

  if #parts == 0 then
    return nil, "the root has no parent"
  end

  local number, node = walk(sb, parts, #parts - 1)

  if not number then return nil, node end

  if node.kind ~= kfs.KIND_DIR then
    return nil, "not a directory"
  end

  return number, node, parts[#parts]
end

function kfs.list(sb, path)
  local number, node = kfs.find(sb, path)

  if not number then return nil, node end

  if node.kind ~= kfs.KIND_DIR then
    return nil, "not a directory"
  end

  local entries, err = kfs.read_dir(sb, node)

  if not entries then return nil, err end

  local names = {}

  for _, e in ipairs(entries) do
    names[#names + 1] = e.name
  end

  table.sort(names)
  return names
end

-- Adds a name to a directory, or replaces where it points.
local function link(sb, dir_number, dir_node, name, inode)
  local entries, err = kfs.read_dir(sb, dir_node)

  if not entries then return nil, err end

  for _, e in ipairs(entries) do
    if e.name == name then
      e.inode = inode
      return kfs.write_dir(sb, dir_number, dir_node, entries)
    end
  end

  entries[#entries + 1] = { inode = inode, name = name }
  return kfs.write_dir(sb, dir_number, dir_node, entries)
end

function kfs.mkdir(sb, path, now)
  local dir_number, dir_node, name = kfs.parent_of(sb, path)

  if not dir_number then return nil, dir_node end

  if entry_in(sb, dir_node, name) then
    return nil, "that name is taken"
  end

  local number, err = kfs.alloc_inode(sb)

  if not number then return nil, err end

  -- Two links: the entry about to be made in its parent, and the one it
  -- would have to itself if this format had a `.` - which it does not, so
  -- the count is the convention rather than a thing to walk.
  local node = { kind = kfs.KIND_DIR, links = 2, size = 0,
                 mtime = now or 0, attrs = 0, extents = {} }

  local ok, werr = kfs.write_inode(sb, number, node)

  if not ok then return nil, werr end

  return link(sb, dir_number, dir_node, name, number)
end

function kfs.store(sb, path, data, now)
  local dir_number, dir_node, name = kfs.parent_of(sb, path)

  if not dir_number then return nil, dir_node end

  local existing = entry_in(sb, dir_node, name)

  if existing then
    local node, err = kfs.read_inode(sb, existing)

    if not node then return nil, err end

    if node.kind == kfs.KIND_DIR then
      return nil, "that is a directory"
    end

    node.mtime = now or node.mtime

    local ok, werr = kfs.write_file(sb, existing, node, data)

    if not ok then return nil, werr end

    return existing
  end

  -- New. The inode and its contents first, then the entry in its parent,
  -- and that ordering is the design: interrupted between the two it leaves
  -- an allocated inode nothing points at, which `fsck` can reclaim. The
  -- other order leaves a directory entry pointing at an inode that is not a
  -- file, which is a corrupt directory.
  local number, err = kfs.alloc_inode(sb)

  if not number then return nil, err end

  local node = { kind = kfs.KIND_FILE, links = 1, size = 0, mtime = now or 0,
                 attrs = 0, extents = {} }

  local ok, werr = kfs.write_file(sb, number, node, data)

  if not ok then return nil, werr end

  local lok, lerr = link(sb, dir_number, dir_node, name, number)

  if not lok then return nil, lerr end

  return number
end

-- Removes a name, and the file it named if nothing else names it.
--
-- The directory entry goes *first*, and that ordering is the mirror of the
-- one `store` uses. Interrupted after the entry is gone, the inode and its
-- blocks are unreachable and `fsck` reclaims them - a leak, which is
-- recoverable. The other order leaves a directory entry pointing at an
-- inode that has been freed and whose blocks may already belong to another
-- file, which is not.
--
-- A directory has to be empty. Removing a full one means walking it, and a
-- walk that fails halfway leaves a tree in a state nothing described - that
-- is a transaction, and it belongs after the journal rather than before it.
function kfs.unlink(sb, path)
  local dir_number, dir_node, name = kfs.parent_of(sb, path)

  if not dir_number then return nil, dir_node end

  local entries, err = kfs.read_dir(sb, dir_node)

  if not entries then return nil, err end

  local at, victim

  for i, e in ipairs(entries) do
    if e.name == name then
      at, victim = i, e.inode
      break
    end
  end

  if not at then return nil, "no such file" end

  local node, ierr = kfs.read_inode(sb, victim)

  if not node then return nil, ierr end

  if node.kind == kfs.KIND_DIR then
    local inside = kfs.read_dir(sb, node)

    if inside and #inside > 0 then
      return nil, "the directory is not empty"
    end
  end

  table.remove(entries, at)

  local ok, derr = kfs.write_dir(sb, dir_number, dir_node, entries)

  if not ok then return nil, derr end

  -- Now unreachable, so what follows can be interrupted without hurting
  -- anything that is still named.
  for _, e in ipairs(node.extents) do
    for i = 0, e.count - 1 do
      kfs.free_block(sb, e.start + i)
    end
  end

  -- Including whatever was said about it. Forgetting this is the leak that
  -- only shows up on a disk that has had files come and go for a while.
  if node.attrs ~= 0 then
    kfs.free_block(sb, node.attrs)
  end

  return kfs.write_inode(sb, victim, { kind = kfs.KIND_FREE, links = 0,
                                       size = 0, mtime = 0, attrs = 0,
                                       extents = {} })
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
