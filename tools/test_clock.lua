-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The local time of a moment, checked on this computer with no machine booted.
--
-- `user/lib/clock.lua` turns seconds since 1970 into where you are: the
-- Deskbar's clock from now, and Info's Modified from the date a file was
-- written (`roadmap.md` 6za step b). The zone is a file in /Home; here it
-- is a table, and the moment is chosen.
--
--   build/host/lua tools/test_clock.lua

local saved = nil

fs = {
  read = function(path)
    if path == "/Devices/clock" then return { epoch = 1790532000, utc = 1 } end
    return saved
  end,
  write = function(_, value) saved = value return true end,
  getattr = function() return { kind = "directory" } end,
  send = function() return { ok = true } end,
}

-- The settings kit, as `use` reaches it in a process.
use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local clock = dofile("user/lib/clock.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

local SEPT27 = 1790532000         -- 27 September 2026, 18:00:00 UTC

local t = clock.at(SEPT27)

check(t.year == 2026 and t.month == 9 and t.day == 27 and t.hour == 18
      and t.min == 0 and clock.DAYS[t.weekday + 1] == "Sun",
      "a moment in UTC is Sunday 27 September 2026, 18:00")

check(clock.long_string(t) == "27 September 2026, 18:00",
      "the long form is the drawings': " .. clock.long_string(t))

clock.set_offset(-180)

local here = clock.at(SEPT27 + 42 * 60)

check(clock.long_string(here) == "27 September 2026, 15:42",
      "three hours west, the same moment is 15:42: "
      .. clock.long_string(here))

check(clock.long_string(clock.at(SEPT27 - 17 * 3600)) == "26 September 2026, 22:00",
      "early in the day west of UTC is the day before: "
      .. clock.long_string(clock.at(SEPT27 - 17 * 3600)))

check(clock.now().epoch == SEPT27 and clock.now().hour == 15,
      "now is the clock's moment, where you are")

check(clock.long_string(nil) == "no clock",
      "no moment says so rather than drawing a date")

if failed == 0 then
  print(("PASS: %d checks on the local time of a moment (UTC and west of "
         .. "it, across midnight, now, and none)."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
