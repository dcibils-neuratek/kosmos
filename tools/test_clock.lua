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

-- The calendar both ways: a date into days and back, over leap days, the
-- turn of a century that is not a leap year and one that is, and before
-- 1970 - what `httpcache.lua` reads an HTTP date with.
clock.set_offset(0)

for _, d in ipairs({ { 1970, 1, 1 }, { 1994, 11, 6 }, { 2000, 2, 29 }, { 2100, 3, 1 },
                     { 2026, 12, 31 }, { 1969, 12, 31 }, { 1900, 2, 28 } }) do
  local back = clock.at(clock.days(d[1], d[2], d[3]) * 86400)

  check(back.year == d[1] and back.month == d[2] and back.day == d[3],
        ("%d-%d-%d into days and back is %d-%d-%d"):format(d[1], d[2], d[3],
                                                           back.year, back.month, back.day))
end

check(clock.days(1994, 11, 6) == 784111777 // 86400, "6 November 1994 is day 9075")

-- The names a moment gives a file, and a length of time.
check(clock.stamp(t) == "2026-09-27-180000", "the stamp a program saves with: " .. clock.stamp(t))
check(clock.minute_stamp(clock.at(SEPT27 + 7 * 60 + 9)) == "2026-09-27 18.07",
      "the stamp a person is given: " .. clock.minute_stamp(clock.at(SEPT27 + 7 * 60 + 9)))
check(clock.duration(187.9) == "3:07" and clock.duration(0) == "0:00"
      and clock.duration(3600) == "60:00" and clock.duration(nil) == "0:00",
      "a length as a player shows it: " .. clock.duration(187.9))

-- A share's Modified column (`docs/sharing.html`, N6): today, yesterday
-- and anything older as a date, each against now and in local time - so
-- 01:30 UTC three hours west is still yesterday's evening.
local NOW = SEPT27 + 15 * 3600            -- 28 September, 09:00 UTC

check(clock.relative(NOW - 3600, NOW, 0) == "Today, 08:00",
      "an hour ago is today: " .. clock.relative(NOW - 3600, NOW, 0))
check(clock.relative(SEPT27 + 5 * 3600 + 51 * 60, NOW, 0) == "Yesterday, 23:51",
      "the evening before is yesterday: "
      .. clock.relative(SEPT27 + 5 * 3600 + 51 * 60, NOW, 0))
check(clock.relative(SEPT27 - 5 * 86400, NOW, 0) == "22 Sep 2026, 18:00",
      "five days before is a date: " .. clock.relative(SEPT27 - 5 * 86400, NOW, 0))
check(clock.relative(SEPT27 + 7 * 3600 + 30 * 60, NOW, -180) == "Yesterday, 22:30",
      "01:30 UTC three hours west is the evening before: "
      .. clock.relative(SEPT27 + 7 * 3600 + 30 * 60, NOW, -180))
check(clock.day_word(NOW - 60, NOW, 0) == "today"
      and clock.day_word(NOW - 86400, NOW, 0) == "yesterday"
      and clock.day_word(NOW - 6 * 86400, NOW, 0) == "22 Sep",
      "a recent server's day: " .. clock.day_word(NOW - 6 * 86400, NOW, 0))

if failed == 0 then
  print(("PASS: %d checks on the local time of a moment (UTC and west of "
         .. "it, across midnight, now, and none; the calendar both ways, the "
         .. "stamps in a file's name, a length of time, and a file's "
         .. "Modified and a recent day as a list says them)."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
