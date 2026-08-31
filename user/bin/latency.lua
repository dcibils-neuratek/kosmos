-- What a yield and a round trip actually cost.
--
--   latency
--
--------------------------------------------------------------------------
-- Why this is a program and not a test in `make test`.
--
-- It measures a property of the *idle thread*, and during `make test` there
-- isn't one: the kernel's first thread runs the suite and never reaches the
-- idle loop, so a yield with an empty runqueue returns immediately and
-- everything looks perfect. A test was written here that did exactly that -
-- it passed with the bug deliberately put back, which is worse than no test
-- at all - and this replaced it.
--
-- The bug it is watching for: the idle loop was `thread_yield(); wfi;`
-- unconditionally, so a thread that yielded switched to idle, idle returned
-- from its own yield, and slept with a runnable thread in the queue. Every
-- yield cost a timer period and every IPC round trip cost two - ten and
-- twenty milliseconds at 100 Hz, on paths whose own cost is measured in
-- tens of counter ticks.
--
-- Nothing saw it. `bench/` has an `ipc_roundtrip` number and it was
-- identical to three decimal places before and after the fix, because it
-- measures two threads that are both runnable and so never goes near the
-- idle thread. A system can be a thousand times slower than it should be
-- and every benchmark can be green.
--
-- The thresholds below are loose on purpose. QEMU numbers are not
-- performance numbers; the statement being made is not "this is fast", it
-- is "this is not being paced by the timer".
--------------------------------------------------------------------------

local N = 200
local TICK_HZ = 100                       -- kernel/main.c: hal_timer_init
local hz = sys.info().counter_hz
local period = hz // TICK_HZ

local function timeit(name, fn)
  fn()
  local t = sys.ticks()
  for _ = 1, N do fn() end
  local per = (sys.ticks() - t) / N
  print(("  %-18s %12.1f ticks  %7.3f ms  %5.2f timer periods")
        :format(name, per, per * 1000 / hz, per / period))
  return per
end

print(("timer at %d Hz, so a period is %d counter ticks")
      :format(TICK_HZ, period))
print()

local yield  = timeit("sys.yield",     function() sys.yield() end)
local trip   = timeit("an IPC round trip", function() fs.getattr("/data") end)
local query  = timeit("a query",       function()
  fs.query("/data", { kind = "latency-probe" })
end)

print()

-- A yield that switches costs a few thousand ticks. A yield that waits for
-- the timer costs a whole period. A tenth of one is nowhere near either.
local ok = true

if yield > period / 10 then
  print(("FAIL: a yield costs %.2f timer periods. It is waiting for the "
         .. "timer rather than switching - see the idle loop in "
         .. "kernel/main.c."):format(yield / period))
  ok = false
end

if trip > period * 0.5 then
  print(("FAIL: a round trip costs %.2f timer periods. Two of those is what "
         .. "a call and a reply cost when each one sleeps until the tick.")
        :format(trip / period))
  ok = false
end

if ok then
  print(("PASS: a yield is %.3f of a timer period and a round trip %.3f. "
         .. "Neither is paced by the clock."):format(yield / period,
                                                     trip / period))
end

print(("(a query is %.0f%% more than the round trip that carries it)")
      :format((query / trip - 1) * 100))
