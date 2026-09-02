-- kosmos: application
-- kosmos: section preferences
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The scheduler, while it is running.
--
--   wm scheduler
--
-- Swap the policy, change the quantum, and watch what it does to a machine
-- that is busy. The point is not configuration - almost nobody needs to
-- change these - it is that a system whose scheduling you can only ask
-- about by rebuilding is one nobody ever asks.
--
-- **What the numbers mean.**
--
--   policy    which algorithm decides who runs next. `priority` is five
--             bands with round robin inside each; `round-robin` is one
--             queue and no bands at all, which is what this machine ran on
--             until recently and is here to be compared against.
--
--   quantum   how long a thread may hold the processor before it is taken
--             away, in timer ticks. **Ten milliseconds is the floor**, and
--             not because anybody chose it: a quantum is counted in timer
--             interrupts and the timer runs at 100 Hz, so a shorter turn
--             needs a faster tick, which is a different change with its own
--             cost. BeOS, whose feel this is chasing, ran a few
--             milliseconds.
--
--   bands     how many priority levels exist. Threads are put in them by
--             what they were given, not by asking: the process handed the
--             screen runs in the display band. There is deliberately no way
--             to promote yourself.
--
-- The load button starts a thread that does nothing but compute, which is
-- the only way to feel any of this: with an idle machine every policy looks
-- identical and every quantum is unused.

local ui    = use("/lib/ui.lua")
local theme = ui.theme

local W, H = 560, 420

local win, err = ui.window{ title = "Scheduler", w = W, h = H, x = 140, y = 80 }

if not win then
  print("scheduler: " .. tostring(err))
  return
end

local info = sys.scheduler()

if not info then
  print("scheduler: this machine does not report its scheduler")
  return
end

local status = ui.label{ x = 12, y = H - 30, w = W - 24, text = "" }

--------------------------------------------------------------------------
-- What it is doing now.
--------------------------------------------------------------------------

local facts = ui.view{ x = 12, y = 44, w = W - 24, h = 108 }

function facts:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:frame(0, 0, self.w, self.h, theme.line)

  local rows = {
    { "policy",  info.policies[info.policy] or "?" },
    { "quantum", ("%d ticks - %.0f ms"):format(info.quantum, info.quantum_ms) },
    { "timer",   ("%d Hz, so %.0f ms a tick"):format(info.tick_hz,
                                                     1000 / info.tick_hz) },
    { "bands",   ("%d, and the floor is one tick"):format(info.bands) },
  }

  for i, row in ipairs(rows) do
    local y = 8 + (i - 1) * (gfx.font.h + 6)

    g:text(10, y, row[1], theme.text_dim, theme.window)
    g:text(96, y, row[2], theme.text, theme.window)
  end
end

local function refresh(said)
  info = sys.scheduler() or info
  status.text = said or ""
  win:paint()
end

--------------------------------------------------------------------------
-- Changing it.
--------------------------------------------------------------------------

win:add(ui.label{ x = 12, y = 12, w = W - 24, text = "How this machine schedules" })
win:add(facts)

win:add(ui.label{ x = 12, y = 164, w = 90, text = "policy" })

local x = 96

for index, name in ipairs(info.policies) do
  local this = index

  win:add(ui.button{
    x = x, y = 160, w = 120, h = 24, text = name,
    on_click = function ()
      local ok, why = sys.set_policy(this)

      if ok then
        refresh("now scheduling with " .. name)
      else
        refresh("could not: " .. tostring(why))
      end
    end,
  })

  x = x + 128
end

win:add(ui.label{ x = 12, y = 204, w = 90, text = "quantum" })

local quanta = { 1, 2, 5, 10, 20, 50 }
x = 96

for _, ticks in ipairs(quanta) do
  local this = ticks

  win:add(ui.button{
    x = x, y = 200, w = 64, h = 24,
    text = ("%d ms"):format(this * 1000 // info.tick_hz),
    on_click = function ()
      local ok, why = sys.set_quantum(this)

      if ok then
        refresh(("a turn is now %d ms"):format(this * 1000 // info.tick_hz))
      else
        refresh("could not: " .. tostring(why))
      end
    end,
  })

  x = x + 72
end

--------------------------------------------------------------------------
-- Something to schedule.
--------------------------------------------------------------------------

win:add(ui.label{
  x = 12, y = 244, w = W - 24,
  text = "An idle machine schedules identically whatever you choose:",
})

local spinners = 0

win:add(ui.button{
  x = 12, y = 268, w = 150, h = 24, text = "add a busy thread",
  on_click = function ()
    -- Detached, so this window keeps answering while it spins. `spin`
    -- exists for exactly this: a program whose whole job is to be busy.
    if run("/bin/spin.lua", "", true) then
      spinners = spinners + 1
      refresh(("%d busy thread(s) running - now change something")
              :format(spinners))
    else
      refresh("could not start one")
    end
  end,
})

win:add(ui.label{
  x = 12, y = 300, w = W - 24,
  text = "Then drag this window, or type. That is the whole measurement.",
})

win:add(ui.label{
  x = 12, y = 320, w = W - 24,
  text = "`htop` shows where the time goes; `procs` shows who is running.",
})

win:add(status)

refresh("read from the kernel, not remembered")

win:run()
