-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The disk server's cache of small reads, on this machine.
--
--   build/host/lua tools/test_blockcache.lua [user/lib/blockcache.lua]
--
-- `blockcache.lua` wraps the four disk calls, so it is tested over stand-ins
-- for them: a disk as a table of blocks, and every call that reached it
-- counted. What it must never do is answer with anything the disk does not
-- hold; what it is for is answering a small read twice with one call. And
-- `test_kfs.lua` runs its whole suite through it as well (`KFS_CACHE=1`),
-- journal, recovery and power losses included.
--
-- The argument is the file to test, so a control can hand it a broken copy.

local path = arg[1] or "user/lib/blockcache.lua"
local failed, passed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local BLOCK = 4096
local disk, calls = {}, 0
local regions = {}

sys = {}

function sys.disk_read(sector, bytes)
  calls = calls + 1
  local out = {}

  for i = 0, bytes // BLOCK - 1 do
    out[#out + 1] = disk[sector // 8 + i] or string.rep("\0", BLOCK)
  end

  return table.concat(out)
end

local refuse = false

function sys.disk_write(sector, data)
  calls = calls + 1

  if refuse then return nil, "the disk refused it" end

  -- By sectors, so a write of part of a block lands in part of it.
  for s = 0, #data // 512 - 1 do
    local b, off = (sector + s) // 8, ((sector + s) % 8) * 512
    local old = disk[b] or string.rep("\0", BLOCK)

    disk[b] = old:sub(1, off) .. data:sub(s * 512 + 1, (s + 1) * 512)
              .. old:sub(off + 513)
  end

  return #data
end

function sys.disk_write_from(sector, region, at, bytes)
  return sys.disk_write(sector, regions[region]:sub(at + 1, at + bytes))
end

local blockcache = assert(loadfile(path))()
local cache = blockcache.wrap(sys, 8, 4)

local function block(c) return string.rep(c, BLOCK) end

-- A small read, twice: one call.
disk[10] = block("a")
calls = 0
check(sys.disk_read(80, BLOCK) == block("a") and sys.disk_read(80, BLOCK) == block("a")
      and calls == 1, "a block read twice was not one call")

-- Written through: the next read is what was written, with no call for it.
calls = 0
sys.disk_write(80, block("b"))
check(disk[10] == block("b"), "a write did not reach the disk")
check(sys.disk_read(80, BLOCK) == block("b") and calls == 1,
      "a read after a write was not what was written, answered without a call")

-- Written from a region: forgotten, and read again from the disk.
regions[1] = block("c")
sys.disk_write_from(80, 1, 0, BLOCK)
calls = 0
check(sys.disk_read(80, BLOCK) == block("c") and calls == 1,
      "a block written from a region was answered from before the write")

-- A write the disk refused: whatever it holds now, not what was kept.
sys.disk_read(80, BLOCK)
refuse = true
sys.disk_write(80, block("d"))
refuse = false
disk[10] = block("e")               -- what a half-done write left, say
calls = 0
check(sys.disk_read(80, BLOCK) == block("e") and calls == 1,
      "a block a refused write covered was still answered from the cache")

-- Part of a block written: that block forgotten.
disk[11] = block("f")
sys.disk_read(88, BLOCK)
sys.disk_write(89, string.rep("g", 512))
calls = 0
check(sys.disk_read(88, BLOCK) == string.rep("f", 512) .. string.rep("g", 512)
                                  .. string.rep("f", BLOCK - 1024)
      and calls == 1, "a block written in part was answered from before")

-- Two blocks in one read, kept as two: either is answered alone.
disk[20], disk[21] = block("h"), block("i")
sys.disk_read(160, 2 * BLOCK)
calls = 0
check(sys.disk_read(168, BLOCK) == block("i") and sys.disk_read(160, 2 * BLOCK)
      == block("h") .. block("i") and calls == 0,
      "a two-block read was not kept as its two blocks")

-- A large read is not kept: a file read in one piece.
calls = 0
sys.disk_read(400, 5 * BLOCK)
sys.disk_read(400, BLOCK)
check(calls == 2, "a read of more than `small` blocks was kept")

-- Bounded, and the least recently used goes first.
cache.clear()

for b = 100, 107 do disk[b] = block("x") end
for b = 100, 107 do sys.disk_read(b * 8, BLOCK) end
sys.disk_read(100 * 8, BLOCK)          -- 100 is now the most recent
disk[108] = block("y")
sys.disk_read(108 * 8, BLOCK)          -- one past the bound: 101 goes
calls = 0
sys.disk_read(100 * 8, BLOCK)
check(calls == 0, "the most recently used block was evicted")
sys.disk_read(101 * 8, BLOCK)
check(calls == 1, "the least recently used block was not the one evicted")
check(cache.stats.evicted >= 1, "nothing was counted evicted")

if failed > 0 then
  print(("FAIL: %d of %d checks on the block cache."):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the block cache (a small read answered twice with one "
       .. "call, written through, a region's write and a refused or partial one "
       .. "forgotten, large reads not kept, bounded by recency).") :format(passed))
