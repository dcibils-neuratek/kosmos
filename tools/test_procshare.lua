-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Processes window's shares, on this machine: each process a share of
-- every tick that passed, the kernel's row beside them, and no idle row.
--
--   build/host/lua tools/test_procshare.lua

local share = assert(loadfile("user/lib/procshare.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local function by_name(rows)
  local out = {}

  for _, r in ipairs(rows) do
    -- A row that is neither a process nor the kernel's is named for what it
    -- is, so the checks below say so rather than this line failing first.
    out[r.kernel and "kernel" or (r.process and r.process.name) or "idle"] = r.pct
  end

  return out
end

local state = {}

-- The first look has nothing to measure since: every share is nought, and
-- there is no kernel row yet.
local first = share.rows(state,
  { { id = 1, name = "init", ticks = 100 }, { id = 15, name = "wm", ticks = 500 } },
  { idle_ticks = 1000, busy_ticks = 700 })

check(#first == 2 and first[1].pct == 0 and first[2].pct == 0,
      "the first look gave shares or a kernel row, with nothing to measure since")

-- A hundred ticks later: sixty idle, forty busy, thirty of them the window
-- manager's, none init's - so ten were the kernel's.
local second = share.rows(state,
  { { id = 1, name = "init", ticks = 100 }, { id = 15, name = "wm", ticks = 530 } },
  { idle_ticks = 1060, busy_ticks = 740 })

local shares = by_name(second)

check(shares.wm == 30, "wm ran 30 ticks of 100 and was given " .. tostring(shares.wm))
check(shares.init == 0, "init ran none and was given " .. tostring(shares.init))
check(shares.kernel == 10,
      "the kernel ran 40 busy ticks less 30 charged to processes, and was given "
      .. tostring(shares.kernel))

-- **No idle row**: two processes and the kernel, and nothing that reads as a
-- process at 60% when the machine was doing nothing.
check(#second == 3,
      ("three rows - two processes and the kernel - and there were %d"):format(#second))

for _, r in ipairs(second) do
  check(not r.idle and not (r.process and r.process.name == "idle"),
        "a row is the machine's idle time, which reads as a process eating it")
end

-- A process started between looks begins at nought, not at the whole of its
-- life so far, and the kernel's row never goes below nought.
local third = share.rows(state,
  { { id = 1, name = "init", ticks = 100 }, { id = 15, name = "wm", ticks = 540 },
    { id = 20, name = "music", ticks = 900 } },
  { idle_ticks = 1150, busy_ticks = 750 })

shares = by_name(third)

check(shares.music == 0,
      "a process started between looks was given " .. tostring(shares.music)
      .. " - its whole life, where it should be nought")
check(shares.wm == 10, "wm ran 10 ticks of 100 and was given " .. tostring(shares.wm))
check(shares.kernel == 0, "the kernel's share went below nought: " .. tostring(shares.kernel))

if failed > 0 then
  print(("FAIL: %d of %d checks on the Processes window's shares."):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the Processes window's shares, on this machine "
       .. "(a share of every tick, the kernel's row, a process started between "
       .. "looks at nought, and no idle row)."):format(passed))
