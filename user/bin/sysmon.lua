-- kosmos: application
-- The machine, in a window.
--
-- The same numbers `monitor` puts in a strip along the bottom and `htop`
-- prints a screenful of, drawn as meters instead. They all read the same
-- three nodes - /dev/kernel, /dev/memory, /dev/cpu - because that is where
-- the numbers are; none of them is a special case with a private way in.
--
-- CPU is the one that needs care and it is the same care in all three: a
-- percentage is the difference between two readings. A single reading says
-- what fraction of all time since boot was busy, which on a machine that
-- has been sitting at a prompt is a number that stops moving.

local ui = use("/lib/ui.lua")
local theme = use("/lib/theme.lua")

local W, H = 340, 260

local win, err = ui.window{ title = "Monitor", w = W, h = H, x = 90, y = 130 }

if not win then
  print("sysmon: " .. tostring(err))
  return
end

local cpu = fs.read("/dev/cpu")
local hz = cpu and cpu.counter_hz or 62500000

local state = {
  pct = 0, threads = 0, threads_max = 1,
  processes = 0, processes_max = 1,
  endpoints = 0, endpoints_max = 1,
  spaces = 0, spaces_max = 1,
  used_mb = 0, total_mb = 1, uptime = 0,
}

local last_idle, last_busy

--------------------------------------------------------------------------
-- A meter: a label, a bar, and the numbers behind it.
--
-- A view rather than four labels, because what it draws is one thing and
-- the arithmetic that turns a fraction into a width has to live somewhere
-- that knows the width. Nothing in here computes a pixel offset - `fill`
-- takes a rectangle - which is the rule `gfx.md` 19.3 exists for.
--------------------------------------------------------------------------

local function meter(spec)
  local v = ui.view{ x = spec.x, y = spec.y, w = spec.w, h = 34 }

  v.label = spec.label
  v.read = spec.read

  function v:draw(g)
    local value, of, text, colour = self.read()
    local frac = (of > 0) and (value / of) or 0

    if frac < 0 then frac = 0 end
    if frac > 1 then frac = 1 end

    g:text(0, 0, self.label, theme.text_dim)

    local right = text or (tostring(value) .. " of " .. tostring(of))
    g:text(self.w - #right * gfx.font.w, 0, right, theme.text)

    local top = gfx.font.h + 4
    g:fill(0, top, self.w, 10, theme.sunken)
    g:frame(0, top, self.w, 10, theme.line)

    local filled = (self.w - 2) * frac // 1

    if filled > 0 then
      g:fill(1, top + 1, filled, 8, colour or theme.accent)
    end
  end

  return v
end

local y = 12

local function add(label, read)
  win:add(meter{ x = 14, y = y, w = W - 28, label = label, read = read })
  y = y + 38
end

add("processor", function()
  return state.pct, 100, ("%d%%"):format(state.pct),
         (state.pct > 80) and theme.bad or theme.good
end)

add("memory", function()
  return state.used_mb, state.total_mb,
         ("%d of %d MB"):format(state.used_mb, state.total_mb)
end)

add("threads", function()
  return state.threads, state.threads_max,
         ("%d of %d"):format(state.threads, state.threads_max)
end)

add("processes", function()
  return state.processes, state.processes_max,
         ("%d of %d"):format(state.processes, state.processes_max)
end)

add("endpoints", function()
  return state.endpoints, state.endpoints_max,
         ("%d of %d"):format(state.endpoints, state.endpoints_max)
end)

local uptime = ui.label{ x = 14, y = H - 26, text = "", color = theme.text_dim }
win:add(uptime)

--------------------------------------------------------------------------
-- The sampling, on the window kit's own clock.
--
-- A view with a `tick` is woken twice a second by the window it is in, so
-- this needs no timer and no loop of its own - and it costs three round
-- trips at a rate a person can read rather than at the rate a loop spins.
--------------------------------------------------------------------------

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  local k = fs.read("/dev/kernel")
  local m = fs.read("/dev/memory")

  if not k or not m then return end

  if last_idle then
    local di = k.idle_ticks - last_idle
    local db = k.busy_ticks - last_busy

    if di + db > 0 then
      state.pct = (db * 100) // (di + db)
    end
  end

  last_idle, last_busy = k.idle_ticks, k.busy_ticks

  state.threads,   state.threads_max   = k.threads, k.threads_max
  state.processes, state.processes_max = k.processes, k.processes_max
  state.endpoints, state.endpoints_max = k.endpoints, k.endpoints_max
  state.spaces,    state.spaces_max    = k.spaces, k.spaces_max
  state.used_mb    = m.total_mb - m.free_mb
  state.total_mb   = m.total_mb
  state.uptime     = sys.ticks() // hz

  uptime.text = ("up %d:%02d, %d address space%s"):format(
    state.uptime // 60, state.uptime % 60,
    state.spaces, (state.spaces == 1) and "" or "s")
end

win:add(sampler)
win:run()
