-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What time it is here, as opposed to what time it is.
--
-- `/dev/clock` reads the board's RTC and answers in UTC, because that is
-- what the hardware knows. This turns that into the time on the wall in
-- front of whoever is looking at the screen.
--
-- **An offset, not a timezone, and the difference is not pedantry.** A
-- timezone is a table of political decisions - when a country moved its
-- clocks, which year it stopped doing so, the half-hour zones, the one that
-- is 45 minutes off - and it changes several times a year. Carrying one
-- means carrying the tzdata and saying which release, the way this system
-- carries a font and names its licence. Until that happens, calling a fixed
-- offset "your timezone" would be claiming a thing that is only true until
-- the next time the rules change under it.
--
-- So this is `UTC-03:00`, said in those words, and it does not move itself
-- twice a year. Somewhere that observes summer time will need setting
-- twice a year until there is a database, and that is the honest cost of
-- not having one.

local clock = {}

local SETTINGS = "/home/.clock"

--
-- Days since 1970 into a year, month and day.
--
-- Howard Hinnant's `civil_from_days`, exact for the proleptic Gregorian
-- calendar with no table and no loop over years: it shifts the era to begin
-- on 1 March, which puts the leap day at the *end* of the year rather than
-- in a hole in the middle of one, and then the months tile evenly.
--
-- The same lines are in `init.lua`, for `/dev/clock` itself, and that is a
-- real duplicate rather than an oversight. `init.lua` is the process that
-- serves `/lib`, so it cannot `use()` something out of a namespace it has
-- not finished building - and a machine that could not say what time it is
-- until its library server was up would be one you could not debug.
--
local function civil(days)
  local z = days + 719468
  local era = (z >= 0 and z or z - 146096) // 146097
  local doe = z - era * 146097
  local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
  local y = yoe + era * 400
  local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
  local mp = (5 * doy + 2) // 153
  local d = doy - (153 * mp + 2) // 5 + 1
  local m = mp + (mp < 10 and 3 or -9)

  if m <= 2 then y = y + 1 end

  return y, m, d
end

clock.DAYS = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }
clock.MONTHS = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" }

--
-- Minutes east of UTC. Montevideo is -180, Berlin is 60, Kathmandu is 345.
--
-- Read from the disk every time rather than cached, so changing it in the
-- settings takes effect in the bar at the top of the screen without either
-- of them knowing about the other. It is one small read a second, against a
-- clock that would otherwise be wrong until you restarted the desktop.
--
function clock.offset()
  local saved = fs.read(SETTINGS)

  if type(saved) == "table" and type(saved.offset) == "number" then
    return saved.offset
  end

  return 0
end

function clock.set_offset(minutes)
  return fs.write(SETTINGS, { offset = minutes })
end

--
-- The offset as people write it: UTC-03:00.
--
-- Signed and zero-padded, because "UTC-3" and "UTC-3:30" do not line up in
-- a list and this is chosen from a list.
--
function clock.offset_name(minutes)
  local sign = minutes < 0 and "-" or "+"
  local abs = minutes < 0 and -minutes or minutes

  return ("UTC%s%02d:%02d"):format(sign, abs // 60, abs % 60)
end

--
-- Now, where you are. Nil when the machine has no clock at all.
--
function clock.now()
  local dev = fs.read("/dev/clock")

  if type(dev) ~= "table" or not dev.epoch then return nil end

  local offset = clock.offset()
  local local_epoch = dev.epoch + offset * 60

  -- Floor division, so a machine set west of UTC on the first hours of the
  -- day lands on the previous day rather than on day zero of nothing.
  local days = local_epoch // 86400
  local secs = local_epoch % 86400

  local y, m, d = civil(days)

  return {
    year = y, month = m, day = d,
    hour = secs // 3600, min = (secs % 3600) // 60, sec = secs % 60,

    -- 1 January 1970 was a Thursday, which is where the 4 comes from.
    weekday = (days + 4) % 7,

    offset = offset,
    epoch = dev.epoch,
  }
end

function clock.time_string(now)
  now = now or clock.now()

  if not now then return "--:--" end

  return ("%02d:%02d"):format(now.hour, now.min)
end

function clock.date_string(now)
  now = now or clock.now()

  if not now then return "no clock" end

  return ("%s %d %s"):format(clock.DAYS[now.weekday + 1], now.day,
                             clock.MONTHS[now.month])
end

return clock
