-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where a window manager pass actually goes.
--
--   frames            measure for five seconds, then report
--   frames 20         for as long as you like
--   frames on         start measuring and leave it running
--   frames read       report without disturbing anything
--   frames off        stop
--
-- **Why this exists.** Kosmos is aiming at a desktop that stays responsive
-- on a Pi 5, and the recurring question is whether the window manager - a
-- Lua process with C primitives underneath it - should be rewritten in C.
-- Every discussion of that has had to guess, because five of the five
-- gated benchmarks measure the kernel and none of them measures a frame.
--
-- So this measures one. `wm` keeps seven counters and hands them over;
-- this divides them by a clock and prints them.
--
-- **The number to look at is `busy max`, not the average.** An average
-- frame time of 2 ms with one pass in a thousand at 40 ms is a desktop
-- that stutters, and the stutter is the thing being fixed. Responsiveness
-- is a promise about the worst case.
--
-- **Waiting is reported apart from working.** Most of an idle pass is
-- `wait_input` asleep, which is the loop behaving correctly rather than
-- costing anything, and folding it in would make an empty desktop look
-- like a busy one.
--
-- **And these are QEMU numbers.** `CLAUDE.md` is clear that they are for
-- catching regressions and not for knowing whether something is fast. What
-- survives the emulator is the *shape*: which stage dominates, and whether
-- the worst pass is a collection. Absolute milliseconds wait for the Pi.

local seconds = 5
local mode    = "run"

local a = args and args:match("%S+")

if a == "on" or a == "off" or a == "read" then
  mode = a
elseif a then
  seconds = tonumber(a) or 5
end

local cpu = fs.read("/dev/cpu") or {}
local HZ  = cpu.counter_hz or 62500000

local function ms(ticks) return ticks * 1000.0 / HZ end
local function us(ticks) return ticks * 1000000.0 / HZ end

--
-- Two different failures wear one shape, so this has to tell them apart.
--
-- `ns.send` returns `nil` and a string when the path cannot be resolved
-- *and* when the server answered `ok = false` - it unwraps the reply, which
-- is why the `reply.ok` branch that used to be here was dead code. So a
-- desktop answering "nothing measured yet" was reported as "no window
-- manager", which sent you looking for the wrong thing. The path is the one
-- failure that names itself.
--
local function ask(msg)
  local reply, err = fs.send("/app/wm", msg)

  if reply then return reply end

  err = tostring(err)

  if err:match("^no such path") then
    print("frames: no window manager is running. Start one with `wm`.")
  else
    print("frames: " .. err)
  end

  return nil
end

--------------------------------------------------------------------------
-- The report.
--------------------------------------------------------------------------

local STAGES = {
  { "wait",     "waiting for input" },
  { "keys",     "keys" },
  { "messages", "application requests" },
  { "pointer",  "pointer" },
  { "waiting",  "answering polls" },
  { "collect",  "reaping" },
  { "compose",  "composing" },
}

local function report(p)
  if p.passes == 0 then
    print("frames: no passes measured.")
    return
  end

  print("")
  print(string.format("%d passes, %d of which composed something",
                      p.passes, p.frames))
  print("")

  -- Busy first and on its own, because it is the answer and everything
  -- below it is the explanation.
  print(string.format("  busy   %8.3f ms total   %7.1f us mean   %7.3f ms WORST",
                      ms(p.busy_total),
                      us(p.busy_total) / p.passes,
                      ms(p.busy_max)))
  print("")
  print("  stage                     total ms     share    worst us    KB/pass")
  print("  --------------------------------------------------------------------")

  for _, s in ipairs(STAGES) do
    local name, label = s[1], s[2]
    local total = p[name .. "_total"] or 0
    local worst = p[name .. "_max"] or 0
    local kb    = (p[name .. "_kb"] or 0) / p.passes

    -- Share of *busy*, so the stages add to a hundred. Waiting is not
    -- busy and is shown with a dash rather than a fraction it is not part
    -- of - it would otherwise be ninety-odd per cent of everything and
    -- bury the six numbers this is for.
    local share = (name == "wait") and "     -"
                  or string.format("%5.1f%%",
                                   (p.busy_total > 0)
                                     and (total * 100.0 / p.busy_total) or 0)

    print(string.format("  %-22s %10.3f    %s   %9.1f   %8.2f",
                        label, ms(total), share, us(worst), kb))
  end

  --
  -- What a pass allocates, which is the number the collector answers to.
  --
  -- The times say which stage is slow; this says which stage will *stop*
  -- the desktop, and they are not the same question. A collection arrives
  -- when the allocator decides, so a stage that costs nothing and allocates
  -- steadily is buying a pause for whichever stage happens to be running
  -- when it lands.
  --
  local alloc = 0

  for _, s in ipairs(STAGES) do alloc = alloc + (p[s[1] .. "_kb"] or 0) end

  print("")
  print(string.format("  %.1f KB a pass allocated, %.0f KB a second at %d passes/s",
                      alloc / p.passes, alloc / p.passes * 60, 60))

  print("")

  if p.frames > 0 then
    local per = p.px / p.frames
    print(string.format("  composed %d rects, %.1f Mpx, %.0f px a frame",
                        p.rects, p.px / 1000000.0, per))

    -- What a pixel cost, which is the number that can be compared against
    -- the C primitives measured on their own. The difference between the
    -- two is what the Lua around them is worth.
    if p.px > 0 then
      print(string.format("  %.1f ns a pixel, %.2f ms a composing pass",
                          ms(p.compose_total) * 1000000.0 / p.px,
                          ms(p.compose_total) / p.frames))
    end
    print("")
  end

  --
  -- The collector, which is the whole reason the C question is open. If
  -- the worst pass and the worst collecting pass are the same number, the
  -- collector is the jitter and moving this process to C would remove it.
  -- If the worst pass is much larger, something else is wrong and C would
  -- not have helped.
  --
  print(string.format("  %d collections, worst collecting pass %.3f ms",
                      p.collections, ms(p.gc_worst)))
  print(string.format("  heap %.0f KB", p.heap))

  if p.collections > 0 and p.busy_max > 0 then
    local blame = p.gc_worst * 100.0 / p.busy_max
    if blame > 90 then
      print("  -> the worst pass was a collection.")
    else
      print(string.format(
        "  -> the worst pass was not a collection (%.0f%% of it at most).",
        blame))
    end
  end

  print("")
end

--------------------------------------------------------------------------

if mode == "off" then
  if ask{ type = "profile", on = false } then print("frames: stopped.") end
  return
end

if mode == "read" then
  local p = ask{ type = "profile" }
  if p then report(p) end
  return
end

if mode == "on" then
  if ask{ type = "profile", on = true } then
    print("frames: measuring. `frames read` for the report.")
  end
  return
end

print(string.format("frames: measuring for %g seconds; use the desktop.",
                    seconds))

--
-- One blocking call, and the window manager answers it when the time is up.
--
-- This program cannot sleep for itself. `sys.wait_input` is the only timed
-- sleep there is and the kernel refuses it to anything that does not own
-- the console - which the window manager does, and must, because input has
-- one reader. The first version of this ignored that refusal, returned
-- instantly, and measured two passes while reporting six seconds.
--
-- Spinning instead would have been worse than wrong. `wm` charges wall
-- clock to whichever stage it is in, so a program burning processor beside
-- it puts its own preemptions into the numbers, and the worst pass would be
-- a measurement of the measuring.
--
-- Blocked in IPC costs a descheduled thread and nothing else.
--
local p = ask{ type = "profile", on = true,
               run_for = math.floor(seconds * HZ) }

if p then report(p) end
