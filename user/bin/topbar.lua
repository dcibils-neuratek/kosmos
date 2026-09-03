-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The strip across the top of the screen.
-- kosmos: application
-- kosmos: section applications
--
--   wm topbar
--
-- Shortcuts on the left, the clock on the right, the Macintosh arrangement.
-- It is the same colour as a title bar because it is the same kind of
-- thing: chrome that belongs to the system rather than to any window.
--
-- **It takes room rather than sitting over things.** The window manager
-- gained one flag for this - `strip = "top"` - and what that flag buys is
-- that every other window opens *below* it and a maximised window stops at
-- its bottom edge. A well-behaved application that simply put itself at y=0
-- would be a window everything else opened on top of.
--
-- **What is not on it is the point.** Every screenshot of a Macintosh menu
-- bar has network, volume, battery and Bluetooth on it, and this machine
-- has none of those - no network stack, no audio, no battery, one keyboard
-- layout. Drawing five indicators for subsystems that do not exist would be
-- the same mistake as an icon for a file type nothing can tell apart: a
-- picture that lies about what the system knows. They arrive when the thing
-- they indicate arrives.
--
-- The clock is here because there is now something to ask: `/dev/clock`,
-- over a PL031 on this board. Before that, the only time this machine could
-- report was how long it had been running.

local ui    = use("/lib/ui.lua")
local clock = use("/lib/clock.lua")

local theme = ui.theme

local screen = fs.read("/dev/screen") or {}
local W = screen.width or 1024

-- As tall as a title bar, and for the same reason: it is one line of the
-- interface font with room around it, and the interface font is a setting.
-- The 20 is `TAB_H` in the window manager; a bar shorter than a title bar
-- reads as a mistake next to one, and a bar taller than the font it holds
-- is what happens when somebody picks 22-pixel text.
local H = math.max(20, gfx.font.h + 4)

local win, err = ui.window{
  title = "Topbar", w = W, h = H, strip = "top",
}

if not win then
  print("topbar: " .. tostring(err))
  return
end

--
-- What sits on the left. Names, not a menu: a menu is the Deskbar's job and
-- having two things that both open the same list would be two things to
-- keep in step.
--
-- Only applications that are actually installed, checked once here rather
-- than discovered when you click and nothing happens.
--
local WANTED = { "tracker", "gallery", "procs", "editor", "calc" }
local shortcuts = {}

for _, name in ipairs(WANTED) do
  if fs.getattr("/bin/" .. name .. ".lua") then
    shortcuts[#shortcuts + 1] = name
  end
end

local said = nil            -- what the last click did, shown briefly

--
-- One view for the whole strip.
--
-- Not a row of `ui.button`s: a button is a bevel, a label and a focus ring,
-- and none of those belong on a menu bar. What this wants is a word you can
-- click, which is a fill and a string.
--
local bar = ui.view{ x = 0, y = 0, w = W, h = H }

local function spans()
  local out = {}
  local x = 8

  for _, name in ipairs(shortcuts) do
    local w = gfx.measure(name) + 12

    out[#out + 1] = { name = name, x = x, w = w }
    x = x + w
  end

  return out
end

--
-- A gradient, in a system whose whole look is one-pixel bevels.
--
-- There is no gradient primitive and there should not be one: a fill is a
-- C loop over a rectangle and that is the right shape for almost everything
-- here. But a bar is twenty rows, so twenty fills *is* the gradient, and
-- twenty operations for the one piece of chrome that is always on screen is
-- a bargain the rest of the interface does not get offered.
--
-- Lighter at the top and settling to the theme's own tab colour by the
-- bottom, which is the direction light comes from in every other bevel in
-- this system. Overdo it and it stops looking like a lit surface and starts
-- looking like a picture of one; a sixth of the way to white is enough to
-- see and not enough to notice.
--
local function lit(colour, k)
  local a = colour & 0xff000000
  local r = (colour >> 16) & 0xff
  local gg = (colour >> 8) & 0xff
  local b = colour & 0xff

  r = r + ((255 - r) * k) // 100
  gg = gg + ((255 - gg) * k) // 100
  b = b + ((255 - b) * k) // 100

  return a | (r << 16) | (gg << 8) | b
end

-- The same, the other way: the bar's own colour with the light taken out.
local function dim(colour, k)
  local a = colour & 0xff000000
  local r = ((colour >> 16) & 0xff) * (100 - k) // 100
  local gg = ((colour >> 8) & 0xff) * (100 - k) // 100
  local b = (colour & 0xff) * (100 - k) // 100

  return a | (r << 16) | (gg << 8) | b
end

function bar:draw(g)
  local top_lift = 18          -- per cent of the way to white, at row 0

  for row = 0, self.h - 1 do
    -- Falls off over the top two thirds and is flat under that, so the bar
    -- has a lit edge rather than looking like it is fading out.
    local t = (row * 3) // 2
    local k = (t < self.h) and (top_lift * (self.h - t)) // self.h or 0

    g:fill(0, row, self.w, 1, lit(theme.tab, k))
  end

  --
  -- A lit row at the top and a shaded one at the bottom, both made from the
  -- bar's own colour.
  --
  -- This was `edge_dark` and then `line` - the two tokens every raised
  -- widget in the kit uses - and it was wrong twice over. Two rows is a
  -- band rather than an edge, and both of those tokens are near-black in a
  -- dark theme, so what appeared under the bar was a two-pixel black frame.
  -- It read as a border because that is what a border looks like, and the
  -- one thing this strip must not look like is a window.
  --
  -- A single row of the bar's colour with the light taken out of it is the
  -- underside of the bar rather than a line drawn beneath it, and it cannot
  -- go black in any theme because it is made of whatever the bar is made
  -- of. Chrome is lit, not outlined.
  --
  g:fill(0, 0, self.w, 1, lit(theme.tab, 55))
  g:fill(0, self.h - 1, self.w, 1, dim(theme.tab, 30))

  --
  -- Text with no background, which everything else here passes.
  --
  -- `g:text` fills the glyph cell before drawing into it, and a cell is a
  -- whole line of the font tall - so a background colour on a gradient
  -- punches a flat sixteen-row rectangle through it at every word. Picking
  -- "the shade of the row the text starts on" was the first attempt and it
  -- is the same bug wearing a better colour: one shade cannot match sixteen
  -- rows of a gradient. Drawn transparent, the glyphs sit on whatever is
  -- already there, which is the gradient.
  --
  local ty = (self.h - gfx.font.h) // 2


  for _, s in ipairs(spans()) do
    local on = (s.name == self.hot)

    if on then g:fill(s.x - 4, 1, s.w, self.h - 2, theme.accent) end

    -- The text's background is the row the text sits on, not `theme.tab`:
    -- `g:text` fills behind the glyphs, so a flat colour here would punch a
    -- flat rectangle through the gradient at every word.
    g:text(s.x + 2, ty, s.name,
           on and theme.text_on or theme.tab_text,
           on and theme.accent or nil)
  end

  --
  -- The right-hand end: time, then the date to the left of it.
  --
  -- Laid out from the right edge inward, so the clock does not move when a
  -- one-digit hour becomes two - which is the thing that makes a menu bar
  -- clock look unsteady.
  --
  local now = clock.now()
  local time = clock.time_string(now)
  local date = clock.date_string(now)

  local tw = gfx.measure(time)
  local dw = gfx.measure(date)

  g:text(self.w - 10 - tw, ty, time, theme.tab_text)
  g:text(self.w - 10 - tw - 12 - dw, ty, date, theme.tab_text)

  if said then
    g:text(self.w - 10 - tw - 12 - dw - 16 - gfx.measure(said), ty, said,
           theme.tab_text)
  end
end

function bar:mouse(action, x, y)
  local _ = y

  if action == "move" or action == "press" then
    self.hot = nil

    for _, s in ipairs(spans()) do
      if x >= s.x - 4 and x < s.x - 4 + s.w then self.hot = s.name end
    end
  end

  if action == "release" then
    if self.hot then
      local ok, why = fs.send("/dev/wm",
                              { type = "launch", program = self.hot })

      -- `ok` alone was not enough, and the bar said "could not start
      -- tracker" over a Tracker that had plainly started. The reply is a
      -- table; what says whether it worked is the field inside it.
      said = ok and nil or ("could not start " .. self.hot
                            .. ": " .. tostring(why))
    end

    self.hot = nil
  end

  return true
end

--
-- Redrawn on the event loop's tick, which is what makes the clock move.
--
-- The whole strip repaints rather than only the digits, and that is cheap
-- enough to be the right answer: it is one fill of a screen-wide band
-- eighteen pixels tall and a handful of strings, against the bookkeeping of
-- working out which four characters changed.
--
function bar:tick()
  self.dirty = true
end

win:add(bar)
win:run()
