-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `user/lib/automations.lua` on the host (`docs/automations.md` A3): when an
-- automation fires next, what a step found put into the next one's words,
-- and what makes a file not an automation. The clock is a table here, so
-- a Monday at 7:59 and a Saturday night can be asked about without waiting
-- for them - the part of `autod` a suite in a guest would take a day to see.

fs = { read = function() return nil end, list = function() return {} end }
sys = { ticks = function() return 0 end }
use = function() return {} end

local A = dofile("user/lib/automations.lua")
local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

-- Monday 12 October 2026, 07:59:30, in a zone the clock has already applied:
-- what `clock.at` returns - the weekday 1, Monday.
local MON_0759 = { epoch = 1000000000, hour = 7, min = 59, sec = 30, weekday = 1 }
local midnight = MON_0759.epoch - 7 * 3600 - 59 * 60 - 30

local function at(time, days) return { kind = "at", time = time, days = days } end

check(A.next_time(at("8:00"), MON_0759) == midnight + 8 * 3600,
      "8:00 every day, asked at 7:59:30, was not this morning")
check(A.next_time(at("7:00"), MON_0759) == midnight + 86400 + 7 * 3600,
      "7:00, already gone today, was not tomorrow")
check(A.next_time(at("8:00", { "mon" }), MON_0759) == midnight + 8 * 3600,
      "8:00 on Mondays, asked on a Monday before eight, was not today")
check(A.next_time(at("7:00", { "mon" }), MON_0759) == midnight + 7 * 86400 + 7 * 3600,
      "7:00 on Mondays, gone today, was not next Monday")
check(A.next_time(at("9:30", { "sat", "sun" }), MON_0759) == midnight + 5 * 86400 + 9 * 3600 + 30 * 60,
      "a weekend time asked on a Monday was not Saturday")
check(A.next_time({ kind = "every", minutes = 30 }, MON_0759) == MON_0759.epoch + 1800,
      "every 30 minutes, never run, was not half an hour from now")
check(A.next_time({ kind = "every", minutes = 30 }, MON_0759, MON_0759.epoch - 600) == MON_0759.epoch + 1200,
      "every 30 minutes did not count from its last run")
check(A.next_time({ kind = "hand" }, MON_0759) == nil, "an automation run by hand had a time")
check(A.next_time({ kind = "folder", path = "/Home" }, MON_0759) == nil, "a folder trigger had a time")

-- What a step found, in the next one's words; a name nothing found is left.
local found = { ["page title"] = "Kosmos builds", file = "/Home/a.txt" }
check(A.fill("Good morning - {page title}", found) == "Good morning - Kosmos builds",
      "{page title} was not filled")
check(A.fill("Copied {file} and {nothing}", found) == "Copied /Home/a.txt and {nothing}",
      "a name nothing found was not left as written")
check(A.fill(42, found) == 42, "a number was not left as it was")

-- What makes a file not an automation, in words.
local good = { name = "Morning", when = at("8:00", { "mon" }),
               steps = { { verb = "press", app = "Mail", control = "Get Mail" } } }
check(A.check(good) == true, "a good automation was refused")

local function refused(a, words)
  local ok, why = A.check(a)
  return not ok and tostring(why):find(words, 1, true) ~= nil, why
end

local r, why = refused({ name = "x", when = { kind = "sometimes" }, steps = good.steps }, "what starts it")
check(r, "an unknown trigger was not refused: " .. tostring(why))
r, why = refused({ name = "x", when = at("25:00"), steps = good.steps }, "hours:minutes")
check(r, "25:00 was not refused: " .. tostring(why))
r, why = refused({ name = "x", when = at("8:00", { "funday" }), steps = good.steps }, "funday is not a day")
check(r, "a day that is not one was not refused: " .. tostring(why))
r, why = refused({ name = "x", when = { kind = "every" }, steps = good.steps }, "needs minutes")
check(r, "every without minutes was not refused: " .. tostring(why))
r, why = refused({ name = "x", when = at("8:00"), steps = { { verb = "press", app = "Mail" } } }, "needs its control")
check(r, "a press without its control was not refused: " .. tostring(why))
r, why = refused({ name = "x", when = at("8:00"), steps = { { verb = "format_disk" } } }, "does nothing Automations knows")
check(r, "a verb that does not exist was not refused: " .. tostring(why))
r, why = refused({ name = "a/b", when = at("8:00"), steps = good.steps }, "no name")
check(r, "a name with a slash was not refused: " .. tostring(why))

-- A step said as a sentence, for the history.
check(A.said({ verb = "press", app = "Mail", control = "Get Mail" }) == "Press Get Mail in Mail",
      "a press was not said as a sentence")

if #fails == 0 then
  print(("PASS: %d checks on automations.lua (a time today, tomorrow, next Monday and at the "
         .. "weekend; every so many minutes from its last run; what a step found filled in and "
         .. "a name nothing found left; seven ways a file is not an automation, each in words)")
        :format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on automations.lua:"):format(#fails, checks))
for _, f in ipairs(fails) do print("  " .. f) end
os.exit(1)
