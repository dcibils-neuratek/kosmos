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
-- **Each process's share and memory over the minute** (S4), for the
-- Processes page's strip: kept for every process from the moment it is
-- seen, so a process chosen after a minute has its minute. Two flat rings
-- of numbers a process - no table a sample - and only the chosen one is
-- made into the lists `ui.history` draws (`S:follow`), when it is chosen.
--
-- **Who ran, asked always** (S6): `sys.sched_lane(c)` for each core, a
-- system call each, its switches a second and the keys' longest wait
-- `meters.lanes`', for the CPU page's lanes and the Overview's switches.
--
-- **Asked only while their page shows** (*Sampling*): the window
-- manager's list of windows, for Processes' **draws** column, once
-- `S:want("windows", true)`; its rendering mode, for the GPU page
-- (`"rendering"`); which application has game mode, for the CPU page
-- (`"game"`, S6); and the drives' `.programs` (`"programs"`) - the chosen
-- drive's while Disks shows, every drive's while Processes shows, whose
-- strip says which drives the chosen process reads and writes (S5). The
-- connections come with the network's totals every pace, since a rate is
-- a difference that needs every reading, and are made into rows only
-- while Network or Processes shows (`"connections"`).
--
-- **A window that shows one part** - GPU Monitor, Disk Monitor and Network
-- Monitor, until S9 retires them, draw System Monitor's page for it - asks
-- only that part: `sample.new(counter_hz, 60, { gpu = true, ... })` names
-- the parts, and nil is all of them.
--
-- **What a sample cost**, for `--tell` and the M700 (`sysmonitor.md`, *The
-- busiest path*): `cost` holds the counter's ticks spent asking and taking
-- differences, the messages and the system calls it made, and the
-- kilobytes it allocated, by `collectgarbage("count")` before and after.

local ui = use("/Kosmos/Libraries/ui.lua")
local meters = use("/Kosmos/Libraries/meters.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local proctable = use("/Kosmos/Libraries/proctable.lua")
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

--------------------------------------------------------------------------
-- How a few things are said, by every page and by the windows that draw
-- one: moved here from Network Monitor, which said them first.
--------------------------------------------------------------------------

-- An address of four bytes, as the network server keeps one.
function M.address(a)
  return #(a or "") == 4 and ("%d.%d.%d.%d"):format(a:byte(1, 4)) or "?"
end

-- A count of packets: 1234 as "1.2 K".
function M.count(n)
  n = n or 0

  if n < 1000 then return tostring(n) end

  local units, i = { "", "K", "M", "G" }, 1

  while n >= 1000 and i < #units do n, i = n / 1000, i + 1 end

  return n >= 100 and ("%.0f %s"):format(n, units[i]) or ("%.1f %s"):format(n, units[i])
end

-- How long since: "40 s", "12 min", "2 h 14 min".
function M.since(seconds)
  local m = seconds // 60

  if m < 1 then return ("%d s"):format(seconds) end
  if m < 60 then return ("%d min"):format(m) end

  return ("%d h %d min"):format(m // 60, m % 60)
end

-- The GPU's memory graph's top: room for the screens and more.
M.GPU_MEMORY = 256 * 1048576

-- Who a process id is, from the names the last sample heard.
function S:holder(id)
  if id == 0 then return "the firmware" end

  return self.names[id] or ("process " .. tostring(id))
end

local function fill_names(names, list)
  for id in pairs(names) do names[id] = nil end
  for _, p in ipairs(list or {}) do names[p.id] = p.name end
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
    -- Where it was held, a column a pace, for the power's graph (S6): a
    -- full column held, a gap not - and nothing at all where the package
    -- keeps no limits, so a machine that measures none marks none.
    ui.history.push(self.held_h, { heat and heat.held and 100 or nil }, cols)
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

    -- Used over the history window, Nebula's part at the floor and the
    -- rest of what is spoken for over it; what is free is the room above
    -- (S6, the Memory page; the Overview's tile draws the same).
    local t = self.totals
    local used = percent(t.used_mb, t.total_mb)
    local nebula = self.kernel and self.kernel.memory_kb
                   and percent(self.kernel.memory_kb // 1024, t.total_mb) or nil

    if used and nebula and nebula > used then nebula = used end

    ui.history.push(self.memory_h, { nebula or (used and 0), used and (used - (nebula or 0)) }, self.columns)
    return true
  end,
}

--
-- A process's ring: its share and its memory, a column a pace, `head` the
-- newest. Numbers in two arrays made once, so a hundred processes are two
-- hundred tables for the collector rather than twelve thousand.
--
local function ring_put(h, columns, pct, kb)
  h.head = h.head % columns + 1
  h.pct[h.head], h.kb[h.head] = pct, kb
  if h.n < columns then h.n = h.n + 1 end
end

-- One of a ring's two series as `ui.history` keeps one, the oldest first.
local function ring_list(h, columns, which)
  local out = {}

  if not h then return out end

  for i = h.n - 1, 0, -1 do
    out[#out + 1] = { h[which][(h.head - 1 - i) % columns + 1] }
  end

  return out
end

-- Every process: a system call, for the busiest five, the Processes page's
-- table and each process's minute.
SOURCES.processes = {
  calls = 1, messages = 0,
  ask = function() return sys.processes() end,
  take = function(self, list)
    if not list then return false end

    fill_names(self.names, list)

    local rows = self.procs:take(list, self.kernel)
    local top = self.busiest
    local hist, cols, now = self.proc_hist, self.columns, self.samples
    local strip = self.strip

    self.proc_rows = rows

    for i = #top, 1, -1 do top[i] = nil end

    for _, r in ipairs(rows) do
      local p = r.process
      local id, kb

      if r.kernel then
        id, kb = 0, (self.kernel or {}).memory_kb or 0
        top[#top + 1] = { id = 0, name = "nebula", pct = r.pct, bytes = kb * 1024 }
      else
        id, kb = p.id, (p.held or p.pages or 0) * 4

        if not p.exited then
          top[#top + 1] = { id = p.id, name = p.name, pct = r.pct, bytes = kb * 1024 }
        end
      end

      local h = hist[id]

      if not h then
        h = { pct = {}, kb = {}, n = 0, head = 0 }
        hist[id] = h
      end

      h.at = now
      ring_put(h, cols, r.pct, kb)

      if id == strip.id then
        ui.history.push(strip.share, { r.pct }, cols)
        ui.history.push(strip.memory, { kb }, cols)
      end
    end

    -- A process gone from the list takes its minute with it.
    for id, h in pairs(hist) do
      if h.at ~= now then hist[id] = nil end
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

    -- **The largest by memory**, for the Memory page (S6): every process
    -- the busiest list has, the most first and ties by id, the first ten
    -- and Nebula - which is first or last, so it is kept apart.
    local big = self.largest

    for i = #big, 1, -1 do big[i] = nil end

    self.nebula_bytes = nil

    for _, r in ipairs(top) do
      if r.id == 0 then self.nebula_bytes = r.bytes else big[#big + 1] = r end
    end

    table.sort(big, function(a, b)
      if a.bytes ~= b.bytes then return a.bytes > b.bytes end
      return a.id < b.id
    end)

    for i = #big, 11, -1 do big[i] = nil end

    for i = #top, 6, -1 do top[i] = nil end

    return true
  end,
}

-- Who ran on each core: a system call a core (S6). Its switches a second
-- and the keys' longest wait are `meters.lanes`'.
SOURCES.lanes = {
  calls = 0, messages = 0,
  ask = function(self)
    local readings = self.lane_readings

    for c = 1, self.cores do readings[c] = sys.sched_lane(c) or false end

    return readings
  end,
  take = function(self, readings, _, at)
    self.who_ran:take(readings, at, self.cores)
    return true
  end,
}

-- Which application has game mode, while the CPU page shows: a message to
-- the window manager, which keeps it (`wm/gamemode.lua`).
SOURCES.game = {
  calls = 0, messages = 1,
  ask = function() return fs.send("/Running/wm", { type = "game", op = "state" }) end,
  take = function(self, r)
    if type(r) ~= "table" or r.ok == false then return false end

    self.game = r.state or {}
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

    local g = self.gpu

    if g:take(s, self.info) then
      local cols, r = self.columns, g.rate

      -- GPU Monitor's four minutes - busy, the clock as a share of its
      -- most, memory against 256 MB, flips against frames - and the
      -- power, a graph for the first time (S5); a nil where the plane is
      -- not measured, which is a gap and never a nought.
      ui.history.push(self.gpu_h, { math.floor((g.busy or 0) * 100 + 0.5) }, cols)
      ui.history.push(self.gpu_clock_h,
                      { (s.max_mhz or 0) > 0 and math.floor(100 * s.mhz / s.max_mhz) or 0 }, cols)
      ui.history.push(self.gpu_mem_h,
                      { math.min(100, math.floor(100 * (g.bytes or 0) / M.GPU_MEMORY + 0.5)) }, cols)
      ui.history.push(self.gpu_disp_h,
                      { r.frames > 0 and math.min(100, math.floor(100 * r.flips / r.frames)) or 0 }, cols)
      ui.history.push(self.gpu_watts_h, { g.watts }, cols)
    end

    return true
  end,
}

-- The window manager's rendering mode, while the GPU page shows: a message.
SOURCES.rendering = {
  calls = 0, messages = 1,
  ask = function() return fs.send("/Running/wm", { type = "rendering" }) end,
  take = function(self, r)
    if type(r) ~= "table" or not r.ok then return false end

    self.render_mode = r.mode
    return true
  end,
}

-- Who each process is, for a window that asks nothing else of them: a
-- system call. The Processes source fills the same names where it runs.
SOURCES.names = {
  calls = 1, messages = 0,
  ask = function() return sys.processes() end,
  take = function(self, list)
    if not list then return false end

    fill_names(self.names, list)
    return true
  end,
}

--
-- **A connection as Network Monitor lists it**: the program the stack named
-- when it was opened, where it goes, its state, and its rates by its own
-- totals (`meters.network`'s `conn`).
--
local function connection(self, c, rin, rout)
  local to = c.kind == "listener" and ("listening on %d"):format(c.local_port)
             or c.kind == "udp" and ("port %d, datagrams"):format(c.local_port)
             or ("%s:%d"):format(M.address(c.remote), c.remote_port)

  return {
    process = c.process,
    program = self.names[c.process] or (c.process > 0 and ("process " .. c.process) or "the stack"),
    to = to,
    state = c.kind == "tcp" and c.state or (c.kind == "udp" and "datagrams" or "listening"),
    rin = rin, rout = rout,
  }
end

-- The network's totals and its connections: a message to the network server.
SOURCES.network = {
  calls = 0, messages = 1,
  ask = function() return fs.net_stats("/Network") end,
  take = function(self, s, _, at)
    if self.net:take(s, at) then
      ui.history.push(self.net_h, { self.net.rin, self.net.rout }, self.columns)
    end

    if not s then return false end

    self.net_s = s

    -- Every connection's rate, every reading: a rate is a difference, and
    -- a connection left out of one reading would count all its bytes as
    -- new in the next. The rows only while a page lists them.
    local rows = self.wants.connections and {} or nil

    for _, c in ipairs(s.connections or {}) do
      local rin, rout = self.net:conn(c)

      if rows then rows[#rows + 1] = connection(self, c, rin, rout) end
    end

    if rows then
      table.sort(rows, function(a, b)
        if a.rin + a.rout ~= b.rin + b.rout then return a.rin + a.rout > b.rin + b.rout end
        return a.program < b.program
      end)

      self.conns = rows
    end

    self.net_open = s.there or #(s.connections or {})
    return true
  end,
}

--
-- **A drive's programs** (`.programs`, `SYS_SENDER`'s counts): who read and
-- wrote it, each by its own totals - a program not seen before has no rate
-- yet, as Disk Monitor had it - the busiest first, then the living, then by
-- name. A drive not asked at the last pace begins its rates again.
--
local function programs_of(self, d, rows, at)
  local f = self.prog_flows[d.key]

  if not f then
    f = meters.flows(self.hz)
    self.prog_flows[d.key] = f
  end

  if f.asked ~= self.samples - 1 then f:restart() end

  f.asked = self.samples
  f:begin(at, false)

  local list, open = {}, 0

  for _, r in ipairs(type(rows) == "table" and rows or {}) do
    local rin, rout = f:rate(r.process, r.read_bytes, r.write_bytes)
    local name = self.names[r.process]

    list[#list + 1] = { process = r.process, program = name or ("process " .. r.process .. ", ended"),
                        rin = rin, rout = rout, last = r.last, alive = name ~= nil }
    if rin + rout > 0 then open = open + 1 end
  end

  table.sort(list, function(a, b)
    if a.rin + a.rout ~= b.rin + b.rout then return a.rin + a.rout > b.rin + b.rout end
    if a.alive ~= b.alive then return a.alive end
    return a.program < b.program
  end)

  self.programs[d.key] = { list = list, open = open }
end

-- The drives whose programs are wanted now: the chosen one's while Disks
-- shows, every drive's while Processes shows.
function S:program_drives()
  local want = self.wants.programs

  if want == "all" then return self.drives end

  for _, d in ipairs(self.drives) do
    if d.key == self.chosen_drive then return { d } end
  end

  return { self.drives[1] }
end

-- A message to each drive's server whose programs are wanted.
SOURCES.programs = {
  calls = 0, messages = 0,
  ask = function(self)
    local list, replies = self:program_drives(), {}

    SOURCES.programs.messages = #list

    for i, d in ipairs(list) do replies[i] = fs.read(d.path .. "/.programs") end

    return list, replies
  end,
  take = function(self, list, replies, at)
    for i, d in ipairs(list or {}) do programs_of(self, d, replies[i], at) end

    return true
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

-- The window manager's windows, while the Processes page shows: how each
-- process draws (its **draws** column). A message a page of windows.
SOURCES.windows = {
  calls = 0, messages = 1,
  ask = function() return wmproto.windows() end,
  take = function(self, desktop)
    if type(desktop) ~= "table" then return false end

    -- How each process draws, worked out where Process Viewer works it out.
    proctable.video(desktop, self.video)

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

-- Whether this sample asks for a part: every part, unless the window that
-- made it named the ones it shows.
function S:has(part)
  return self.parts == nil or self.parts[part] == true
end

function S:arrange()
  local list = {}

  if self:has("cpu") then
    list[#list + 1] = SOURCES.cpu
    SOURCES.lanes.calls = self.cores
    list[#list + 1] = SOURCES.lanes
  end
  if self:has("machine") then list[#list + 1] = SOURCES.machine end

  if self:has("processes") then
    list[#list + 1] = SOURCES.processes
  else
    list[#list + 1] = SOURCES.names
  end

  if self.has_battery then list[#list + 1] = SOURCES.battery end
  if self.gpu_lib then list[#list + 1] = SOURCES.gpu end
  -- The network wherever it has a card - and always for a window that
  -- names it, which says "No network card" from what the stack answers.
  if self:has("network") and (self.card or self.parts) then list[#list + 1] = SOURCES.network end

  for _, d in ipairs(self.drives) do list[#list + 1] = drive_source(d) end

  if self.llm_now then list[#list + 1] = SOURCES.llm end

  if self.wants.windows then list[#list + 1] = SOURCES.windows end
  if self.wants.rendering then list[#list + 1] = SOURCES.rendering end
  if self.wants.game then list[#list + 1] = SOURCES.game end
  if self.wants.programs and #self.drives > 0 then list[#list + 1] = SOURCES.programs end

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

--
-- `parts`, when given, is the set of parts a window shows - `cpu`,
-- `machine`, `processes`, `battery`, `gpu`, `network`, `drives`, `llm` -
-- and only those are asked; who each process is is asked always, since
-- every list of holders and programs names them.
--
function M.new(counter_hz, columns, parts)
  local load0 = sys.cpuload() or {}
  local cores = math.max(1, #load0)
  local info = sys.info() or {}
  local input = nil

  -- The input band's number, whose longest wait is the keys'.
  for n, name in pairs((sys.scheduler() or {}).band_names or {}) do
    if name == "input" then input = n end
  end

  local self = setmetatable({
    hz = counter_hz, columns = columns or 60, cores = cores, samples = 0,
    pace_ticks = counter_hz, info = info, kernel = nil, parts = parts,
    cpu = meters.cpu(counter_hz, load0, cores),
    hot = meters.heat(counter_hz, info, sys.ticks()),
    procs = meters.processes(), net = meters.network(counter_hz),
    gpu = meters.gpu(counter_hz), llm = meters.llm(),
    who_ran = meters.lanes(counter_hz, input), lane_readings = {}, game = nil,
    largest = {}, nebula_bytes = nil,
    drive = {}, drives = {}, totals = {}, temps = {}, busiest = {}, alive = 0,
    proc_rows = {}, strip = { id = nil }, video = {}, wants = {},
    names = {}, conns = {}, net_open = 0, net_s = nil, render_mode = nil,
    programs = {}, prog_flows = {}, chosen_drive = nil,
    cost = nil,
  }, S)

  self:clear()

  -- The battery, where the board reads one.
  if self:has("battery") then
    local b = fs.read("/Devices/battery")

    self.has_battery = type(b) == "table" and b.present == 1
    if self.has_battery then self.battery = b end
  end

  -- The GPU, where its driver knows one; the driver's sentence where not
  -- ("no GPU this driver knows"), which the Overview and the GPU page say.
  local gpu_lib = self:has("gpu") and library("gpu")

  if gpu_lib then
    local ok, s, why = pcall(gpu_lib.stats)

    if ok and s then
      self.gpu_lib, self.gpu_model = gpu_lib, s.model
      self.gpu:take(s, info)
    else
      self.gpu_why = ok and tostring(why) or tostring(s)
    end
  elseif self:has("gpu") then
    self.gpu_why = "no GPU library"
  end

  -- The network's card by its name, and where it is on the network.
  if self:has("network") then
    local ok, driven = pcall(hardware.network, sys.bus())

    driven = ok and driven or {}

    local have, net = pcall(fs.net_info, "/Network")

    self.net_info = have and type(net) == "table" and net or nil
    self.card = (driven[1] and driven[1].name) or (self.net_info and self.net_info.card and "Network card")
  end

  if self:has("drives") then self:find_drives() end
  if self:has("llm") then self:look_for_llm() end
  self:arrange()
  return self
end

-- Every history begun again: a pace changed, so columns taken at one pace
-- beside columns at another would be a time axis that lies.
function S:clear()
  self.core, self.all = {}, {}

  for c = 1, self.cores do self.core[c] = {} end

  self.heat_h, self.watts_h, self.memory_h, self.held_h = {}, {}, {}, {}
  self.gpu_h, self.net_h, self.llm_h = {}, {}, {}
  self.gpu_clock_h, self.gpu_mem_h, self.gpu_disp_h, self.gpu_watts_h = {}, {}, {}, {}
  self.proc_hist = {}
  self.strip.share, self.strip.memory = {}, {}

  for _, m in pairs(self.drive) do m.history = {} end
end

--
-- **A source asked only while a page shows** (`windows`, for Processes):
-- in the list from the next sample, and out of it again.
--
function S:want(name, on)
  if on == false then on = nil end

  if self.wants[name] == on then return end

  self.wants[name] = on
  self:arrange()
end

--
-- **The process the strip follows**: its share and memory over the minute
-- made into the two lists `ui.history` draws, from its ring - once, when it
-- is chosen, on the click that chose it - and pushed to a sample at a time
-- after. nil follows nothing.
--
function S:follow(id)
  local strip = self.strip

  if strip.id == id then return end

  local h = id and self.proc_hist[id]

  strip.id = id
  strip.share = ring_list(h, self.columns, "pct")
  strip.memory = ring_list(h, self.columns, "kb")
end

function S:restart()
  self:clear()
  self.cpu:restart(sys.cpuload())
  self.net:restart()
  self.gpu:restart()
  self.who_ran:restart()

  for _, m in pairs(self.drive) do m:restart() end
  for _, f in pairs(self.prog_flows) do f:restart() end

  self.samples = 0
end

--------------------------------------------------------------------------
-- A sample: every source asked in turn, on the window's clock.
--------------------------------------------------------------------------

function S:take()
  self.samples = self.samples + 1

  -- A stick comes and goes, and the LLM server starts and stops.
  if self.samples % 5 == 0 and self:has("drives") then
    self:find_drives()
    self:arrange()
  end

  if self.samples % 10 == 0 and not self.llm_now and self:has("llm") then
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
