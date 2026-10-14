-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `user/lib/scheduling.lua` on the host (`docs/scheduler.md` G1): the
-- Scheduler's choice kept by name and put back in force at the desktop's
-- start. The kernel's answers and the settings file are stood in for, so
-- what is held is the library's own logic - which number a kept name
-- becomes, that nothing is set that is already so, and that a name the
-- kernel does not have is said and not guessed at.

local kernel = { policy = 1, quantum = 25, policies = { "priority", "round-robin" } }
local set_calls = {}

sys = {
  scheduler = function()
    return { policy = kernel.policy, quantum = kernel.quantum, policies = kernel.policies }
  end,
  set_policy = function(n)
    set_calls[#set_calls + 1] = "policy " .. n
    if not kernel.policies[n] then return nil, "no such policy" end
    kernel.policy = n
    return true
  end,
  set_quantum = function(t)
    set_calls[#set_calls + 1] = "quantum " .. t
    kernel.quantum = t
    return true
  end,
}

local stored = {}

use = function(path)
  if path == "/Kosmos/Libraries/prefs.lua" then
    return {
      open = function(name)
        stored[name] = stored[name] or {}
        local t = stored[name]
        return {
          get = function(_, k) return t[k] end,
          set = function(_, k, v) t[k] = v end,
        }
      end,
    }
  end
  error("no " .. path)
end

local scheduling = dofile("user/lib/scheduling.lua")
local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

-- Nothing kept: nothing applied, nothing set.
local ok = scheduling.apply()
check(ok and #set_calls == 0, "with nothing kept, apply set " .. table.concat(set_calls, ", "))

-- Kept by name, and the name becomes the kernel's number for it.
scheduling.keep{ policy = "round-robin", quantum = 10 }
check(stored.scheduler.policy == "round-robin" and stored.scheduler.quantum == 10,
      "keep did not keep the name and the quantum")
check(scheduling.index_of("round-robin") == 2 and scheduling.index_of("none") == nil,
      "index_of did not find round-robin as 2, or found a policy that is not there")

ok = scheduling.apply()
check(ok and kernel.policy == 2 and kernel.quantum == 10,
      ("apply left policy %d, quantum %d"):format(kernel.policy, kernel.quantum))

-- Already so: nothing set again.
set_calls = {}
ok = scheduling.apply()
check(ok and #set_calls == 0, "apply set what was already in force: " .. table.concat(set_calls, ", "))

-- Keeping one part leaves the other.
scheduling.keep{ quantum = 5 }
check(stored.scheduler.policy == "round-robin" and stored.scheduler.quantum == 5,
      "keeping the quantum touched the policy")

-- A policy the kernel does not have: said, and nothing guessed.
scheduling.keep{ policy = "fifo" }
set_calls = {}
local why
ok, why = scheduling.apply()
check(not ok and tostring(why):find("no policy called fifo", 1, true)
      and not table.concat(set_calls, " "):find("policy", 1, true),
      "an unknown policy was not said, or something was set for it: " .. tostring(why))

if #fails == 0 then
  print(("PASS: %d checks on scheduling.lua (a choice kept by name, made the kernel's number "
         .. "at the start, nothing set that is already so, an unknown policy said)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on scheduling.lua:"):format(#fails, checks))
for _, f in ipairs(fails) do print("  " .. f) end
os.exit(1)
