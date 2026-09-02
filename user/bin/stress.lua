-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Does the machine still work after being used for a while?
--
--   stress            forty rounds
--   stress 200        as many as you like
--
-- **What this asks that nothing else does.** `make test` checks components
-- from inside and `make screenshot` drives the machine the way a person
-- does - once. Both pass on a machine that works the first time and runs
-- out of something the fiftieth.
--
-- Every resource bug this system has had was of that shape:
--
--   * capability slots ran out on the *sixteenth* ranged read, because a
--     server took one per request and nothing gave them back
--   * a region was allocated per font per size and never released, so page
--     two of a document failed while page one was fine
--   * two `memobj` races needed a second thread allocating at the same
--     moment as the first was freeing
--   * a server promoted into the input band starved the machine only once
--     something else was busy
--
-- None of those is visible in a single pass. All of them are obvious in a
-- loop that counts.
--
-- **The counters are the test.** `sys.info` reports every fixed pool the
-- kernel has - regions, endpoints, threads, processes, free pages - so
-- "nothing leaked" is a number rather than an impression. That reporting
-- was added the day its absence cost two evenings of looking at the
-- allocator while 117,000 pages sat free.

local rounds = tonumber(args and args:match("%d+")) or 40

local function snapshot()
  local i = sys.info()

  return {
    regions   = i.regions_used,
    endpoints = i.endpoints_used,
    threads   = i.threads_used,
    processes = i.processes_held,
    pages     = i.pages_free,
  }
end

local function report(name, before, after, slack)
  local moved = after - before

  -- Free pages fall as things are used, so the sign is inverted for them:
  -- what matters is that they come back.
  print(("  %-10s %6d -> %-6d  %+d"):format(name, before, after, moved))

  return math.abs(moved) <= slack
end

--------------------------------------------------------------------------

print(("stress: %d rounds"):format(rounds))

local start = snapshot()
local failed = nil

--
-- Warm the machine first, then take the baseline.
--
-- The first round of anything allocates what every later round reuses - a
-- library loaded, a buffer made once and kept. Counting from before that
-- reports growth that is not a leak, and a test that cries wolf on its
-- first run is a test nobody keeps.
--
do
  local cap = sys.memory(2)
  if cap then sys.release(cap) end
  run("/bin/hello.lua", "", false)
end

local base = snapshot()
local half = nil

for n = 1, rounds do
  -- Two thirds through, for the question that actually matters about
  -- memory. Not halfway: this machine settles at about round forty of
  -- sixty, so a midpoint reference still has warm-up in it and reports a
  -- steady machine as a leaking one.
  if n == (rounds * 2) // 3 then half = snapshot() end

  --
  -- A region made, mapped, written through, read back and given away.
  -- This is the path that exhausted the capability table, and the one the
  -- `memobj` races live on.
  --
  local cap = sys.memory(4)

  if not cap then
    failed = ("round %d: no region (%d in use)"):format(n, snapshot().regions)
    break
  end

  local at = sys.memory_map(cap)

  if not at then
    failed = ("round %d: a region would not map"):format(n)
    break
  end

  local marker = ("round-%04d"):format(n)

  sys.region_write(cap, 0, marker)

  if sys.region_read(cap, 0, #marker) ~= marker then
    failed = ("round %d: a region did not hold what was written"):format(n)
    break
  end

  if not sys.release(cap) then
    failed = ("round %d: a region would not release"):format(n)
    break
  end

  --
  -- And a process, which is a thread, an endpoint, an address space and a
  -- capability table - created, run to completion and reaped.
  --
  if not run("/bin/uselib.lua", "", false) then
    failed = ("round %d: a program would not start"):format(n)
    break
  end

  --
  -- And reaped, which the caller has to do.
  --
  -- `run(path, args, false)` reads as "run and wait" and does not: `detach`
  -- is passed to the *child*, telling it whether to run the program
  -- synchronously, and the launcher never calls `sys.wait`. The shell reaps
  -- its own children and so does the window manager; a program that runs
  -- programs has to as well.
  --
  -- Without this the process table - thirty-two slots - fills at round
  -- twenty-two and nothing else can start. Which is what this test found on
  -- its first run, and is exactly the shape of thing it exists for: a
  -- machine that works perfectly for twenty-one rounds.
  --
  while sys.wait(true) do end

  if n % 10 == 0 then
    local now = snapshot()
    print(("  after %3d: regions %d, endpoints %d, threads %d, pages free %d")
          :format(n, now.regions, now.endpoints, now.threads, now.pages))
  end
end

--------------------------------------------------------------------------

local final = snapshot()

print("")
print("what the machine held, before and after:")

--
-- The pools are counted against the baseline and given no slack at all: a
-- region or an endpoint still held after everything was released is one
-- leak per round, so anything but zero is real.
--
-- **Memory is asked a different question.** Free pages fall for a while and
-- then stop - a process image faulted in, a Lua heap grown to its working
-- size, a buffer made once and reused - and that is not a leak, it is a
-- machine warming up. Asking "did any page get used" fails on a healthy
-- system, which is how the first version of this test failed: 363 pages
-- over 60 rounds, of which every one was spent in the first 40 and none
-- after.
--
-- So the last third is compared with the point it started. Something still
-- taking memory there is either leaking or has not settled yet, and the
-- difference is visible in the per-ten trend printed above: a leak falls at
-- a steady rate for ever, warm-up decelerates and stops. If this fails on a
-- machine whose trend is still flattening, the answer is more rounds rather
-- than a looser threshold.
--
local ok = true

ok = report("regions",   base.regions,   final.regions,   0) and ok
ok = report("endpoints", base.endpoints, final.endpoints, 0) and ok
ok = report("threads",   base.threads,   final.threads,   0) and ok
ok = report("processes", base.processes, final.processes, 2) and ok

if half then
  print("")
  print("and memory over the last third, which is the one that matters:")
  ok = report("pages free", half.pages, final.pages, 8) and ok
else
  ok = report("pages free", base.pages, final.pages, rounds) and ok
end

print("")

if failed then
  print("STRESS FAIL: " .. failed)
elseif not ok then
  print("STRESS FAIL: the machine did not give back what it took")
else
  print(("STRESS PASS: %d rounds, nothing leaked"):format(rounds))
end

print(("  (started at regions %d, pages free %d)")
      :format(start.regions, start.pages))
