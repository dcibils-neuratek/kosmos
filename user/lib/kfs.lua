-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
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
--   * **ext3's journal**, and its ordering rather than any cleverness -
--     write the blocks, then one header that is the commit, then copy them
--     home. Below, under "The journal", with the argument for each step.
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

-- The journal: where a transaction's blocks are written before they go
-- where they belong ("The journal", below) - the filesystem's structure,
-- and since 24 September not a file's contents, which go once to blocks
-- nothing points at yet (`write_file`, `design.md` 8.3b). Sized at a
-- megabyte, round rather than derived. A transaction of more than
-- JOURNAL_BLOCKS - 2 blocks of structure is refused whole; a file's size
-- no longer counts towards it. It did, when the data went through here
-- too, and a recording passed a megabyte in four seconds.
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

--------------------------------------------------------------------------
-- The transaction in progress, if there is one.
--
-- `kfs.begin()` starts collecting writes instead of performing them;
-- `kfs.commit()` puts them on the disk in an order that survives losing
-- power at any point. Between those two, `write_block` records and
-- `read_block` answers out of the record - because the code doing the work
-- reads back what it just wrote constantly (allocate a block, then read the
-- bitmap again) and it must see its own changes.
--
-- `freed` is every block this transaction gave back, and none of them is
-- handed out again before it commits. A file's contents are written
-- straight to new blocks, and a block freed here is still the old file's
-- on the disk until the commit - so reusing one would put the new bytes
-- into the old file if the power went first.
--
-- Held as whole blocks rather than as a diff. A block is 4 KB and a
-- transaction here is a handful of them, so the simple thing costs about
-- sixty kilobytes at worst in a process with two megabytes.
--------------------------------------------------------------------------

local txn = nil

--
-- The most one disk call moves, in whole blocks: 31 on a stick or the
-- kernel's disk since storage at full speed, step 3, where every call was one
-- block. Asked of `sys.disk()` and kept once it answers; a disk that does not
-- say moves a block at a time, which is what every disk here once did, and a
-- disk not there yet is asked again next time rather than remembered as one.
--
local run_blocks = nil

local function blocks_a_call()
  if run_blocks then return run_blocks end

  -- A stand-in with no `sys.disk` at all - a host tool's - moves a block at a
  -- time, and is not asked again.
  if type(sys.disk) ~= "function" then
    run_blocks = 1
    return run_blocks
  end

  local d = sys.disk()

  if type(d) ~= "table" then return 1 end

  run_blocks = math.max(1, (tonumber(d.most) or kfs.BLOCK) // kfs.BLOCK)
  return run_blocks
end

function kfs.read_block(n)
  if txn and txn.blocks[n] then
    return txn.blocks[n]
  end

  return sys.disk_read(n * kfs.PER_BLOCK, kfs.BLOCK)
end

--
-- `count` blocks from `first`, as one string, in as few disk calls as the disk
-- allows. A block the open transaction holds is answered from there, as
-- `read_block` answers it - so a run with one of those in it is read a block
-- at a time, which is the rare case and the simple one.
--
function kfs.read_blocks(first, count)
  if txn then
    for n = first, first + count - 1 do
      if txn.blocks[n] then
        local parts = {}

        for m = first, first + count - 1 do
          local bytes = kfs.read_block(m)

          if not bytes then return nil, "reading a block" end

          parts[#parts + 1] = bytes
        end

        return table.concat(parts)
      end
    end
  end

  local per = blocks_a_call()
  local parts = {}
  local at, left = first, count

  while left > 0 do
    local n = math.min(per, left)
    local bytes, why = sys.disk_read(at * kfs.PER_BLOCK, n * kfs.BLOCK)

    if not bytes or #bytes ~= n * kfs.BLOCK then
      return nil, why or "a short read"
    end

    parts[#parts + 1] = bytes
    at, left = at + n, left - n
  end

  return (#parts == 1) and parts[1] or table.concat(parts)
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

  if txn then
    if not txn.blocks[n] then
      txn.order[#txn.order + 1] = n
    end

    txn.blocks[n] = bytes

    if #txn.order > kfs.JOURNAL_BLOCKS - 2 then
      txn.too_big = true
    end

    return true
  end

  return sys.disk_write(n * kfs.PER_BLOCK, bytes)
end

--------------------------------------------------------------------------
-- The journal.
--
-- What it is for, stated plainly: **an operation on this filesystem either
-- happened or did not, even if the power goes off in the middle of it.**
--
-- Without one, `store` writes a directory block, an inode, a bitmap block
-- and a data block, in some order, and losing power between any two of them
-- leaves the disk in a state no rule covers - a directory entry pointing at
-- an inode that was never written, a block marked used that nothing owns, a
-- file whose size says forty bytes and whose extent was never allocated.
-- Those are not lost data. They are a filesystem that disagrees with
-- itself, and every later operation builds on the disagreement.
--
-- Borrowed from ext3, and it is the *ordering* that does the work rather
-- than any cleverness:
--
--   1. Write every changed block into the journal, with a list of where
--      each one belongs. Nothing at its real address has moved yet.
--   2. Write the header saying "this transaction is complete", with a
--      checksum over everything above it. **This single block write is the
--      commit.** Before it, the transaction did not happen. After it, it
--      did, and the disk simply has not caught up.
--   3. Copy each block from the journal to where it belongs.
--   4. Write the header back to empty.
--
-- Lose power in 1, and mount finds an uncommitted header and ignores
-- everything - the operation never happened. Lose power in 3, and mount
-- finds a committed header and *replays* it, which finishes the job. Lose
-- power in 4 and the replay happens again, which is harmless: writing the
-- same blocks to the same places twice is the same as doing it once. That
-- last property is what "idempotent" is worth here, and it is why replay
-- can be dumb.
--
-- **The assumption, said out loud:** a single 4 KB block write either
-- happens or does not. That is not free on real hardware - a drive losing
-- power mid-sector can tear one - which is why the header carries a
-- checksum over the whole transaction. A torn commit fails the checksum and
-- is treated as never having happened, which is the safe direction.
--
-- **What goes through it is structure** - inodes, the bitmap, directories,
-- attributes - and not a file's contents, since 24 September. Those go
-- once, before the commit, to blocks nothing points at yet, which is
-- ext4's `data=ordered` and the reason is `write_file`'s. The guarantee
-- above is the same: an operation happened or did not.
--
-- **What this does not do:** batch. ext3 gets much of its speed from
-- collecting many operations into one journal write, because a disk costs
-- about the same for a large write as a small one. Here every operation is
-- its own transaction, which is the slow and simple thing. The measurement
-- that would justify batching does not exist yet; when `score` says the
-- filesystem group is the bottleneck, that is the moment.
--------------------------------------------------------------------------

-- Exported, because two other things need them: the test that has to build
-- a header by hand to check the state field is honoured, and eventually
-- fsck, which has to be able to say what the journal is holding.
kfs.J_MAGIC     = 0x4b4a524e          -- "KJRN"
kfs.J_EMPTY     = 0
kfs.J_COMMITTED = 1

-- header: magic, state, count, checksum, sequence
kfs.J_HEADER = "<I4I4I4I4I8"

local J_MAGIC     = kfs.J_MAGIC
local J_EMPTY     = kfs.J_EMPTY
local J_COMMITTED = kfs.J_COMMITTED
local J_HEADER    = kfs.J_HEADER

-- A checksum good enough to catch a torn write. FNV-1a, in C.
--
-- It was in Lua first, and the measurement is worth keeping because it is
-- the clearest example in this system of where the language line actually
-- falls. Creating a file went from 21 a second to 14 when the journal
-- landed. Stubbing the checksum out brought it back to 20.5 - so the
-- journal's own work, writing every block twice, costs about two percent,
-- and hashing four kilobytes a byte at a time through the interpreter cost
-- the other thirty.
--
-- Nothing about the *structure* of this file wanted to be C. One loop over
-- bytes did, and only a profile could have said which.
local function checksum(parts)
  local h = 0x811c9dc5

  for _, part in ipairs(parts) do
    h = sys.fnv1a(part, h)
  end

  return h
end

-- Everything a transaction is, on the disk:
--
--   journal_at + 0   the header
--   journal_at + 1   where each block belongs, as 4-byte numbers
--   journal_at + 2   the blocks themselves, in that order
--
-- One descriptor block caps a transaction at 1024 entries, and the journal
-- caps it at 254. The second limit is the real one.

function kfs.begin()
  if txn then return nil, "a transaction is already open" end

  txn = { blocks = {}, order = {}, freed = {}, too_big = false }
  return true
end

function kfs.rollback()
  txn = nil
end

--
-- Puts the transaction on the disk, in the order above.
--
-- Writes go through `sys.disk_write` directly rather than through
-- `kfs.write_block`, because `write_block` is the thing that records into
-- the transaction and this is the code emptying it.
--
local function raw_write(n, bytes)
  if #bytes < kfs.BLOCK then
    bytes = bytes .. string.rep("\0", kfs.BLOCK - #bytes)
  end

  return sys.disk_write(n * kfs.PER_BLOCK, bytes)
end

--
-- Blocks that sit one after another from `first`, in as few disk calls as the
-- disk allows, each padded to a whole block as `raw_write` pads one. It was a
-- call a block, and a transaction of N blocks was 2N + 3 calls (storage at
-- full speed, step 3).
--
local function write_run(first, blocks)
  local per = blocks_a_call()
  local i = 1

  while i <= #blocks do
    local piece = {}

    for k = i, math.min(#blocks, i + per - 1) do
      local bytes = blocks[k]

      if #bytes < kfs.BLOCK then
        bytes = bytes .. string.rep("\0", kfs.BLOCK - #bytes)
      end

      piece[#piece + 1] = bytes
    end

    local ok, err = sys.disk_write((first + i - 1) * kfs.PER_BLOCK,
                                   table.concat(piece))

    if not ok then return nil, err end

    i = i + #piece
  end

  return true
end

--
-- `stop` is where to pretend the power went, and it exists because the
-- alternative is not testing this at all.
--
-- The guarantee the journal makes is about one instant: after the commit
-- block has landed and before the last data block has reached its home.
-- Under QEMU that window is a few milliseconds inside a fifty-millisecond
-- write, and a SIGKILL aimed at it lands there by luck. `run_power.py`
-- kills the machine anyway and proves something else worth proving - that
-- the filesystem is never left inconsistent - but it cannot aim.
--
-- So the instant is a parameter. `"after-commit"` returns as soon as the
-- transaction is durable and before any of it has been applied, which is
-- exactly the state a mount has to recover from. Nothing in the system
-- passes it; `tools/test_kfs.lua` does.
--
function kfs.commit(sb, stop)
  if not txn then return nil, "no transaction is open" end

  local t = txn

  -- Closed first, so that anything below writing through `write_block`
  -- reaches the disk instead of being recorded into the transaction it is
  -- trying to finish.
  txn = nil

  if #t.order == 0 then return true end

  if t.too_big then
    return nil, "more blocks changed than the journal can hold"
  end

  local at = sb.journal_at

  -- 1. The descriptor and the data. Nothing at its real address has moved.
  local descriptor = {}
  local data = {}

  for i, block in ipairs(t.order) do
    descriptor[i] = string.pack("<I4", block)
    data[i] = t.blocks[block]
  end

  local desc_bytes = table.concat(descriptor)

  -- The descriptor and the data sit one after another in the journal, so
  -- they go in together, in as few calls as the disk allows.
  local journal = { desc_bytes }

  for i, bytes in ipairs(data) do
    journal[i + 1] = bytes
  end

  local ok, err = write_run(at + 1, journal)

  if not ok then return nil, err end

  -- 2. The commit. One block, and the whole guarantee turns on it.
  local sum = checksum({ desc_bytes, table.unpack(data) })

  ok, err = raw_write(at, string.pack(J_HEADER, J_MAGIC, J_COMMITTED,
                                      #t.order, sum, 0))

  if not ok then return nil, err end

  if stop == "after-commit" then
    return true
  end

  -- 3. Where the blocks actually belong. Sorted, so blocks that are
  --    neighbours on the disk go in one call; the order no longer matters,
  --    because the commit above has already made each of them what the
  --    transaction says.
  local homes = {}

  for i, block in ipairs(t.order) do
    homes[i] = { block = block, bytes = data[i] }
  end

  table.sort(homes, function(a, b) return a.block < b.block end)

  local i = 1

  while i <= #homes do
    local run = { homes[i].bytes }
    local j = i

    while j < #homes and homes[j + 1].block == homes[j].block + 1 do
      j = j + 1
      run[#run + 1] = homes[j].bytes
    end

    ok, err = write_run(homes[i].block, run)

    if not ok then
      -- Left committed on purpose. The next mount replays it and finishes
      -- what this could not, which is exactly the case the journal is for.
      return nil, err
    end

    i = j + 1
  end

  -- 4. Done with. A crash before this replays a transaction that has
  --    already been applied, which writes the same bytes to the same
  --    places and changes nothing.
  raw_write(at, string.pack(J_HEADER, J_MAGIC, J_EMPTY, 0, 0, 0))

  return true
end

--
-- What a mount does before anything else looks at the disk.
--
-- Returns the number of blocks replayed, so a caller can say so. Silence
-- about a replay would hide the only evidence that the machine did not shut
-- down cleanly.
--
function kfs.recover(sb)
  local head = kfs.read_block(sb.journal_at)

  if not head then return 0 end

  local magic, state, count, sum = string.unpack(J_HEADER, head)

  if magic ~= J_MAGIC or state ~= J_COMMITTED then
    return 0
  end

  if count == 0 or count > kfs.JOURNAL_BLOCKS - 2 then
    return 0
  end

  local desc = kfs.read_block(sb.journal_at + 1)

  if not desc then return 0 end

  local data = {}

  for i = 1, count do
    data[i] = kfs.read_block(sb.journal_at + 1 + i)

    if not data[i] then return 0 end
  end

  -- The checksum decides. A transaction whose commit block landed but whose
  -- data did not is exactly what this catches, and the safe answer is to
  -- treat it as never having happened.
  if checksum({ desc:sub(1, count * 4), table.unpack(data) }) ~= sum then
    raw_write(sb.journal_at, string.pack(J_HEADER, J_MAGIC, J_EMPTY, 0, 0, 0))
    return 0
  end

  for i = 1, count do
    local block = string.unpack("<I4", desc, (i - 1) * 4 + 1)

    raw_write(block, data[i])
  end

  raw_write(sb.journal_at, string.pack(J_HEADER, J_MAGIC, J_EMPTY, 0, 0, 0))

  return count
end

--------------------------------------------------------------------------
-- The block bitmap.
--
-- One bit per block, set when the block is in use. Read and written a block
-- at a time rather than held in memory: the bitmap for a large disk is
-- bigger than a process's heap, and a filesystem that only works on small
-- disks is a filesystem that fails the first time it matters.
--------------------------------------------------------------------------

--
-- `count` bits from `first`, set or cleared, each bitmap block read and
-- written once. It was a bit at a time, a whole block rebuilt for each, and
-- a file of a thousand blocks was a thousand of those - and a scan of the
-- bitmap from its start for every one to find it.
--
local function bitmap_range(sb, first, count, used)
  local per = kfs.BLOCK * 8
  local done = 0

  while done < count do
    local block = first + done
    local at = sb.bitmap_at + block // per
    local bytes = kfs.read_block(at)

    if not bytes then return nil, "reading the bitmap" end

    local lo = block % per                   -- first bit, in this block
    local n = math.min(count - done, per - lo)
    local hi = lo + n                        -- one past the last
    local b0, b1 = lo // 8, (hi - 1) // 8    -- the bytes it touches
    local mid = {}

    for byte = b0, b1 do
      local from = math.max(lo, byte * 8) - byte * 8
      local to = math.min(hi, byte * 8 + 8) - byte * 8
      local mask = ((1 << to) - 1) & ~((1 << from) - 1)
      local v = bytes:byte(byte + 1)

      v = used and (v | mask) or (v & ~mask)
      mid[#mid + 1] = string.char(v & 0xff)
    end

    local ok, err = kfs.write_block(at, bytes:sub(1, b0) .. table.concat(mid)
                                        .. bytes:sub(b1 + 2))

    if not ok then return nil, err end

    done = done + n
  end

  return true
end

--
-- Up to `want` free blocks one after another, marked used: the first free
-- block, and as many after it as are free too. Returns where they start and
-- how many; a file takes them a run at a time until it has what it needs.
--
-- A block this transaction freed is not free here (`txn.freed`, above).
-- Scanned a byte at a time so that a full byte is skipped in one test -
-- which is the difference between scanning a 64 MB disk in thousands of
-- steps and in hundreds of thousands.
--
function kfs.alloc_run(sb, want)
  local freed = txn and txn.freed
  local per = kfs.BLOCK * 8
  local start, count = nil, 0

  for at = 0, sb.bitmap_blocks - 1 do
    local bytes = kfs.read_block(sb.bitmap_at + at)

    if not bytes then return nil, "reading the bitmap" end

    for byte = 0, kfs.BLOCK - 1 do
      local v = bytes:byte(byte + 1)

      if start or v ~= 0xff then
        for bit = 0, 7 do
          local block = at * per + byte * 8 + bit

          if block >= sb.blocks then goto found end

          local free = (v & (1 << bit)) == 0
                       and not (freed and freed[block])

          if start then
            if not free then goto found end
            count = count + 1
          elseif free then
            start, count = block, 1
          end

          if start and count >= want then goto found end
        end
      end
    end
  end

  ::found::

  if not start then return nil, "the disk is full" end

  local ok, err = bitmap_range(sb, start, count, true)

  if not ok then return nil, err end

  return start, count
end

-- The first free block, or nil.
function kfs.alloc_block(sb)
  local block, err = kfs.alloc_run(sb, 1)

  if not block then return nil, err end

  return block
end

-- `count` blocks from `first` given back - and, inside a transaction, kept
-- from being handed out again until it commits.
local function free_run(sb, first, count)
  if txn then
    for b = first, first + count - 1 do txn.freed[b] = true end
  end

  return bitmap_range(sb, first, count, false)
end

function kfs.free_block(sb, block)
  return free_run(sb, block, 1)
end

--
-- How many blocks are free, counted out of the bitmap.
--
-- **Counted, because nothing keeps the number.** The superblock has no free
-- count and never had one, and the disk server answered `.super` with
-- `blocks - data_at` - every block past the metadata, as though nothing had
-- ever been written. A disk the host tool had filled with fourteen megabytes
-- said "30 of 32 MB free" on the ThinkPad, and `df` said the same.
--
-- Not kept beside the bitmap either. That would be a second copy of one
-- fact, and a transaction rolled back or a journal replayed would have to
-- put both right. The bitmap is what `alloc_block` believes, so it is what
-- this reads - through `read_block`, so a transaction in progress sees its
-- own allocations.
--
-- **Only the bits for blocks the disk has.** `mkfs` marks the tail of the
-- last bitmap block used, and a count that leaned on that would be trusting
-- a byte past the end of the disk to say so.
--
-- A byte that is all one thing is counted by `gsub`, in C, and only a byte
-- that is both - the edge of what is allocated - is looked at bit by bit. A
-- loop over every bit in Lua is 32,768 steps a bitmap block, and this runs
-- every time a Terminal opens.
--
local FREE_IN = {}                      -- clear bits, for each byte value

for v = 0, 255 do
  local n = 0

  for bit = 0, 7 do
    if (v & (1 << bit)) == 0 then n = n + 1 end
  end

  FREE_IN[v] = n
end

function kfs.free_blocks(sb)
  local per_block = kfs.BLOCK * 8
  local free = 0

  for at = 0, sb.bitmap_blocks - 1 do
    local bits = math.min(per_block, sb.blocks - at * per_block)

    -- A superblock claiming more bitmap than the disk needs. `sub` counts a
    -- negative end from the far end of the string, so this must stop here.
    if bits <= 0 then break end

    local bytes = kfs.read_block(sb.bitmap_at + at)

    if not bytes then return nil, "reading the bitmap" end

    local whole = bits // 8
    local span  = bytes:sub(1, whole)

    free = free + select(2, span:gsub("\0", "")) * 8

    for byte in span:gmatch("[^\0\255]") do
      free = free + FREE_IN[byte:byte()]
    end

    -- The last few blocks, when the disk ends partway through a byte.
    for bit = 0, bits % 8 - 1 do
      if (bytes:byte(whole + 1) & (1 << bit)) == 0 then free = free + 1 end
    end
  end

  return free
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

--
-- A window of a file, without reading the rest of it.
--
-- `read_file` returns the whole thing as one string, which is right for a
-- settings file and impossible for a hundred-megabyte one - it would not
-- fit in the heap, and the heap is for the process's own data anyway.
--
-- This is `pread`: give it an offset and a length and it touches only the
-- blocks that span them. It is the piece that lets a file be larger than
-- anything that has to hold it - the caller reads a window at a time into
-- a buffer it owns, which is how every system has done this since the
-- 1970s.
--
-- Walks the extents to find where `offset` lands rather than assuming the
-- file is contiguous. It usually is - that is what extents are for - and
-- the walk is over twelve entries at most.
--
function kfs.read_range(sb, node, offset, want)
  if offset >= node.size then return "" end

  if offset + want > node.size then
    want = node.size - offset
  end

  local parts = {}
  local at = 0                     -- where this extent starts in the file
  local finish = offset + want     -- one past the last byte wanted

  for _, e in ipairs(node.extents) do
    local span = e.count * kfs.BLOCK

    if at >= finish then break end

    -- The part of the window inside this extent, read as one run of blocks
    -- in as few disk calls as the disk allows, and cut to the bytes asked
    -- for: only its first block can be entered part way, and only its last
    -- left part way.
    if offset < at + span then
      local from = math.max(offset, at)
      local to = math.min(finish, at + span)
      local first = (from - at) // kfs.BLOCK
      local last = (to - at - 1) // kfs.BLOCK
      local bytes = kfs.read_blocks(e.start + first, last - first + 1)

      if not bytes then return nil, "reading a file" end

      local skip = from - (at + first * kfs.BLOCK)

      parts[#parts + 1] = bytes:sub(skip + 1, skip + (to - from))
    end

    at = at + span
  end

  return table.concat(parts)
end

function kfs.read_file(sb, node)
  local parts = {}
  local left = node.size

  -- An extent at a time, each in as few disk calls as the disk allows.
  for _, e in ipairs(node.extents) do
    if left <= 0 then break end

    local count = math.min(e.count, (left + kfs.BLOCK - 1) // kfs.BLOCK)
    local bytes = kfs.read_blocks(e.start, count)

    if not bytes then return nil, "reading a file" end

    if left < count * kfs.BLOCK then
      bytes = bytes:sub(1, left)
    end

    parts[#parts + 1] = bytes
    left = left - count * kfs.BLOCK
  end

  return table.concat(parts)
end

local function release(sb, node)
  for _, e in ipairs(node.extents) do
    free_run(sb, e.start, e.count)
  end

  node.extents = {}
  node.size = 0
end

--
-- **A file's contents are written once, straight to their blocks** -
-- never through the journal (`design.md` 8.3b). The blocks are new, taken
-- by this write and never one this transaction freed, so until the commit
-- nothing on the disk points at them: lose the power before it and the old
-- file is whole and these are free space; after it, the new file is. Only
-- the structure - the inode, the bitmap, the directory - is journalled. It
-- was all of it, the bytes too, so each went to the disk twice and no file
-- could be larger than the journal: a megabyte, less its metadata.
--
-- In as few disk calls as the disk allows, the last block padded.
--
local function write_data(first, bytes)
  local per = blocks_a_call()
  local blocks = (#bytes + kfs.BLOCK - 1) // kfs.BLOCK

  if #bytes < blocks * kfs.BLOCK then
    bytes = bytes .. string.rep("\0", blocks * kfs.BLOCK - #bytes)
  end

  local i = 0

  while i < blocks do
    local n = math.min(per, blocks - i)
    local ok, err = sys.disk_write((first + i) * kfs.PER_BLOCK,
                                   bytes:sub(i * kfs.BLOCK + 1,
                                             (i + n) * kfs.BLOCK))

    if not ok then return nil, err end

    i = i + n
  end

  return true
end

--
-- **Where a file's bytes come from**: a string, or a reader over somebody
-- else's buffer - `{ size = n, read = function(offset, length) }` - so a
-- file larger than this process's heap is written a piece at a time. The
-- disk server hands one over the caller's region; it used to read the
-- whole region into one string first, and refused anything over a megabyte.
--
local PIECE_BLOCKS = 16                  -- 64 KB of a reader at a time

local function source_of(data)
  if type(data) == "string" then
    return #data, function(offset, length)
      return data:sub(offset + 1, offset + length)
    end
  end

  return math.max(0, math.floor(tonumber(data.size) or 0)), data.read
end

function kfs.write_file(sb, number, node, data)
  -- Rewritten whole rather than in place. Overwriting a file with a shorter
  -- one has to release the blocks it no longer needs, and the version that
  -- kept them was a leak that only showed up as a disk filling with nothing
  -- on it. Released first, and in a transaction not handed out again until
  -- it commits (`free_run`), so this write never lands on the old file.
  release(sb, node)

  local size, read = source_of(data)
  local left = (size + kfs.BLOCK - 1) // kfs.BLOCK
  local offset = 0

  while left > 0 do
    local start, got = kfs.alloc_run(sb, left)

    if not start then
      release(sb, node)
      return nil, got
    end

    local last = node.extents[#node.extents]

    if last and last.start + last.count == start then
      last.count = last.count + got      -- it follows: extend, do not add
    elseif #node.extents >= kfs.EXTENTS then
      free_run(sb, start, got)
      release(sb, node)
      return nil, "the file is too fragmented for " .. kfs.EXTENTS
                  .. " extents"
    else
      node.extents[#node.extents + 1] = { start = start, count = got }
    end

    local b = 0

    while b < got do
      local n = math.min(got - b, PIECE_BLOCKS)
      local want = math.min(n * kfs.BLOCK, size - offset)
      local bytes, why = read(offset, want)

      if type(bytes) ~= "string" or #bytes ~= want then
        release(sb, node)
        return nil, why or "the file's bytes came up short"
      end

      local ok, werr = write_data(start + b, bytes)

      if not ok then
        release(sb, node)
        return nil, werr
      end

      offset = offset + want
      b = b + n
    end

    left = left - got
  end

  node.size = size

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

-- A new name for the same inode: in this directory, or in another one.
--
-- **A directory entry is edited; nothing is copied.** That is the whole
-- reason a filesystem has this operation rather than leaving it to whoever
-- is asking: renaming a four-megabyte song by copying and deleting reads and
-- writes four megabytes to change eleven characters, and does it
-- non-atomically, so a failure halfway leaves two files or none.
--
-- **`to` is a name or a path**, and the difference is a slash. A name means
-- the same directory. A path means anywhere on this disk, and then it is the
-- same edit twice - the entry leaves one directory and joins another, the
-- inode is untouched, and the blocks holding the file never move. Both
-- writes happen inside one journal transaction, so a move is atomic in the
-- way a copy-and-delete never was.
--
-- Crossing to a *different* filesystem is still not this function's to do,
-- and it says so by failing: the path walk starts at this disk's root, so a
-- destination that is not on this disk has no parent here. `files.move`
-- takes that as its cue to copy instead, which is honest about what it
-- costs.
--
-- The destination is added before the source entry is removed. With the
-- journal both land or neither does, so the order cannot be observed - but
-- the order that survives losing the journal is the one that leaves two
-- names for a file rather than none.
--
function kfs.rename(sb, path, to)
  if type(to) ~= "string" or to == "" then
    return nil, "a name cannot be empty"
  end

  local dir_number, dir_node, name = kfs.parent_of(sb, path)

  if not dir_number then return nil, dir_node end

  local to_number, to_node, to_name = dir_number, dir_node, to

  if to:find("/") then
    if to:sub(1, #path + 1) == path .. "/" then
      return nil, "a directory cannot be moved into itself"
    end

    to_number, to_node, to_name = kfs.parent_of(sb, to)

    if not to_number then return nil, to_node end
  end

  local entries, err = kfs.read_dir(sb, dir_node)

  if not entries then return nil, err end

  local at

  for i, e in ipairs(entries) do
    if e.name == name then at = i end
  end

  if not at then return nil, "no such file" end

  --
  -- Within one directory it is one edit and one write, which is worth
  -- keeping separate: reading the same directory twice and writing it twice
  -- would work and would be two chances for the second write to disagree
  -- with the first.
  --
  if to_number == dir_number then
    for _, e in ipairs(entries) do
      if e.name == to_name then return nil, "that name is taken" end
    end

    entries[at].name = to_name

    local ok, derr = kfs.write_dir(sb, dir_number, dir_node, entries)

    if not ok then return nil, derr end

    return true
  end

  if to_node.kind ~= kfs.KIND_DIR then
    return nil, "not a directory"
  end

  local inode = entries[at].inode

  local there, terr = kfs.read_dir(sb, to_node)

  if not there then return nil, terr end

  for _, e in ipairs(there) do
    if e.name == to_name then return nil, "that name is taken" end
  end

  there[#there + 1] = { inode = inode, name = to_name }

  local ok, derr = kfs.write_dir(sb, to_number, to_node, there)

  if not ok then return nil, derr end

  --
  -- Re-read rather than reusing what was read above. `write_dir` may have
  -- moved the destination's blocks, and if the two directories were the
  -- same one - which cannot happen here, but would be a silent corruption
  -- if it ever did - the copy in hand would be stale.
  --
  local mine, merr = kfs.read_dir(sb, dir_node)

  if not mine then return nil, merr end

  for i, e in ipairs(mine) do
    if e.name == name then
      table.remove(mine, i)
      break
    end
  end

  return kfs.write_dir(sb, dir_number, dir_node, mine)
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
--
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
    free_run(sb, e.start, e.count)
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

-- The directories a Kosmos disk has, made at format time.
--
-- `layout.md` describes them: what the system ships, what somebody
-- installed, what somebody made. They are made here rather than by
-- whoever mounts the disk because a formatted disk should *be* a Kosmos
-- disk - the first thing that happened without this was `save notes.txt`
-- failing on a freshly formatted drive, because `/home` was a mount point
-- with nothing behind it.
kfs.LAYOUT = { "/system", "/user", "/home" }

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

  -- And the layout, now that there is a filesystem to make it in. After
  -- the superblock rather than before, because `mkdir` needs a mounted
  -- filesystem to allocate out of - and because a format interrupted
  -- between the two leaves a valid, empty disk rather than an invalid one.
  for _, name in ipairs(kfs.LAYOUT) do
    local made, derr = kfs.mkdir(sb, name, now or 0)

    if not made then
      return nil, "making " .. name .. ": " .. tostring(derr)
    end
  end

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
