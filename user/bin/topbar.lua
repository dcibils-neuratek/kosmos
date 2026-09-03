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

--
-- Taller than a title bar, which it did not used to be.
--
-- It started at `TAB_H`, on the reasoning that a bar shorter than a title
-- bar reads as a mistake next to one. True, and it does not follow that
-- matching is right: a title bar is a handle on one window, and this is the
-- one strip that is always there. At the same height it read as a window
-- that had lost its way to the top of the screen.
--
-- Eight pixels of air around the line of text rather than four. Derived
-- from the font because the font is a setting, with a floor so that a small
-- font does not produce a sliver.
local H = math.max(26, gfx.font.h + 10)

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
--
-- The clock's own name in `hot`, so one field says which thing is lit.
--
-- A string nothing else can equal, rather than a boolean beside `hot`: two
-- variables that must never both be set is a state machine with an illegal
-- state in it, and this way there is one.
--
local CLOCK = "\0clock"

--
-- Sized from what the window manager *granted*, not from what was asked.
--
-- Those are two different numbers whenever the compositor clamps one, and
-- an application that draws to its own copy leaves whatever it did not
-- reach showing. The reply to `ui.window` carries the real size for exactly
-- this reason, and taking it from there means there is one copy of the
-- fact rather than two that agree until they do not.
--
local bar = ui.view{ x = 0, y = 0, w = win.w, h = win.h }

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

function bar:draw(g)
  --
  -- How far toward white the top row goes.
  --
  -- Started at 18 per cent, which measured correctly and could not be seen:
  -- on a saturated yellow the eye has very little to compare against, and a
  -- gradient nobody notices is a gradient that is not there. 38 is still
  -- well short of looking like a picture of a lit surface, which is the
  -- failure in the other direction.
  --
  local top_lift = 38

  -- Over the whole height rather than the top two thirds. With no line at
  -- the bottom the gradient is what marks the bottom, and a fade that
  -- stopped early left a flat band that looked like one.
  for row = 0, self.h - 1 do
    local k = (top_lift * (self.h - 1 - row)) // (self.h - 1)

    g:fill(0, row, self.w, 1, lit(theme.tab, k))
  end

  --
  -- No edge rows at all: the gradient is the whole of it.
  --
  -- There were two, a lit one on top and a shaded one underneath, on the
  -- argument that every raised thing in this kit has both. Three versions
  -- of that were wrong in the same direction. The first used `edge_dark`
  -- and `line`, which are near-black in a dark theme, so a two-pixel black
  -- frame appeared under the bar. The second used one row of the bar's own
  -- colour darkened, which is honest and *still reads as a line*, because
  -- a row of a different colour along an edge is what a border is - what it
  -- is made of does not change what it looks like.
  --
  -- So: none. A surface that is lighter at the top and settles by the
  -- bottom is already saying which way is up, and it says it without
  -- drawing a boundary. The strip is not an object sitting on the screen
  -- with edges; it is the top of the screen.
  --

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

  --
  -- The clock is a control, and remembering where it starts is what makes
  -- it one. A clock showing the wrong time with no way to say so from the
  -- clock is the thing every person hits first on a new machine - the
  -- setting exists, in Date & Time, and nothing on screen points at it.
  --
  self.clock_x = self.w - 10 - tw - 12 - dw

  if self.hot == CLOCK then
    g:fill(self.clock_x - 6, 1, self.w - self.clock_x + 4, self.h - 2,
           theme.accent)
  end

  local ink = (self.hot == CLOCK) and theme.text_on or theme.tab_text

  g:text(self.w - 10 - tw, ty, time, ink)
  g:text(self.clock_x, ty, date, ink)

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

    if self.clock_x and x >= self.clock_x - 6 then self.hot = CLOCK end
  end

  if action == "release" then
    if self.hot then
      local want = (self.hot == CLOCK) and "datetime" or self.hot
      local reply, why = fs.send("/dev/wm",
                                 { type = "launch", program = want })

      --
      -- An `if`, because `a and nil or b` is not a conditional.
      --
      -- This was `said = reply and nil or (...)`, which cannot produce nil:
      -- `reply and nil` is nil whatever `reply` was, and `nil or b` is b.
      -- So the bar reported "could not start tracker" over a Tracker that
      -- had plainly started, on every launch, and the reason it said `nil`
      -- for the error is that there was not one.
      --
      -- Twice I read that line and blamed the reply. The pitfall is that
      -- the expression *looks* like a ternary and is one only when the
      -- middle value can never be false.
      --
      if reply then
        said = nil
      else
        said = "could not start " .. want .. ": " .. tostring(why)
      end
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
