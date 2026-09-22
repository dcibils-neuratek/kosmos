-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- `/lib/textsize.lua`: a window's own text size, as the Terminal and Log
-- View keep it (`roadmap.md` 5zc). On the host, because it is arithmetic
-- over a settings file and a face name - `fs` and the kit are stood in for
-- here, which is what makes it host-testable at all.
--
-- Usage: lua tools/test_textsize.lua

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

-- The kit, as much of it as this uses: the faces in force, and a face at
-- another size - the role itself when the size is the role's, as `ui.sized`
-- answers.
local function kit(px)
  return {
    theme = { fonts = { mono = { font = "ibmplexmono", px = px or 16 } } },
    sized = function(role, want)
      return want == (px or 16) and role or (role .. "@" .. want)
    end,
  }
end

local textsize = dofile("user/lib/textsize.lua")
local ui = kit()

-- 1. Nothing saved: the desktop's size, and the role's own face.
do
  files["/home/.terminal"] = nil

  local size = textsize.new(ui, "/home/.terminal")

  check(size:size() == 16, "with nothing saved the size is " .. size:size())
  check(size:face() == "mono", "with nothing saved the face is " .. size:face())
  check(size.px == nil, "with nothing saved a size of its own was kept")
end

-- 2. A step up and a step down, each written down.
do
  files["/home/.terminal"] = nil

  local size = textsize.new(ui, "/home/.terminal")

  check(size:step(1) == true and size:size() == 18,
        "a step up from 16 gave " .. size:size())
  check(files["/home/.terminal"].text_px == 18,
        "the step up was not saved")
  check(size:face() == "mono@18", "at 18 the face is " .. size:face())

  check(size:step(-1) == true and size:size() == 16,
        "a step down from 18 gave " .. size:size())
  check(files["/home/.terminal"].text_px == nil,
        "back at the desktop's size, a size of its own was still saved")

  check(size:step(-1) == true and size:size() == 14,
        "a step down from 16 gave " .. size:size())
end

-- 3. The ends: nothing past the smallest or the largest.
do
  files["/home/.log"] = { text_px = 32 }

  local size = textsize.new(ui, "/home/.log")

  check(size:size() == 32, "32 saved came back as " .. size:size())
  check(size:step(1) == false, "a step past the largest was taken")

  size:set(12)

  check(size:size() == 12 and size:step(-1) == false,
        "a step past the smallest was taken")
end

-- 4. A size saved that is not a step - a file edited by hand - is the
-- nearest step, so the menu's steps still land on themselves afterwards.
do
  files["/home/.log"] = { text_px = 21 }

  local size = textsize.new(ui, "/home/.log")

  check(size:size() == 20, "21 saved came back as " .. size:size())
end

-- 5. A desktop whose `mono` is not a step: Actual size is that size, and a
-- step from it is the next step either way rather than nothing.
do
  files["/home/.log"] = nil

  local size = textsize.new(kit(17), "/home/.log")

  check(size:size() == 17, "the desktop's 17 came back as " .. size:size())
  check(size:step(1) == true and size:size() == 18,
        "a step up from 17 gave " .. size:size())
  check(size:step(-1) == true and size:size() == 17,
        "a step down from 18 with a desktop at 17 gave " .. size:size()
        .. " - the desktop's own size is a stop, so Actual size is one of "
        .. "the places stepping lands")
end

-- 6. The menu: three choices and a separator, and they do what they say.
do
  files["/home/.terminal"] = nil

  local size = textsize.new(ui, "/home/.terminal")
  local items = size:items()

  check(#items == 4 and items[3].separator == true,
        "the menu is " .. #items .. " items with no separator in the middle")

  items[1].on_choose()
  check(size:size() == 18, "Larger text gave " .. size:size())

  items[2].on_choose()
  check(size:size() == 16, "Smaller text gave " .. size:size())

  items[1].on_choose()
  items[4].on_choose()
  check(size:size() == 16 and size.px == nil,
        "Actual size left it at " .. size:size())
end

if fails == 0 then
  print(("PASS: %d checks on a window's own text size (the desktop's size "
         .. "when nothing is saved, a step each way, the ends, a hand-edited "
         .. "size, a desktop between steps, and the menu)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on textsize.lua."):format(fails, checks))
os.exit(1)
