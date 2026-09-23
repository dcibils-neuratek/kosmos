-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The benchmark, over the serial line.
--
--   score
--
-- The same measurements `sysbench` draws, printed instead. This is the one
-- that matters for a new board: the first time Kosmos comes up on real
-- hardware there is a cable and a prompt and no display at all, and that
-- is exactly the machine somebody wants a number for.
--
-- It prints each result as it arrives rather than at the end, because on a
-- slow machine the difference between "working" and "hung" is whether
-- anything is coming out of the cable.

local bench = use("/lib/bench.lua")

print(("Kosmos benchmark: %d measurements, about %d minutes.")
      :format(#bench.TESTS, math.ceil(#bench.TESTS * bench.SECONDS / 60)))
print("")
print("Numbers taken under an emulator measure the emulator. Compare two")
print("runs on the same footing - two boards, or one board before and")
print("after a change - never a guest against real silicon.")
print("")

local results = {}
local skip_group = nil

local i = 0

-- A `while` and not a `for`, because the loop sometimes has to skip ahead
-- and assigning to a `for` variable in Lua changes a copy - the first
-- version did exactly that and quietly measured a group it had already
-- decided to skip.
while i < #bench.TESTS do
  i = i + 1

  local t = bench.TESTS[i]

  if skip_group == t.group then
    results[i] = { skipped = true, why = "the group before it" }
    goto continue
  end

  skip_group = nil

  local co = bench.measure(t)
  local value, why

  -- Driven to completion here. `sysbench` resumes the same coroutine one
  -- slice at a time so its window keeps drawing; a console program has
  -- nothing to draw and can simply let it run.
  while true do
    local ok, a, b = coroutine.resume(co)

    if not ok then
      value, why = nil, tostring(a)
      break
    end

    if coroutine.status(co) == "dead" then
      value, why = a, b
      break
    end
  end

  if type(value) == "number" then
    local r = bench.record(results, i, value)

    print(("%-12s %-22s %14.1f %s  %s")
          :format(t.group, t.name, value, t.unit,
                  r.score and ("%.0f"):format(r.score) or ""))
  else
    results[i] = { skipped = true, why = tostring(why) }
    print(("%-12s %-22s %14s  (%s)")
          :format(t.group, t.name, "skipped", tostring(why)))

    -- The rest of the group goes with it. There is no point timing a read
    -- of a file that could not be written, and four more failures would
    -- read as four separate faults rather than one missing disk.
    skip_group = t.group
  end

  ::continue::
end

print("")

for _, line in ipairs(bench.report(results)) do
  print(line)
end

bench.cleanup()
