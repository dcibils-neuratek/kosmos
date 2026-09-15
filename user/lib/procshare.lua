-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What share of the machine each process had since the last look, for the
-- Processes window.
--
--   local share = use("/lib/procshare.lua")
--   local state = {}
--   for _, r in ipairs(share.rows(state, sys.processes(),
--                                 fs.read("/dev/kernel"))) do ... end
--
-- **A share of the machine, not of the work that happened.** Each process's
-- ticks since the last look, over every tick that passed - idle ones
-- included, which is what `sysmon` divides by - so a process at 100% is one
-- eating the machine, and an idle desktop reads near nothing everywhere.
--
-- **One row that is not a process: the kernel's**, the busy ticks the kernel
-- counted less every tick charged to a process - Nebula's own threads.
--
-- **And no idle row.** There was one, and on the ThinkPad it sat at the top of
-- the list at 99%, which a person reads as a process eating the machine.
-- Diego, 15 September: "its confusing as it looks like there is a process
-- consuming most of the cpu all the time". What is idle is what the
-- percentages leave, and Monitor draws it.
--
-- Pure: the processes and the kernel's counters come in, and `state` keeps the
-- last look, so `tools/test_procshare.lua` holds it to numbers on the Mac.

local share = {}

function share.rows(state, processes, counters)
  local elapsed = 0

  if counters and state.total then
    elapsed = (counters.idle_ticks + counters.busy_ticks) - state.total
  end

  local rows, charged, now = {}, 0, {}
  local before = state.ticks or {}

  for _, p in ipairs(processes or {}) do
    local delta = p.ticks - (before[p.id] or p.ticks)

    now[p.id] = p.ticks
    charged = charged + delta
    rows[#rows + 1] = {
      process = p,
      pct = (elapsed > 0) and math.min(100, delta * 100 // elapsed) or 0,
    }
  end

  if counters and elapsed > 0 and state.busy then
    local in_kernel = (counters.busy_ticks - state.busy) - charged

    if in_kernel < 0 then in_kernel = 0 end

    rows[#rows + 1] = { kernel = true, pct = in_kernel * 100 // elapsed }
  end

  state.ticks = now

  if counters then
    state.total = counters.idle_ticks + counters.busy_ticks
    state.busy = counters.busy_ticks
  end

  return rows
end

return share
