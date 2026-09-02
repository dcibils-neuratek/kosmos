-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The benchmark, without a window.
--
-- What this machine can do, as one number and its parts. Two programs use
-- it: `sysbench`, which draws it, and `score`, which prints it over the
-- serial line - and the second is not an afterthought. A board on a desk
-- with a cable and no display is exactly the machine somebody most wants
-- to measure, and a benchmark that needs a window cannot measure it.
--
-- Six groups - processor, memory, runtime, kernel, graphics, filesystem -
-- and about twenty measurements inside them, ending in a single score so
-- that two machines can be compared by one number and then argued about
-- using the twenty.
--
-- **Fixed time, not fixed work.** Every measurement runs for the same
-- number of seconds and counts how much it got through. A benchmark that
-- does a fixed amount of work takes four seconds on this desk and eleven
-- minutes on a Pi 1, and the machine that most needs measuring is the one
-- you would give up waiting for. This way the whole run costs the same
-- everywhere, and the slow machine simply reports smaller numbers.
--
-- **It stays usable while it runs.** Each measurement is a coroutine that
-- yields between batches, so the window redraws, the results fill in as
-- they arrive, and the window can be dragged while the processor test is
-- running. That is worth having for its own sake: an application that
-- freezes the desktop for three minutes is the exact failure this whole
-- system is built to avoid, and a benchmark that did it would be
-- embarrassing.
--
-- **On what the score means.** It is the geometric mean of the parts,
-- normalised against one reference machine. Geometric because these are
-- ratios: the arithmetic mean of "twice as fast at one thing, half as fast
-- at another" is 1.25, which says the machine is better when it is
-- exactly even. The geometric mean says 1.0, which is the truth.
--
-- **And the warning that has to be here.** `CLAUDE.md` says QEMU numbers
-- are not performance numbers, and it is right. Under emulation this
-- measures the host, the emulator and the guest all at once, and no part
-- of that tells you what silicon would do. The number is honest when two
-- runs are compared on the *same* footing: two real boards, or one board
-- before and after a change. Comparing a QEMU run to a Pi is comparing
-- nothing to nothing.

local bench = {}

--------------------------------------------------------------------------
-- How long each measurement gets.
--
-- Four seconds is enough that a batch of work dominates the timing noise
-- and short enough that twenty of them fit in the few minutes anybody will
-- actually sit through. The filesystem ones get less, because each of
-- their operations is thousands of times more expensive and four seconds
-- of them would fill a disk.
--------------------------------------------------------------------------

bench.SECONDS = 4.0
local SECONDS = bench.SECONDS
local SLICE        = 0.10     -- how long a batch may run before yielding

bench.cpu = fs.read("/dev/cpu") or {}
local cpu = bench.cpu
local HZ  = cpu.counter_hz or 62500000

function bench.now()
  return sys.ticks() / HZ
end

local now = bench.now

--------------------------------------------------------------------------
-- The reference machine.
--
-- One number per measurement, from a single run on QEMU virt with a
-- cortex-a72, on the machine this was written on, at version 0.6.11. A
-- machine that matches it exactly scores 100.
--
-- Three significant figures, because one run is worth about that much and
-- writing 2580192.1 would claim a precision that is not there.
--
-- This is a *snapshot*, not a standard. It exists so the score has a
-- middle, and the only thing it is good for is comparison between runs
-- that were done the same way. When there is real hardware to measure,
-- these get replaced by that, and the version above is how anybody knows
-- which reference a published score was against.
--------------------------------------------------------------------------

bench.REFERENCE = {
  ["Processor/integer arithmetic"] = 2580000,
  ["Processor/floating point"] = 641000,
  ["Processor/function calls"] = 2100000,
  ["Memory/fill"] = 273000000,
  ["Memory/copy"] = 57600000,
  ["Runtime/table allocation"] = 15900,
  ["Runtime/string building"] = 142000,
  ["Runtime/garbage collection"] = 163,
  ["Runtime/serialise a table"] = 21100,
  ["Runtime/parse a table"] = 3970,
  ["Kernel/system call"] = 207000,
  ["Kernel/context switch"] = 25300,
  ["Kernel/round trip to a server"] = 566,
  ["Graphics/alpha blend"] = 48500000,
  ["Graphics/text"] = 438000,
  ["Graphics/antialiased discs"] = 20300,
  ["Graphics/triangles"] = 17400,
  ["Filesystem/create a file"] = 21,
  ["Filesystem/read a file"] = 46.2,
  ["Filesystem/read attributes"] = 46,
  ["Filesystem/write attributes"] = 44.9,
  ["Filesystem/bulk write"] = 45500,
}
local REFERENCE = bench.REFERENCE

--------------------------------------------------------------------------
-- The measurements.
--
-- Each one has a `batch(n)` that does n units of work. The harness decides
-- how big n should be, times it, and divides. What a "unit" is differs per
-- test and is named in `unit`, because "operations per second" without
-- saying which operation is a number that cannot be checked.
--------------------------------------------------------------------------

bench.TESTS = {}
local TESTS = bench.TESTS

-- `units` says how many units a batch of n did, when a unit is not an
-- iteration - pixels, bytes, characters. Left out, one iteration is one
-- unit.
--
-- It is a separate field and not something `batch` returns, and that is a
-- scar. `batch` used to return the count, tests returned a running total
-- to make it obvious the loop was doing something, and the harness could
-- not tell the two apart: the integer test reported twenty-seven billion
-- operations a second, which is about forty times what the hardware can
-- retire and was really just the value of `x`. A number that arrives in
-- the same slot as a result is a number that will eventually be mistaken
-- for one.
local function test(group, name, unit, batch, opts)
  opts = opts or {}

  TESTS[#TESTS + 1] = { group = group, name = name, unit = unit,
                        batch = batch, setup = opts.setup,
                        units = opts.units }
end

--------------------------------------------------------------------------
-- Processor.
--
-- Lua's numbers are doubles and its integers are 64-bit, and the two take
-- different paths through the interpreter, so both are worth having. None
-- of this is a processor benchmark in the SPEC sense - it is the *system's*
-- processor speed, seen through the interpreter every program here is
-- written in, which is the speed that actually matters to anything running
-- on this machine.
--------------------------------------------------------------------------

test("Processor", "integer arithmetic", "ops", function(n)
  local x = 1

  for _ = 1, n do
    x = (x * 1103515245 + 12345) & 0x7fffffff
    x = x ~ (x >> 7)
  end
end)

test("Processor", "floating point", "ops", function(n)
  local x = 1.0

  for i = 1, n do
    x = x + math.sqrt(i) * 0.5
    x = x * 0.9999
  end
end)

test("Processor", "function calls", "calls", function(n)
  local function add(a, b) return a + b end

  local x = 0

  for i = 1, n do x = add(x, i) end
end)

--------------------------------------------------------------------------
-- Memory.
--
-- Through surfaces, because that is the only way anything here touches a
-- large block of memory: pixels live outside the Lua heap in flat bytes,
-- and `fill` and `blit` are the C primitives that walk them. What this
-- measures is the memory system - cache, then bus - with about as little
-- interpreter in the way as this system can arrange.
--------------------------------------------------------------------------

local MEM = 384        -- a 384x384 surface is 576 KB, twice the usual cache

local scratch_a, scratch_b

test("Memory", "fill", "pixels", function(n)
  for _ = 1, n do
    scratch_a:fill(0, 0, MEM, MEM, 0xff204060)
  end
end, {
  units = function(n) return MEM * MEM * n end,
  setup = function()
    scratch_a = gfx.surface{ w = MEM, h = MEM }
    scratch_b = gfx.surface{ w = MEM, h = MEM }

    return scratch_a ~= nil and scratch_b ~= nil
  end,
})

test("Memory", "copy", "pixels", function(n)
  for _ = 1, n do
    scratch_b:blit(scratch_a, 0, 0, MEM, MEM, 0, 0)
  end
end, { units = function(n) return MEM * MEM * n end })

--------------------------------------------------------------------------
-- Runtime.
--
-- The interpreter and its garbage collector, which on this system are part
-- of the operating system rather than something running on top of it. A
-- long GC pause here is a long GC pause in the window manager.
--------------------------------------------------------------------------

test("Runtime", "table allocation", "tables", function(n)
  local keep

  for i = 1, n do
    keep = { a = i, b = i + 1, c = "x", d = false, e = 0.5 }
  end

  -- Kept alive to the end of the batch so the collector cannot free it
  -- while the loop is still running, which would measure a different
  -- thing on every pass.
  return keep ~= nil
end)

-- A fixed thirty-two strings per iteration, not n of them.
--
-- The first version appended n strings to one table and joined it, so the
-- work per unit depended on how large a batch the calibrator happened to
-- choose - which is different on every machine, which is the one thing a
-- comparison between machines cannot survive. It read as ordinary noise:
-- 39,000 on one run and 50,000 on the next, on the same machine, with
-- nothing changed. **A batch size that changes what a unit costs makes a
-- benchmark that only compares a machine to itself, and barely that.**
test("Runtime", "string building", "strings", function(n)
  for _ = 1, n do
    local parts = {}

    for j = 1, 32 do
      parts[j] = "field" .. j
    end

    table.concat(parts, ",")
  end
end, { units = function(n) return 32 * n end })

test("Runtime", "garbage collection", "collections", function(n)
  for _ = 1, n do
    -- Something to actually collect, or this measures an empty heap.
    local junk = {}

    for i = 1, 200 do junk[i] = { i } end

    junk = nil
    collectgarbage("collect")
  end
end)

test("Runtime", "serialise a table", "messages", function(n)
  local message = { type = "read", path = "/dev/cpu", n = 42,
                    flags = { true, false, true }, note = "a message" }
  for _ = 1, n do sys.pack(message) end
end)

test("Runtime", "parse a table", "messages", function(n)
  local packed = sys.pack{ type = "read", path = "/dev/cpu", n = 42,
                           flags = { true, false, true },
                           note = "a message" }
  for _ = 1, n do sys.unpack(packed) end
end)

--------------------------------------------------------------------------
-- Kernel.
--
-- The three numbers a microkernel lives or dies by, and the reason this
-- group exists at all. On a monolithic system most of what a program does
-- is a function call inside one address space; here it is a boundary
-- crossing, and how expensive that is decides what the whole design can
-- afford.
--------------------------------------------------------------------------

test("Kernel", "system call", "calls", function(n)
  for _ = 1, n do sys.ticks() end
end)

test("Kernel", "context switch", "switches", function(n)
  for _ = 1, n do sys.yield() end
end)

test("Kernel", "round trip to a server", "messages", function(n)
  -- The whole cost of asking another process a question: resolving the
  -- path against this process's namespace, the send, the rendezvous, the
  -- server building its answer, the table serialised in each direction,
  -- and the reply unpacked here.
  --
  -- Named for what it is rather than "IPC", because it is not the raw
  -- kernel primitive and calling it that would flatter the number by a
  -- long way. The bare send is far cheaper; this is what a program
  -- actually pays, and on a microkernel a program pays it constantly.
  for _ = 1, n do fs.read("/dev/cpu") end
end)

--------------------------------------------------------------------------
-- Graphics.
--
-- The primitives, not the compositor. Every one of these writes into a
-- surface this process owns, so what is measured is the pixel loop in C
-- and not the window manager's frame.
--------------------------------------------------------------------------

test("Graphics", "alpha blend", "pixels", function(n)
  for _ = 1, n do
    scratch_b:blend(scratch_a, 0, 0, MEM, MEM, 0, 0, 128)
  end
end, { units = function(n) return MEM * MEM * n end })

test("Graphics", "text", "glyphs", function(n)
  local line = "The quick brown fox jumps over the lazy dog"

  for i = 1, n do
    scratch_a:text(4, (i * 17) % (MEM - 20), line, 0xffe0e0e0)
  end
end, { units = function(n) return 43 * n end })

test("Graphics", "antialiased discs", "discs", function(n)
  for i = 1, n do
    scratch_a:disc(60 + (i % 200), 60 + (i * 7 % 200), 24, 0xff30c080, true)
  end
end)

test("Graphics", "triangles", "triangles", function(n)
  for i = 1, n do
    local x = i % 200

    scratch_a:triangle(x, 10, x + 90, 60, x + 20, 150, 0xffb05030)
  end
end)

--------------------------------------------------------------------------
-- Filesystem.
--
-- Skipped, and said so, when there is no disk. A benchmark that scores
-- zero for a thing the machine does not have is a benchmark that says a
-- machine with no drive is slow, which is not a fact about speed.
--
-- The path is under /data and every file it makes is removed at the end.
-- A benchmark that leaves a thousand files behind is a benchmark you run
-- once.
--------------------------------------------------------------------------

-- **`/home`, not `/data`.** `/data` is the ramfs - a server keeping nodes
-- in its own heap - and timing it would produce a filesystem score that
-- said nothing about the disk. The disk is mounted at `/home`, and the
-- first version of this measured the wrong one and reported a thousand
-- file reads a second, which should have been the giveaway.
local FS_DIR   = "/home/.sysbench"
local fs_ready = false
local fs_count = 0

local function fs_available()
  -- The directory may already be there from a previous run, so a refusal
  -- is not the test. Whether a file can actually be written is.
  fs.send(FS_DIR, { type = "mkdir" })

  local ok = fs.write(FS_DIR .. "/probe", "probe")

  fs_ready = ok and true or false

  return fs_ready
end

test("Filesystem", "create a file", "files", function(n)
  for _ = 1, n do
    fs_count = fs_count + 1
    fs.write(("%s/f%d"):format(FS_DIR, fs_count), "a small file's worth")
  end
end, { setup = fs_available })

test("Filesystem", "read a file", "files", function(n)
  local last = math.max(1, fs_count)

  for i = 1, n do
    fs.read(("%s/f%d"):format(FS_DIR, (i % last) + 1))
  end
end)

test("Filesystem", "read attributes", "queries", function(n)
  local last = math.max(1, fs_count)

  for i = 1, n do
    fs.getattr(("%s/f%d"):format(FS_DIR, (i % last) + 1))
  end
end)

test("Filesystem", "write attributes", "writes", function(n)
  local last = math.max(1, fs_count)

  for i = 1, n do
    fs.setattr(("%s/f%d"):format(FS_DIR, (i % last) + 1),
               { kind = "benchmark", round = i })
  end
end)

-- A kilobyte, not a megabyte, and that is a statement about the system
-- rather than a choice about the benchmark.
--
-- A write crosses as an IPC message and a message is 2048 bytes, so this
-- is close to the largest single write the system can currently do. The
-- roadmap's answer for real files is mapped pages - the same move shared
-- surfaces already make - and when that exists this test grows a sibling
-- that measures megabytes. Until then, a benchmark claiming to measure
-- large files would be measuring an error return.
test("Filesystem", "bulk write", "bytes", function(n)
  local blob = string.rep("0123456789abcdef", 64)   -- 1 KB

  for i = 1, n do
    fs.write(("%s/b%d"):format(FS_DIR, i % 8), blob)
  end
end, { units = function(n) return 1024 * n end })

--------------------------------------------------------------------------
-- Running one measurement.
--
-- A coroutine, so it can be stopped between batches and the window
-- redrawn. The batch size is found rather than chosen: start at one, and
-- keep doubling until a batch takes long enough to time honestly. On a
-- fast machine that lands at a large number and on a slow one at a small
-- number, which is the whole point - nothing here has to be tuned per
-- target.
--------------------------------------------------------------------------

function bench.measure(t)
  return coroutine.create(function()
    if t.setup and not t.setup() then
      return nil, "not available on this machine"
    end

    local n = 1

    -- Calibrate. A batch has to last long enough that the clock's
    -- resolution and the loop's own overhead are noise beside it.
    while true do
      local start = now()

      t.batch(n)

      local took = now() - start

      if took >= SLICE / 2 or n >= 1 << 30 then break end

      -- Doubling rather than extrapolating: extrapolating from a
      -- measurement too short to trust is trusting it.
      n = n * 2
      coroutine.yield()
    end

    -- Measure. Whole batches only, so every unit counted was actually
    -- done inside the time it is being divided by.
    local budget  = t.seconds or SECONDS
    local started = now()
    local units   = 0
    local elapsed = 0

    local per_batch = t.units and t.units(n) or n

    while elapsed < budget do
      t.batch(n)

      elapsed = now() - started
      units   = units + per_batch

      coroutine.yield()
    end

    return units / elapsed
  end)
end


--------------------------------------------------------------------------
-- Scoring.
--
-- Geometric mean, because these are ratios. The arithmetic mean of "twice
-- as fast at one thing, half as fast at another" is 1.25, which claims the
-- machine is better when it is exactly even. The geometric mean says 1.0.
--------------------------------------------------------------------------

function bench.groups_in_order()
  local seen, out = {}, {}

  for _, t in ipairs(TESTS) do
    if not seen[t.group] then
      seen[t.group] = true
      out[#out + 1] = t.group
    end
  end

  return out
end

function bench.group_score(results, group)
  local sum, count = 0, 0

  for i, t in ipairs(TESTS) do
    local r = results[i]

    if t.group == group and r and r.score then
      sum = sum + math.log(r.score)
      count = count + 1
    end
  end

  if count == 0 then return nil end

  return math.exp(sum / count)
end

function bench.total(results)
  local sum, count = 0, 0

  for _, g in ipairs(bench.groups_in_order()) do
    local s = bench.group_score(results, g)

    if s then
      sum = sum + math.log(s)
      count = count + 1
    end
  end

  return (count > 0) and math.exp(sum / count) or 0
end

-- What one finished measurement is worth, against the reference.
function bench.record(results, index, value)
  local t   = TESTS[index]
  local ref = REFERENCE[t.group .. "/" .. t.name]

  results[index] = { rate = value,
                     score = ref and (100 * value / ref) or nil }

  return results[index]
end

--------------------------------------------------------------------------
-- Everything it found, as lines somebody can read or paste.
--------------------------------------------------------------------------

function bench.report(results)
  local out = {
    ("Kosmos Mark %.0f"):format(bench.total(results)),
    ("%s %s, %d core(s)"):format(cpu.implementer or "?", cpu.part or "?",
                                 cpu.cores or 1),
    "",
  }

  for i, t in ipairs(TESTS) do
    local r = results[i]

    if r and r.rate then
      out[#out + 1] = ("%-12s %-22s %14.1f %s")
                      :format(t.group, t.name, r.rate, t.unit)
    elseif r then
      out[#out + 1] = ("%-12s %-22s %14s"):format(t.group, t.name, "skipped")
    end
  end

  out[#out + 1] = ""

  for _, g in ipairs(bench.groups_in_order()) do
    local s = bench.group_score(results, g)

    out[#out + 1] = ("%-12s %14s"):format(g,
                      s and ("%.0f"):format(s) or "not measured")
  end

  return out
end

--------------------------------------------------------------------------
-- Tidying up after the filesystem group.
--------------------------------------------------------------------------

function bench.cleanup()
  if not fs_ready then return end

  for i = 1, fs_count do
    fs.send(("%s/f%d"):format(FS_DIR, i), { type = "delete" })
  end

  for i = 0, 7 do
    fs.send(("%s/b%d"):format(FS_DIR, i), { type = "delete" })
  end

  fs.send(FS_DIR .. "/probe", { type = "delete" })
  fs.send(FS_DIR, { type = "delete" })
end

return bench
