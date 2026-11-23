-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_GLDirectMode
-- kosmos: name GPU Monitor
-- kosmos: section system
-- The graphics processor: how busy it is, how fast it runs and how long it
-- sleeps, its power, its memory and who holds it, and what the display and
-- the desktop take from it - now and over the last minute.
--
--   wm gpumon
--
-- Diego, 17 October 2026: "we need to vreate a gpu monitor app now to see
-- memory pressure, gpu usage, etc", with Windows' GPU page as the picture.
-- Drawn in `docs/gpumonitor.html` and agreed ("go ahead, build it as
-- drawn"); `docs/gpumonitor.md` is what it is made of, and this was G2.
--
-- **Its body is System Monitor's GPU page now** (`docs/sysmonitor.md` S5):
-- `/Kosmos/Libraries/sysmon/gpu.lua` draws it here and there, so there is
-- one body and not two that look alike, with the power over the minute as
-- a graph and the temperature beside it. This window is the header, the
-- dots and a sample that asks only the GPU (`sysmon/sample.lua`, its parts
-- named). S9 retires it, and `gpumon` opens System Monitor on its GPU page
-- (decision 3).
--
-- **It measures nothing itself.** The GPU's driver reads its registers and
-- keeps its buffers (`GPU_OP_STATS`, `gpu.stats`), Nebula reads the GPU's
-- energy (`sys.info().gpu_energy_uj`), and the sample asks once a pace and
-- takes the difference between two readings, as the other monitors do.

local ui = use("/Kosmos/Libraries/ui.lua")
local sample = use("/Kosmos/Libraries/sysmon/sample.lua")
local gpu_page = use("/Kosmos/Libraries/sysmon/gpu.lua")
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- Monitor's three paces, and what sixty columns of each come to.
local PACES = {
  { half = 1, menu = "Update every 0.5 s", heading = "The last 30 seconds",
    foot = "A column every half second; the newest at the right." },
  { half = 2, menu = "Update every 1 s",   heading = "The last minute",
    foot = "A column a second; the newest at the right." },
  { half = 4, menu = "Update every 2 s",   heading = "The last 2 minutes",
    foot = "A column every two seconds; the newest at the right." },
}
local pace = PACES[2]

-- `--tell`: a line a sample, for a suite to read. Not without it.
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil

local function pace_ticks(p) return counter_hz * p.half // 2 end

-- The GPU, and the processor beside it for the graphics plane's energy and
-- the package's temperature; the rendering mode while the page shows,
-- which is always here.
local S = sample.new(counter_hz, 60, { cpu = true, gpu = true })

S.pace_ticks = pace_ticks(pace)
S:want("rendering", true)

local page = gpu_page.new{ S = S, pace = function() return pace end, prefix = "gpumon:" }

local win, err = ui.window{ title = "GPU Monitor", w = page.w, h = L.head + page.h,
                            x = 130, y = 90, header = true }

if not win then
  print("gpumon: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The header: the name and the dots.
--------------------------------------------------------------------------

local set_pace
local more = ui.iconbutton{ icon = "more" }

more.on_click = function()
  local items = {}

  for _, p in ipairs(PACES) do
    items[#items + 1] = { text = p.menu, mark = (p == pace),
                          on_choose = function() set_pace(p) end }
  end

  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)
end

win:add(ui.header{ x = 0, y = 0, w = page.w, title = "GPU Monitor", right = { more }, title_bar = true })

local body = ui.view{ x = 0, y = L.head, w = page.w, h = page.h }

function body:draw(g) page:draw(g) end

win:add(body)

--------------------------------------------------------------------------
-- The sampling, on the window kit's clock set to the pace.
--------------------------------------------------------------------------

local function take()
  S:take()

  local line = TELL and page:tell_line()

  if line then print("gpumon: " .. line) end
end

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick() take() end

win:add(sampler)

set_pace = function(p)
  pace = p
  win.tick_every = pace_ticks(p)
  win.last_tick = sys.ticks()
  S.pace_ticks = win.tick_every

  -- Begun again: columns at one pace beside columns at another would be a
  -- time axis that lies.
  S:restart()
  take()
  print(("gpumon: %s"):format(p.menu:lower()))
end

take()
print(S.gpu_lib and ("gpumon: " .. tostring(S.gpu_model)) or ("gpumon: no GPU: " .. tostring(S.gpu_why)))
win.tick_every = pace_ticks(pace)
win:run()
