-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What time it is here, as opposed to what time it is.
--
-- `/Devices/clock` reads the board's RTC and answers in UTC, because that is
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

-- Kept by the settings kit, as "clock" (`prefs.lua`).
local function prefs() return use("/Kosmos/Libraries/prefs.lua") end

--
-- Days since 1970 into a year, month and day.
--
-- Howard Hinnant's `civil_from_days`, exact for the proleptic Gregorian
-- calendar with no table and no loop over years: it shifts the era to begin
-- on 1 March, which puts the leap day at the *end* of the year rather than
-- in a hole in the middle of one, and then the months tile evenly.
--
-- The same lines are in `init.lua`, for `/Devices/clock` itself, and that is a
-- real duplicate rather than an oversight. `init.lua` is the process that
-- serves `/Kosmos/Libraries`, so it cannot `use()` something out of a namespace it has
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

--
-- **And back: a year, month and day into days since 1970** - Hinnant's
-- `days_from_civil`, the same shifted era the other way. For a date that
-- arrives as words rather than as a count, an HTTP `Date` or `Expires`
-- (`httpcache.lua`), which kept its own copy until the review before 0.11
-- found the calendar in two halves in two files.
--
function clock.days(y, m, d)
  y = m <= 2 and y - 1 or y

  local era = (y >= 0 and y or y - 399) // 400
  local yoe = y - era * 400
  local doy = (153 * (m + (m > 2 and -3 or 9)) + 2) // 5 + d - 1
  local doe = yoe * 365 + yoe // 4 - yoe // 100 + doy

  return era * 146097 + doe - 719468
end

clock.DAYS = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }
clock.MONTHS = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" }
clock.FULL_MONTHS = { "January", "February", "March", "April", "May", "June",
                      "July", "August", "September", "October", "November",
                      "December" }

--
-- Minutes east of UTC. Montevideo is -180, Berlin is 60, Kathmandu is 345.
--
-- Read from the disk every time rather than cached, so changing it in the
-- settings takes effect in the bar at the top of the screen without either
-- of them knowing about the other. It is one small read a second, against a
-- clock that would otherwise be wrong until you restarted the desktop.
--
function clock.offset()
  local saved = prefs().read("clock")

  if type(saved.offset) == "number" then
    return saved.offset
  end

  return 0
end

function clock.set_offset(minutes)
  return prefs().open("clock"):set("offset", minutes)
end

--
-- **The offsets anybody lives at**, in minutes east of UTC: every whole hour
-- from -12 to +14, and the half and quarter hours places actually keep -
-- Newfoundland, India, Nepal, the Chathams. One list, read by Preferences'
-- Date & Time page - which a window of its own read too until 5 October
-- 2026, when it went for being the page twice.
--
clock.OFFSETS = {
  -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180,
  -120, -60, 0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420,
  480, 540, 570, 600, 630, 660, 720, 765, 780, 840,
}

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
  local dev = fs.read("/Devices/clock")

  if type(dev) ~= "table" or not dev.epoch then return nil end

  return clock.at(dev.epoch)
end

--
-- **Any moment, where you are**: `now`'s answer for `epoch` rather than for
-- this second - a file's Modified (`roadmap.md` 6za step b), which is a
-- date the disk kept rather than the time it is.
--
function clock.at(epoch, offset)
  offset = offset or clock.offset()
  local local_epoch = epoch + offset * 60

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
    epoch = epoch,
  }
end

function clock.time_string(now)
  now = now or clock.now()

  if not now then return "--:--" end

  return ("%02d:%02d"):format(now.hour, now.min)
end

-- As the drawings write a file's date: 27 September 2026, 18:42.
function clock.long_string(t)
  if not t then return "no clock" end

  return ("%d %s %d, %02d:%02d"):format(t.day, clock.FULL_MONTHS[t.month],
                                        t.year, t.hour, t.min)
end

--
-- **A moment as a list of files says it** (`docs/sharing.html`'s Modified
-- column): "Today, 11:20", "Yesterday, 23:51", or "3 Oct 2026, 22:14" -
-- against `now`, both seconds since 1970, in local time at `offset`
-- minutes (`clock.offset()` when not given: a list of two thousand asks it
-- once and hands it in, since it is a read of the settings each time).
--
function clock.relative(epoch, now, offset)
  offset = offset or clock.offset()

  local t = clock.at(epoch, offset)
  local day = (epoch + offset * 60) // 86400
  local today = (now + offset * 60) // 86400
  local hm = ("%02d:%02d"):format(t.hour, t.min)

  if day == today then return "Today, " .. hm end
  if day == today - 1 then return "Yesterday, " .. hm end

  return ("%d %s %d, %s"):format(t.day, clock.MONTHS[t.month], t.year, hm)
end

--
-- **And a day as a list of recent things says it**: "today", "yesterday",
-- or "2 Oct" - Connect to Server's recent servers.
--
function clock.day_word(epoch, now, offset)
  offset = offset or clock.offset()

  local day = (epoch + offset * 60) // 86400
  local today = (now + offset * 60) // 86400

  if day == today then return "today" end
  if day == today - 1 then return "yesterday" end

  local t = clock.at(epoch, offset)

  return ("%d %s"):format(t.day, clock.MONTHS[t.month])
end

function clock.date_string(now)
  now = now or clock.now()

  if not now then return "no clock" end

  return ("%s %d %s"):format(clock.DAYS[now.weekday + 1], now.day,
                             clock.MONTHS[now.month])
end

--
-- **A moment in the name of a file a program saves**: 2026-10-05-142301.
-- No spaces, so a prompt can name it, and it sorts as the moments do.
-- `screenshot`, `profile` and Disk Benchmark each wrote it out themselves.
--
function clock.stamp(t)
  return ("%04d-%02d-%02d-%02d%02d%02d"):format(t.year, t.month, t.day,
                                               t.hour, t.min, t.sec)
end

--
-- **And in the name of one a person keeps**: 2026-10-05 14.23, to the
-- minute and with a dot where a colon may not go - a recording's, an
-- export's - and a number after it for a second one in the same minute,
-- which is `files.free_name`'s to add.
--
function clock.minute_stamp(t)
  return ("%04d-%02d-%02d %02d.%02d"):format(t.year, t.month, t.day,
                                            t.hour, t.min)
end

--
-- **How long something lasts, as a player shows it**: 3:07 - minutes, and
-- the seconds in two digits. An hour is 60:00 rather than 1:00:00, as a
-- song's length and a film's place have always been written here. Music,
-- Video, the camera's recording and Groove's song each had a copy.
--
function clock.duration(seconds)
  local whole = math.floor(tonumber(seconds) or 0)

  return ("%d:%02d"):format(whole // 60, whole % 60)
end

return clock
