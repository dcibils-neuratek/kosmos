-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What System Monitor knows: every source asked once a pace, whatever page
-- shows, its differences taken by `meters.lua`, and a column pushed into
-- each history (`docs/sysmonitor.md`, *Sampling, and what each page asks*).
--
--   local sample = use("/Kosmos/Libraries/sysmon/sample.lua")
--   local S = sample.new(counter_hz, 60)
--   S.pace_ticks = counter_hz          -- the window's pace, in counter ticks
--   S:take()                           -- on the window's clock
--
-- **Asked always, so a page opened late has its history.** The pages only
-- read what is here; none of them asks anything of its own in a repaint
-- (*Nothing on the desktop waits on a server*).
--
-- **The window counts nothing.** Every number is the kernel's, a driver's
-- or a server's, and every difference is `meters.lua`'s: this holds the
-- meters, says which reading goes to which, and keeps the histories.
--
-- **Nothing waits on a server, and this window asks eight of them**
-- (`sysmonitor.md`, rule 1). Each source is asked in turn, the cheap ones
-- first; one whose last answer took longer than a quarter of the pace is
-- asked only every fourth pace and is `slow`, which its tile says. A pace
-- a source was not asked in is a gap in its history, never a nought. A
-- call with a deadline (`roadmap.md`) is what takes the rule away.
--
-- **What a sample cost**, for `--tell` and the M700 (`sysmonitor.md`, *The
-- busiest path*): `cost` holds the counter's ticks spent asking and taking
-- differences, the messages and the system calls it made, and the
-- kilobytes it allocated, by `collectgarbage("count")` before and after.

local ui = use("/Kosmos/Libraries/ui.lua")
local meters = use("/Kosmos/Libraries/meters.lua")
local drivelist = use("/Kosmos/Libraries/drivelist.lua")
local hardware = use("/Kosmos/Libraries/hardware.lua")

local M = {}

local S = {}
S.__index = S

-- The libraries only some machines need, loaded when one answers.
local function library(name)
  local ok, lib = pcall(use, "/Kosmos/Libraries/" .. name .. ".lua")

  return ok and lib or nil
end

--------------------------------------------------------------------------
-- The sources, cheapest first. Each `ask` returns its readings, and `take`
-- is handed them with the counter's time they arrived; `messages` and
-- `calls` are what one ask costs, counted rather than measured.
--------------------------------------------------------------------------

local function percent(part, whole)
  return (whole and whole > 0 and part) and (part * 100 // whole) or nil
end

local SOURCES = {}

-- The processors, their heat and their power: two system calls.
SOURCES.cpu = {
  calls = 2, messages = 0,
  ask = function() return sys.cpuload(), sys.info() end,
  take = function(self, load, info, at)
    self.info = info or self.info

    if not self.cpu:take(load) then return false end

    local cpu, cols = self.cpu, self.columns

    for c = 1, self.cores do
      if cpu.taken[c] then ui.history.push(self.core[c], { cpu.kernel[c], cpu.user[c] }, cols) end
      self.temps[c] = load[c] and load[c].temp_c or nil
    end

    ui.history.push(self.all, { cpu.all_kernel, cpu.all_user }, cols)

    local heat = self.hot:take(info or {}, at, load)

    ui.history.push(self.heat_h, { heat and heat.temp_c or nil }, cols)
    ui.history.push(self.watts_h, { heat and heat.watts or nil }, cols)
    return true
  end,
}

-- Memory and the kernel's pools: two messages to the devices server.
SOURCES.machine = {
  calls = 0, messages = 2,
  ask = function() return fs.read("/Devices/kernel"), fs.read("/Devices/memory") end,
  take = function(self, k, m)
    if type(k) == "table" then self.kernel = k end

    meters.totals(k, m, self.totals)

    local t = self.totals

    ui.history.push(self.memory_h, { percent(t.used_mb, t.total_mb) }, self.columns)
    return true
  end,
}

-- Every process: a system call, for the busiest five.
SOURCES.processes = {
  calls = 1, messages = 0,
  ask = function() return sys.processes() end,
  take = function(self, list)
    if not list then return false end

    local rows = self.procs:take(list, self.kernel)
    local top = self.busiest

    for i = #top, 1, -1 do top[i] = nil end

    for _, r in ipairs(rows) do
      local p = r.process

      if r.kernel then
        top[#top + 1] = { id = 0, name = "nebula", pct = r.pct,
                          bytes = ((self.kernel or {}).memory_kb or 0) * 1024 }
      elseif p and not p.exited then
        top[#top + 1] = { id = p.id, name = p.name, pct = r.pct,
                          bytes = (p.held or p.pages or 0) * 4096 }
      end
    end

    self.alive = 0

    for _, p in ipairs(list) do
      if not p.exited then self.alive = self.alive + 1 end
    end

    -- The most first, ties by id: a list that does not shuffle while
    -- nothing changes.
    table.sort(top, function(a, b)
      if a.pct ~= b.pct then return a.pct > b.pct end
      return a.id < b.id
    end)

    for i = #top, 6, -1 do top[i] = nil end

    return true
  end,
}

-- The battery, where there is one: a message.
SOURCES.battery = {
  calls = 0, messages = 1,
  ask = function() return fs.read("/Devices/battery") end,
  take = function(self, b)
    self.battery = (type(b) == "table" and b.present == 1 and b.percent) and b or nil
    return self.battery ~= nil
  end,
}

-- The GPU, where its driver knows one: a message to `intelgpu`.
SOURCES.gpu = {
  calls = 0, messages = 1,
  ask = function(self) return self.gpu_lib.stats() end,
  take = function(self, s)
    if not s then return false end

    if self.gpu:take(s, self.info) then
      ui.history.push(self.gpu_h, { math.floor((self.gpu.busy or 0) * 100 + 0.5) }, self.columns)
    end

    return true
  end,
}

-- The network's totals: a message to the network server.
SOURCES.network = {
  calls = 0, messages = 1,
  ask = function() return fs.net_stats("/Network") end,
  take = function(self, s, _, at)
    if self.net:take(s, at) then
      ui.history.push(self.net_h, { self.net.rin, self.net.rout }, self.columns)
    end

    return s ~= nil
  end,
}

-- The LLM server's counts, while it runs: a message.
SOURCES.llm = {
  calls = 0, messages = 1,
  ask = function(self) return self.llm_lib.stats() end,
  take = function(self, s)
    if not s then
      self.llm_now = nil
      return false
    end

    local wrote, answer = self.llm:take(s)

    self.llm_now = s
    if answer then self.llm_last = answer end

    ui.history.push(self.llm_h, { wrote }, self.columns)
    return true
  end,
}

-- Each by its name, which `slow` and `--tell` say.
for name, src in pairs(SOURCES) do src.name = name end

-- A drive: a message to whichever server holds it, named by its path.
local function drive_source(d)
  return {
    name = d.key, calls = 0, messages = 1, drive = d,
    ask = function() return fs.read(d.path .. "/.device") end,
    take = function(self, c, _, at)
      local m = self.drive[d.key]

      if m:take(c, at) then ui.history.push(m.history, { m.rin, m.rout }, self.columns) end

      return not m.silent
    end,
  }
end

--------------------------------------------------------------------------
-- What there is to ask, worked out when the window opens: a part the
-- machine does not have gets no source and no tile.
--------------------------------------------------------------------------

function S:find_drives()
  local ok, list = pcall(drivelist.monitored)

  self.drives = ok and list or self.drives or {}

  for _, d in ipairs(self.drives) do
    if not self.drive[d.key] then
      self.drive[d.key] = meters.drive(self.hz, { history = {} })
    end
  end
end

function S:arrange()
  local list = { SOURCES.cpu, SOURCES.machine, SOURCES.processes }

  if self.has_battery then list[#list + 1] = SOURCES.battery end
  if self.gpu_lib then list[#list + 1] = SOURCES.gpu end
  if self.card then list[#list + 1] = SOURCES.network end

  for _, d in ipairs(self.drives) do list[#list + 1] = drive_source(d) end

  if self.llm_now then list[#list + 1] = SOURCES.llm end

  -- What each source knew of itself - slow, how long it took - kept across
  -- the list made again.
  local was = self.sources or {}
  local by = {}

  for _, src in ipairs(was) do by[src.drive and src.drive.key or src] = src end

  for i, src in ipairs(list) do
    local before = by[src.drive and src.drive.key or src]

    if before and before.drive then list[i] = before end
  end

  self.sources = list
end

-- Whether the LLM server answers now: asked when the window opens and every
-- tenth pace after, since it starts and stops on its own.
function S:look_for_llm()
  self.llm_lib = self.llm_lib or library("llm")

  if not self.llm_lib then return end

  local ok, s = pcall(self.llm_lib.stats)

  self.llm_now = ok and s or nil
end

function M.new(counter_hz, columns)
  local load0 = sys.cpuload() or {}
  local cores = math.max(1, #load0)
  local info = sys.info() or {}
  local self = setmetatable({
    hz = counter_hz, columns = columns or 60, cores = cores, samples = 0,
    pace_ticks = counter_hz, info = info, kernel = nil,
    cpu = meters.cpu(counter_hz, load0, cores),
    hot = meters.heat(counter_hz, info, sys.ticks()),
    procs = meters.processes(), net = meters.network(counter_hz),
    gpu = meters.gpu(counter_hz), llm = meters.llm(),
    drive = {}, drives = {}, totals = {}, temps = {}, busiest = {}, alive = 0,
    cost = nil,
  }, S)

  self:clear()

  -- The battery, where the board reads one.
  local b = fs.read("/Devices/battery")

  self.has_battery = type(b) == "table" and b.present == 1
  if self.has_battery then self.battery = b end

  -- The GPU, where its driver knows one; the driver's sentence where not
  -- ("no GPU this driver knows"), which the Overview says.
  local gpu_lib = library("gpu")

  if gpu_lib then
    local ok, s, why = pcall(gpu_lib.stats)

    if ok and s then
      self.gpu_lib, self.gpu_model = gpu_lib, s.model
      self.gpu:take(s, info)
    else
      self.gpu_why = ok and tostring(why) or tostring(s)
    end
  end

  -- The network's card by its name, and where it is on the network.
  local ok, driven = pcall(hardware.network, sys.bus())

  driven = ok and driven or {}

  local have, net = pcall(fs.net_info, "/Network")

  self.net_info = have and type(net) == "table" and net or nil
  self.card = (driven[1] and driven[1].name) or (self.net_info and self.net_info.card and "Network card")

  self:find_drives()
  self:look_for_llm()
  self:arrange()
  return self
end

-- Every history begun again: a pace changed, so columns taken at one pace
-- beside columns at another would be a time axis that lies.
function S:clear()
  self.core, self.all = {}, {}

  for c = 1, self.cores do self.core[c] = {} end

  self.heat_h, self.watts_h, self.memory_h = {}, {}, {}
  self.gpu_h, self.net_h, self.llm_h = {}, {}, {}

  for _, m in pairs(self.drive) do m.history = {} end
end

function S:restart()
  self:clear()
  self.cpu:restart(sys.cpuload())
  self.net:restart()
  self.gpu:restart()

  for _, m in pairs(self.drive) do m:restart() end

  self.samples = 0
end

--------------------------------------------------------------------------
-- A sample: every source asked in turn, on the window's clock.
--------------------------------------------------------------------------

function S:take()
  self.samples = self.samples + 1

  -- A stick comes and goes, and the LLM server starts and stops.
  if self.samples % 5 == 0 then
    self:find_drives()
    self:arrange()
  end

  if self.samples % 10 == 0 and not self.llm_now then
    self:look_for_llm()
    if self.llm_now then self:arrange() end
  end

  local quarter = self.pace_ticks // 4
  local asking, taking, messages, calls = 0, 0, 0, 0
  local kb = collectgarbage("count")

  for _, src in ipairs(self.sources) do
    if src.slow and self.samples % 4 ~= 0 then
      -- Not asked this pace: a gap in its history, not a nought.
      src.skipped = true
    else
      local t0 = sys.ticks()
      local ok, a, b = pcall(src.ask, self)
      local t1 = sys.ticks()

      src.took, src.slow, src.skipped = t1 - t0, (t1 - t0) > quarter, false
      messages, calls = messages + src.messages, calls + src.calls

      if ok then
        src.answered = src.take(self, a, b, t1) and true or false
      else
        src.answered = false
      end

      asking, taking = asking + (t1 - t0), taking + (sys.ticks() - t1)
    end
  end

  self.cost = { asking = asking, taking = taking, messages = messages, calls = calls,
                kb = collectgarbage("count") - kb }
end

-- Whether a source has been slow to answer: its tile says so.
function S:slow(name)
  for _, src in ipairs(self.sources) do
    if src.name == name then return src.slow end
  end

  return false
end

-- The machine's busy now, in per cent: the mean of the cores.
function S:busy()
  local cpu, sum = self.cpu, 0

  for c = 1, self.cores do sum = sum + cpu.user[c] + cpu.kernel[c] end

  return sum // self.cores
end

-- How fast the machine runs now, the mean of the cores that said, in GHz.
function S:speed()
  local speeds = self.cpu.speeds

  if not speeds then return nil end

  local sum, n = 0, 0

  for c = 1, self.cores do
    if speeds[c] then sum, n = sum + speeds[c], n + 1 end
  end

  return n > 0 and (sum / n / 1000) or nil
end

-- The hottest core's temperature, or nil where no core says.
function S:hottest()
  local most = nil

  for c = 1, self.cores do
    local t = self.temps[c]

    if t and (not most or t > most) then most = t end
  end

  return most
end

return M
