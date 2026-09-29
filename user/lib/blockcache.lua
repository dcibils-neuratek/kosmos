-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- **The disk's small reads, kept** (storage at full speed, `roadmap.md`;
-- `design.md` 8.3d).
--
-- A 4 KB read of a file in `/Home` was six disk calls, and one of them
-- carried the file's bytes. The rest were the path: while an index exists
-- every request is put in the disk's spelling and then found - two walks -
-- and each walk read the root's inode block and its directory again, and two
-- more blocks for every directory deeper (`testing.md` 18.270). Metadata is
-- read far more often than it changes, and this process is the only one that
-- writes the disk.
--
-- **Around the four disk calls, so nothing goes round it.** kfs looks
-- `sys.disk_read` and the rest up at each call, and every block it reads or
-- writes - the journal, the bitmap, an inode, a directory, a file's bytes -
-- passes through one of them. A cache inside kfs would have to be told by
-- each of seven places that write; this one is told by the writing itself.
--
-- **Only what the disk holds.** A write goes to the disk first, and a block
-- kept here is then replaced by what was written, or forgotten when the
-- write failed or was not whole blocks. A write from a region - a file's
-- bytes - forgets what it covers rather than keeping it: those are not what
-- this is for. A read into a region passes straight through, since the disk
-- is always current.
--
-- **Small reads only**: `small` blocks or fewer, which is an inode block, a
-- bitmap block, a directory, a settings file. A file read as a string in one
-- piece would push all of that out for bytes nobody reads twice.
--
-- **Bounded**, `most` blocks, the least recently used going first: this
-- process has a 2 MB heap.
--
local blockcache = {}

local BLOCK = 4096
local SECTOR = 512
local PER = BLOCK // SECTOR

--
-- Wraps `sys`'s disk calls in place. Answers the cache's own handle: `clear`,
-- for a caller that changed the disk behind it - only a test does - and
-- `stats`, counts of hits, misses and blocks evicted.
--
function blockcache.wrap(sys, most, small)
  local read, write = sys.disk_read, sys.disk_write
  local write_from = sys.disk_write_from
  local kept, used = {}, {}
  local count, clock = 0, 0
  local stats = { hits = 0, misses = 0, evicted = 0 }

  local function forget(sector, bytes)
    local first = sector // PER
    local last = (sector + math.max(1, (bytes + SECTOR - 1) // SECTOR) - 1) // PER

    for b = first, last do
      if kept[b] then
        kept[b], used[b] = nil, nil
        count = count - 1
      end
    end
  end

  local function keep(b, bytes)
    if not kept[b] then
      if count >= most then
        local oldest, at = nil, math.huge

        for k, t in pairs(used) do
          if t < at then oldest, at = k, t end
        end

        kept[oldest], used[oldest] = nil, nil
        count = count - 1
        stats.evicted = stats.evicted + 1
      end

      count = count + 1
    end

    kept[b] = bytes
    clock = clock + 1
    used[b] = clock
  end

  sys.disk_read = function(sector, bytes)
    if sector % PER ~= 0 or bytes % BLOCK ~= 0 or bytes <= 0
       or bytes > small * BLOCK then
      return read(sector, bytes)
    end

    local first, n = sector // PER, bytes // BLOCK
    local parts = {}

    for i = 0, n - 1 do
      local b = kept[first + i]

      if not b then
        parts = nil
        break
      end

      parts[i + 1] = b
    end

    if parts then
      stats.hits = stats.hits + 1

      for i = 0, n - 1 do
        clock = clock + 1
        used[first + i] = clock
      end

      return n == 1 and parts[1] or table.concat(parts)
    end

    stats.misses = stats.misses + 1

    local got, why = read(sector, bytes)

    if type(got) == "string" and #got == bytes then
      for i = 0, n - 1 do
        keep(first + i, n == 1 and got or got:sub(i * BLOCK + 1, (i + 1) * BLOCK))
      end
    end

    return got, why
  end

  sys.disk_write = function(sector, data)
    local wrote, why = write(sector, data)

    if wrote and sector % PER == 0 and #data % BLOCK == 0 then
      local first = sector // PER

      for i = 0, #data // BLOCK - 1 do
        if kept[first + i] then
          kept[first + i] = data:sub(i * BLOCK + 1, (i + 1) * BLOCK)
        end
      end
    else
      forget(sector, #data)
    end

    return wrote, why
  end

  if write_from then
    sys.disk_write_from = function(sector, region, at, bytes)
      forget(sector, bytes)
      return write_from(sector, region, at, bytes)
    end
  end

  return {
    stats = stats,
    clear = function()
      kept, used, count = {}, {}, 0
    end,
  }
end

return blockcache
