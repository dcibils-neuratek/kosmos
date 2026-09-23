-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A text size of a window's own, for the windows that are made of text in
-- the monospace face: the Terminal and Log View.
--
--   local textsize = use("/lib/textsize.lua")
--   local size = textsize.new(ui, "/home/.terminal")
--   size:face()                   -- the face to measure and draw with
--   size:size()                   -- its size, to hand `g:text`
--   size:items()                  -- the menu, built at the press
--
-- Diego, 22 September 2026, on the ThinkPad: "the monospace font in terminal
-- and log view needs to be 16px at least", and then "a way to increase font
-- size in the menu of the log viewer and terminal". The first is the looks'
-- `mono` at 16. This is the second: Larger, Smaller and Actual size, kept
-- per window in a settings file of its own like `/home/.music`, so a
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
-- by size is eight: every step but the role's own fits at once.
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
-- `path` is the window's settings file; `changed`, if given, is called
-- after a change. A choice from the window's own menu needs none: the kit
-- paints a window again after any menu choice.
--
function textsize.new(ui, path, changed)
  local self = setmetatable({ ui = ui, path = path, changed = changed },
                            methods)
  local saved = fs.read(path)

  self.px = nil

  if type(saved) == "table" and math.type(saved.text_px) == "integer" then
    self.px = nearest(saved.text_px)
  end

  return self
end

-- The desktop's `mono` size, which is Actual size.
function methods:default()
  local mono = self.ui.theme.fonts.mono

  return (mono and mono.px) or 16
end

-- The size in force: the window's own, or the desktop's.
function methods:size()
  return self.px or self:default()
end

-- What to measure with: the role itself at its own size, a face of this
-- size otherwise.
function methods:face()
  return self.ui.sized("mono", self:size())
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

  local ok, why = fs.write(self.path, { text_px = self.px })

  if not ok then
    print("textsize: not saved to " .. self.path .. ": " .. tostring(why))
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
