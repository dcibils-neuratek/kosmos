-- kosmos: application
-- kosmos: icon System_Kernel
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

local W, H = 560, 520

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

local L = ui.layout
local refresh

--------------------------------------------------------------------------
-- The window, as `docs/apps.html` draws it (`roadmap.md` 5zp): what the
-- scheduler is doing in one card, the two things a person may change in
-- another, and the one thing that makes either visible - a thread with
-- nothing to do but compute - as the header's verb. It was a framed box of
-- facts, two rows of buttons named after their values, and five lines of
-- text at positions chosen one at a time.
--------------------------------------------------------------------------

local quanta = { 1, 2, 5, 10, 20, 50 }

local function ms(ticks) return ticks * 1000 // info.tick_hz end

local header, cards              -- built below; `refresh` fills them

local spinners = 0
local said = "read from the kernel, not remembered"

local function groups()
  local policies, turns = {}, {}

  for index, name in ipairs(info.policies) do
    policies[#policies + 1] = { index, name }
  end

  --
  -- **The value in force is always one of the choices**, in its place in
  -- the order. The kernel starts at 25 ticks, which none of the offered six
  -- was, so the dropdown showed a bare "25" - a number in the units of
  -- nothing else on the page.
  --
  local offered, seen = {}, false

  for _, ticks in ipairs(quanta) do
    if not seen and ticks > info.quantum then
      offered[#offered + 1] = info.quantum
      seen = true
    end

    if ticks == info.quantum then seen = true end
    offered[#offered + 1] = ticks
  end

  if not seen then offered[#offered + 1] = info.quantum end

  for _, ticks in ipairs(offered) do
    turns[#turns + 1] = { ticks, ("%d ms"):format(ms(ticks)) }
  end

  local policy = ui.dropdown{
    choices = policies, value = info.policy,
    on_change = function(_, index)
      local ok, why = sys.set_policy(index)

      refresh(ok and ("now scheduling with " .. info.policies[index])
              or ("could not: " .. tostring(why)))
    end,
  }

  local quantum = ui.dropdown{
    choices = turns, value = info.quantum,
    on_change = function(_, ticks)
      local ok, why = sys.set_quantum(ticks)

      refresh(ok and ("a turn is now %d ms"):format(ms(ticks))
              or ("could not: " .. tostring(why)))
    end,
  }

  return {
    { name = "Now", rows = {
        { label = "Policy", value = info.policies[info.policy] or "?" },
        { label = "Quantum", note = ("%d ticks"):format(info.quantum),
          value = ("%.0f ms"):format(info.quantum_ms) },
        { label = "Timer", value = ("%d Hz · %.0f ms a tick")
                                   :format(info.tick_hz, 1000 / info.tick_hz) },
        { label = "Bands", note = "the floor is one tick",
          value = tostring(info.bands) } } },
    { name = "Choose", rows = {
        { label = "Policy", control = policy },
        { label = "Quantum", note = "how long a thread may hold a processor",
          control = quantum } } },
  }
end

--
-- **Read again, and said again**, after anything changes - the kernel is
-- the authority, so the card shows what it now reports rather than what
-- was asked for.
--
function refresh(words)
  info = sys.scheduler() or info
  said = words or said
  header.sub = said
  cards:set(groups())
  win:paint()
end

header = ui.header{
  x = 0, y = 0, w = W, title = "Scheduler", sub = said,
  right = { ui.button{
    text = "Add a busy thread",
    on_click = function()
      -- Detached, so this window keeps answering while it spins. `spin`
      -- exists for exactly this: a program whose whole job is to be busy.
      if run("/bin/spin.lua", "", true) then
        spinners = spinners + 1
        refresh(("%d busy thread%s - now change something")
                :format(spinners, spinners == 1 and "" or "s"))
      else
        refresh("could not start one")
      end
    end } },
}

--
-- An idle machine schedules identically whatever is chosen, which is the
-- thing to know before choosing - so it is the page's note.
--
cards = ui.cards{
  x = 0, y = L.head, w = W, h = H - L.head,
  groups = groups(),
  foot = "An idle machine schedules the same whatever you choose. Add a "
         .. "busy thread, then drag a window or type.",
}

win:add(header)
win:add(cards)

win:run()
