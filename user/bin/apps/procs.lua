-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon TeamIcon
-- kosmos: name Process Viewer
-- kosmos: section system
-- kosmos: needs processes
-- Every process, and what it is costing.
--
-- BeOS's ProcessController, which put a bar beside each team and let you
-- see at a glance which one was eating the machine. The same idea and the
-- same reason: a number in a column tells you a process used forty-one
-- ticks, and a bar tells you which one to look at.
--
-- The numbers come from `sys.processes()`, the same call `htop` and `ps`
-- use. Nothing here is privileged - the kernel will tell any process what
-- the table looks like, because a process table is not authority: knowing
-- that something exists is not being able to reach it.
--
-- **Its table is System Monitor's** (`docs/sysmonitor.md` S4): the
-- columns, the sorting, the selection, the drawing and End are
-- `/Kosmos/Libraries/proctable.lua`'s, which the Processes page draws
-- through as well, so there is one table and not two that look alike.
-- What is here is the window around it: its header, its band, and the
-- sample that fills it. S9 retires this window for that page.

local ui = use("/Kosmos/Libraries/ui.lua")
local proctable = use("/Kosmos/Libraries/proctable.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")

--
-- The counter's rate, read once (`architecture.md` §5): `sys.ticks()`
-- counts at whatever this board runs its counter at, and there is no ratio
-- to carry in your head.
--
local cpu = fs.read("/Devices/cpu") or {}
local counter_hz = cpu.counter_hz or 62500000
local _, KERNEL_LEVEL = proctable.privilege(cpu)

local W, H = 1080, 482

-- **As `docs/apps.html` draws it** (`roadmap.md` 5zp): the kit's header
-- with what the machine is doing beside the title and End and the dots at
-- its end; the four pools under it in a band 14 above and 10 below, 18 in;
-- and the table from there to the window's edges.
local L = ui.layout
local BAND_TOP, BAND_SIDE, BAND_FOOT = 14, 18, 10
local METER_H = 35                       -- a line, 6, and the bar's 8

local win, err = ui.window{ title = "Processes", w = W, h = H, x = 150, y = 90,
                            header = true }

-- After the window, so the faces are the look's.
local LIST_Y = L.head + BAND_TOP + METER_H + BAND_FOOT

if not win then
  print("procs: " .. tostring(err))
  return
end

-- What kind each process is: `prockind.lua` decides, from what each file
-- declares, read once - what is installed does not change while this runs.
local declared = proctable.declared()

-- Each process's share and the machine's totals: `meters.processes`.
local meters = use("/Kosmos/Libraries/meters.lua")
local measured = meters.processes()

-- The scheduling bands by the kernel's names for them, so a band added
-- there - game, 14 October - is named here without a change.
local BANDS = (sys.scheduler() or {}).band_names or {}

local video = {}

--------------------------------------------------------------------------
-- The table: the shared one, following all four edges so it grows with the
-- window (`ui.md` 16.4), its arrangement kept in `procs`.
--------------------------------------------------------------------------

local list = proctable.new{
  prefix = "procs:", x = 0, y = LIST_Y, w = W, h = H - LIST_Y,
  follow = { "left", "right", "top", "bottom" },
  window = function() return win end, cpu = cpu, prefs = "procs", say_window = true,
}

-- Declared before the header refers to them and defined below.
local end_selected
local sampler

--------------------------------------------------------------------------
-- The header: what the machine is doing on the left, and on the right End
-- - why this window is open when it is open - and a `...` for the rest.
-- The order is chosen at the headings (`roadmap.md` 6m).
--------------------------------------------------------------------------

local function more_menu()
  return {
    { text = "Refresh", on_choose = function() sampler:tick() end },
  }
end

local more = ui.iconbutton{ icon = "more" }

more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, more_menu())
end

win:add(list.view)

--------------------------------------------------------------------------
-- What the machine has left, above the list of what is using it: a total
-- beside the detail it is the total *of*. Each pool a level
-- (`proctable.level`).
--------------------------------------------------------------------------

local totals_state = {
  used_mb = 0, total_mb = 1,
  threads = 0, threads_max = 1,
  processes = 0, processes_max = 1,
  endpoints = 0, endpoints_max = 1,
  spaces = 0, spaces_max = 1,
}

local function meter(spec)
  local v = ui.view{ x = spec.x, y = spec.y, w = spec.w, h = METER_H }

  function v:draw(g)
    local value, of, text = spec.read()

    proctable.level(g, 0, 0, self.w, self.h, spec.label, value, of, text)
  end

  return v
end

do
  local n    = 4
  local gap  = 14
  local each = (W - 2 * BAND_SIDE - gap * (n - 1)) // n
  local ty   = L.head + BAND_TOP

  local pools = {
    { "Memory", function()
        return totals_state.used_mb, totals_state.total_mb,
               ("%d of %d MB"):format(totals_state.used_mb, totals_state.total_mb)
      end },
    { "Threads", function() return totals_state.threads, totals_state.threads_max end },
    { "Processes", function() return totals_state.processes, totals_state.processes_max end },
    { "Endpoints", function() return totals_state.endpoints, totals_state.endpoints_max end },
  }

  for i = 1, n do
    win:add(meter{ x = BAND_SIDE + (i - 1) * (each + gap), y = ty, w = each,
                   label = pools[i][1], read = pools[i][2] })
  end
end

--
-- What the machine is doing, beside the title - and what the last End did,
-- in its place for a few seconds, since a sentence overwritten on the next
-- sample is a sentence nobody reads.
--
local summary, said, said_at = "", nil, 0

-- **This program may end anything, and that is the point of it**:
-- `needs processes` becomes `SPAWN_PROCCTL`, which lets `SYS_KILL` reach a
-- process this one did not start (`proctable.finish`).
function end_selected()
  local r = list:chosen()

  if not r then return end

  said, said_at = proctable.finish(r), sys.ticks()
end

local header = ui.header{
  x = 0, y = 0, w = W, title = "Processes", sub = "",
  right = { ui.button{ text = "End", on_click = function() end_selected() end },
            more },
  title_bar = true,
}

-- Last, so the focus starts in the table and Tab reaches End after it.
win:add(header)

--------------------------------------------------------------------------
-- Sampling: a share of the processor is the difference between two
-- readings, the same as everywhere else here.
--------------------------------------------------------------------------

sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

local said_kernel_kb = false

function sampler:tick()
  local k = fs.read("/Devices/kernel")
  local procs_now = sys.processes()

  -- Which processes have windows, and how those windows draw.
  proctable.video(wmproto.windows(), video)

  if not procs_now then return end

  local share = measured:take(procs_now, k)
  local rows = proctable.rows(share, { kernel = k, video = video, declared = declared,
                                       bands = BANDS, kernel_level = KERNEL_LEVEL,
                                       kernel_threads = measured.kernel_threads })

  -- What Nebula holds, said once, for the display harness.
  for _, r in ipairs(rows) do
    if r.synthetic and r.kb > 0 and not said_kernel_kb then
      said_kernel_kb = true
      print(("procs: nebula holds %d KB of %d KB"):format(r.kb,
            ((fs.read("/Devices/memory") or {}).total_mb or 0) * 1024))
    end
  end

  list:set(rows)

  meters.totals(k, fs.read("/Devices/memory"), totals_state)

  summary = proctable.summary(measured.count, measured.threads, measured.kernel_threads,
                              totals_state.spaces, sys.ticks() // counter_hz)

  if said and sys.ticks() - said_at > 4 * counter_hz then said = nil end

  header.sub = said or summary
end

win:add(sampler)
win:run()
