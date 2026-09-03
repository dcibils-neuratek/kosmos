-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where you are, in hours from UTC.
-- kosmos: application
-- kosmos: section preferences
--
-- The board's clock reads UTC and that is all it knows. This says how far
-- from it you are, so the bar at the top of the screen shows the time on
-- your wall rather than the time in Greenwich.
--
-- **An offset, not a timezone.** A timezone is a table of political
-- decisions - which years a country observed summer time, when it stopped,
-- the half-hour zones, the one that is 45 minutes off - and it is revised
-- several times a year. Having one means carrying the database and naming
-- its release, the way this system carries a font and names its licence.
-- Until then this is a number you set, it does not change itself twice a
-- year, and it is labelled `UTC-03:00` rather than "your timezone" because
-- that is what it is.
--
-- The list has the half-hour and quarter-hour offsets in it. They are not
-- decoration: India is +05:30, Nepal is +05:45, and a list of whole hours
-- would be a list that cannot describe where a billion and a half people
-- live.

local ui    = use("/lib/ui.lua")
local clock = use("/lib/clock.lua")

local W, H = 300, 300

local win, err = ui.window{ title = "Date & Time", w = W, h = H,
                            x = 240, y = 150 }

if not win then
  print("datetime: " .. tostring(err))
  return
end

--
-- Every offset in use anywhere, in minutes east of UTC.
--
-- Written out rather than generated from -12 to +14 in steps of 15, which
-- would be 105 entries of which most have never been anybody's time. These
-- are the ones that exist.
--
local OFFSETS = {
  -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180,
  -120, -60, 0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420,
  480, 540, 570, 600, 630, 660, 720, 765, 780, 840,
}

local names = {}
local index_of = {}

for i, mins in ipairs(OFFSETS) do
  names[i] = clock.offset_name(mins)
  index_of[names[i]] = mins
end

local status = ui.label{ x = 12, y = H - 30, w = W - 24, text = "" }
local shown  = ui.label{ x = 12, y = 10, w = W - 24, text = "" }

local function refresh()
  local now = clock.now()

  if not now then
    shown.text = "this machine has no clock"
    return
  end

  shown.text = ("%s  %s   %s"):format(clock.date_string(now),
                                      clock.time_string(now),
                                      clock.offset_name(now.offset))
end

local list = ui.list{
  x = 12, y = 10 + gfx.font.h + 6, w = W - 24, h = H - 80 - gfx.font.h,
  items = names,
  on_select = function(_, item)
    local mins = index_of[item]

    if not mins then return end

    local ok, why = clock.set_offset(mins)

    status.text = ok and ("set to " .. item)
                  or ("not saved: " .. tostring(why))

    refresh()
  end,
}

-- Opened on whatever is already set, rather than at the top of the list.
do
  local at = clock.offset_name(clock.offset())

  for i, name in ipairs(names) do
    if name == at then list.selected = i end
  end
end

--
-- The clock keeps running while this window is open, so the line at the top
-- has to as well - otherwise it shows the time you opened the window, which
-- is the one time it is guaranteed not to be.
--
local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function ticker:tick()
  refresh()
end

win:add(shown)
win:add(list)
win:add(status)
win:add(ticker)

refresh()
win:run()
