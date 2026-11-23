-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Readings turned into rates: two snapshots of a source and the time
-- between them, giving what a monitor draws (`docs/sysmonitor.md`, S2).
--
--   local meters = use("/Kosmos/Libraries/meters.lua")
--   local cpu = meters.cpu(counter_hz, sys.cpuload())
--   cpu:take(sys.cpuload())          -- then cpu.user[c], cpu.kernel[c]
--
-- **One door for the arithmetic, which was written five times.** Monitor
-- split a core's busy into user and kernel, the Deskbar's meters worked
-- out busy again from `/Devices/kernel`, Disk Monitor and Network Monitor
-- each had a loop of byte rates by a key, GPU Monitor its watts, LLM
-- Monitor its tokens a second, Processes its totals. Each moved here as it
-- was, and each window reads it (*Kits supply, applications orchestrate*).
-- System Monitor's pages are its next readers.
--
-- **The window asks; this subtracts.** Nothing here calls a server: a meter
-- is handed the reading the window took, and the counter's time it took it
-- at, so `tools/test_meters.lua` holds every difference to recorded
-- readings on the Mac. What each meter wants is said beside it.
--
-- **The rules every meter keeps:**
--
--   - **A first reading has nothing to say.** A rate is a difference, so
--     a meter with no reading before answers nothing - never nought, which
--     a window would draw as an idle machine before it had looked.
--   - **A source that did not answer changes nothing.** `take(nil)` keeps
--     the last reading and the last rates, so the next answer is measured
--     from the last one that came.
--   - **A counter that went backwards began again from nought** - a server
--     started again, a handle used again - and its difference is nought,
--     never a rate below zero (LLM Monitor's rule, now everyone's).
--   - **A 32-bit counter wraps**, and is subtracted modulo 2^32: the GPU's
--     (`gpu.since`) and its power plane's microjoules.
--
-- **What it allocates per refresh: nothing of its own.** Each meter's
-- results live in fields of the meter, written in place - numbers, and for
-- the processor two arrays a core made once. The keyed rates (a drive's
-- programs, the connections) swap two pairs of tables and clear the older,
-- so a steady set of keys costs no table at all. What is allocated is the
-- libraries' this calls, as they were before the move: `pulse.speeds`'
-- and `pulse.heat`'s tables, `gpu.since`'s, and `procshare`'s rows. The
-- readings themselves - `sys.cpuload()`'s tables, `fs.read`'s - are the
-- window's, as in every monitor.
--
-- `pulse.lua`, `gpu.lua` and `procshare.lua` are used only by the meter
-- that needs them, when it is made: the Deskbar, which wants busy and
-- memory, does not load the GPU's protocol.

local meters = {}

local function library(name)
  return use("/Kosmos/Libraries/" .. name .. ".lua")
end

-- How much a counter grew from `a` to `b`: nought when it went backwards.
local function grew(a, b)
  return b >= a and b - a or 0
end

meters.grew = grew

-- How much of a period was busy, in whole per cent: the busy ticks over
-- every tick, idle ones included. nil when no tick passed.
function meters.busy_share(idle, busy)
  if idle + busy <= 0 then return nil end

  return (busy * 100) // (idle + busy)
end

-- Every meter is a table of its results with these methods; `into`, where
-- a constructor takes one, is a table of the caller's to become the meter,
-- so a window keeps its own fields - a history - beside the meter's.
local function made(methods, into, fields)
  local self = into or {}

  for k, v in pairs(fields) do self[k] = v end

  return setmetatable(self, methods)
end

local function clear(t)
  for k in pairs(t) do t[k] = nil end
end

--------------------------------------------------------------------------
-- The processor, a core at a time: `sys.cpuload()`.
--------------------------------------------------------------------------

--
-- **User and kernel, in per cent, between two readings of one core.**
-- Busy from the ticks; how much of busy was the kernel's from the
-- counters, which are measured where the ticks can only sample
-- (`percpu.h`). Moved from `sysmon` whole.
--
function meters.split(a, b)
  local busy = meters.busy_share(b.idle - a.idle, b.busy - a.busy)

  if not busy then return 0, 0 end

  local du = (b.user_counter or 0) - (a.user_counter or 0)
  local dk = (b.kernel_counter or 0) - (a.kernel_counter or 0)

  if du + dk <= 0 then return busy, 0 end

  local k = (busy * dk + (du + dk) // 2) // (du + dk)

  return busy - k, k
end

local CPU = {}
CPU.__index = CPU

--
-- `first` is a reading taken now, `cores` how many to follow (its length by
-- default). After `take`:
--
--   user[c], kernel[c]     per cent, kept from before for a core either
--                          reading left out; 0 until a core has two
--   taken[c]               whether core c was measured this time
--   all_user, all_kernel   the machine's, the measured cores' sum over all
--   speeds                 `pulse.speeds`: MHz a core, or nil
--
function meters.cpu(counter_hz, first, cores)
  cores = cores or math.max(1, #(first or {}))

  local self = made(CPU, nil, { hz = counter_hz, last = first or {}, cores = cores,
                                user = {}, kernel = {}, taken = {},
                                all_user = 0, all_kernel = 0, speeds = nil,
                                pulse = library("pulse") })

  for c = 1, cores do self.user[c], self.kernel[c], self.taken[c] = 0, 0, false end

  return self
end

-- A reading of `sys.cpuload()`; false when there was none to take.
function CPU:take(load)
  if not load then return false end

  local last, su, sk = self.last, 0, 0

  for c = 1, self.cores do
    local was, now = last[c], load[c]

    if was and now then
      local u, k = meters.split(was, now)

      self.user[c], self.kernel[c] = u, k
      su, sk = su + u, sk + k
    end

    self.taken[c] = (was and now) and true or false
  end

  self.all_user, self.all_kernel = su // self.cores, sk // self.cores
  self.speeds = self.pulse.speeds(last, load, self.hz)
  self.last = load
  return true
end

-- Measuring begun again from `load`, a reading taken now (kept as it was
-- when there is none).
function CPU:restart(load)
  self.last = load or self.last
end

--------------------------------------------------------------------------
-- The processor's heat and power: `sys.info()` beside `sys.cpuload()`.
--------------------------------------------------------------------------

local HEAT = {}
HEAT.__index = HEAT

--
-- `info` and `at` the first reading and the counter when it was taken.
-- After `take`, `heat` is `pulse.heat`'s table, or nil where nothing is
-- known (QEMU's processor).
--
function meters.heat(counter_hz, info, at)
  return made(HEAT, nil, { hz = counter_hz, info = info, at = at, heat = nil,
                           pulse = library("pulse") })
end

function HEAT:take(info, at, load)
  self.heat = self.pulse.heat(self.info, info,
                              self.hz > 0 and (at - self.at) / self.hz or nil, load)
  self.info, self.at = info, at
  return self.heat
end

--------------------------------------------------------------------------
-- The machine's busy, from `/Devices/kernel`: the bars' meter.
--------------------------------------------------------------------------

local BUSY = {}
BUSY.__index = BUSY

function meters.busy()
  return made(BUSY, nil, { idle = nil, busy = nil })
end

-- A reading of `/Devices/kernel`: per cent busy since the last, or nil
-- for the first, or when there was no reading (which is not kept).
function BUSY:take(k)
  if type(k) ~= "table" then return nil end

  local pct = nil

  if self.idle then
    pct = meters.busy_share(grew(self.idle, k.idle_ticks or 0),
                            grew(self.busy, k.busy_ticks or 0))
  end

  self.idle, self.busy = k.idle_ticks, k.busy_ticks
  return pct
end

--------------------------------------------------------------------------
-- Memory and the kernel's pools: `/Devices/memory`, `/Devices/kernel`.
--------------------------------------------------------------------------

-- Used and total, in MB, from a reading of `/Devices/memory`; nil when it
-- is not one.
function meters.memory(m)
  if type(m) ~= "table" or not m.total_mb or not m.free_mb then return nil end

  return m.total_mb - m.free_mb, m.total_mb
end

local POOLS = { "threads", "processes", "endpoints", "spaces" }

--
-- The machine's totals written into `out`, in place: `used_mb` and
-- `total_mb` from `m`, and each pool's count and ceiling (`threads`,
-- `threads_max`, ...) from `k`. A reading missing leaves its fields as
-- they were.
--
function meters.totals(k, m, out)
  if type(k) == "table" then
    for _, p in ipairs(POOLS) do out[p], out[p .. "_max"] = k[p], k[p .. "_max"] end
  end

  local used, total = meters.memory(m)

  if used then out.used_mb, out.total_mb = used, total end

  return out
end

--------------------------------------------------------------------------
-- Processes: `sys.processes()` beside `/Devices/kernel`.
--------------------------------------------------------------------------

local PROCS = {}
PROCS.__index = PROCS

--
-- Each process's share is `procshare.lua`'s, which stays its door; this
-- keeps its state and adds the totals. After `take`:
--
--   count            processes in the list, ended ones included
--   threads          every thread the kernel holds
--   kernel_threads   what is left of them when every live process's are
--                    taken away: an idle thread a core and Nebula's workers
--
function meters.processes()
  return made(PROCS, nil, { state = {}, count = 0, threads = 0, kernel_threads = 0,
                            share = library("procshare") })
end

-- `procshare`'s rows for this reading.
function PROCS:take(list, k)
  local rows = self.share.rows(self.state, list, k)
  local theirs = 0

  for _, p in ipairs(list or {}) do
    if not p.exited then theirs = theirs + (p.threads or 1) end
  end

  self.count = #(list or {})
  self.threads = type(k) == "table" and k.threads or 0
  self.kernel_threads = math.max(0, self.threads - theirs)
  return rows
end

--------------------------------------------------------------------------
-- The GPU: `gpu.stats()` beside `sys.info()`.
--------------------------------------------------------------------------

local GPU = {}
GPU.__index = GPU

--
-- After `take`:
--
--   now      the latest snapshot
--   rate     `gpu.since` from the one before: flips, frames, asleep,
--            blitter, render, seconds - kept from before when no time passed
--   busy     either engine at work, the larger share, 0 to 1
--   bytes    what the buffers hold
--   watts    the graphics plane's power, where `sys.info()` measures it
--
function meters.gpu(counter_hz)
  return made(GPU, nil, { hz = counter_hz, now = nil, rate = nil, busy = nil, bytes = nil,
                          watts = nil, last = nil, last_info = nil, gpu = library("gpu") })
end

-- `s` a snapshot or nil, `info` `sys.info()`'s (a table); true when there
-- are new rates.
function GPU:take(s, info)
  if not s then return false end

  local fresh = false

  if self.last then
    self.rate = self.gpu.since(self.last, s, self.hz)

    local rate = self.rate

    if rate then
      local bytes = 0

      for _, b in ipairs(s.buffers or {}) do bytes = bytes + b.bytes end

      self.bytes = bytes
      self.busy = math.max(rate.blitter, rate.render or 0)

      local a, b = self.last_info and self.last_info.gpu_energy_uj, info and info.gpu_energy_uj

      -- PP1's microjoules, wrapping at 32 bits in `sysinfo`'s spare word.
      if a and b then self.watts = ((b - a) & 0xffffffff) / 1e6 / rate.seconds end

      fresh = true
    end
  end

  self.now, self.last, self.last_info = s, s, info
  return fresh
end

-- Measuring begun again at the next snapshot.
function GPU:restart()
  self.last = nil
end

--------------------------------------------------------------------------
-- A drive: its server's `.device`.
--------------------------------------------------------------------------

local DRIVE = {}
DRIVE.__index = DRIVE

--
-- After `take`:
--
--   rin, rout             bytes a second read and written (0 until two)
--   read_ms, write_ms     how long a read and a write took, or nil when
--                         there were none
--   cache                 per cent of the looks the cache answered, kept
--                         from before when there were none
--   counts                the latest reading, the totals since it started
--   silent                true once the drive has failed to answer
--
function meters.drive(counter_hz, into)
  return made(DRIVE, into, { hz = counter_hz, rin = 0, rout = 0, last = nil, at = nil })
end

-- A reading of `.device`, or anything else for none; true when there are
-- new rates.
function DRIVE:take(c, at)
  if type(c) ~= "table" then
    self.silent = true
    return false
  end

  local was, fresh = self.last, false

  if was and at > self.at then
    local hz = self.hz
    local seconds = (at - self.at) / hz
    local dr, dw = grew(was.reads, c.reads), grew(was.writes, c.writes)

    self.rin = grew(was.read_bytes, c.read_bytes) / seconds
    self.rout = grew(was.write_bytes, c.write_bytes) / seconds
    self.read_ms = dr > 0 and grew(was.read_counter_ticks, c.read_counter_ticks) / dr / hz * 1000 or nil
    self.write_ms = dw > 0 and grew(was.write_counter_ticks, c.write_counter_ticks) / dw / hz * 1000 or nil

    local hits = grew(was.cache_hits, c.cache_hits)
    local asks = hits + grew(was.cache_misses, c.cache_misses)

    self.cache = asks > 0 and hits * 100 // asks or self.cache
    fresh = true
  end

  self.last, self.at, self.counts = c, at, c
  return fresh
end

function DRIVE:restart()
  self.last = nil
end

--------------------------------------------------------------------------
-- Rates by a key: a drive's programs (`.programs`), the connections.
--------------------------------------------------------------------------

local FLOWS = {}
FLOWS.__index = FLOWS

function meters.flows(counter_hz)
  return made(FLOWS, nil, { hz = counter_hz, at = nil, seconds = 0, nought = false,
                            was_in = {}, was_out = {}, now_in = {}, now_out = {} })
end

--
-- A reading begun at `at`. `nought` says what a key not seen before
-- means: true, that it began since and every byte it has is new (a
-- connection opened between two readings); false, that it has no rate yet.
--
function FLOWS:begin(at, nought)
  self.seconds = self.at and (at - self.at) / self.hz or 0
  self.at, self.nought = at, nought and true or false
  self.was_in, self.now_in = self.now_in, self.was_in
  self.was_out, self.now_out = self.now_out, self.was_out
  clear(self.now_in)
  clear(self.now_out)
end

-- One key's totals in and out: its bytes a second each way since the last
-- reading, 0 and 0 when there is nothing to measure from.
function FLOWS:rate(key, bytes_in, bytes_out)
  self.now_in[key], self.now_out[key] = bytes_in, bytes_out

  local a, b = self.was_in[key], self.was_out[key]

  if a == nil then
    if not self.nought then return 0, 0 end
    a, b = 0, 0
  end

  local s = self.seconds

  if s <= 0 then return 0, 0 end

  return grew(a, bytes_in) / s, grew(b, bytes_out) / s
end

-- Every key forgotten, and the time: the next reading has nothing before it.
function FLOWS:restart()
  self.at = nil
  clear(self.now_in)
  clear(self.now_out)
end

--------------------------------------------------------------------------
-- The network: `fs.net_stats`.
--------------------------------------------------------------------------

local NET = {}
NET.__index = NET

--
-- After `take`, `rin` and `rout` are the machine's bytes a second in and
-- out (0 until two readings), and `conn(c)` gives one connection's.
--
function meters.network(counter_hz)
  return made(NET, nil, { rin = 0, rout = 0, last = nil, flows = meters.flows(counter_hz) })
end

-- A reading of `fs.net_stats`; true when the totals have new rates. Call
-- `conn` for each of its connections after it.
function NET:take(s, at)
  if not s then return false end

  local flows, last, fresh = self.flows, self.last, false

  -- A connection not there at the last reading opened since, so every byte
  -- it has is new: a short one - a request, an answer, closed - would
  -- otherwise never be seen twice and never have a rate.
  flows:begin(at, last ~= nil)

  if last and flows.seconds > 0 then
    self.rin = grew(last.bytes_in, s.bytes_in) / flows.seconds
    self.rout = grew(last.bytes_out, s.bytes_out) / flows.seconds
    fresh = true
  end

  self.last = s
  return fresh
end

function NET:conn(c)
  return self.flows:rate(c.handle, c.bytes_in, c.bytes_out)
end

-- The totals measured again from the next reading; the connections keep
-- theirs.
function NET:restart()
  self.last = nil
end

--------------------------------------------------------------------------
-- The LLM server: `llm.stats()`.
--------------------------------------------------------------------------

local LLM = {}
LLM.__index = LLM

function meters.llm()
  return made(LLM, nil, { last = nil, answers = nil })
end

--
-- A reading of `llm.stats()`. Answers three things, each nil when there is
-- nothing to say: tokens written a second since the last reading (a
-- column; none when the server's clock did not move on), and, when an
-- answer ended since, its tokens a second and the seconds to its first.
--
function LLM:take(s)
  if not s then return nil end

  local last, rate, answer, first = self.last, nil, nil, nil

  if last and s.up_us > last.up_us then
    rate = grew(last.written_tokens, s.written_tokens) / ((s.up_us - last.up_us) / 1e6)
  end

  if self.answers and s.answers > self.answers and s.answer_us > 0 then
    answer, first = s.answer_tokens * 1e6 / s.answer_us, s.first_us / 1e6
  end

  self.answers, self.last = s.answers, s
  return rate, answer, first
end

return meters
