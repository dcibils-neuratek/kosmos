-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The window manager's frame profile, a part of `wm.lua` in a file of its
-- own (`roadmap.md` 6zn). What follows is the section as it stood there;
-- its state is fields of the table this returns, which `wm.lua` reads and
-- sets as `P.profiling`, `P.measuring` and the rest - a flag one side
-- sets and the other reads has to be one value, never a copy of it.
--
-- The frame profile.
--
-- Where a pass of the loop below actually goes, measured rather than
-- argued. The system is aiming at a fast desktop on a Pi 5 and every
-- discussion about moving this process into C has had to guess at whether
-- its Lua half is five per cent of a frame or fifty. This is the thing
-- that answers that.
--
-- **Off by default, and costing nothing while it is off.** The timer reads
-- are behind `profiling`, so a desktop nobody asked to measure itself does
-- not make eight extra syscalls a hundred times a second.
--
-- **Waiting is not working, and the two are counted apart.** Most of a
-- pass on an idle desktop is `wait_input` sleeping, which is the loop
-- doing its job rather than costing anything. `busy` is every stage except
-- that one, and `busy.max` - the worst pass ever seen - is the number that
-- matters: responsiveness is a promise about the worst case.
--
-- **Pixels, so compose time can be divided by something.** Knowing that
-- compose took 3 ms means nothing without knowing whether it drew a
-- cursor or the whole screen. With both, the quotient can be compared
-- against what the C primitives do on their own, and the difference is
-- what the Lua around them costs.
--
-- **And the collector, because that is the whole argument.** C is proposed
-- here for jitter rather than throughput: a GC pause is about 1.25 ms and
-- arrives when it chooses, against a 16 ms frame. So a pass during which
-- the heap shrank is a pass a collection finished in, and the worst of
-- those is recorded separately. If the worst pass overall is also a
-- collection pass, the argument is made. If it is not, it is not.

local P = {}


P.STAGES = { "wait", "keys", "messages", "pointer",
                 "waiting", "collect", "compose" }

P.profiling = false

--
-- Whether *this* pass is being measured, decided once at the top of it.
--
-- Not the same question as `profiling`, and conflating them was a crash on
-- the first run: `frames` turns the profile on by sending a message, which
-- is handled in the middle of a pass, so a pass that began unmeasured
-- reached the next stage boundary with no reading to subtract from. A pass
-- measures throughout or not at all - which is also the only way its
-- stages can add up to its total.
--
P.measuring = false

P.prof = nil
P.pass_busy = 0

--
-- A client blocked in `fs.send` until its measurement is over.
--
-- `frames` cannot sleep for itself: `sys.wait_input` is refused to anything
-- that does not own the console, and this process owns it. Spinning instead
-- would be worse than useless - a program burning processor beside the loop
-- it is measuring lands in whichever stage the loop was preempted in, and
-- the measurement would be of the measuring.
--
-- So the wait happens here, and the client spends it blocked in IPC, which
-- costs a descheduled thread and nothing else. Same shape as `poll`.
--
P.waiting = nil

function P.reset()
  P.prof = { passes = 0, frames = 0, rects = 0, px = 0, drawn = 0,
           busy = { total = 0, max = 0 },
           gc = { collections = 0, worst = 0 } }

  for _, name in ipairs(P.STAGES) do
    P.prof[name] = { total = 0, max = 0, kb = 0 }
  end
end

--
-- Charge the time since `t0` to a stage, and hand back the reading so the
-- next stage starts where this one ended - one clock read per boundary
-- rather than two.
--
-- **And the heap it grew**, which is why this now takes and returns two
-- numbers instead of one.
--
-- The first profile said the Lua half of this loop is about a tenth of a
-- busy pass and composing is the rest - so the question of rewriting it in
-- C was answered no. What the same profile also said was that the worst
-- collecting pass was 5.7 ms against a 16 ms frame, and *that* is not a
-- question about which language the loop is written in. It is a question
-- about what the loop allocates, and nothing here could say.
--
-- Per stage rather than per pass, for the same reason the times are per
-- stage: "this pass allocated 40 KB" tells you there is a problem and
-- nothing about where.
--
-- `collectgarbage("count")` is a call per stage boundary, seven a pass, and
-- only when measuring. A stage in which a collection ran shows a *fall*,
-- and a fall is charged as zero rather than as a negative: it did allocate,
-- and how much is unknowable once something else freed more.
--
function P.charge(stage, t0, h0, idle)
  local now = sys.ticks()
  local heap = collectgarbage("count")
  local took = now - t0
  local grew = heap - h0
  local s = P.prof[stage]

  s.total = s.total + took
  if took > s.max then s.max = took end
  if grew > 0 then s.kb = s.kb + grew end
  if not idle then P.pass_busy = P.pass_busy + took end

  return now, heap
end

-- Everything measured, flattened: the serialiser crosses this as a table of
-- scalars, and a stage is two numbers rather than a structure worth naming
-- twice. Times are counter ticks - what a tick is worth is `/Devices/cpu`'s
-- business and the reporting program's, not this one's.
function P.report()
  local out = { ok = true, profiling = P.profiling,
                passes = P.prof.passes, frames = P.prof.frames,
                rects = P.prof.rects, px = P.prof.px, drawn = P.prof.drawn,
                busy_total = P.prof.busy.total, busy_max = P.prof.busy.max,
                collections = P.prof.gc.collections, gc_worst = P.prof.gc.worst,
                heap = collectgarbage("count") }

  for _, name in ipairs(P.STAGES) do
    out[name .. "_total"] = P.prof[name].total
    out[name .. "_max"]   = P.prof[name].max
    out[name .. "_kb"]    = P.prof[name].kb
  end

  return out
end

--
-- Answer a client whose measurement is over.
--
-- Called at the very top of a pass, before that pass decides whether it is
-- being measured, and stopping first so that it is not. Building a report
-- takes a few microseconds and charging them to a stage would land them in
-- `max` - which is the one number this whole thing exists to report, and
-- the last place to put an artefact of reporting it.
--
function P.due()
  if not P.waiting then return end
  if sys.ticks() < P.waiting.deadline then return end

  local who = P.waiting.who

  P.profiling = false
  P.waiting = nil

  pcall(sys.reply, who, P.report())
end

return P
