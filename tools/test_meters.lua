-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `/Kosmos/Libraries/meters.lua` on this machine: every source's
-- differences from readings in the shapes the sources give, the M700's
-- (`docs/sysmonitor.md` S2) - an i7-6700 at a 3.41 GHz counter, its HD 530
-- asleep in RC6 on 17 October and rendering Flight at 37-42%, 52 MB/s over
-- its Ethernet, phi-2 writing 4.51 tokens a second - with a counter that
-- wrapped, a server started again, a source that vanished, and a first
-- reading with nothing before it.
--
--   build/host/lua tools/test_meters.lua            -- must pass
--   build/host/lua tools/test_meters.lua CONTROL    -- must fail
--
-- A control breaks the library itself - its source changed as it is
-- loaded - so a pass under one says these checks do not bite:
--
--   no-seconds   a rate not divided by the time between readings
--   no-wrap      a counter that went backwards subtracted as it is
--   no-first     a first reading answered as though nought came before it
--   no-lanes     the Scheduler's switches counted without the time between readings

local control = arg and arg[1]

local CONTROLS = {
  ["no-seconds"] = { { "/ seconds", "/ 1" }, { "/ flows.seconds", "/ 1" }, { "/ s, grew", "/ 1, grew" } },
  ["no-wrap"]    = { { "return b >= a and b - a or 0", "return b - a" } },
  ["no-first"]   = { { "local a, b = self.was_in[key], self.was_out[key]",
                       "local a, b = self.was_in[key] or 0, self.was_out[key] or 0" },
                     { "  if self.idle then\n", "  if true then self.idle = self.idle or 0; self.busy = self.busy or 0\n" } },
  ["no-lanes"]   = { { "* self.hz / (at - self.at)", "* 1" } },
}

if control and not CONTROLS[control] then
  print("test_meters: no such control: " .. control)
  os.exit(2)
end

local checks, fails = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    fails = fails + 1
    print("  FAIL: " .. what)
  end
end

local function near(a, b, within)
  return type(a) == "number" and math.abs(a - b) <= (within or 1e-6)
end

local function read(path)
  local f = assert(io.open(path))
  local text = f:read("a")

  f:close()
  return text
end

-- The guest's `use`, over the tree: the libraries the meters load are pure
-- at load time and touch `fs` only when a window asks them to.
function use(path)
  local name = path:match("^/Kosmos/Libraries/(.+)$")

  return assert(loadfile("user/lib/" .. name))()
end

local source = read("user/lib/meters.lua")

for _, change in ipairs(control and CONTROLS[control] or {}) do
  local plain = change[1]:gsub("%p", "%%%0")
  local n

  source, n = source:gsub(plain, (change[2]:gsub("%%", "%%%%")))
  assert(n > 0, "the control " .. control .. " found nothing to change: " .. change[1])
end

local meters = assert(load(source, "@user/lib/meters.lua"))()

local HZ = 3410000000                 -- the M700's counter, the TSC
local SECOND = HZ

--------------------------------------------------------------------------
-- 1. The processor, a core at a time: four cores of the i7-6700's eight.
--------------------------------------------------------------------------

-- A core's reading as `sys.cpuload()` gives it: ticks idle and busy, the
-- busy time's user and kernel in counter units, APERF and MPERF.
local function core(idle, busy, user, kernel, actual, reference, temp)
  return { idle = idle, busy = busy, user_counter = user, kernel_counter = kernel,
           clock_actual = actual, clock_reference = reference, temp_c = temp }
end

do
  -- user 3 to kernel 1 of a half-busy core: 50% busy, 13 of it the kernel's.
  local u, k = meters.split(core(100, 100, 0, 0), core(150, 150, 3000, 1000))

  check(u == 37 and k == 13, ("half busy, a quarter of it the kernel's: %s/%s, not 37/13")
        :format(tostring(u), tostring(k)))

  -- No tick passed: nothing, not a share.
  u, k = meters.split(core(100, 100, 0, 0), core(100, 100, 0, 0))
  check(u == 0 and k == 0, "no tick passed and a share was given")

  -- Busy with no counters (an older kernel): all of busy is the user's.
  u, k = meters.split({ idle = 0, busy = 0 }, { idle = 25, busy = 75 })
  check(u == 75 and k == 0, "busy with no counters: " .. tostring(u) .. "/" .. tostring(k))
end

do
  local first = { core(1000, 0, 0, 0, 0, 0, 38), core(1000, 0, 0, 0, 0, 0, 39),
                  core(1000, 0, 0, 0, 0, 0, 37), core(1000, 0, 0, 0, 0, 0, 40) }
  local cpu = meters.cpu(HZ, first)

  check(cpu.cores == 4, "four cores read as " .. tostring(cpu.cores))
  check(cpu.user[1] == 0 and cpu.kernel[1] == 0 and cpu.taken[1] == false and cpu.speeds == nil,
        "a first reading said something before a second")

  -- A second later: core 1 flat out at 3.41 GHz, a fifth in the kernel;
  -- core 2 half busy at 1.70; core 3 idle and asleep; core 4 a quarter.
  local second = {
    core(1000, 100, 8e8, 2e8, 3410000000, 3410000000, 62),
    core(1050, 50, 5e8, 0, 852500000, 1705000000, 51),
    core(1100, 0, 0, 0, 0, 0, 37),
    core(1075, 25, 1e8, 1e8, 425000000, 425000000, 44),
  }

  check(cpu:take(second) == true, "a second reading was not taken")
  check(cpu.user[1] == 80 and cpu.kernel[1] == 20, ("core 1: %d/%d, not 80/20")
        :format(cpu.user[1], cpu.kernel[1]))
  check(cpu.user[2] == 50 and cpu.kernel[2] == 0, ("core 2: %d/%d"):format(cpu.user[2], cpu.kernel[2]))
  check(cpu.user[3] == 0 and cpu.kernel[3] == 0, "an idle core was busy")
  -- Half of a quarter is 12.5, and the kernel's share is rounded to nearest.
  check(cpu.user[4] == 12 and cpu.kernel[4] == 13, ("core 4, a quarter half in the kernel: %d/%d")
        :format(cpu.user[4], cpu.kernel[4]))
  check(cpu.all_user == (80 + 50 + 0 + 12) // 4 and cpu.all_kernel == (20 + 0 + 0 + 13) // 4,
        ("the machine: %d/%d"):format(cpu.all_user, cpu.all_kernel))
  check(cpu.speeds and near(cpu.speeds[1], 3410) and near(cpu.speeds[2], 1705) and cpu.speeds[3] == nil,
        "the speeds: " .. tostring(cpu.speeds and cpu.speeds[1]) .. ", "
        .. tostring(cpu.speeds and cpu.speeds[2]))
  check(cpu.taken[1] and cpu.taken[4], "a measured core was not marked taken")

  -- The source did not answer: nothing changes.
  check(cpu:take(nil) == false and cpu.user[1] == 80, "no reading changed the meter")

  -- A core missing from the next reading keeps what it last said, and is
  -- not marked taken; the others go on from the reading before.
  local third = { core(1100, 100, 8e8, 2e8), nil, core(1100, 100, 1e8, 1e8), core(1175, 25, 1e8, 1e8) }

  third[2] = nil
  cpu:take(third)
  check(cpu.taken[2] == false and cpu.user[2] == 50, "a core that vanished lost its last share")
  check(cpu.user[1] == 0 and cpu.taken[1], "core 1 idle since: " .. cpu.user[1])
  check(cpu.user[3] == 50 and cpu.kernel[3] == 50, ("core 3 now busy, half in the kernel: %d/%d")
        :format(cpu.user[3], cpu.kernel[3]))

  -- Begun again from a reading now: the next difference is from it.
  cpu:restart({ core(0, 0, 0, 0), core(0, 0, 0, 0), core(0, 0, 0, 0), core(0, 0, 0, 0) })
  cpu:take({ core(10, 90, 9, 0), core(100, 0, 0, 0), core(50, 50, 1, 1), core(0, 100, 0, 1) })
  check(cpu.user[1] == 90 and cpu.user[3] == 25 and cpu.kernel[4] == 100,
        "after a restart: " .. cpu.user[1] .. ", " .. cpu.user[3] .. ", " .. cpu.kernel[4])
  cpu:restart(nil)
  check(cpu.last ~= nil, "a restart with no reading forgot the last one")
end

--------------------------------------------------------------------------
-- 2. The processor's heat and power: `sys.info()`, the M700's package.
--------------------------------------------------------------------------

do
  local info = { package_c = 54, limit_c = 100, energy_uj = 900000000, power_limit_mw = 65000,
                 power_burst_mw = 81250, held_hot = 0, held_power = 0 }
  local heat = meters.heat(HZ, info, 0)
  local later = { package_c = 62, limit_c = 100, energy_uj = 900000000 + 56000000,
                  power_limit_mw = 65000, power_burst_mw = 81250, held_hot = 0, held_power = 1 }
  local h = heat:take(later, 2 * SECOND, {})

  check(h and h.temp_c == 62 and near(h.watts, 28) and h.limit_w == 65 and h.held == "power",
        "56 J over 2 s, held at its limit: " .. tostring(h and h.watts) .. " W, "
        .. tostring(h and h.held))
  check(heat.at == 2 * SECOND and heat.info == later, "the heat kept its first reading")

  -- QEMU's processor: nothing known, and no table claiming otherwise.
  local q = meters.heat(62500000, {}, 0)

  check(q:take({}, 62500000, { core(0, 0, 0, 0) }) == nil, "QEMU's processor was given a heat")
end

--------------------------------------------------------------------------
-- 3. The bars' busy, from `/Devices/kernel`.
--------------------------------------------------------------------------

do
  local busy = meters.busy()

  check(busy:take({ idle_ticks = 40000, busy_ticks = 10000 }) == nil,
        "a first reading of busy said a share")
  check(busy:take({ idle_ticks = 40750, busy_ticks = 10250 }) == 25,
        "750 idle beside 250 busy was not 25%")
  check(busy:take(nil) == nil and busy:take("no") == nil, "no reading gave a share")
  check(busy:take({ idle_ticks = 41000, busy_ticks = 11000 }) == 75,
        "after a missing reading busy was not measured from the last one")
  check(busy:take({ idle_ticks = 41000, busy_ticks = 11000 }) == nil, "no tick passed and a share was given")
  -- Counters that went backwards (a reading from somewhere else): nought,
  -- never a share below zero.
  check(busy:take({ idle_ticks = 100, busy_ticks = 100 }) == nil, "counters gone backwards gave a share")

  -- A share of a whole, which the prompt's programs take: memory used of
  -- all there is (`htop`), the GPU's rows of a step's and a layer's
  -- neurons that fired (`llm`). Rounded down; nothing of a whole of nought.
  check(meters.share(1464, 16384) == 8, "1464 MB of 16384 was not 8%")
  check(meters.share(2559, 10240) == 24 and meters.share(10240, 10240) == 100,
        "2559 of 10240 rows was not 24%, or all of them not 100%")
  check(meters.share(0, 0) == nil and meters.share(5, 0) == nil, "a whole of nought gave a share")
  check(meters.busy_share(750, 250) == meters.share(250, 1000), "busy is not busy's share of every tick")
end

--------------------------------------------------------------------------
-- 4. Memory and the pools, and the processes' totals.
--------------------------------------------------------------------------

do
  local used, total = meters.memory({ total_mb = 16384, free_mb = 14920 })

  check(used == 1464 and total == 16384, "memory: " .. tostring(used) .. " of " .. tostring(total))
  check(meters.memory(nil) == nil and meters.memory({ total_mb = 16384 }) == nil,
        "a reading that is not one gave a memory")

  local out = { used_mb = 0, total_mb = 1, threads = 0 }
  local k = { threads = 141, threads_max = 32768, processes = 47, processes_max = 4096,
              endpoints = 212, endpoints_max = 8192, spaces = 47, spaces_max = 4096 }

  meters.totals(k, { total_mb = 16384, free_mb = 14920 }, out)
  check(out.threads == 141 and out.threads_max == 32768 and out.spaces == 47 and out.endpoints_max == 8192
        and out.used_mb == 1464 and out.total_mb == 16384, "the totals were not written")
  meters.totals(nil, nil, out)
  check(out.threads == 141 and out.used_mb == 1464, "a missing reading wiped the totals")

  -- Processes: procshare's rows, and the kernel's threads what is left.
  local p = meters.processes()
  local list = { { id = 1, name = "init", ticks = 100, threads = 1 },
                 { id = 9, name = "windowserver", ticks = 1000, threads = 3 },
                 { id = 30, name = "flight", ticks = 5000, threads = 6 },
                 { id = 31, name = "gone", ticks = 7, threads = 2, exited = true } }
  local rows = p:take(list, { idle_ticks = 1000, busy_ticks = 1000, threads = 22 })

  check(#rows == 4 and rows[1].pct == 0, "a first look gave shares, or not a row a process")
  check(p.count == 4 and p.threads == 22 and p.kernel_threads == 22 - (1 + 3 + 6),
        ("totals: %d processes, %d threads, %d in the kernel"):format(p.count, p.threads, p.kernel_threads))

  list[3] = { id = 30, name = "flight", ticks = 5400, threads = 6 }
  rows = p:take(list, { idle_ticks = 1500, busy_ticks = 1500, threads = 22 })

  local flight, kernel = nil, nil

  for _, r in ipairs(rows) do
    if r.kernel then kernel = r.pct elseif r.process.name == "flight" then flight = r.pct end
  end

  check(flight == 40 and kernel == 10, ("flight %s%%, the kernel %s%%: not 40 and 10")
        :format(tostring(flight), tostring(kernel)))
  check(p:take(list, nil) and p.threads == 0 and p.kernel_threads == 0,
        "no kernel reading left the threads as they were")
end

--------------------------------------------------------------------------
-- 5. The GPU: the M700's HD 530, `gpu.stats` and `sys.info()`.
--------------------------------------------------------------------------

local function snapshot(at, f)
  local s = { device = 0x1912, model = "HD Graphics 530", width = 1720, height = 1440,
              mhz = 0, max_mhz = 1150, min_mhz = 350, rc6 = 0, flips = 0, frames = 0,
              blitter_us = 0, render_us = 0, ticks = at,
              buffers = { { holder = 0, bytes = 2433 * 4096 }, { holder = 9, bytes = 2433 * 4096 },
                          { holder = 11, bytes = 4 * 4096 } } }

  for k, v in pairs(f or {}) do s[k] = v end

  return s
end

do
  local g = meters.gpu(HZ)

  check(g:take(nil, {}) == false and g.now == nil, "no GPU gave a snapshot")
  check(g:take(snapshot(0, { rc6 = 5000, frames = 100, flips = 7 }), { gpu_energy_uj = 10 }) == false
        and g.rate == nil, "a first snapshot gave rates")

  -- 17 October, idle: 2.0 s, all of it in RC6 at 1280 ns a count, 60
  -- frames and 3.5 flips a second.
  local idle = snapshot(2 * SECOND, { rc6 = 5000 + 1562500, frames = 220, flips = 14 })

  check(g:take(idle, { gpu_energy_uj = 10 }) == true, "a second snapshot gave no rates")
  check(near(g.rate.asleep, 1) and near(g.rate.frames, 60) and near(g.rate.flips, 3.5),
        ("idle: asleep %s, %s frames, %s flips a second"):format(tostring(g.rate.asleep),
        tostring(g.rate.frames), tostring(g.rate.flips)))
  check(g.busy == 0 and g.watts == 0 and g.bytes == (2433 * 2 + 4) * 4096,
        "idle: busy " .. tostring(g.busy) .. ", " .. tostring(g.watts) .. " W, " .. tostring(g.bytes))
  check(g.now == idle, "the latest snapshot is not the one taken")

  -- Flight in front: render 40% of a second, the blitter 2%; PP1's
  -- microjoules wrapping past 2^32 in the second, 7.5 J; RC6's count too.
  local flight = snapshot(3 * SECOND, { rc6 = 0xffffff00, render_us = 400000, blitter_us = 20000,
                                        frames = 280, flips = 38, mhz = 1100 })

  g:take(flight, { gpu_energy_uj = 0xffffffff - 2499999 })
  check(near(g.rate.render, 0.4) and near(g.busy, 0.4) and near(g.rate.flips, 24),
        "Flight: render " .. tostring(g.rate.render) .. ", busy " .. tostring(g.busy))

  local wrapped = snapshot(4 * SECOND, { rc6 = 0x00000100 + 390625 - 0x200, render_us = 770000,
                                         blitter_us = 20000, frames = 340, flips = 62, mhz = 1100 })

  g:take(wrapped, { gpu_energy_uj = 5000000 })
  check(near(g.watts, 7.5), "PP1 wrapped past 32 bits: " .. tostring(g.watts) .. " W, not 7.5")
  check(near(g.rate.asleep, 0.5, 1e-3), "RC6 wrapped past 32 bits: asleep " .. tostring(g.rate.asleep))
  check(near(g.rate.render, 0.37), "37% rendering read " .. tostring(g.rate.render))

  -- The driver went away: the meter keeps its last, and the next snapshot
  -- is measured from the last that came.
  check(g:take(nil, {}) == false and g.now == wrapped and near(g.rate.render, 0.37),
        "a GPU that stopped answering lost the last rates")
  g:take(snapshot(6 * SECOND, { rc6 = 0x00000100 + 390625 - 0x200, render_us = 770000 + 840000,
                                frames = 460, flips = 62 }), { gpu_energy_uj = 5000000 })
  check(near(g.rate.render, 0.42) and near(g.rate.frames, 60), "measured across a gap: render "
        .. tostring(g.rate.render) .. ", frames " .. tostring(g.rate.frames))

  -- Begun again: no rates until a second snapshot, the last ones kept.
  g:restart()
  check(g:take(snapshot(7 * SECOND), {}) == false and near(g.rate.render, 0.42),
        "a restart gave rates from one snapshot, or forgot the last")

  -- No power plane: no watts said, the last ones kept.
  local q = meters.gpu(HZ)

  q:take(snapshot(0), {})
  q:take(snapshot(SECOND), {})
  check(q.watts == nil, "a GPU with no power plane was given watts")

  -- No time between two snapshots: no rate, never a division by nought.
  q:take(snapshot(SECOND), {})
  check(q.rate == nil, "no time passed and the GPU had rates")
end

--------------------------------------------------------------------------
-- 6. A drive: its server's `.device`, the M700's SATA disk.
--------------------------------------------------------------------------

local function device(f)
  local d = { reads = 0, writes = 0, read_bytes = 0, write_bytes = 0, read_counter_ticks = 0,
              write_counter_ticks = 0, cache_hits = 0, cache_misses = 0 }

  for k, v in pairs(f or {}) do d[k] = v end

  return d
end

do
  local s = meters.drive(HZ, { history = {} })

  check(s.history and s.rin == 0 and s.rout == 0, "a drive's meter lost the caller's fields")
  check(s:take(device({ reads = 100, read_bytes = 1e6, cache_hits = 10, cache_misses = 10 }), 0) == false
        and s.rin == 0 and s.counts, "a first reading of a drive gave rates")

  -- Two seconds: 200 MB read in 1600 reads at 0.25 ms each, 40 MB written
  -- in 80 at 2 ms, 300 looks of which 240 the cache answered.
  local c = device({ reads = 1700, read_bytes = 1e6 + 200e6, read_counter_ticks = 1600 * HZ // 4000,
                     writes = 80, write_bytes = 40e6, write_counter_ticks = 80 * HZ // 500,
                     cache_hits = 250, cache_misses = 70 })

  check(s:take(c, 2 * SECOND) == true, "a second reading of a drive gave no rates")
  check(near(s.rin, 100e6) and near(s.rout, 20e6), ("%s and %s bytes a second, not 100 MB and 20 MB")
        :format(tostring(s.rin), tostring(s.rout)))
  check(near(s.read_ms, 0.25, 1e-4) and near(s.write_ms, 2, 1e-4), ("a read %s ms, a write %s ms")
        :format(tostring(s.read_ms), tostring(s.write_ms)))
  check(s.cache == 80 and s.counts == c, "the cache's share: " .. tostring(s.cache))

  -- Nothing happened: no times, the cache's share as it was.
  check(s:take(c, 3 * SECOND) and s.rin == 0 and s.read_ms == nil and s.cache == 80,
        "an idle second changed the cache or gave times")

  -- The server started again: its counts from nought, no rate below zero.
  s:take(device({ reads = 10, read_bytes = 4096 }), 4 * SECOND)
  check(s.rin == 0 and s.rout == 0, "a drive's server started again read " .. s.rin .. " bytes a second")

  -- The drive vanished: silent, and the rates it had kept.
  s:take(device({ reads = 10, read_bytes = 4096 + 1048576 }), 5 * SECOND)
  check(near(s.rin, 1048576), "after the restart the rate was " .. s.rin)
  check(s:take(nil, 6 * SECOND) == false and s.silent and near(s.rin, 1048576),
        "a drive that vanished was not silent, or lost its rates")

  -- No time passed: no rate.
  s:restart()
  s:take(device(), 7 * SECOND)
  check(s:take(device({ read_bytes = 1e9 }), 7 * SECOND) == false, "no time passed and a drive had a rate")
end

--------------------------------------------------------------------------
-- 7. Rates by a key, and the network: the M700's 52 MB/s.
--------------------------------------------------------------------------

do
  local f = meters.flows(HZ)

  f:begin(0, false)
  check(select(2, f:rate(30, 1000, 2000)) == 0, "a first reading gave a program a rate")
  f:begin(SECOND, false)

  local rin, rout = f:rate(30, 1000 + 500, 2000 + 1500)
  local nin = f:rate(31, 9e6, 0)

  check(near(rin, 500) and near(rout, 1500), "a program's rates: " .. rin .. ", " .. rout)
  check(nin == 0, "a program not seen before had a rate without nought before it")

  -- Keys come and go; one gone and back is new again.
  f:begin(2 * SECOND, true)
  f:rate(31, 9e6 + 1e6, 0)
  f:begin(3 * SECOND, true)
  rin = f:rate(30, 1500 + 3000, 0)
  check(near(rin, 4500), "a key gone a reading counted from nought: " .. rin)

  f:restart()
  f:begin(4 * SECOND, true)
  check(f:rate(30, 1e9, 0) == 0, "after a restart a key had a rate with no time before it")
end

do
  local net = meters.network(HZ)
  local function stats(i, o, conns) return { bytes_in = i, bytes_out = o, connections = conns or {} } end

  check(net:take(nil, 0) == false, "no stack answered and the network had rates")
  check(net:take(stats(1e9, 2e9, { { handle = 4, bytes_in = 1e8, bytes_out = 0 } }), 0) == false
        and net.rin == 0, "a first reading of the network gave rates")
  check(net:conn({ handle = 4, bytes_in = 1e8, bytes_out = 0 }) == 0,
        "a connection in the first reading had a rate")

  -- 100 MB from the Mac in 1.92 s: 52 MB/s in, 1.3 MB/s of answers out; a
  -- connection opened since counts from nought.
  local at = SECOND * 192 // 100

  check(net:take(stats(1e9 + 100e6, 2e9 + 2.5e6), at) == true, "a second reading gave no rates")
  check(near(net.rin, 100e6 / 1.92, 1) and near(net.rout, 2.5e6 / 1.92, 1),
        "the network: " .. net.rin .. " in, " .. net.rout .. " out")

  local rin = net:conn({ handle = 4, bytes_in = 1e8 + 96e6, bytes_out = 0 })
  local fresh = net:conn({ handle = 9, bytes_in = 4e6, bytes_out = 0 })

  check(near(rin, 50e6, 1) and near(fresh, 4e6 / 1.92, 1),
        "connections: " .. rin .. " and a new one " .. fresh)

  -- The stack started again: its totals from nought, no rate below zero.
  -- Each reading says its connections, as Network Monitor's loop does.
  net:take(stats(5000, 100), at + SECOND)
  net:conn({ handle = 4, bytes_in = 1e8 + 96e6, bytes_out = 0 })
  check(net.rin == 0 and net.rout == 0, "a stack started again read " .. net.rin)

  -- The totals begun again keep each connection's.
  net:restart()
  check(net:take(stats(6000, 100), at + 2 * SECOND) == false, "a restart gave totals from one reading")
  check(net:conn({ handle = 4, bytes_in = 1e8 + 96e6 + 1e6, bytes_out = 0 }) > 0,
        "a restart of the totals forgot the connections")
  check(net:conn({ handle = 77, bytes_in = 1e6, bytes_out = 0 }) == 0,
        "after a restart a new connection counted from nought with nothing before it")
end

--------------------------------------------------------------------------
-- 8. The LLM server: phi-2 on the M700, `llm.stats()`.
--------------------------------------------------------------------------

do
  local l = meters.llm()
  local function stats(up, written, answers, f)
    local s = { up_us = up, written_tokens = written, answers = answers, answer_tokens = 0,
                answer_us = 0, first_us = 0 }

    for k, v in pairs(f or {}) do s[k] = v end

    return s
  end

  check(l:take(nil) == nil, "no server gave a rate")
  check(l:take(stats(10e6, 100, 3)) == nil, "a first reading gave a rate")

  -- Two seconds writing at 4.51 tokens a second: 9.02 tokens is 9, so the
  -- column says 4.5; an answer of 40 tokens in 8.87 s ended, its first word
  -- after 1.2 s.
  local rate, answer, first = l:take(stats(12e6, 109, 4, { answer_tokens = 40, answer_us = 8869180,
                                                          first_us = 1200000 }))

  check(near(rate, 4.5) and near(answer, 4.51, 1e-3) and near(first, 1.2),
        ("phi-2: %s tokens a second, an answer at %s, first word %s s")
        :format(tostring(rate), tostring(answer), tostring(first)))

  -- Nothing ended since: no answer, the column at nought.
  rate, answer = l:take(stats(14e6, 109, 4, { answer_tokens = 40, answer_us = 8869180 }))
  check(rate == 0 and answer == nil, "an answer counted twice, or an idle column not nought")

  -- The server started again: its clock and counts from nought - no
  -- column, then measured from the new start, never below zero.
  rate = l:take(stats(1e6, 0, 0))
  check(rate == nil, "a server started again gave a column across its restart")
  rate = l:take(stats(3e6, 10, 0))
  check(near(rate, 5), "after a restart the rate was " .. tostring(rate))

  -- Tokens fewer with the clock on (a count from somewhere else): nought.
  rate = l:take(stats(5e6, 2, 0))
  check(rate == 0, "a count gone backwards gave " .. tostring(rate))
end

-- Who ran (`meters.lanes`, S6): two cores read two seconds apart, so a
-- rate that forgot the time between readings is caught (`no-lanes`).
do
  local lanes = meters.lanes(HZ, 6)
  local function lane(sw, keys)
    return { ticks = 0, switches = sw, bands = "", waits = { 0, 0, 0, 0, 0, 0, keys } }
  end

  check(lanes:take({ lane(100, 0), lane(50, 0) }, 0, 2) == false and lanes.switches == nil,
        "lanes: a first reading has no rate")
  lanes:take({ lane(1100, 0), lane(2050, HZ // 500) }, 2 * HZ, 2)
  check(lanes.switches == 1500, "lanes: switches a second across the cores, two seconds apart")
  check(math.abs(lanes.keys_ms - 2) < 1e-9, "lanes: the keys' longest wait, in ms")
  lanes:take({ lane(10, 0), false }, 3 * HZ, 2)
  check(lanes.switches == 0 and lanes.lanes[2].switches == 2050,
        "lanes: a counter that went back is nought, and a core that gave none keeps its lane")
end

print(("test_meters: %d checks, %d failed%s"):format(checks, fails,
      control and (" (control " .. control .. ")") or ""))
os.exit(fails == 0 and 0 or 1)
