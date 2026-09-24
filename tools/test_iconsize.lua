-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- `/lib/iconsize.lua`: how big the icons are where a grid of them is drawn
-- (`roadmap.md` 5za). On the host, because it is arithmetic over a settings
-- file - `fs` is stood in for here, which is what makes it host-testable.
--
-- Usage: lua tools/test_iconsize.lua

local checks, fails = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    fails = fails + 1
    print("  " .. what)
  end
end

-- The settings files, in memory.
local files = {}

fs = {
  read = function(path) return files[path] end,
  write = function(path, value) files[path] = value return true end,
}

local iconsize = dofile("user/lib/iconsize.lua")

local DESK, WIN = "desktop_icon_px", "window_icon_px"

-- 1. Nothing saved: 32, which is what every icon was before this existed.
do
  files["/home/.tracker"] = nil

  local icons = iconsize.new("/home/.tracker", DESK)

  check(icons:size() == 32, "with nothing saved the size is " .. icons:size())
  check(icons.px == nil, "with nothing saved a size of its own was kept")
end

-- 2. Each of the three chosen, and written down under its own key.
do
  files["/home/.tracker"] = nil

  local icons = iconsize.new("/home/.tracker", DESK)

  check(icons:set(64) == true and icons:size() == 64,
        "64 chosen gave " .. icons:size())
  check(files["/home/.tracker"][DESK] == 64, "64 was not saved")

  check(icons:set(16) == true and icons:size() == 16,
        "16 chosen gave " .. icons:size())
  check(files["/home/.tracker"][DESK] == 16, "16 was not saved")

  check(icons:set(32) == true and icons:size() == 32,
        "32 chosen gave " .. icons:size())
  check(files["/home/.tracker"][DESK] == nil,
        "back at the default, a size of its own was still saved")

  check(icons:set(32) == false, "choosing the size already in force changed it")
end

-- 3. Two places in one file, and neither wipes the other - the desktop and
-- a Tracker window are the same program.
do
  files["/home/.tracker"] = nil

  local desk = iconsize.new("/home/.tracker", DESK)
  local win  = iconsize.new("/home/.tracker", WIN)

  desk:set(64)
  win:set(16)

  check(files["/home/.tracker"][DESK] == 64 and files["/home/.tracker"][WIN] == 16,
        "one place's choice wiped the other's: "
        .. tostring(files["/home/.tracker"][DESK]) .. " and "
        .. tostring(files["/home/.tracker"][WIN]))

  -- And read back, which is the claim that survives a restart.
  check(iconsize.new("/home/.tracker", DESK):size() == 64
        and iconsize.new("/home/.tracker", WIN):size() == 16,
        "the sizes did not come back")
end

-- 4. A size no export exists for - a file edited by hand, or a number from
-- a menu that no longer exists - is the default rather than a stretch.
do
  files["/home/.tracker"] = { [DESK] = 48 }

  check(iconsize.new("/home/.tracker", DESK):size() == 32,
        "48 saved came back as "
        .. iconsize.new("/home/.tracker", DESK):size())

  local icons = iconsize.new("/home/.tracker", DESK)

  check(icons:set(48) == false and icons:size() == 32,
        "48 was accepted from the outside")
end

-- 5. The menu: three items, exactly one marked, and choosing one takes.
do
  files["/home/.tracker"] = nil

  local icons = iconsize.new("/home/.tracker", DESK)
  local items = icons:items()
  local marked = 0

  for _, it in ipairs(items) do
    if it.mark then marked = marked + 1 end
    check(it.mark ~= nil, it.text .. " has no mark field, so the menu would "
                          .. "not give the column to every row")
  end

  check(#items == 3, "the menu is " .. #items .. " items")
  check(marked == 1, marked .. " items are marked")
  check(items[2].mark == true, "at 32 the marked item is not the second")

  items[3].on_choose()
  check(icons:size() == 64, "the third item gave " .. icons:size())
  check(icons:items()[3].mark == true and icons:items()[2].mark == false,
        "the mark did not move with the choice")
end

-- 6. The cell each size wants: the width is the icon with the same air
-- either side at every size and never below 112, so Large widens; the
-- height is the icon, a gap and two lines for a name. At 32 the pair is the
-- 84 by 72 that was compiled in before there was a choice.
do
  local GH = 16
  local w32, h32 = iconsize.cell(32, GH)

  check(w32 == 112 and h32 == 72,
        "at 32 the cell is " .. w32 .. "x" .. h32 .. ", not 112x72 - "
        .. "Diego: \"make the space for the file name wider like macos does\"")

  local w16, h16 = iconsize.cell(16, GH)

  check(w16 == 112, "at 16 the cell is " .. w16 .. " wide - below 112 a name "
                   .. "has nowhere to go, so Small keeps the width Medium has")
  check(h16 == 56, "at 16 the cell is " .. h16 .. " tall")

  local w64, h64 = iconsize.cell(64, GH)

  check(w64 == 144, "at 64 the cell is " .. w64 .. " wide, and should be the "
                    .. "icon with the same 40 pixels either side that 112 "
                    .. "gives a 32 - Diego: \"yes widen the cell at 64\"")
  check(h64 == 104, "at 64 the cell is " .. h64 .. " tall")

  -- Every step up is the same step in both, which is what makes the display
  -- harness able to hold the move of a column's last row to `N * 32`.
  check(w64 - w32 == 64 - 32 and h64 - h32 == 64 - 32,
        "a cell should grow by exactly what the icon grows by once it is "
        .. "past the floor: 32 to 64 moved it " .. (w64 - w32) .. " and "
        .. (h64 - h32))

  -- And a taller face makes a taller cell, two lines of it.
  local _, tall = iconsize.cell(32, 20)

  check(tall == h32 + 8, "a face four pixels taller made the cell "
                         .. (tall - h32) .. " taller, and a name is two lines")
end

-- 7. The changed hook, which is what recomputes a caller's cells.
do
  files["/home/.tracker"] = nil

  local told = 0
  local icons = iconsize.new("/home/.tracker", DESK,
                             function() told = told + 1 end)

  icons:set(64)
  icons:set(64)
  icons:set(16)

  check(told == 2, "the hook was called " .. told .. " times for two changes")
end

if fails == 0 then
  print(("PASS: %d checks on the icon size where a grid of them is drawn "
         .. "(the default, each of the three saved and read back, two places "
         .. "in one file, a size no export exists for, the marked menu, the "
         .. "cell each size wants, and the hook)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on iconsize.lua."):format(fails, checks))
os.exit(1)
