-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Disk Benchmark, without a window: how fast a drive or a filesystem is.
--
-- Two programs use it, as `bench.lua` has two: `diskbench` prints it, and
-- the window `docs/diskbench.html` draws will draw it. It exists to make
-- Kosmos's storage fast, so the first thing it must be is honest about what
-- it measured.
--
-- **Two kinds of thing to point it at.** A *drive* is read through its
-- blocks and never written, so its numbers are the device and its driver. A
-- *filesystem* is measured through a test file, as CrystalDiskMark does, and
-- the file is removed afterwards. The same drive both ways says what the
-- filesystem costs.
--
-- **CrystalDiskMark's four rows, and the ones Kosmos cannot run yet say
-- why** rather than hold a number for something else. Nothing queues: the
-- block protocol takes one request at a time, so eight or thirty-two at once
-- is not a thing that can be asked. A random write on kfs cannot be asked
-- either, since a write replaces the whole file.
--
-- **Fixed time, not fixed work**, as `bench.lua` argues: every run lasts the
-- same seconds, and the best of a few is kept. Under an emulator these
-- numbers measure the emulator; compare runs on the same footing.

local blocks = use("/lib/blocks.lua")

local diskbench = {}

local HZ = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

local function now()
  return sys.ticks() / HZ
end

--
-- What the disk server says its device has cost so far, in counter ticks:
-- the time inside its block reads and writes (`diskfs_main` in `init.lua`).
-- `/home/.device` touches no disk, so asking adds nothing to the answer. Nil
-- from a server that does not count, and then no share is claimed.
--
-- Every request the server answered in the meantime is in it - the desktop's
-- as well as this benchmark's - so a share is honest on a quiet machine.
--
local function device_ticks()
  local d = fs.read("/home/.device")

  if type(d) ~= "table" or not d.read_counter_ticks then return nil end

  return (d.read_counter_ticks or 0) + (d.write_counter_ticks or 0)
end

diskbench.SECONDS = 2
diskbench.RUNS = 3

diskbench.ROWS = {
  { id = "seq",   name = "sequential 1 MB", queue = 1 },
  { id = "seq8",  name = "sequential 1 MB", queue = 8 },
  { id = "rnd",   name = "random 4 KB",     queue = 1 },
  { id = "rnd32", name = "random 4 KB",     queue = 32 },
}

local QUEUED = "not yet: one command at a time"

--
-- A megabyte, round. It was 768 KB because that was the most one write
-- could store - `diskfs` refused more than a megabyte and kfs's journal,
-- which every block of a file went through, less - which the first run of
-- this found. Since 24 September a file's bytes go once, outside the
-- journal (`design.md` 8.3b), and a write is bounded by the disk.
--
local kfs = use("/lib/kfs.lua")
local FILE_BYTES = 1024 * 1024
local FILE_DIR = "/home/.diskbench"

-- Positions that do not repeat in a pattern a cache could learn, and the same
-- ones every run, so two runs ask the drive for the same work.
local function positions(seed)
  local state = seed

  return function(n)
    state = state * 6364136223846793005 + 1442695040888963407
    return (state >> 33) % n
  end
end

--
-- What can be measured here: `/home`, and every USB stick the driver holds.
-- The kernel's disk - the NVMe - is not on the list, because only the disk
-- server may read it (`sys_disk` in `kernel/syscall.c`).
--
function diskbench.targets()
  local out = {}

  if fs.getattr("/home") then
    out[#out + 1] = { kind = "file", path = "/home", name = "/home" }
  end

  local units = blocks.units()

  for unit = 0, (units or 0) - 1 do
    local info = blocks.info(unit)

    if info then
      out[#out + 1] = {
        kind = "drive", unit = unit, blocks = info.blocks,
        block_size = info.block_size,
        name = ("usb %d: %s %s"):format(unit, info.vendor, info.product),
      }
    end
  end

  return out
end

--------------------------------------------------------------------------
-- A filesystem, through a test file.
--------------------------------------------------------------------------

local function file_setup()
  fs.send(FILE_DIR, { type = "mkdir" })

  local ctx = { path = FILE_DIR .. "/test" }

  ctx.big = sys.memory(FILE_BYTES // 4096)
  ctx.small = sys.memory(1)

  if not ctx.big or not ctx.small then
    return nil, "no memory for the test file's megabyte"
  end

  -- Bytes that are not all one value, so nothing below can answer a page of
  -- zeros more cheaply than a page of data.
  local next_byte = positions(0x9E3779B9)
  local page = {}

  for i = 1, 4096 do
    page[i] = string.char(next_byte(256))
  end

  sys.region_write(ctx.big, 0, string.rep(table.concat(page), FILE_BYTES // 4096))

  local wrote, why = fs.write_from(ctx.path, ctx.big, FILE_BYTES)

  if wrote ~= FILE_BYTES then
    return nil, "the test file could not be written: " .. tostring(why)
  end

  return ctx
end

local function file_teardown(ctx)
  fs.send(ctx.path, { type = "delete" })

  if ctx.big then sys.release(ctx.big) end
  if ctx.small then sys.release(ctx.small) end
end

local function file_ops(ctx)
  local where = positions(0x51ED)
  local pages = FILE_BYTES // 4096

  return {
    probe = device_ticks,
    note = ("a %d KB file, read and written whole: its bytes written once, "
            .. "its structure through the journal")
           :format(FILE_BYTES // 1024),

    seq = {
      read = function()
        return fs.read_into(ctx.path, ctx.big, 0, FILE_BYTES)
      end,
      write = function()
        return fs.write_from(ctx.path, ctx.big, FILE_BYTES)
      end,
    },

    rnd = {
      read = function()
        return fs.read_into(ctx.path, ctx.small, where(pages) * 4096, 4096)
      end,
      write = "not yet: a write replaces the whole file",
    },
  }
end

--------------------------------------------------------------------------
-- A drive, through its blocks. Read only, and never anything else.
--------------------------------------------------------------------------

local function drive_setup(target)
  local reader, why = blocks.open()

  if not reader then return nil, why end

  return { reader = reader, target = target }
end

local function drive_teardown(ctx)
  ctx.reader:close()
end

local function drive_ops(ctx)
  local t = ctx.target
  local run = blocks.TRANSFER_MOST // t.block_size
  local small = math.max(1, 4096 // t.block_size)
  local where = positions(0x51ED)
  local lba = 0
  local NEVER = "never: a drive's blocks are not written"

  return {
    note = ("in %d KB reads, the most one USB read moves")
           :format(blocks.TRANSFER_MOST // 1024),

    seq = {
      read = function()
        if lba + run > t.blocks then lba = 0 end

        local moved, why = ctx.reader:fill(t.unit, lba, run)

        lba = lba + run
        return moved, why
      end,
      write = NEVER,
    },

    rnd = {
      read = function()
        return ctx.reader:fill(t.unit, where(t.blocks - small), small)
      end,
      write = NEVER,
    },
  }
end

--------------------------------------------------------------------------
-- Running them.
--------------------------------------------------------------------------

--
-- One run: `op` as many times as fit in `seconds`. Yields now and then, so a
-- window resuming this keeps drawing; `diskbench` just runs it through.
--
local function timed(op, seconds, probe)
  local count, bytes = 0, 0
  local device_before = probe and probe()
  local began = now()
  local last_yield = began
  local t = began

  repeat
    local moved, why = op()

    if not moved or moved <= 0 then
      return nil, tostring(why or "nothing was moved")
    end

    count = count + 1
    bytes = bytes + moved
    t = now()

    if t - last_yield > 0.1 then
      coroutine.yield()
      last_yield = now()
    end
  until t - began >= seconds

  local took = t - began
  local device_after = probe and probe()
  local device

  -- The share of the run the device took, clamped: the two readings bracket
  -- the run from outside it, so a rounding can put it a hair past either end.
  if device_before and device_after then
    device = math.max(0, math.min(1, (device_after - device_before) / HZ / took))
  end

  return {
    bytes_per_second = bytes / took,
    per_second = count / took,
    ms = took * 1000 / count,
    device = device,
  }
end

local function best_of(op, seconds, runs, probe)
  local best

  for _ = 1, runs do
    local r, why = timed(op, seconds, probe)

    if not r then return { why = why } end

    if not best or r.bytes_per_second > best.bytes_per_second then
      best = r
    end
  end

  return best
end

--
-- The whole measurement of one target, as a coroutine. It yields
-- `{ row = id, cell = { read = ..., write = ... } }` as each row finishes,
-- and returns the result `diskbench.save` keeps.
--
function diskbench.measure(target, seconds, runs)
  seconds = seconds or diskbench.SECONDS
  runs = runs or diskbench.RUNS

  return coroutine.create(function()
    local setup, teardown, ops

    if target.kind == "file" then
      setup, teardown, ops = file_setup, file_teardown, file_ops
    else
      setup, teardown, ops = drive_setup, drive_teardown, drive_ops
    end

    local ctx, why = setup(target)

    if not ctx then
      return { target = target.name, why = why }
    end

    local table_of = ops(ctx)
    local result = {
      target = target.name, kind = target.kind, note = table_of.note,
      seconds = seconds,
      runs = runs, version = (sys.build() or {}).version, rows = {},
    }

    for _, row in ipairs(diskbench.ROWS) do
      local cell = {}
      local asked = table_of[row.id]

      for _, side in ipairs({ "read", "write" }) do
        local op = asked and asked[side]

        if row.queue > 1 then
          cell[side] = { why = QUEUED }
        elseif type(op) == "string" then
          cell[side] = { why = op }
        else
          cell[side] = best_of(op, seconds, runs, table_of.probe)
        end
      end

      result.rows[row.id] = cell
      coroutine.yield({ row = row.id, cell = cell })
    end

    teardown(ctx)
    return result
  end)
end

--
-- A run kept as a file in `/home/benchmarks`, named by when it was taken and
-- tagged with what it measured, so Tracker can list them and two can be
-- compared.
--
function diskbench.save(result)
  local DIR = "/home/benchmarks"
  local clock = use("/lib/clock.lua")
  local t = clock.now()
  local name

  if t then
    name = ("%04d-%02d-%02d-%02d%02d%02d"):format(t.year, t.month, t.day,
                                                 t.hour, t.min, t.sec)
  else
    name = ("run-%d"):format(sys.ticks())
  end

  fs.send(DIR, { type = "mkdir" })

  local lines = {
    "target " .. tostring(result.target),
    "version " .. tostring(result.version),
    ("seconds %s runs %d"):format(tostring(result.seconds), result.runs),
  }

  for _, row in ipairs(diskbench.ROWS) do
    local cell = result.rows[row.id] or {}

    for _, side in ipairs({ "read", "write" }) do
      local c = cell[side] or {}

      if c.bytes_per_second then
        lines[#lines + 1] = ("%s %s %.0f %.1f %.3f"):format(
          row.id, side, c.bytes_per_second, c.per_second, c.ms)
      else
        lines[#lines + 1] = ("%s %s - %s"):format(row.id, side,
                                                  tostring(c.why))
      end
    end
  end

  local path = ("%s/%s.bench"):format(DIR, name)
  local ok, why = fs.write(path, table.concat(lines, "\n") .. "\n")

  if not ok then return nil, why end

  fs.setattr(path, { kind = "disk benchmark", target = result.target,
                     version = result.version })

  return path
end

return diskbench
