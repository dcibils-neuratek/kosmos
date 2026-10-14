-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- How the machine schedules, as a person chose it: the policy and the
-- quantum kept as settings, and put in force when the desktop starts.
--
--   local scheduling = use("/Kosmos/Libraries/scheduling.lua")
--   scheduling.saved()                 -> { policy = "round-robin", quantum = 10 }
--   scheduling.keep{ policy = name }   the choice kept, nothing else touched
--   scheduling.apply()                 -> what was put in force, and why not
--
-- **One place for both halves** (`docs/scheduler.md` G1): the Scheduler
-- window keeps what is chosen in it, and the window manager applies it once
-- as the desktop starts, as it applies the look - so a machine schedules the
-- way its person left it after every start. Diego, 11 October 2026: "I cant
-- change the scheduler policy any more", and of a restart: "it just might
-- need a reboot". It does not: since 13 October each core changes over by
-- itself (`sched_switch_to`), so a choice takes effect at once and the
-- setting is only what brings it back.
--
-- **The policy is kept by its name**, not its number: the number is the
-- kernel's order of policies, which a new one would shift, and a setting
-- that silently meant another policy after an update is the failure this
-- avoids. A name the kernel does not have is left alone and said.

local scheduling = {}

local prefs = use("/Kosmos/Libraries/prefs.lua")

scheduling.NAME = "scheduler"

local function file() return prefs.open(scheduling.NAME, {}) end

-- What was chosen: a table with `policy` and `quantum` where they were.
function scheduling.saved()
  local f = file()

  return { policy = f:get("policy"), quantum = tonumber(f:get("quantum")) }
end

-- A choice kept: `t.policy` a name, `t.quantum` ticks.
function scheduling.keep(t)
  local f = file()

  if t.policy ~= nil then f:set("policy", t.policy) end
  if t.quantum ~= nil then f:set("quantum", t.quantum) end
end

-- The kernel's number for a policy's name, or nil.
function scheduling.index_of(name, info)
  for i, n in ipairs((info or sys.scheduler() or {}).policies or {}) do
    if n == name then return i end
  end
end

-- What was kept, put in force: true, or false and a sentence for each part
-- that could not be.
function scheduling.apply()
  local info = sys.scheduler()
  local kept = scheduling.saved()
  local said = {}

  if not info then return false, "this machine does not report its scheduler" end

  if kept.policy then
    local index = scheduling.index_of(kept.policy, info)

    if not index then
      said[#said + 1] = ("no policy called %s here"):format(kept.policy)
    elseif index ~= info.policy then
      local ok, why = sys.set_policy(index)

      if not ok then said[#said + 1] = ("%s refused: %s"):format(kept.policy, tostring(why)) end
    end
  end

  if kept.quantum and kept.quantum ~= info.quantum then
    local ok, why = sys.set_quantum(kept.quantum)

    if not ok then said[#said + 1] = ("a quantum of %d refused: %s"):format(kept.quantum, tostring(why)) end
  end

  return #said == 0, table.concat(said, "; ")
end

return scheduling
