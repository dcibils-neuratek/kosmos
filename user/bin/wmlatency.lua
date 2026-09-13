-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: section system
-- How long the window manager takes to answer when it has nothing to do.
--
--   wm wmlatency
--
-- Every application with a window asks the window manager something every
-- frame - `poll` for events, `commit` for a finished picture - and waits for
-- the answer. The work behind an idle answer is nothing, so what the round
-- trip costs is how long the manager takes to *notice* it was asked. On the
-- ThinkPad the Super Nintendo spent 6.5 ms emulating a frame and managed one
-- every 23, with the machine 95% idle; this is the number that says where the
-- rest went.
--
-- It opens a small window, asks two hundred times for events it does not
-- have, and judges the average against a scheduler tick: an answer that waits
-- for the manager's sleep to end costs ticks, and one that wakes it costs a
-- small part of one. Loose on purpose, like `latency`: QEMU numbers are not
-- performance numbers, and the statement is "not paced by a sleep", not
-- "fast".

local wmproto = use("/lib/wmproto.lua")

local N = 200

-- Both clocks asked for, never assumed: `latency.lua` has 100 Hz written into
-- it, and the kernel runs at 250.
local info = sys.info() or {}
local counter_hz = info.counter_hz or 62500000
local tick_hz = info.tick_hz or 250
local tick = counter_hz / tick_hz

local win, err = fs.send("/app/wm", {
  type = "open", title = "wmlatency", w = 240, h = 60, x = 40, y = 60,
})

if not win then
  print("wmlatency: " .. tostring(err))
  return
end

local handle = win.window

-- Once untimed, so the window exists on the manager's side before the clock
-- starts, and the first answer's setup is not in the average.
if not wmproto.poll(handle, 0) then
  print("wmlatency: the window manager went away")
  return
end

local total, worst = 0, 0

for _ = 1, N do
  local started = sys.ticks()

  if not wmproto.poll(handle, 0) then
    print("wmlatency: the window manager went away")
    return
  end

  local took = sys.ticks() - started

  total = total + took

  if took > worst then
    worst = took
  end
end

local average = total / N

print(("wmlatency: a round trip to the window manager takes %.3f ms on "
       .. "average and %.3f ms at worst, against a scheduler tick of %.3f ms")
      :format(average * 1000 / counter_hz, worst * 1000 / counter_hz,
              tick * 1000 / counter_hz))

if average > tick / 2 then
  print(("FAIL: the window manager answers in %.2f scheduler ticks on "
         .. "average. It answers when its sleep ends, not when it is asked.")
        :format(average / tick))
else
  print(("PASS: the window manager answers in %.3f of a scheduler tick on "
         .. "average. It wakes when it is asked."):format(average / tick))
end
