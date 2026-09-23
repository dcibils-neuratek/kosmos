-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pulse
-- kosmos: section system
-- The processors, in a window.
--
-- **This used to be the whole machine and is now one thing.** It carried
-- meters for memory, threads, processes, endpoints and address spaces
-- alongside the processor, and those have moved to `procs`, where they sit
-- above a table of the processes that are using them. A total belongs
-- beside the detail it is the total *of*; here it was five unrelated
-- numbers keeping one another company because they happened to arrive in
-- the same reply.
--
-- What is left is BeOS's Pulse: an identity box saying what processor this
-- is, and one segmented bar per processor. `/lib/pulse.lua` draws it and
-- says why the layout is worth copying - `cores` draws the same panel with
-- buttons under it.
--
-- The care this needs is the care every reader of these counters needs
-- once: **a percentage is the difference between two readings.** A single
-- reading says what fraction of all time since boot was busy, which on a
-- machine that has been sitting at a prompt is a number that stopped
-- moving. `sys.cpuload` reports totals since boot, per processor, and the
-- subtraction below is the whole of what makes them a meter.

local ui = use("/lib/ui.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme
local pulse = use("/lib/pulse.lua")

--
-- Two numbers, and the rows are the larger one.
--
-- `cpus_present` is what the machine has; `cpus` is what this kernel
-- schedules on. Today that is four and one - `docs/smp.md` step three
-- starts the other three, each claims its own `struct percpu`, and each
-- parks in `wfi`. **A row per processor that exists**, because the three
-- that are parked are the subject rather than an omission.
--
local info       = sys.info() or {}
local SCHEDULING = info.cpus or 1
local ONLINE     = info.cpus_online or SCHEDULING
local CORES      = info.cpus_present or ONLINE

local ident = pulse.identity()

local W = 380
local H = 14 + pulse.height(CORES, #ident) + 14

local win, err = ui.window{ title = "Monitor", w = W, h = H, x = 90, y = 130 }

if not win then
  print("sysmon: " .. tostring(err))
  return
end

local pct = {}
local last = {}

win:add(pulse.panel{
  x = 14, y = 14, w = W - 28,
  cores = CORES, online = ONLINE, scheduling = SCHEDULING,
  ident = ident,
  read = function(c) return pct[c] end,
})

--------------------------------------------------------------------------
-- The sampling, on the window kit's own clock.
--
-- A view with a `tick` is woken twice a second by the window it is in, so
-- this needs no timer and no loop of its own - and it costs one round trip
-- at a rate a person can read rather than at the rate a loop spins.
--
-- One, and it used to be three: `/dev/kernel` and `/dev/memory` were read
-- here every tick for meters that have moved to `procs`.
--------------------------------------------------------------------------

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  local load = sys.cpuload()

  if load then
    for i = 1, #load do
      local was = last[i]

      if was then
        local di = load[i].idle - was.idle
        local db = load[i].busy - was.busy

        if di + db > 0 then
          pct[i] = (db * 100) // (di + db)
        end
      end

      last[i] = { idle = load[i].idle, busy = load[i].busy }
    end
  end

end

win:add(sampler)
win:run()
