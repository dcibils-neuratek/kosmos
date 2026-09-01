-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The filesystem format, tested on this machine instead of the target.
--
--   build/host/lua tools/test_kfs.lua
--
-- `kfs.lua` is pure arithmetic over blocks. The only thing it wants from
-- the system is a way to read and write one, so given those as stubs over
-- a string it can be exercised here in a fraction of a second.
--
-- **This is what makes the journal testable at all.** The guarantee it
-- offers is about losing power at an exact instant - after the commit
-- block lands and before the last data block reaches its home. That window
-- is a few milliseconds inside a fifty-millisecond write, and a SIGKILL
-- aimed at a running QEMU hits it by luck or not at all. `run_power.py`
-- kills the machine five times and proves the filesystem is never left
-- inconsistent, which is worth proving and is not the same thing. Here the
-- instant is chosen.
--
-- It does not replace the guest tests. The same source runs here, but not
-- on the same machine, against the same libc, or through the same
-- syscalls. This answers "is the format correct"; `make test` and
-- `make disktest` answer "does it work on the machine".

local SECTORS = 8192          -- 4 MB, enough for a layout with room over

--------------------------------------------------------------------------
-- A disk, as a table of blocks.
--
-- Sparse on purpose: the parts nothing has written read back as zeroes,
-- which is what a freshly zeroed disk does, and it keeps a four-megabyte
-- image to however many blocks the test actually touched.
--------------------------------------------------------------------------

local disk = {}
local writes = 0

-- Everything the library expects to find in `sys`, and nothing else. If
-- kfs starts using something new, this fails loudly here rather than
-- quietly doing nothing.
sys = {}

function sys.disk_read(sector, bytes)
  local out = {}
  local block = sector // 8

  for i = 0, (bytes // 4096) - 1 do
    out[#out + 1] = disk[block + i] or string.rep("\0", 4096)
  end

  return table.concat(out)
end

function sys.disk_write(sector, data)
  local block = sector // 8

  for i = 0, (#data // 4096) - 1 do
    disk[block + i] = data:sub(i * 4096 + 1, (i + 1) * 4096)
    writes = writes + 1
  end

  return true
end

function sys.disk()
  return { sectors = SECTORS, sector_size = 512, bytes = SECTORS * 512 }
end

-- Attributes are serialised with the C serialiser, which is not here. A
-- stand-in that handles the flat tables of strings and numbers the tests
-- use, and refuses anything else rather than pretending.
function sys.pack(value)
  assert(type(value) == "table", "the stand-in packs tables only")

  local parts = {}

  for k, v in pairs(value) do
    assert(type(k) == "string", "the stand-in wants string keys")
    assert(type(v) == "string" or type(v) == "number",
           "the stand-in wants string or number values")
    parts[#parts + 1] = ("%s=%s:%s"):format(k, type(v), tostring(v))
  end

  table.sort(parts)
  return table.concat(parts, "\n")
end

function sys.unpack(text)
  local out = {}

  for line in tostring(text):gmatch("[^\n]+") do
    local k, kind, v = line:match("^([^=]+)=([^:]+):(.*)$")

    if k then out[k] = (kind == "number") and tonumber(v) or v end
  end

  return out
end

function sys.ticks() return 12345 end

-- The checksum lives in C on the target. The same arithmetic, here, so the
-- format tested is the format that ships.
function sys.fnv1a(bytes, seed)
  local h = seed or 0x811c9dc5

  for i = 1, #bytes do
    h = (h ~ bytes:byte(i)) & 0xffffffff
    h = (h * 16777619) & 0xffffffff
  end

  return h
end

--------------------------------------------------------------------------

local kfs = assert(loadfile("user/lib/kfs.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local function fresh()
  disk = {}
  local sb = assert(kfs.mkfs(SECTORS, 1))
  return assert(kfs.mount())
end

--------------------------------------------------------------------------
-- The format itself.
--------------------------------------------------------------------------

local sb = fresh()

check(sb.magic == kfs.MAGIC, "a fresh disk mounts")
check(sb.journal_at > sb.inodes_at, "the journal sits after the inodes")
check(sb.data_at == sb.journal_at + kfs.JOURNAL_BLOCKS,
      "the data starts after the journal's reserved space")

assert(kfs.store(sb, "/hello", "world", 1))
check(kfs.read_file(sb, select(2, kfs.find(sb, "/hello"))) == "world",
      "a file reads back")

--------------------------------------------------------------------------
-- A transaction is invisible until it commits.
--------------------------------------------------------------------------

sb = fresh()
assert(kfs.store(sb, "/before", "old", 1))

assert(kfs.begin())
assert(kfs.store(sb, "/during", "new", 2))

-- Read through the library: it must see its own uncommitted writes, or the
-- code doing the work cannot allocate a block and then read the bitmap.
check(kfs.find(sb, "/during") ~= nil,
      "an open transaction sees its own writes")

local before_commit = writes
kfs.rollback()

check(kfs.find(sb, "/during") == nil,
      "a rolled-back transaction left nothing behind")
check(kfs.find(sb, "/before") ~= nil,
      "and did not disturb what was already there")

--------------------------------------------------------------------------
-- Recovery: the instant a power cut cannot be aimed at.
--
-- The transaction is committed to the journal and then the machine
-- "stops" - the blocks are never copied to where they belong. That is the
-- state the journal exists for, and the next mount has to finish the job.
--------------------------------------------------------------------------

sb = fresh()
assert(kfs.store(sb, "/keep", "kept", 1))

assert(kfs.begin())
assert(kfs.store(sb, "/crashed", "survived", 2))
assert(kfs.commit(sb, "after-commit"))

-- Before recovery, the file is not there: only the journal knows about it.
local remounted = assert(kfs.mount())
check(kfs.find(remounted, "/crashed") == nil,
      "a committed transaction is not visible before it is replayed")

local replayed = kfs.recover(remounted)

check(replayed > 0, "recovery replayed the transaction")
check(kfs.find(remounted, "/crashed") ~= nil,
      "and the file is there afterwards")
check(kfs.read_file(remounted, select(2, kfs.find(remounted, "/crashed")))
      == "survived", "with the right contents")
check(kfs.find(remounted, "/keep") ~= nil,
      "and what was there before is still there")

-- Twice, because a crash between applying the blocks and clearing the
-- header replays a transaction that has already been applied. Writing the
-- same bytes to the same places has to be the same as doing it once.
check(kfs.recover(remounted) == 0,
      "a replayed journal is not replayed again")

--------------------------------------------------------------------------
-- An uncommitted journal is ignored.
--------------------------------------------------------------------------

sb = fresh()
assert(kfs.store(sb, "/only", "one", 1))

local clean = {}
for k, v in pairs(disk) do clean[k] = v end

assert(kfs.begin())
assert(kfs.store(sb, "/never", "should not appear", 2))
assert(kfs.commit(sb, "after-commit"))

-- The transaction is in the journal, and the header says the journal is
-- empty - the machine died one step earlier, after the blocks were written
-- and before the commit landed.
--
-- A *valid* header saying empty, not a blank block. Blanking it was the
-- first version and it tested nothing: with no magic, recovery refuses it
-- whether or not it checks the state at all. Removing the state check from
-- `recover` left this passing, which is how the hole was found - a test
-- that cannot fail when the thing it names is deleted is not testing that
-- thing.
--
-- Everything about the header stays valid except the one field being
-- tested. Magic, count and checksum are the real ones from the commit
-- that just happened; only the state says empty.
--
-- Getting this right took three attempts, and each wrong one passed.
-- Blanking the block was refused for having no magic. A hand-built header
-- with count zero was refused for the count. Both would have passed with
-- the state check deleted from `recover`, which is the definition of a
-- test that is not testing what its name says. **The way to find that out
-- is to delete the thing and watch, and it is worth doing for every check
-- that guards a rule rather than a value.**
--
local head = disk[sb.journal_at]
local magic, _, count, sum, seq = string.unpack(kfs.J_HEADER, head)

disk[sb.journal_at] = string.pack(kfs.J_HEADER, magic, kfs.J_EMPTY,
                                  count, sum, seq)
                      .. head:sub(25)

remounted = assert(kfs.mount())

check(kfs.recover(remounted) == 0,
      "an uncommitted journal replays nothing")
check(kfs.find(remounted, "/never") == nil,
      "and the operation simply did not happen")

--------------------------------------------------------------------------
-- A torn commit is refused.
--
-- The header says committed and the data behind it is not what was
-- written - a drive that lost power in the middle of a block. The
-- checksum has to catch it, and the safe answer is that the transaction
-- never happened rather than half of it did.
--------------------------------------------------------------------------

sb = fresh()
assert(kfs.store(sb, "/anchor", "anchored", 1))

assert(kfs.begin())
assert(kfs.store(sb, "/torn", "this should not survive", 2))
assert(kfs.commit(sb, "after-commit"))

-- Corrupt one byte of the first data block in the journal.
local victim = sb.journal_at + 2
local bytes = disk[victim]
disk[victim] = string.char((bytes:byte(1) + 1) % 256) .. bytes:sub(2)

remounted = assert(kfs.mount())

check(kfs.recover(remounted) == 0, "a torn transaction is not replayed")
check(kfs.find(remounted, "/torn") == nil,
      "and none of it reached the filesystem")
check(kfs.find(remounted, "/anchor") ~= nil,
      "while what was already committed is untouched")

--------------------------------------------------------------------------

if failed > 0 then
  print(("\nFAIL: %d of %d checks on the format failed.")
        :format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the filesystem format, on this machine.")
      :format(passed))
