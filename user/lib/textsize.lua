-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A text size of a window's own, for the windows that are made of text:
-- the Terminal and Log View in the monospace face, and Text Editor in the
-- text face (`roadmap.md` 6zs).
--
--   local textsize = use("/Kosmos/Libraries/textsize.lua")
--   local size = textsize.new(ui, "terminal")
--   local size = textsize.new(ui, "texteditor", nil, "ui")
--   size:face()                   -- the face to measure and draw with
--   size:size()                   -- its size, to hand `g:text`
--   size:items()                  -- the menu, built at the press
--
-- Diego, 22 September 2026, on the ThinkPad: "the monospace font in terminal
-- and log view needs to be 16px at least", and then "a way to increase font
-- size in the menu of the log viewer and terminal". The first is the looks'
-- `mono` at 16. This is the second: Larger, Smaller and Actual size, kept
-- per window in a settings file of its own like `/Home/Preferences/music`, so a
-- Terminal made larger is still larger tomorrow.
--
-- **A size of its own, not the desktop's `mono`.** Changing the role would
-- change every terminal-like window at once, and the Appearance panel has
-- no face sizes any more on purpose (`roadmap.md` 5y). A window that is
-- only text is the one place a person reasonably wants its text alone
-- larger - and it is independent of the scale (`roadmap.md` 5z), which
-- multiplies this too.
--
-- **The face is asked for on every draw**, through `ui.sized`, and never
-- kept: the kit gives sized faces back whenever the desktop's faces change
-- (`gfx.release_faces`), so an index held across that would name a slot
-- that is gone.
--
-- **The caller's `ui`, handed in**, not one of this file's own: `use` runs
-- a library again and returns a new table, and only the kit the window was
-- opened with is told when the faces change.

local textsize = {}

-- The steps, in pixels at 100 per cent. Eight, and the kit's pool of faces
-- by size is sixty-four: every step fits at once, with room to spare.
textsize.STEPS = { 12, 14, 16, 18, 20, 24, 28, 32 }

local methods = {}
methods.__index = methods

local function nearest(px)
  local best, far = textsize.STEPS[1], math.huge

  for _, step in ipairs(textsize.STEPS) do
    if math.abs(step - px) < far then best, far = step, math.abs(step - px) end
  end

  return best
end

--
-- `name` is the window's settings, as the settings kit names them
-- (`prefs.lua`: "terminal" is `/Home/Preferences/terminal`); `changed`, if
-- given, is called after a change. A choice from the window's own menu
-- needs none: the kit paints a window again after any menu choice. `role`
-- is the face being sized, `mono` unless it says otherwise.
--
function textsize.new(ui, name, changed, role)
  local store = use("/Kosmos/Libraries/prefs.lua").open(name)
  local self = setmetatable({ ui = ui, store = store, changed = changed,
                              role = role or "mono" }, methods)
  local saved = store.text_px

  self.px = nil

  if math.type(saved) == "integer" then
    self.px = nearest(saved)
  end

  return self
end

-- The desktop's size for the role, which is Actual size.
function methods:default()
  local face = self.ui.theme.fonts[self.role]

  return (face and face.px) or 16
end

-- The size in force: the window's own, or the desktop's.
function methods:size()
  return self.px or self:default()
end

-- What to measure with: the role itself at its own size, a face of this
-- size otherwise.
function methods:face()
  return self.ui.sized(self.role, self:size())
end

function methods:set(px)
  --
  -- The desktop's own size is kept as *nothing*, so a window follows a look
  -- that changes it rather than freezing the size it had when it was chosen.
  --
  -- Written out rather than `(px == self:default()) and nil or px`, which
  -- cannot ever give nil - `true and nil` is nil, and `nil or px` is px. The
  -- test caught it: Actual size left the size where it was.
  --
  local want = px

  if px == self:default() then want = nil end

  if want == self.px then return false end

  self.px = want

  -- **Into the file, beside whatever else the window keeps there** - Text
  -- Editor's recent documents - rather than over it: the settings kit reads,
  -- changes this one key and writes.
  local ok, why = self.store:set("text_px", self.px)

  if not ok then
    print("textsize: not saved to " .. self.store:path() .. ": " .. tostring(why))
  end

  if self.changed then self.changed() end

  return true
end

-- The stops: the steps, and the desktop's own size among them - a look may
-- name a size that is not a step, and Actual size has to be somewhere a
-- person stepping through can land.
function methods:stops()
  local out, default = {}, self:default()

  for _, step in ipairs(textsize.STEPS) do
    if step ~= default then out[#out + 1] = step end
  end

  out[#out + 1] = default
  table.sort(out)

  return out
end

-- The next stop up or down from the size in force, and nothing past the
-- ends.
function methods:step(by)
  local now, stops, pick = self:size(), self:stops(), nil

  if by > 0 then
    for i = 1, #stops do
      if stops[i] > now then pick = stops[i] break end
    end
  else
    for i = #stops, 1, -1 do
      if stops[i] < now then pick = stops[i] break end
    end
  end

  return pick ~= nil and self:set(pick)
end

-- The menu, which is behind the `...` in the window's header (`ui.md`
-- 16.20) and was a `View` menu on a menu bar before that. Built each time
-- it is opened, so `Actual size` knows what the desktop's size is *now*.
function methods:items()
  return {
    { text = "Larger text",  on_choose = function() self:step(1) end },
    { text = "Smaller text", on_choose = function() self:step(-1) end },
    { separator = true },
    { text = "Actual size",  on_choose = function() self:set(self:default()) end },
  }
end

return textsize
