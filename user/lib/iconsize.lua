-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- How big the icons are, in a place that draws a grid of them: the desktop
-- and Tracker's icon view (`roadmap.md` 5za).
--
--   local iconsize = use("/lib/iconsize.lua")
--   local icons = iconsize.new("/home/.tracker", "desktop_icon_px", redraw)
--   icons:size()                  -- 16, 32 or 64, in points
--   { title = "Icons", items = icons:items() }   -- a menu, one marked
--
-- Diego, 22 September 2026: "with the new icon sizes we should also be able
-- to select icon size on desktop, tracker icon view and else", and of which
-- sizes: "16,32,64 are the correct ones".
--
-- **Those three and no others**, because those are the three Haiku exports
-- the image carries (`assets/icons/README.md`), so at 100 per cent every
-- icon is drawn pixel for pixel with nothing averaged. A fourth size would
-- be the 64 shrunk, which is what `gc:icon` does for the Deskbar's 24 and
-- is right there and wrong here: a size somebody *chooses* should be the
-- best picture there is of it.
--
-- **Points, not pixels**, like every other size once there is a scale
-- (`roadmap.md` 5z): 32 at 150 per cent is 48 pixels, averaged down from
-- the 64 by the window manager, which upgrades an icon's source itself
-- (`scale.op`). Nothing here knows the scale, and that is the point of
-- where the conversion lives.
--
-- **Kept per place, in one file with a key for each.** The desktop and a
-- Tracker window are the same program - the desktop is Tracker with the
-- frame taken off - so one settings file holds both, and `set` re-reads it
-- before writing so a window choosing its own size does not wipe what the
-- desktop chose. Two processes writing the same file at the same instant
-- would still lose one of the two, and that is a race worth naming and not
-- worth a lock: what it costs is a size somebody has to choose again.

local iconsize = {}

-- In points. The three Haiku exports, smallest first, which is the order a
-- menu lists them in.
iconsize.SIZES = { 16, 32, 64 }

-- What a place gets when it has never been asked. 32 is what every icon in
-- Kosmos was before this existed, so an upgrade changes nothing on screen.
iconsize.DEFAULT = 32

local NAMES = {
  [16] = "Small icons",
  [32] = "Medium icons",
  [64] = "Large icons",
}

local methods = {}
methods.__index = methods

local function known(px)
  for _, size in ipairs(iconsize.SIZES) do
    if size == px then return true end
  end

  return false
end

--
-- `path` is the settings file, `key` names the place inside it, and
-- `changed`, if given, is called after a change - the caller's cells are
-- worked out from the size, so something has to recompute them.
--
function iconsize.new(path, key, changed)
  local self = setmetatable({ path = path, key = key, changed = changed },
                            methods)
  local saved = fs.read(path)

  self.px = nil

  -- A file edited by hand can say anything, including a size no export
  -- exists for. Anything but one of the three is the default rather than a
  -- picture stretched to a number somebody typed.
  if type(saved) == "table" and known(saved[key]) then
    self.px = saved[key]
  end

  return self
end

function methods:size()
  return self.px or iconsize.DEFAULT
end

function methods:set(px)
  if not known(px) then return false end

  -- The default is kept as *nothing*, the same way a window's own text size
  -- is (`textsize.lua`): a place that never chose follows a default that
  -- changes rather than freezing the one that was in force the day
  -- somebody opened the menu.
  local want = (px ~= iconsize.DEFAULT) and px or nil

  if want == self.px then return false end

  self.px = want

  -- Read, change one key, write: the file holds a key for each place.
  local saved = fs.read(self.path)

  if type(saved) ~= "table" then saved = {} end

  saved[self.key] = self.px

  local ok, why = fs.write(self.path, saved)

  if not ok then
    print("iconsize: not saved to " .. self.path .. ": " .. tostring(why))
  end

  if self.changed then self.changed() end

  return true
end

-- The menu's items, marked with the one in force. Built fresh each time,
-- which is why a menu that shows these takes a function rather than a list
-- (`ui.menu_items`).
function methods:items()
  local out, now = {}, self:size()

  for _, px in ipairs(iconsize.SIZES) do
    out[#out + 1] = {
      text = NAMES[px],
      mark = (px == now),
      on_choose = function() self:set(px) end,
    }
  end

  return out
end

return iconsize
