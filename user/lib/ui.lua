-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The UI kit: a view tree, follow modes, and widgets.
--
--   local ui = use("/lib/ui.lua")
--
--   local win = ui.window{ title = "example", w = 400, h = 240 }
--   win:add(ui.label{ x = 12, y = 12, text = "Hello" })
--   win:add(ui.button{ x = 12, y = 40, w = 120, text = "Press me",
--                      on_click = function() ... end })
--   win:run()
--
--------------------------------------------------------------------------
-- What this is, and what it deliberately is not.
--
-- `ui.md` 16.2 and 16.6. A view is a rectangle with children and a draw
-- handler, and drawing produces *commands* rather than pixels: `gc:fill`
-- and `gc:text` append tables to a list, and the list goes to the window
-- manager in a message. Nothing here ever touches a pixel, and nothing here
-- computes a pixel offset - `gfx.md` 19.3, and the pitch is not width * 4.
--
-- Because the commands are data rather than calls, a view that has not
-- changed can have its list resent without re-running its handler, and a
-- window's drawing can be logged, replayed, or inspected from the shell.
-- BeOS could do none of that: it handed the application a pointer into the
-- app_server's buffer with a lock around it.
--
-- No pointer device exists yet, so this is keyboard-driven throughout: Tab
-- moves the focus, Enter and Space activate. That is not a placeholder to
-- be thrown away - a UI that cannot be driven from the keyboard is a UI
-- that has decided some people should not use it - and the pointer, when it
-- arrives, sets focus and clicks the thing under it, which is two lines
-- against the same model.
--------------------------------------------------------------------------

local theme = use("/lib/theme.lua")
local wmproto = use("/lib/wmproto.lua")

local ui = { theme = theme }

--
-- The two clock rates, asked once.
--
-- `sys.ticks()` is the counter and every timeout is in scheduler ticks, so
-- anything converting between them needs both. They are read on first use
-- rather than at load: this is a library, and a process that never opens a
-- window should not pay a `/dev/cpu` read for it. The lazy default used to
-- do that read *every pass* of every window's loop, which is a syscall a
-- hundred times a second to learn a number that cannot change.
--
local cached_tick_hz, cached_per_tick

local function ui_tick_hz()
  if not cached_tick_hz then
    cached_tick_hz = (sys.info() or {}).tick_hz or 100
  end

  return cached_tick_hz
end

--
-- **How long "again" is**, for a second click: a second of the counter, read
-- from the machine rather than assumed. The tree measured why it is a whole
-- second and not half of one (its comment, and `ui.md` 16.8c); the list uses
-- the same number, so a second click means the same thing in both.
--
local cached_again

local function ui_again()
  if not cached_again then
    cached_again = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
  end

  return cached_again
end

local function ui_per_tick()
  if not cached_per_tick then
    local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

    cached_per_tick = math.max(1, hz // ui_tick_hz())
  end

  return cached_per_tick
end

local GW, GH = gfx.font.w, gfx.font.h

--
-- **The fixed layout** (`theme.metrics`, `roadmap.md` 5x): a row is 24
-- pixels, a button 28, a field 26, whatever face is in force. Widgets are
-- those sizes; the words in them are placed by the face - centred in the
-- box by `gfx.height()`, asked when drawn - and a look's faces are chosen
-- to fit. `GH` is the face as it stood when this file loaded, and it used
-- to size rows too, which is how a larger face moved everything below it.
--
local ROW, BUTTON, FIELD = theme.metrics.row, theme.metrics.button,
                           theme.metrics.field

ui.metrics = theme.metrics

-- Where a line of words of the face in force starts, in a box `h` tall.
local function centred(h) return (h - gfx.height()) // 2 end

--------------------------------------------------------------------------
-- The graphics context.
--
-- Accumulates commands with an origin and a clip rectangle, so a view draws
-- in its own coordinates and cannot draw outside itself. The clip is
-- applied here rather than sent along, because the window manager's
-- primitives clip to the *window* and know nothing about views.
--------------------------------------------------------------------------

local gc = {}

--------------------------------------------------------------------------
-- A colour, resolved at the moment it is drawn.
--
-- Widgets take colours as either a number or the *name* of one in the
-- palette, and a name is looked up on every draw. That is what makes an
-- application follow a theme change.
--
-- The distinction matters because the obvious thing does not work:
-- `ui.label{ color = theme.text }` reads the palette once, at
-- construction, and freezes that number. The window then follows a theme
-- change while the label inside it does not - which is how a light theme
-- ended up with near-invisible headings, because they were still holding
-- the dark palette's near-white.
--
-- So: `ui.label{ color = "text_dim" }`, and the widget asks the palette
-- when it draws. A number still works and still means exactly that colour,
-- which is what an application wants when it is drawing something that is
-- not part of the theme at all.
--------------------------------------------------------------------------
local function shade(c)
  if type(c) == "string" then return theme[c] end
  return c
end

gc.__index = gc

local function new_gc()
  return setmetatable({ ops = {}, ox = 0, oy = 0,
                        cx = 0, cy = 0, cw = 1 << 30, ch = 1 << 30 }, gc)
end

-- Enters a child's coordinate system, returning what is needed to leave it.
function gc:push(x, y, w, h)
  local saved = { self.ox, self.oy, self.cx, self.cy, self.cw, self.ch }

  self.ox = self.ox + x
  self.oy = self.oy + y

  -- The new clip is the intersection with the old one, in absolute
  -- coordinates. A child of a clipped parent is clipped by both, which is
  -- the whole point of a tree.
  local nx, ny = self.ox, self.oy
  local x0 = (nx > self.cx) and nx or self.cx
  local y0 = (ny > self.cy) and ny or self.cy
  local x1 = math.min(nx + w, self.cx + self.cw)
  local y1 = math.min(ny + h, self.cy + self.ch)

  self.cx, self.cy = x0, y0
  self.cw = (x1 > x0) and (x1 - x0) or 0
  self.ch = (y1 > y0) and (y1 - y0) or 0

  return saved
end

function gc:pop(saved)
  self.ox, self.oy, self.cx, self.cy, self.cw, self.ch =
    saved[1], saved[2], saved[3], saved[4], saved[5], saved[6]
end

function gc:fill(x, y, w, h, color)
  color = shade(color)
  local ax, ay = self.ox + x, self.oy + y

  -- Clipped here so the command that leaves is already inside the view. A
  -- rectangle entirely outside produces no command at all, which is the
  -- cheapest possible way to draw something invisible.
  local x0 = (ax > self.cx) and ax or self.cx
  local y0 = (ay > self.cy) and ay or self.cy
  local x1 = math.min(ax + w, self.cx + self.cw)
  local y1 = math.min(ay + h, self.cy + self.ch)

  if x1 <= x0 or y1 <= y0 then return end

  self.ops[#self.ops + 1] = { op = "fill", x = x0, y = y0,
                              w = x1 - x0, h = y1 - y0, color = color }
end

--
-- A rounded rectangle, filled or outlined.
--
-- **Not clipped like `fill` is**, and that is the one difference worth
-- knowing. A rounded shape's corners belong to the whole control, so a
-- rectangle cut down to the visible part would round the *cut* - a button
-- half off the edge of a list would grow a corner in the middle of it. The
-- primitive clips itself against the surface, so what is lost is the view's
-- own clipping, and every caller here draws a control that is inside its
-- view already.
--
local function round_op(self, kind, x, y, w, h, color, r)
  if w <= 0 or h <= 0 then return end

  self.ops[#self.ops + 1] = { op = kind, x = self.ox + x, y = self.oy + y,
                              w = w, h = h, color = shade(color),
                              r = r or 0 }
end

function gc:fill_round(x, y, w, h, color, r)
  round_op(self, "fill_round", x, y, w, h, color, r)
end

function gc:frame_round(x, y, w, h, color, r)
  round_op(self, "frame_round", x, y, w, h, color, r)
end

--
-- **The longest prefix of `s` that fits in `budget` pixels**, as a byte
-- count and the width it takes, or nil when the string is not valid UTF-8
-- and the caller has to fall back.
--
-- Measured rather than counted, and that is the whole point of it: a
-- proportional face has no cell, so there is no number of characters that
-- means a width. Binary search, so a label costs about four `gfx.measure`
-- calls and a line of a terminal about ten - and the case that matters,
-- text that fits, costs one and never comes here at all.
--
local function fits(s, face, budget)
  local n = utf8.len(s)

  if not n then return nil end

  local lo, hi = 0, n

  while lo < hi do
    local mid = (lo + hi + 1) // 2
    local stop = utf8.offset(s, mid + 1)
    local head = stop and s:sub(1, stop - 1) or s

    if gfx.measure(head, face) <= budget then lo = mid else hi = mid - 1 end
  end

  local stop = utf8.offset(s, lo + 1)
  local bytes = stop and (stop - 1) or #s

  return bytes, gfx.measure(s:sub(1, bytes), face)
end

--
-- `role` picks the face: "ui" when it is not given, "mono" for a terminal,
-- "text" for a paragraph.
--
-- It had no such argument, so every string an application drew went out as
-- an op with no role on it and the compositor drew all of them in the
-- widget font. Choosing a monospace font in the settings changed the
-- setting, changed nothing on screen, and reported success - the terminal
-- kept drawing in whatever the widgets were using, which for a
-- proportional face means the columns it is made of stop lining up.
--
-- `px` asks for that role's font at a size of its own, which is what a title
-- larger than the three roles needs. **A size crosses, never a face number**:
-- `gfx`'s `role_of` does take a number, so an index would resolve in the
-- compositor's process - where that slot was never loaded - and the text
-- would quietly come out in the 8x16 bitmap. Each side resolves the size
-- against its own pool instead, and measuring happens in the face that draws.
function gc:text(x, y, s, color, bg, role, px)
  color, bg = shade(color), shade(bg)
  local ax, ay = self.ox + x, self.oy + y

  local face = px and ui.sized(role, px) or role

  -- **Asked, not remembered.** `GH` above is `gfx.font.h` as it was when
  -- this file loaded, which is before a process has been told what the
  -- desktop's faces are - the same staleness that had the Deskbar
  -- measuring its own width in the wrong font (`testing.md` 18.122). One
  -- call, and it answers for the face this text is actually drawn in.
  local GH = gfx.height(face)

  if ay + GH <= self.cy or ay >= self.cy + self.ch then return end

  --
  -- **Clipped by measuring, which it did by counting cells.**
  --
  -- Still by the character rather than the pixel - a half glyph is worse
  -- than a missing one, and the alternative is a clip rectangle in the
  -- blitter, which is `gfx`'s business and not this file's. What changed is
  -- how many characters that is: `room` was `width // GW`, one cell per
  -- character, and a cell is something only a bitmap font has.
  --
  -- It was exact for as long as every glyph was eight pixels wide, and the
  -- day the desktop's face became IBM Plex it started cutting the last
  -- character off anything whose *count* passed the box while its *width*
  -- sat well inside it: `New folder` in a 96-pixel button became `New
  -- folde` with twenty-five pixels to spare, `Delete` became `Delet`.
  --
  -- `fits` measures instead, and the text that fits - nearly all of it -
  -- pays one `gfx.measure` and no slicing at all.
  --
  local room = self.cx + self.cw - ax

  if room <= 0 then return end

  local shown = s

  --
  -- The left edge, when a view has been scrolled: drop whole characters
  -- until what is left starts at or after the clip, and move `ax` by what
  -- was dropped rather than by a multiple of anything.
  --
  if ax < self.cx then
    local want = self.cx - ax
    local bytes, wide = fits(s, face, want)

    if bytes == nil then
      -- Not valid UTF-8, which a browser will hand over on purpose. The
      -- old cell arithmetic is wrong in the same old way rather than in a
      -- new one.
      local cell = math.max(1, gfx.measure("0", face))
      local skip = (want + cell - 1) // cell

      ax = ax + skip * cell
      shown = s:sub(skip + 1)
      room = self.cx + self.cw - ax
    else
      -- One character more than fits in `want`, so the first one shown
      -- begins at or past the clip rather than under it.
      local stop = utf8.offset(s, utf8.len(s:sub(1, bytes)) + 2)

      if stop == nil then return end

      ax = ax + gfx.measure(s:sub(1, stop - 1), face)
      shown = s:sub(stop)
      room = self.cx + self.cw - ax
    end

    if room <= 0 or shown == "" then return end
  end

  -- And the right edge, which is one measurement when the whole string
  -- fits and a search when it does not.
  if gfx.measure(shown, face) > room then
    local bytes = fits(shown, face, room)

    shown = bytes and shown:sub(1, bytes)
            or shown:sub(1, math.max(0, room //
                                        math.max(1, gfx.measure("0", face))))
  end

  if shown == "" then return end

  self.ops[#self.ops + 1] = { op = "text", x = ax, y = ay,
                              s = shown, color = color, bg = bg,
                              role = role, px = px }
end

--
-- A picture from `assets/`, composited.
--
-- Named, not carried: the op says which asset and the compositor loads it,
-- because a decoded 32x32 icon is four kilobytes and a message is two. That
-- is the same division as everywhere else here - this process decides what
-- is drawn, the process that owns the pixels draws it.
--
-- The clipping is the reason this is a verb rather than a line each caller
-- writes. `gc:fill` clips because a rectangle that runs out of its view
-- must not reach the compositor, and an image has the same problem with an
-- extra half: the *source* origin has to move with the destination, or an
-- icon scrolled half off the top of a list would draw its top half at the
-- top of the list rather than its bottom half.
--
--
-- **At a size, since 22 September.** `size` was how much of a 32-pixel
-- icon to show, so anything else was a crop - Music's speaker at 14 would
-- have been its top-left corner - and a 32-pixel Deskbar could only have
-- held its icons edge to edge (`roadmap.md` 5v). Haiku's icons come at 16,
-- 32 and 64 (`assets/icons/README.md`), so those three are drawn as
-- exported, pixel for pixel, and any other size is the 64 averaged down to
-- it (`stretch`'s `smooth`), which keeps an outline a nearest-neighbour
-- shrink would drop rows of.
--
-- **A shrunk icon is drawn whole or not at all**: `stretch` clips only
-- where it lands on the window, not to a view inside it, so one cut by its
-- view's edge would draw past it. Nothing that shrinks an icon scrolls.
--
local ICON_EXPORTS = { [16] = "16x16/", [32] = "", [64] = "64x64/" }

function gc:icon(x, y, name, size)
  size = size or 32

  local ax, ay = self.ox + x, self.oy + y
  local export = ICON_EXPORTS[size]

  if not export then
    if ax < self.cx or ay < self.cy or ax + size > self.cx + self.cw
       or ay + size > self.cy + self.ch or size <= 0 then
      return
    end

    self.ops[#self.ops + 1] = {
      op = "image", asset = "64x64/" .. name, alpha = true, smooth = true,
      sx = 0, sy = 0, w = 64, h = 64,
      x = ax, y = ay, dw = size, dh = size,
    }

    return
  end

  local x0 = (ax > self.cx) and ax or self.cx
  local y0 = (ay > self.cy) and ay or self.cy
  local x1 = math.min(ax + size, self.cx + self.cw)
  local y1 = math.min(ay + size, self.cy + self.ch)

  if x1 <= x0 or y1 <= y0 then return end

  self.ops[#self.ops + 1] = {
    op = "image", asset = export .. name, alpha = true,
    sx = x0 - ax, sy = y0 - ay,
    w = x1 - x0, h = y1 - y0,
    x = x0, y = y0,
  }
end

--
-- **A line icon, in a colour** (`roadmap.md` 5zp, `tools/lineicons.py`).
--
-- The mockups' small grey icons - a category in Preferences' sidebar, the
-- search and the menu in a header - carried as their coverage alone and
-- painted in whatever colour the caller names, so the same picture is the
-- grey of a row and the accent of the chosen one in every look.
--
-- Whole or not at all, like a shrunk `icon`: a 15-pixel glyph half off the
-- edge of a list is a smudge, and nothing that draws these scrolls.
--
function gc:line_icon(x, y, name, color, size)
  size = size or 15

  local ax, ay = self.ox + x, self.oy + y

  if ax < self.cx or ay < self.cy or ax + size > self.cx + self.cw
     or ay + size > self.cy + self.ch then
    return
  end

  self.ops[#self.ops + 1] = {
    op = "tint", asset = ("line/%s-%d.png"):format(name, size),
    x = ax, y = ay, w = size, h = size, color = shade(color),
  }
end

-- A one-pixel frame, which is what this kit uses instead of a bevel.
--
-- A filled triangle, in the view's own coordinates.
--
-- The three points are given rather than a box and a direction, because the
-- shapes that want this are a play arrow, a disclosure arrow and a slider's
-- notch, and each wants its own proportions. `gfx` has taken doubles for
-- these since it was written, so a half pixel is expressible and the edges
-- come out where they were asked for.
--
-- Clipped by the view's rectangle like everything else: what falls outside
-- is not drawn, and the compositor clips again against the window.
--
function gc:triangle(x1, y1, x2, y2, x3, y3, color)
  local ax, ay = self.ox, self.oy
  local left = math.min(x1, x2, x3) + ax
  local right = math.max(x1, x2, x3) + ax
  local top = math.min(y1, y2, y3) + ay
  local bottom = math.max(y1, y2, y3) + ay

  if right <= self.cx or left >= self.cx + self.cw
     or bottom <= self.cy or top >= self.cy + self.ch then
    return
  end

  self.ops[#self.ops + 1] = {
    op = "triangle", color = shade(color),
    x1 = x1 + ax, y1 = y1 + ay,
    x2 = x2 + ax, y2 = y2 + ay,
    x3 = x3 + ax, y3 = y3 + ay,
  }
end

--
-- **A named picture, drawn at a size.**
--
-- `ui.image` is a widget: it holds a picture, pans it, and says so when there
-- is none. That is right for Photo and wrong inside a window that draws its
-- own layout - Music paints a cover into a square it has already decided the
-- size of, and when there is no cover it wants to paint something else there
-- rather than a placeholder saying so.
--
-- The whole picture, scaled to the box, through the same command `ui.image{
-- fit = true }` sends (`testing.md` 18.82).
--
function gc:picture(x, y, w, h, name)
  if not name or w <= 0 or h <= 0 then return end

  local ax, ay = self.ox + x, self.oy + y

  if ax + w <= self.cx or ax >= self.cx + self.cw
     or ay + h <= self.cy or ay >= self.cy + self.ch then
    return
  end

  local size = fs.send("/app/wm", { type = "image_size", asset = name })

  if not size or (size.w or 0) <= 0 then return end

  self.ops[#self.ops + 1] = {
    op = "image", asset = name,
    sx = 0, sy = 0, w = size.w, h = size.h,
    x = ax, y = ay, dw = w, dh = h,
  }
end

function gc:frame(x, y, w, h, color)
  color = shade(color)
  self:fill(x, y, w, 1, color)
  self:fill(x, y + h - 1, w, 1, color)
  self:fill(x, y, 1, h, color)
  self:fill(x + w - 1, y, 1, h, color)
end

--------------------------------------------------------------------------
-- Edges, which is what `ui.md` 16.8b decided the look is made of.
--
-- Light on the top and left, dark on the bottom and right, and a thing is
-- raised. Swap them and it is sunken. That is the whole trick, and it is
-- worth being exact about why it is worth two commands: it is not shading,
-- it is a sentence. Raised means you can press this. Sunken means content
-- lives in here. Read from the corner of the eye, without looking straight
-- at it and without a label.
--
-- **Built from `gc:fill` rather than from the op list**, so they inherit
-- the clip and the origin for free and cannot drift from the one place that
-- knows how clipping works. Four fills, no new op, nothing new in the
-- window manager.
--
-- **Colour names, not numbers.** `shade` resolves a name at the moment it
-- draws, which is what lets a theme change repaint every widget without any
-- of them subscribing to anything - `ui.md` 16.9. Passing `theme.edge_light`
-- here would freeze this palette's grey into the widget, which is exactly
-- the bug that left a light theme with near-invisible headings.
--
-- One pixel, not two. A 1998 chamfer was two, and at these sizes the second
-- pixel reads as a blur rather than as depth: the tab is twenty pixels tall
-- and a button is smaller than that.
--------------------------------------------------------------------------

-- The two edges, given which way round they go.
local function bevel(g, x, y, w, h, top_left, bottom_right)
  if w < 2 or h < 2 then return end

  g:fill(x, y, w - 1, 1, top_left)          -- top
  g:fill(x, y, 1, h - 1, top_left)          -- left
  g:fill(x, y + h - 1, w, 1, bottom_right)  -- bottom
  g:fill(x + w - 1, y, 1, h, bottom_right)  -- right
end

-- Something you can press.
--
-- Two rings, not one, when there is room for two.
--
-- A single-pixel bevel reads as an outline at any distance; two read as a
-- moulded edge. That is the whole difference between a control that looks
-- drawn on and one that looks like it sticks out, and it is why every
-- interface of the period this system is arguing with - Motif, Windows 95,
-- Platinum - drew two. The outer ring is the hard one and the inner is the
-- soft one, so the light side goes white then face and the dark side goes
-- line then edge_dark: a bright corner, a shoulder, and then the face.
--
-- **Only when there is room.** A 14-pixel title-bar box or a checkbox with
-- two rings on each side has four pixels of bevel and six of anything else,
-- which does not read as moulded, it reads as a box with a hole in it. The
-- same systems drew thin bevels on small controls for the same reason.
--
-- Four more fills on a control that qualifies. `ui.md` 16.8b measured the
-- first ring at about four operations a window and called it free; this is
-- the second helping of the same, and a window has tens of controls rather
-- than hundreds.
--
local DOUBLE_MIN = 16

--
-- How round a control is in a flat look.
--
-- Seven points, the drawings' `.btn`, `.drop` and `.field` since 24
-- September - it was six, and a control's corner a pixel tighter than the
-- dropdown beside it is the kind of difference that reads as two kits.
-- Small enough that a 16-pixel checkbox does not become a circle, and the
-- primitive clamps it to half the shorter side, so a control too small to
-- take it comes out square rather than wrong.
--
local CONTROL_R = 7

--
-- **A flat look draws a hairline where a dimensional one draws a bevel.**
--
-- Diego, 23 September 2026: "the endeavor theme uses flat shading and our
-- theme uses bevels in the deskbar and else, lets use flat shading like the
-- mockups". `theme.flat` is a look's own property (`theme.lua`), so the
-- four dimensional looks are untouched and Endeavour is one line.
--
-- One rule for both: raised and sunken are the same rectangle in a flat
-- look, because the whole idea of the two is *light*, and a surface with no
-- light has nothing to say about which way it faces. What separates a
-- control from its ground is then the line alone - which is why the look
-- that wants this has `line` and `line_soft` a shade apart rather than the
-- deep grey a bevelled one needs.
--
function gc:raised(x, y, w, h, face)
  if face then self:fill(x, y, w, h, face) end

  if theme.flat then
    -- Filled again, rounded this time: the square fill above painted the
    -- corners, and a frame drawn round them would leave the colour outside
    -- its own outline.
    if face then self:fill_round(x, y, w, h, face, CONTROL_R) end

    self:frame_round(x, y, w, h, theme.line_soft, CONTROL_R)
    return
  end

  if w >= DOUBLE_MIN and h >= DOUBLE_MIN then
    bevel(self, x, y, w, h, "edge_light", "line")
    bevel(self, x + 1, y + 1, w - 2, h - 2, face or "raised", "edge_dark")
  else
    bevel(self, x, y, w, h, "edge_light", "edge_dark")
  end
end

-- Something you can put things in.
function gc:sunken(x, y, w, h, face)
  if face then self:fill(x, y, w, h, face) end

  if theme.flat then
    if face then self:fill_round(x, y, w, h, face, CONTROL_R) end

    self:frame_round(x, y, w, h, theme.line_soft, CONTROL_R)
    return
  end

  if w >= DOUBLE_MIN and h >= DOUBLE_MIN then
    bevel(self, x, y, w, h, "edge_dark", "edge_light")
    bevel(self, x + 1, y + 1, w - 2, h - 2, "line", face or "window")
  else
    bevel(self, x, y, w, h, "edge_dark", "edge_light")
  end
end

--
-- A separator: two lines that read as a scored groove rather than as a
-- drawn line. Horizontal when it is wider than it is tall.
--
function gc:groove(x, y, w, h)
  if (w or 1) >= (h or 1) then
    self:fill(x, y, w, 1, "edge_dark")
    self:fill(x, y + 1, w, 1, "edge_light")
  else
    self:fill(x, y, 1, h, "edge_dark")
    self:fill(x + 1, y, 1, h, "edge_light")
  end
end

--------------------------------------------------------------------------
-- Scrollbars.
--
-- Drawn *into* a widget rather than added beside it as a child view, and
-- that is a decision rather than a shortcut. `view:hit` returns the deepest
-- child and a press grabs it until release, so a scrollbar that were a
-- child of a list would take the press and the list would never learn the
-- drag happened - and the two would then need to talk to each other about
-- a scroll position they both already have.
--
-- So a well that scrolls draws its own bar and answers its own clicks in
-- the coordinates it already has. One widget, one scroll position.
--
-- Sunken trough, raised thumb: `ui.md` 16.8b, and here it is doing real
-- work rather than decoration. Which part of a scrollbar you can drag is
-- exactly the question the two edges answer.
--------------------------------------------------------------------------

--
-- **Wide enough to hit.** Fourteen pixels was a bar you aimed at rather
-- than reached for, and a scrollbar is the one control in a window that is
-- always used with the pointer and never with the keyboard - so the size
-- of the target *is* the design.
--
-- Sixteen is Mac OS 8 and 9's, which is where the arrows at each end come
-- from and where the sunken-trough-and-raised-thumb vocabulary comes from
-- too. Taking the width from somewhere else while taking everything else
-- from there is how a control ends up looking almost right.
--
-- It costs sixteen columns of every well that scrolls, which is the trade:
-- two columns of content for a bar somebody can use without aiming.
--
local SCROLL_W = 16

--
-- An arrow button at each end, which is Mac OS 8 and 9 and is not decoration.
--
-- A trough alone can only page. Scrolling by *one* row - which is what you
-- want most of the time, and the only thing you want when the list is nearly
-- as tall as its view - had no gesture at all: a page in a five-row list is
-- five rows, so a click either did nothing or went past what you were
-- looking at.
--
-- They cost the track two squares' worth of height, so a bar too short to
-- have both and still leave a usable track has neither. A scrollbar that is
-- all buttons is not a scrollbar.
--
local ARROW = SCROLL_W

--
-- **No arrows in a flat look**, which draws the thumb alone - the drawings'
-- scrollbar - and pages with the trough it does not draw. Asked here, so
-- the drawing, the hit test and the drag all lose them together.
--
local function has_arrows(h)
  return not theme.flat and h >= 4 + 2 * ARROW + 24
end

--
-- Where everything is, or nil when it all fits and there is no bar.
--
-- One function for the drawing, the hit test and the drag, because they have
-- to agree and the way they stop agreeing is you click one thing and another
-- one moves - the same reason `boxes_x` exists in the window manager.
--
-- `top` is one-based, like the rest of the list code.
--
local function thumb_of(h, total, shown, top)
  if total <= shown then return nil end

  local arrows = has_arrows(h) and ARROW or 0
  local y0 = 2 + arrows
  local track = h - 4 - 2 * arrows
  local size = math.max(16, (track * shown) // total)
  local room = track - size
  local at = ((top - 1) * room) // math.max(1, total - shown)

  return y0 + at, size, room, y0
end

--
-- A triangle, four rows tall, centred in the button.
--
-- Drawn rather than vendored: it is eleven pixels and it has to be the
-- theme's ink, which a picture could not be.
--
local function triangle(g, x, y, up)
  for k = 0, 3 do
    local w = 1 + k * 2

    g:fill(x + (ARROW - w) // 2, up and (y + 5 + k) or (y + 8 - k),
           w, 1, "text")
  end
end

--
-- **The thumb is the scrollbar's own grey, with a grip across it.**
--
-- It wore the tab's colour from 22 September - Diego: "i want the
-- scrollbar handle to be colored after the tab bar color as an accent
-- color like how macos 9 had it" - and he took that back on 24 September,
-- looking at a pale blue thumb in a grey trough beside a list's blue
-- selection: "the scroll bars look bad now with the colors", "We should go
-- back to scrollbars and handle with the same color". Three colours in a
-- strip sixteen pixels wide, one of them the title bar's, is chrome
-- reaching into a window's contents. So the thumb is raised in the
-- controls' face and the trough sunken in the window's, as they were.
--
-- Four ridges, each a lit line over a shaded one - raised, in the same
-- vocabulary as the bevel around them - eight pixels wide and centred, in
-- the look's own edge colours, so a new look repaints them like everything
-- else.
--
local GRIP_W, RIDGES = 8, 4

local function draw_scrollbar(g, w, h, total, shown, top)
  local y, size = thumb_of(h, total, shown, top)

  if not y then return false end

  local x = w - SCROLL_W - 2

  --
  -- **In a flat look, a pill and nothing else**: 6 across, 5 in from the
  -- edge, round-ended, in a grey between the list's ground and its dim
  -- words - `docs/apps.html`'s list. The column it sits in is the same 16,
  -- so the trough still pages and the hit test is unchanged.
  --
  if theme.flat then
    g:fill_round(w - 5 - 6, y, 6, size,
                 theme.mix(theme.sunken, theme.text_dim, 450), 3)
    return true
  end

  g:sunken(x, 2, SCROLL_W, h - 4, "window")
  g:raised(x + 1, y, SCROLL_W - 2, size, "raised")

  if size >= 2 * RIDGES + 6 then
    local gx = x + 1 + (SCROLL_W - 2 - GRIP_W) // 2
    local gy = y + (size - 2 * RIDGES) // 2
    local lit, dark = theme.edge_light, theme.edge_dark

    for i = 0, RIDGES - 1 do
      g:fill(gx, gy + 2 * i, GRIP_W, 1, lit)
      g:fill(gx, gy + 2 * i + 1, GRIP_W, 1, dark)
    end
  end

  if has_arrows(h) then
    g:raised(x + 1, 3, SCROLL_W - 2, ARROW - 2, "raised")
    triangle(g, x + 1, 3, true)

    g:raised(x + 1, h - 2 - ARROW, SCROLL_W - 2, ARROW - 2, "raised")
    triangle(g, x + 1, h - 2 - ARROW, false)
  end

  return true
end

ui.SCROLL_W = SCROLL_W

--
-- The whole interaction, in one place.
--
-- This used to be a hit test that answered a press, and each of the four
-- lists in this system carried its own twenty-five lines of drag on top of
-- it. All four had the same bug, because all four were the same code: the
-- drag ran inside `if x >= self.w - SCROLL_W - 2`, so it only continued
-- while the pointer stayed within the fourteen pixels of the bar. Slip two
-- pixels left - which you do, because you are looking at the list and not
-- at the bar - and the drag stopped without saying so, and the press fell
-- through to the rows underneath and changed the selection.
--
-- A drag that only works if you drag in a straight line is a drag that does
-- not work. So the widget hands its own state over and this decides
-- everything, including whether the event was the bar's at all: **while a
-- drag is live it always is**, wherever the pointer has got to.
--
-- Returns the new `top`, or nil when the event belongs to whatever is under
-- the bar rather than to the bar.
--
function ui.scrollbar_mouse(w_, action, x, y, w, h, total, shown, top)
  local drag = w_.bar_drag

  if action == "release" then
    w_.bar_drag = nil

    return drag and top or nil
  end

  if drag and action == "move" then
    local _, _, room = thumb_of(h, total, shown, drag.top)
    local span = total - shown

    if not room or room <= 0 then return top end

    --
    -- Rounded, and rounded symmetrically.
    --
    -- `//` floors, and floor is not symmetric about zero: dragging up by one
    -- pixel gave `(-1 * span) // room` = -1 and dragging down by one gave 0.
    -- So the list jumped the instant you moved up and resisted moving down,
    -- which is what "jerky" was.
    --
    local dy = y - drag.y
    local moved

    if dy >= 0 then
      moved = (dy * span + room // 2) // room
    else
      moved = -((-dy * span + room // 2) // room)
    end

    return math.min(math.max(1, drag.top + moved), math.max(1, span + 1))
  end

  if action ~= "press" then return nil end

  -- Not a press on the bar, and no drag to continue: not ours.
  if total <= shown or x < w - SCROLL_W - 2 then return nil end

  -- The buttons, one row each. A held button does not repeat: nothing here
  -- has a clock, and a widget that wanted one would need the event loop to
  -- wake it rather than a timer of its own.
  if has_arrows(h) then
    if y < 2 + ARROW then
      return math.max(1, top - 1)
    elseif y >= h - 2 - ARROW then
      return math.min(math.max(1, total - shown + 1), top + 1)
    end
  end

  local ty, size = thumb_of(h, total, shown, top)

  if not ty then return nil end

  -- Above the thumb is a page back and below it a page forward, which is
  -- what a trough has always meant.
  if y < ty then
    return math.max(1, top - shown)
  elseif y >= ty + size then
    return math.min(total - shown + 1, top + shown)
  end

  -- On the thumb. Remember where it was grabbed, so the list moves by how
  -- far the pointer moved rather than jumping to it.
  w_.bar_drag = { y = y, top = top }

  return top
end

ui.scrollbar = draw_scrollbar

--------------------------------------------------------------------------
-- A processor meter, drawn the way BeOS drew one.
--
-- Discrete segments on a dark ground rather than a smooth fill - Pulse's
-- look, and the reason to take it is not nostalgia. **A segmented bar has
-- a resolution you can see.** A smooth fill at 61% and one at 66% are two
-- pictures nobody can tell apart; twenty-two lit segments against
-- twenty-four is a thing the eye reads without measuring, and a meter that
-- moves in steps makes a *change* visible where a continuous one only makes
-- a level visible. That is the difference between a display you glance at
-- and one you have to study, which is the whole argument for having it on
-- screen at all.
--
-- Green and red, not green alone. The reference is all green because BeOS
-- had nothing to say with a second colour; this system does - `sysmon`
-- turning red above eighty per cent is what `check_idle` measures, and what
-- proves the claim that nothing in this desktop polls. Keeping the colour
-- rule and taking the segmentation is taking the part of the reference that
-- was about *how a meter reads*, and not the part that was about what BeOS
-- happened to know.
--
-- The dark green ground is constant across both palettes on purpose. An
-- unlit LED is a colour, not an absence, and the light palette is BeOS
-- panel grey - which is exactly the surface Pulse's dark green panel sat
-- on.
--
-- Derived from `theme.good` rather than named, so a theme that moves its
-- green takes the ground with it. Two numbers instead of two more palette
-- entries, for the same reason `theme.chrome` derives its gradient.
--
local function leds(g, x, y, w, h, frac, colour)
  if frac < 0 then frac = 0 end
  if frac > 1 then frac = 1 end

  --
  -- **Far darker than they look like they should be**, and the first
  -- attempt got this wrong in a way only a screenshot showed. Ninety off
  -- the green channel leaves 85 against the lit 185, which is under two to
  -- one - and at the size a meter actually is, an unlit bar read as a lit
  -- one. A meter that says a hundred per cent when the machine is idle is
  -- worse than no meter, so the ratio has to be obvious rather than
  -- measurable: 30 against 185 is six to one, and the segments now read as
  -- off.
  --
  local ground = theme.lift(theme.good, -170)
  local unlit  = theme.lift(theme.good, -155)

  g:fill(x, y, w, h, ground)

  --
  -- Six lit and two dark, which is what makes them read as segments rather
  -- than as a bar with a texture. The count falls out of the width instead
  -- of being chosen, so the same meter is right in a 300-pixel window and
  -- in a 120-pixel one.
  --
  local pitch = 8
  local seg   = 6
  local n     = (w + (pitch - seg)) // pitch

  if n < 1 then n = 1 end

  --
  -- Round *up*, so that any work at all lights a segment.
  --
  -- One per cent of twenty-four segments is a quarter of one, and rounding
  -- down draws an empty bar on a machine that is doing something - which is
  -- the one reading a meter must never give. Full is still full: at
  -- `frac == 1` this is exactly `n`.
  --
  local lit = math.ceil(n * frac)

  for i = 0, n - 1 do
    local sx = x + i * pitch

    if sx + seg > x + w then
      break                       -- a part-segment at the end is not one
    end

    g:fill(sx, y, seg, h, (i < lit) and colour or unlit)
  end
end

ui.leds = leds

--------------------------------------------------------------------------
-- Keys, decoded.
--
-- The window manager forwards *bytes*, so an arrow arrives as the three of
-- an ANSI escape sequence - 27, 91, then 65 to 68 - and a widget that wants
-- to know "up" has to reassemble them. `win:run()` has always done that
-- inline, which was fine while it was the only reader.
--
-- It is not any more. An application with a direct window drives its own
-- loop and reads the poll reply itself: cube3d, plasma, Doom. Doom is what
-- found this - every arrow key arrived as an Escape followed by two
-- characters, so the menus jumped out instead of moving.
--
-- One decoder, then, and both loops use it. Negative codes so they can
-- never collide with a character: -1 up, -2 down, -3 right, -4 left, which
-- is A B C D and therefore the order the terminal sends rather than the
-- order anybody would choose.
--------------------------------------------------------------------------

ui.UP, ui.DOWN, ui.RIGHT, ui.LEFT = -1, -2, -3, -4

local ARROWS = { [65] = -1, [66] = -2, [67] = -3, [68] = -4 }

--
-- Returns a function: give it one byte, get back nought, one or two codes.
--
-- Two, and that is the part worth being careful about. A byte can resolve
-- more than one key, because 27 is ambiguous until the byte after it: if
-- that byte is not 91 then the 27 was a real Escape *and* the byte is a key
-- of its own, and both have to come out. Returning one and remembering the
-- other for next time sounds equivalent and is not - the next call arrives
-- with its own byte, and the remembered one has to displace it. That was
-- the first attempt, and it ate two bytes out of every three: three presses
-- of Down moved a Doom menu once.
--
-- Lua returns two values as easily as one, so it returns two.
--
-- Stateful, because three bytes make one arrow, so each reader needs its
-- own decoder.
--
function ui.key_decoder()
  local escape = 0

  return function(c)
    if escape == 1 then
      if c == 91 then
        escape = 2

        return nil
      end

      --
      -- An Escape that was not the start of a sequence.
      --
      -- Both bytes used to be dropped here, so a real Escape did nothing
      -- and took the next key with it. Invisible in a widget kit, where
      -- little is bound to Escape; very visible in a game, where it is the
      -- menu.
      --
      escape = 0

      -- The byte after it may itself be an Escape, and then the guessing
      -- starts again on that one.
      if c == 27 then
        escape = 1

        return 27
      end

      return 27, c
    end

    if escape == 2 then
      escape = 0

      return ARROWS[c]
    end

    if c == 27 then
      escape = 1

      return nil
    end

    return c
  end
end

--------------------------------------------------------------------------
-- Trees.
--
-- A hierarchy you can open and close, which is the other half of what a file
-- manager is made of. `ui.md` 16.4's follow modes place it; everything below
-- is about what it draws and how it is walked.
--
-- **Children are asked for, not held.** A node carries a `children` function
-- that is called the first time it is opened and never again, so a tree of a
-- filesystem does not read the filesystem to be shown - it reads one
-- directory when you open one. A tree that loaded eagerly would walk every
-- disk on the machine to draw a pane four rows tall.
--
-- **One flat view, hit-tested by arithmetic**, for the same reason menus
-- are: `view:hit` returns the deepest child and a press grabs it, so a row
-- that were a view of its own would eat the drag. The visible rows are
-- flattened on each draw, which is a walk over what is open rather than
-- over what exists.
--------------------------------------------------------------------------

local TREE_INDENT = 14
local TREE_ARROW = 10

-- Every open node, in the order they are drawn.
local function tree_rows(nodes, depth, out)
  for _, n in ipairs(nodes) do
    out[#out + 1] = { node = n, depth = depth }

    --
    -- **A node that starts open fetches its children here.**
    --
    -- This read `n.open and n.kids`, and `children` was called only from the
    -- mouse handler - so a root created with `open = true` and a `children`
    -- callback drew its triangle, claimed to be open, and never had anything
    -- under it. Tracker's Drives group was exactly that: the callback was
    -- never called, so not even its "nothing plugged in" row appeared.
    --
    if n.open and not n.kids and n.children then
      n.kids = n.children(n) or {}
    end

    if n.open and n.kids then
      tree_rows(n.kids, depth + 1, out)
    end
  end

  return out
end

function ui.tree(spec)
  local v = ui.view(spec)

  v.w = v.w > 0 and v.w or 180
  v.h = v.h > 0 and v.h or (ROW * 8)
  v.focusable = true
  v.roots = v.roots or {}
  v.top = 1
  v.chosen = nil

  --
  -- **Which row a height in the tree is**, and the node on it - or nil.
  --
  -- One formula for everything that asks: the tree's own clicks, and a
  -- caller's drop or right-click (Tracker's Places). It lived inline in
  -- `mouse` while that was the only asker; a copy in an application would be
  -- the same arithmetic against a row height it does not own, and would
  -- drift the first time the face changed.
  --
  function v:node_at(y)
    local r = self.rows and self.rows[self.top + (y - 2) // ROW]

    return r and r.node, r
  end

  function v:draw(g)
    g:sunken(0, 0, self.w, self.h, "sunken")

    if self.focused and self.keyed then
      g:frame(1, 1, self.w - 2, self.h - 2, "ring")
    end

    local rows = tree_rows(self.roots, 0, {})
    local shown = (self.h - 4) // ROW

    self.rows = rows
    self.shown = shown

    if self.top > #rows - shown + 1 then self.top = #rows - shown + 1 end
    if self.top < 1 then self.top = 1 end

    self.bar = draw_scrollbar(g, self.w, self.h, #rows, shown, self.top)

    local room = self.w - 4 - (self.bar and SCROLL_W + 2 or 0)

    for i = 0, shown - 1 do
      local r = rows[self.top + i]

      if not r then break end

      local y = 2 + i * ROW
      local x = 4 + r.depth * TREE_INDENT

      --
      -- **A heading is a label, not a place.** `drives.html` wants Places,
      -- System and Drives as groups over the rows rather than as rows you
      -- can stand in, so a heading never highlights and never reports a
      -- selection - see `mouse` below, which returns before `chosen` moves.
      --
      --
      -- **Two ways a row can be unselectable, and they look different.** A
      -- `heading` names a group; a `quiet` row is an ordinary row that has
      -- nothing to go to - "no drives plugged in". Making the second a
      -- heading drew it at heading weight and it read as a fourth group.
      --
      local head = r.node.heading and true or false
      local on = (not head) and (not r.node.quiet)
                 and (r.node == self.chosen)
      local bg = on and theme.accent or theme.sunken
      local fg = on and theme.text_on
                     or ((head or r.node.quiet) and theme.text_dim
                                                or theme.text)

      if on then g:fill(2, y, room, ROW, bg) end

      --
      -- The marker, and only on something that can be opened. Built from
      -- fills like the menu's: pointing right when shut and down when open,
      -- which is the one convention every tree in every system shares.
      --
      if r.node.children or r.node.kids then
        local ax, ay = x, y + (ROW - 5) // 2

        for k = 0, 4 do
          if r.node.open then
            -- Down: widest at the top.
            local run = 5 - k

            if run > 0 and k < 3 then
              g:fill(ax + k, ay + k, run, 1, fg)
            end
          else
            local run = 3 - math.abs(k - 2)

            if run > 0 then g:fill(ax + 1, ay + k, run, 1, fg) end
          end
        end
      end

      g:text(x + TREE_ARROW, y + centred(ROW), tostring(r.node.text or "?"),
             fg, bg)

      --
      -- **What a row is, beside what it is called**: `KOSMOS HOME` and then
      -- `kfs`, quieter and to the right (`drives.html`).
      --
      -- Measured with `gfx.measure` rather than `#text * gfx.font.w`, which
      -- is the whole reason that binding exists: the drawing face is
      -- proportional, and a right edge computed from a glyph count lands in
      -- the wrong place - the mistake that cost three wrong guesses on
      -- Music's footer (`testing.md` 18.85).
      --
      if r.node.note and not on then
        local note = tostring(r.node.note)
        local width = gfx.measure(note, "ui")
        local at = room - width - 2

        -- Only when it does not crowd the name. A note that overlapped
        -- would be worse than no note at all.
        if at > x + TREE_ARROW + gfx.measure(tostring(r.node.text or ""), "ui") + 6 then
          g:text(at, y + centred(ROW), note, theme.text_dim, bg)
        end
      end
    end
  end

  --
  -- **How long two presses may be apart and still be one gesture.**
  --
  -- Read from the machine rather than assumed: `sys.ticks` is CNTFRQ_EL0,
  -- 62.5 MHz under QEMU's TCG and 24 MHz when the same machine runs on this
  -- Mac's own cores under `hvf`. A constant here would be one interval in
  -- one case and a different one in the other - which is the mistake
  -- `tracker.lua` names where it reads the frequency for its own timer.
  --
  -- **A second, not half of one, and the difference was measured.** The
  -- counter is the generic timer and QEMU advances it against the host's
  -- clock, while the guest's execution lags behind under TCG - so two
  -- presses 0.12 seconds apart on this Mac arrived 46,187,937 ticks apart,
  -- which at 62.5 MHz is three quarters of a second as the machine counts
  -- it. Against half a second the gesture could not be made at all. A whole
  -- second is what a person manages on an emulated desktop and is still far
  -- below two clicks meant as two.
  --
  local AGAIN = ui_again()

  function v:mouse(action, x, y)
    local to = ui.scrollbar_mouse(self, action, x, y, self.w, self.h,
                                  #(self.rows or {}), self.shown or 1,
                                  self.top)

    if to then
      self.top = to

      return true
    end

    if action ~= "press" then return true end

    local _, r = self:node_at(y)

    if not r then return true end

    --
    -- The marker opens; anything else selects. Two targets in one row, and
    -- the marker is the narrow one, so it is tested first.
    --
    local ax = 4 + r.depth * TREE_INDENT

    if (r.node.children or r.node.kids) and x >= ax and x < ax + TREE_ARROW then
      if r.node.open then
        r.node.open = false
      else
        -- Asked once. A node that has been opened keeps its children, so
        -- closing and opening again does not read the directory twice.
        if not r.node.kids and r.node.children then
          r.node.kids = r.node.children(r.node) or {}
        end

        r.node.open = true
      end

      return true
    end

    -- A heading names a group, and a quiet row has nowhere to go.
    if r.node.heading or r.node.quiet then return true end

    --
    -- **Clicking a row again opens it**, anywhere in the row - Diego, 16
    -- September: "I want double click to open the folders like home and
    -- desktop, not only clicking on the little arrow on the left". The
    -- marker is ten pixels wide and it was the only way in.
    --
    -- **This kit had no notion of a double click, deliberately.**
    -- `tracker.lua` refused to add one: "adding it to serve a single caller
    -- would be a widget change made for an application, which is the wrong
    -- way round". That reasoning was right about its own case and does not
    -- hold here - a tree is not one caller, and every sidebar built on this
    -- widget gets the same fiddly target. So the gesture lives in the kit.
    --
    -- The first press still selects, which is what a single click has always
    -- meant; the second adds opening to it rather than replacing it.
    --
    local now = sys.ticks()

    if r.node == self.last_row and (now - (self.last_press or 0)) < AGAIN
       and (r.node.children or r.node.kids) then
      if r.node.open then
        r.node.open = false
      else
        if not r.node.kids and r.node.children then
          r.node.kids = r.node.children(r.node) or {}
        end

        r.node.open = true
      end
    end

    self.last_row, self.last_press = r.node, now

    self.chosen = r.node

    if self.on_select then self.on_select(self, r.node) end

    return true
  end

  return v
end

--------------------------------------------------------------------------
-- Splitters.
--
-- A grip between two views that changes where the boundary is. It moves the
-- *views*, which is the whole of it: everything else about a split pane is
-- the two things either side, and they are ordinary views that already know
-- how to be a size.
--------------------------------------------------------------------------

function ui.splitter(spec)
  -- Before the view is made, for the reason `ui.header` gives: this default
  -- was set afterwards from the day it was written, and was never read.
  spec.follow = spec.follow or { "left", "top", "bottom" }

  local v = ui.view(spec)

  v.w = v.w > 0 and v.w or 6

  function v:draw(g)
    g:fill(0, 0, self.w, self.h, theme.window)
    g:groove(self.w // 2 - 1, 0, 2, self.h)
  end

  function v:mouse(action, x)
    if action == "press" then
      self.holding = x
    elseif action == "move" and self.holding then
      if self.on_move then self:on_move(x - self.holding) end
    elseif action == "release" then
      self.holding = nil
    end

    return true
  end

  return v
end

--------------------------------------------------------------------------
-- Views.
--
-- Follow modes rather than a constraint solver, `ui.md` 16.4. Each edge
-- either keeps its distance to the parent's matching edge or scales with
-- it. Four booleans and no solver, which covers what layouts actually do
-- and fails honestly at what they do not.
--------------------------------------------------------------------------

local view = {}
view.__index = view

function ui.view(spec)
  local v = setmetatable(spec or {}, view)

  v.x = v.x or 0
  v.y = v.y or 0
  v.w = v.w or 0
  v.h = v.h or 0
  v.children = v.children or {}

  --
  -- Which edges are pinned. Left and top by default, which is the behaviour
  -- of something that simply sits where it was put.
  --
  -- Written either way, because `ui.md` 16.4 has always documented the list
  -- form and this file has always read the set form:
  --
  --   follow = { "left", "right", "top" }     what the documentation says
  --   follow = { left = true, right = true }  what the code read
  --
  -- A list produced a table whose `.left` was nil, so every edge came out
  -- unpinned and the widget silently did not move. Nothing caught it because
  -- no application had ever used `follow` at all - there was nothing to
  -- resize until now, so the whole path was dead code that happened to
  -- compile.
  --
  v.follow = v.follow or { left = true, top = true }

  for i = 1, #v.follow do
    local edge = v.follow[i]

    if type(edge) == "string" then
      v.follow[edge] = true
    end
  end

  return v
end

function view:add(child)
  self.children[#self.children + 1] = child
  child.parent = self

  -- The distances that follow modes preserve, recorded the moment the child
  -- is placed. Recording them later would preserve whatever the last resize
  -- happened to leave.
  child._insets = {
    left   = child.x,
    top    = child.y,
    right  = self.w - (child.x + child.w),
    bottom = self.h - (child.y + child.h),
  }

  return child
end

function view:resize(w, h)
  self.w, self.h = w, h

  for _, c in ipairs(self.children) do
    local f, i = c.follow, c._insets or {}

    if f.left and f.right then
      c.x = i.left or c.x
      c.w = w - (i.left or 0) - (i.right or 0)
    elseif f.right then
      c.x = w - (i.right or 0) - c.w
    else
      c.x = i.left or c.x
    end

    if f.top and f.bottom then
      c.y = i.top or c.y
      c.h = h - (i.top or 0) - (i.bottom or 0)
    elseif f.bottom then
      c.y = h - (i.bottom or 0) - c.h
    else
      c.y = i.top or c.y
    end

    if c.w < 0 then c.w = 0 end
    if c.h < 0 then c.h = 0 end

    c:resize(c.w, c.h)
  end
end

function view:paint(g)
  --
  -- A view can be absent rather than empty.
  --
  -- There was no way to say so, and applications paid for it: Tracker's
  -- rename box sat permanently under the list because a box you never use is
  -- cheaper than a widget change made for one caller. It is not one caller -
  -- any panel that appears when asked for wants this - and it is three lines
  -- in the three places that walk the tree.
  --
  -- Hidden here, in `hit` and in `focusables`, which is the whole of it: not
  -- drawn, not clickable, not reachable by Tab. A widget that was invisible
  -- and still took the focus would be a window where Tab stops at nothing.
  --
  if self.hidden then return end

  -- A view whose size depends on what it is about to draw gets to say so
  -- first, because the clip below is its size and is applied before the
  -- drawing happens.
  if self.measure then self:measure() end

  local saved = g:push(self.x, self.y, self.w, self.h)

  if self.draw then self:draw(g) end

  for _, c in ipairs(self.children) do
    c:paint(g)
  end

  g:pop(saved)
end

-- A view is clickable when it says what to do about it.
--
-- `dispatch_mouse` grabs whatever the hit test lands on *if it has a
-- `mouse`*, so without this a plain view built with an `on_click` was never
-- grabbed and its handler never ran. A grid of colour swatches is exactly
-- that, and clicking one did nothing at all.
--
-- The coordinates are the view's own, so a swatch grid can work out which
-- swatch was hit without knowing where it sits on screen.
function view:mouse(action, x, y)
  if action == "press" and self.on_click then
    self.on_click(self, x, y)
    return true
  end

  return false
end

--
-- The deepest view containing a point, and that point in its coordinates.
--
-- Backwards through the children, because they are drawn in order and a
-- later one is on top: the thing you can see is the thing you hit. A view
-- with no children returns itself, so a panel is hit where its label is not.
--
function view:hit(x, y)
  for i = #self.children, 1, -1 do
    local c = self.children[i]

    if not c.hidden
       and x >= c.x and x < c.x + c.w and y >= c.y and y < c.y + c.h then
      return c:hit(x - c.x, y - c.y)
    end
  end

  return self, x, y
end

-- Depth first, in tree order, which is the order Tab moves in.
function view:focusables(out)
  out = out or {}

  if self.hidden then return out end

  if self.focusable then out[#out + 1] = self end

  for _, c in ipairs(self.children) do
    c:focusables(out)
  end

  return out
end

--------------------------------------------------------------------------
-- Widgets.
--
-- BeOS's vocabulary. Each one is a view with a draw handler and, if it
-- takes input, a `key` handler; there is no class hierarchy because Lua
-- does not need one - `ui.md` 16.9.
--------------------------------------------------------------------------

function ui.label(spec)
  local v = ui.view(spec)

  --
  -- **Words a person reads, in the face for reading** (`roadmap.md` 5zp).
  -- The mockups draw a window's text at 13.5 and its controls at 12.5, and
  -- the kit drew both in `ui` - so every label in the system was a control's
  -- size. A label is `text` unless it names a role; a `heading` still says
  -- so, and a label that wants to sit in a control's line can ask for `ui`.
  --
  v.role = v.role or "text"

  --
  -- **As tall as its face, unless told.** It was `GH` - the `ui` face's
  -- cell - and a view is clipped to its own height, so a label in a larger
  -- face would have lost the bottoms of its g's and y's without anything
  -- saying so. Measured again with the width, because a look can change the
  -- face after the label was made.
  --
  v.fixed_height = (spec.h or 0) > 0
  if not v.fixed_height then v.h = gfx.height(v.role) end

  --
  -- A label with no width given is as wide as its text - *currently*, not
  -- as it was when it was made.
  --
  -- Sizing it once at construction is the obvious thing and it is wrong in
  -- a way that is very quiet: a label created empty and filled in later gets
  -- a width of zero, and a view is clipped to its own width, so it never
  -- draws anything again. Two status lines in this system were blank for
  -- exactly that reason and neither looked like a bug - they looked like
  -- nothing had happened yet.
  --
  v.fixed_width = (spec.w or 0) > 0

  --
  -- **And in a face of its role**, when it names one: `role = "heading"`
  -- for the headings a window's groups have - "Look", "Wallpaper" - which
  -- is what the heading role is for, so an application stops choosing a
  -- size of its own for each.
  --
  function v:measure()
    if not self.fixed_width then
      self.w = gfx.measure(tostring(self.text or ""), self.role)
    end

    if not self.fixed_height then
      self.h = gfx.height(self.role)
    end
  end

  v:measure()

  function v:draw(g)
    g:text(0, 0, tostring(self.text or ""),
           shade(self.color) or theme.text, shade(self.bg), self.role)
  end

  return v
end

--
-- `disabled` greys a button's label and makes it answer nothing - for an
-- action that exists but is not offered yet, as the Drives app's Format...
-- is: shown, so a person can see it will be there, and not pressable.
--
--
-- The drawings' chevron: a "v" seven across and three down with arms two
-- pixels wide, which at this size is what a 1.8-unit stroke on a 16-unit
-- path comes to once it is anti-aliased. `cx` is its middle and `cy` its
-- top. A dropdown's, and a button's that opens a menu.
--
local function chevron(g, cx, cy)
  g:fill(cx - 3, cy,     2, 1, theme.text_dim)
  g:fill(cx + 2, cy,     2, 1, theme.text_dim)
  g:fill(cx - 2, cy + 1, 2, 1, theme.text_dim)
  g:fill(cx + 1, cy + 1, 2, 1, theme.text_dim)
  g:fill(cx - 1, cy + 2, 3, 1, theme.text_dim)
end

function ui.button(spec)
  local v = ui.view(spec)
  --
  -- Sized from the theme's `button_pad` when a caller gives no size: 5 by
  -- 12 in every theme but Plex, which is the 10 and 24 this said before
  -- the numbers were the theme's (`roadmap.md` 5s).
  --
  --
  -- **The drawings' button** (`docs/apps.html`, `roadmap.md` 5zp): the
  -- dropdown's height, its words with 12 either side and a one-pixel rule,
  -- so a button and a dropdown in one header are one box with and without a
  -- chevron. It was the words plus 32 and 28 tall.
  --
  -- `go = true` is the verb that starts something - Start, Add a worker,
  -- Apply - filled with the accent, as the drawings fill it. One per
  -- window, at most: two filled buttons are two answers to "what does this
  -- window do".
  --
  --
  -- **With a picture and a chevron**, it is `docs/tracker2.html`'s place
  -- button: a line icon 11 in, the words 7 after it in `role` (the label
  -- face, there), and a chevron 7 after those and 11 from the edge - a
  -- button that says where you are and opens a menu of where else. `fit`
  -- sizes it again when its words change, since that is what it is for.
  --
  local PAD, GAP, ICON, CHEV = 11, 7, 15, 11

  function v:fit()
    if self.fixed_width then return end

    local words = gfx.measure(tostring(self.text or ""), self.role)

    if self.icon or self.chevron then
      self.w = 1 + PAD + (self.icon and ICON + GAP or 0) + words
               + (self.chevron and GAP + CHEV or 0) + PAD + 1
    else
      self.w = words + 26
    end
  end

  v.h = v.h > 0 and v.h or BUTTON
  v.fixed_width = v.w > 0
  v:fit()
  v.focusable = not v.disabled

  function v:draw(g)
    local face = self.pressed and theme.accent or theme.raised

    if self.go and not self.disabled then
      local fill = self.pressed and theme.lift(theme.accent, -24)
                   or theme.accent

      g:fill_round(0, 0, self.w, self.h, fill, CONTROL_R)

      if self.focused and self.keyed then
        g:frame_round(0, 0, self.w, self.h, theme.ring, CONTROL_R)
        g:frame_round(1, 1, self.w - 2, self.h - 2, theme.text_on,
                      CONTROL_R - 1)
      end

      local label = tostring(self.text or "")

      g:text((self.w - gfx.measure(label)) // 2, centred(self.h), label,
             theme.text_on, fill)
      return
    end

    --
    -- Raised, and sunken while it is held - which is the oldest trick in
    -- the vocabulary and still the clearest: the button goes *in* under
    -- your finger. `ui.md` 16.8b.
    --
    if self.pressed then
      g:sunken(0, 0, self.w, self.h, face)
    else
      g:raised(0, 0, self.w, self.h, face)
    end

    -- The focus ring goes inside the bevel rather than over it, so a
    -- focused button is still visibly a button.
    if self.focused and self.keyed then
      g:frame(1, 1, self.w - 2, self.h - 2, "ring")
    end

    local label = tostring(self.text or "")
    local tx = (self.w - gfx.measure(label, self.role)) // 2
    local ty = (self.h - gfx.height(self.role)) // 2
    local press = self.pressed and 1 or 0

    if self.icon or self.chevron then
      tx = 1 + PAD + (self.icon and ICON + GAP or 0)

      if self.icon then
        g:line_icon(1 + PAD + press, (self.h - ICON) // 2 + press, self.icon,
                    theme.text_dim)
      end

      if self.chevron then
        chevron(g, self.w - 1 - PAD - CHEV // 2 - 1 + press,
                self.h // 2 - 1 + press)
      end
    end

    -- And the label moves with it, a pixel down and right, because a
    -- control that goes in takes its label with it.
    g:text(tx + press, ty + press, label,
           self.disabled and theme.text_dim
           or (self.pressed and theme.text_on or theme.text), face,
           self.role)
  end

  function v:key(c)
    if self.disabled then return false end

    if c == 10 or c == 13 or c == 32 then
      if self.on_click then self.on_click(self) end
      return true
    end
    return false
  end

  --
  -- Pressed on the way down, fired on the way up, and only if the pointer
  -- is still on the button.
  --
  -- That is not decoration. Every graphical system since the Macintosh has
  -- let you press a button, think better of it, slide off and release - and
  -- a button that fires on the press takes that away. It is the reason the
  -- window manager forwards movement while a button is held.
  --
  function v:mouse(action, x, y)
    local inside = x >= 0 and x < self.w and y >= 0 and y < self.h

    if self.disabled then return true end

    if action == "press" then
      self.pressed = true
    elseif action == "move" then
      self.pressed = inside
    elseif action == "release" then
      local fire = self.pressed and inside
      self.pressed = false

      if fire and self.on_click then self.on_click(self) end
    end

    return true
  end

  return v
end

function ui.checkbox(spec)
  local v = ui.view(spec)
  v.h = v.h > 0 and v.h or ROW
  v.w = v.w > 0 and v.w
        or ((v.text or "") == "" and 18
            or (gfx.measure(tostring(v.text)) + 18 + 8))
  v.focusable = true
  v.checked = v.checked or false

  --
  -- **In a flat look, the drawings' box**: 18 across with a radius of 5, a
  -- white well in a one-pixel rule, and ticked, the accent with a white
  -- check - the switch's colours, since a tick and a switch are the same
  -- state promised differently. The drawings have no checkbox of their own,
  -- so it takes its shape from the controls they do have: the dropdown's
  -- well and rule, the switch's accent.
  --
  local function draw_flat(self, g)
    local box = 18
    local by = (self.h - box) // 2

    if self.checked then
      g:fill_round(0, by, box, box, theme.accent, 5)
      g:line_icon((box - 15) // 2, by + (box - 15) // 2, "check", 0xffffffff)
    else
      g:fill_round(0, by, box, box, theme.sunken, 5)
      g:frame_round(0, by, box, box, theme.line, 5)
    end

    if self.focused and self.keyed then
      g:frame_round(-2, by - 2, box + 4, box + 4, theme.ring, 7)
    end

    g:text(box + 8, centred(self.h), tostring(self.text or ""), theme.text)
  end

  -- A 16-pixel box, as fixed as the row it sits in, and the words beside it.
  function v:draw(g)
    if theme.flat then return draw_flat(self, g) end

    local box = 16
    local by = (self.h - box) // 2

    g:sunken(0, by, box, box, "sunken")

    if self.focused and self.keyed then
      g:frame(1, by + 1, box - 2, box - 2, "ring")
    end

    if self.checked then
      g:fill(3, by + 3, box - 6, box - 6, theme.good)
    end

    g:text(box + 8, centred(self.h), tostring(self.text or ""), theme.text)
  end

  function v:key(c)
    if c == 32 or c == 10 or c == 13 then
      self.checked = not self.checked
      if self.on_change then self.on_change(self, self.checked) end
      return true
    end
    return false
  end

  function v:mouse(action, x, y)
    if action == "release" and x >= 0 and x < self.w
       and y >= 0 and y < self.h then
      self.checked = not self.checked
      if self.on_change then self.on_change(self, self.checked) end
    end

    return true
  end

  return v
end

--
-- A switch: a checkbox for a setting rather than for a choice in a dialog.
--
-- **The same state as `ui.checkbox` and a different promise about when it
-- takes effect.** A checkbox sits in a dialog and means "this is what I will
-- ask for"; a switch sits in a row of settings and means "this is how the
-- machine is *now*", so it paints its new state on the press and the thing
-- behind it follows. That is `instant-feedback` applied to a control rather
-- than to a window: never wait for a reply to draw what the hand just did.
--
-- Drawn as a track and a knob instead of a box and a tick, because a row of
-- settings is read by scanning down the right edge and a knob's *position*
-- is legible at a glance where a tick is not.
--
-- Arrived with Preferences (`roadmap.md` 5zh) and is in the kit rather than
-- in that application because Diego, seeing the mockup: "we might replicate
-- it all over the system".
--
--
-- **As `docs/preferences.html` draws it**: a pill 40 by 23, the accent when
-- on and `track` when off, and a round white knob 18 across that sits 3 in
-- from whichever end it is at, with a one-pixel shadow under it. It was a
-- rectangle with a square knob for its first two versions - a switch in
-- outline rather than a switch.
--
-- The knob is white in every look, including the dark ones, because that
-- is what makes it read as a thing on top of the rail rather than a hole in
-- it; every system that draws one draws it white.
--
local KNOB_WHITE  = 0xffffffff
local KNOB_SHADOW = 0x38000000

function ui.switch(spec)
  local v = ui.view(spec)
  local W, H, KNOB = 40, 23, 18

  --
  -- **As tall as the pill and its ring**, not a row: a switch sits in a
  -- card's row, centred by its own height, and a view seven pixels taller
  -- than what it draws made that row 55 where the drawings' is 48.
  --
  v.h = v.h > 0 and v.h or (H + 2)
  v.w = v.w > 0 and v.w or W
  v.focusable = true
  v.on = v.on or false

  local function flip(self)
    self.on = not self.on
    if self.on_change then self.on_change(self, self.on) end
  end

  function v:draw(g)
    local y = (self.h - H) // 2
    local kx = self.on and (W - KNOB - 3) or 3
    local ky = y + (H - KNOB) // 2

    g:fill_round(0, y, W, H, self.on and theme.accent or theme.track, H // 2)

    if self.focused and self.keyed then
      g:frame_round(-2, y - 2, W + 4, H + 4, theme.ring, H // 2 + 2)
    end

    -- The shadow is the same disc one pixel lower, drawn first: `0 1px 2px`
    -- in the drawing, which at this size is a one-pixel crescent.
    g:fill_round(kx, ky + 1, KNOB, KNOB, KNOB_SHADOW, KNOB // 2)
    g:fill_round(kx, ky, KNOB, KNOB, KNOB_WHITE, KNOB // 2)
  end

  function v:key(c)
    if c == 32 or c == 10 or c == 13 then flip(self) return true end
    return false
  end

  function v:mouse(action, x, y)
    if action == "release" and x >= 0 and x < self.w
       and y >= 0 and y < self.h then
      flip(self)
    end

    return true
  end

  return v
end

--
-- A dropdown: the value in force, and a menu of the others under it.
--
-- **It shows the value rather than the setting's name**, because the name is
-- already the label to its left and a control that repeats it says nothing.
-- So the width follows the longest choice and not the current one, or the
-- row would shift every time somebody changed it - which is the kind of
-- thing that is invisible in a screenshot and unbearable in use.
--
-- The menu is the window's own (`window:open_menu`), so it is the same menu
-- the menu bar and a right-click open: one implementation, one look, and a
-- choice marked the way `ui.menu_items` marks any other.
--
function ui.dropdown(spec)
  local v = ui.view(spec)

  v.focusable = true
  v.choices = v.choices or {}
  v.value = v.value

  --
  -- **Measured off `docs/preferences.html` at 1:1**, not estimated from its
  -- CSS: 31 tall, a one-pixel border in `line_soft`, a radius of 7, then 9
  -- of padding, the words, 7 of gap, an 11-pixel box for the chevron and 9
  -- more - so as wide as its words plus 38. The chevron's ink is a small
  -- stroked "v" whose middle sits 15 in from the right-hand edge.
  --
  -- It was the window's grey with a square border and a filled triangle,
  -- 24 tall - which is a dropdown in outline, the way the old switch was.
  --
  -- **As wide as its widest choice**, so the control does not change width
  -- as the value changes under a person's pointer.
  --
  local PAD, GAP, CHEV = 9, 7, 11

  function v:fit()
    local widest = 0

    for _, c in ipairs(self.choices) do
      local w = gfx.measure(tostring(c[2]))
      if w > widest then widest = w end
    end

    if not self.fixed_width then
      self.w = 1 + PAD + widest + GAP + CHEV + PAD + 1
    end
  end

  v.fixed_width = (spec.w or 0) > 0
  v.h = v.h > 0 and v.h or 31
  v:fit()

  function v:name()
    for _, c in ipairs(self.choices) do
      if c[1] == self.value then return tostring(c[2]) end
    end

    return tostring(self.value == nil and "" or self.value)
  end

  function v:draw(g)
    g:fill_round(0, 0, self.w, self.h, theme.sunken, 7)
    g:frame_round(0, 0, self.w, self.h,
                  self.focused and self.keyed and theme.ring or theme.line_soft, 7)

    g:text(1 + PAD, centred(self.h), self:name(), theme.text)

    --
    -- The chevron, as the button's (`chevron` above it).
    --
    chevron(g, self.w - 1 - PAD - CHEV // 2 - 1, self.h // 2 - 1)
  end

  --
  -- Where this sits in its window, and which window that is.
  --
  -- **Both by walking `parent`**, which `ui.view` has kept since the first
  -- container and which nothing had needed until a widget wanted to open
  -- something *outside* itself. A menu is drawn by the window, in the
  -- window's coordinates, so a control that opens one has to say where it is
  -- in those - and only the chain knows, because a view's own `x` is its
  -- offset inside whatever holds it.
  --
  -- The window is taken from the chain too, so an application places a
  -- dropdown exactly as it places a button and passes nothing extra. A
  -- `window` in the spec wins, for a caller that has one and no chain yet.
  --
  function v:where()
    local x, y, at = self.x, self.y, self.parent
    local win = self.window

    while at do
      if not win then win = at.window end

      x = x + (at.x or 0)
      y = y + (at.y or 0)
      at = at.parent
    end

    --
    -- **On the screen, not in the window.** `window:open_menu` places a
    -- window of its own and the window manager puts windows on the screen,
    -- which is why `origin_x` exists at all - `ui.menubar` adds it too
    -- (`window:move` keeps it current). A menu opened without it lands
    -- wherever the window happens not to be.
    --
    if win then
      x = x + (win.origin_x or 0)
      y = y + (win.origin_y or 0)
    end

    return x, y, win
  end

  function v:open()
    local items = {}

    for _, c in ipairs(self.choices) do
      local value, name = c[1], c[2]

      items[#items + 1] = {
        text = tostring(name),
        mark = (value == self.value),
        on_choose = function()
          self.value = value
          if self.on_change then self.on_change(self, value) end
        end,
      }
    end

    local x, y, win = self:where()

    if win and win.open_menu then
      win:open_menu(x, y + self.h, items)
      return true
    end

    return false
  end

  function v:key(c)
    if c == 32 or c == 10 or c == 13 then return self:open() end
    return false
  end

  function v:mouse(action, x, y)
    if action == "release" and x >= 0 and x < self.w
       and y >= 0 and y < self.h then
      self:open()
    end

    return true
  end

  return v
end

function ui.field(spec)
  local v = ui.view(spec)
  --
  -- **The inset is the theme's `field_pad`**, 3 by 4 in every theme but
  -- Plex - the 6 and the 4 this had written in (`roadmap.md` 5s) - and it
  -- is read where the words are drawn *and* where a click is turned into a
  -- column, which have to agree or a click lands a letter off.
  --
  v.h = v.h > 0 and v.h or FIELD
  v.w = v.w > 0 and v.w or 200
  v.focusable = true
  v.text = v.text or ""
  v.caret = #v.text + 1

  --
  -- All of it, or none of it.
  --
  -- **A field gets select-all and not a dragged selection**, and that is a
  -- decision rather than an unfinished one. A single line of text has one
  -- selection anybody actually makes - the whole thing, to replace it -
  -- and the machinery `ui.editor` needs for a range is an anchor, a
  -- normaliser, a three-piece draw and five keys that have to agree about
  -- it. Here that would be spent to let somebody drag over half a URL.
  --
  -- So the state is a boolean, and every one of the four edit commands has
  -- an honest answer for it.
  --
  v.all = false

  function v:draw(g)
    -- A well: content lives in here, and the bevel says so. The focus
    -- ring goes inside it rather than over it, so a focused field is
    -- still visibly a field.
    g:sunken(0, 0, self.w, self.h, "sunken")

    --
    -- The ring on the field's own edge, rounded as it is. It was a square
    -- frame a pixel inside, which in a flat look drew four corners across a
    -- rounded box.
    --
    if self.focused then
      if theme.flat then
        g:frame_round(0, 0, self.w, self.h, "ring", CONTROL_R)
      else
        g:frame(1, 1, self.w - 2, self.h - 2, "ring")
      end
    end

    -- The drawings' `.field`: a one-pixel rule and 9 of padding inside it.
    local inset = 10
    local room = (self.w - 2 * inset) // GW

    --
    -- What the field is for, while there is nothing in it.
    --
    -- In `text_dim` and gone the moment anything is typed, so it can never
    -- be mistaken for content. A search box with no label beside it is a
    -- box, and a box does not say what it searches.
    --
    if self.hint and self.text == "" and not self.focused then
      g:text(inset, centred(self.h), tostring(self.hint):sub(1, room),
             theme.text_dim, theme.sunken)
      return
    end

    local from = math.max(1, self.caret - room + 1)
    local shown = self.text:sub(from, from + room - 1)

    local ty = centred(self.h)

    if self.all and self.text ~= "" then
      -- The caret's own colours, for the reason `ui.editor` uses them: a
      -- selection is a widened cursor, and every palette has already had
      -- to make those two readable against each other.
      g:fill(inset, ty, #shown * GW, gfx.height(), theme.ring)
      g:text(inset, ty, shown, theme.sunken, theme.ring)
      return
    end

    g:text(inset, ty, shown, theme.text, theme.sunken)

    if self.focused then
      local cx = inset + (self.caret - from) * GW
      g:fill(cx, ty, 1, gfx.height(), theme.ring)
    end
  end

  --
  -- `on_change` fires on every edit; `on_enter` only on Return.
  --
  -- Two callbacks because the two costs are different, and a search box is
  -- where that shows: filtering a list already in hand is free and should
  -- happen as you type, while asking a server is a message and should
  -- happen when you say so.
  --
  local function changed(self)
    if self.on_change then self.on_change(self, self.text) end
  end

  function v:key(c)
    --
    -- Anything at all ends the selection, and a key that puts a character
    -- in replaces what was selected first.
    --
    -- Backspace is finished once the selection is gone - taking a further
    -- character would eat one nobody selected. A printable falls through
    -- into the insert below, which fires `on_change` itself, so clearing
    -- here must not fire it as well: a search box would run its filter
    -- twice for one keystroke. And Enter is not in the list at all,
    -- because submitting a field is not editing it.
    --
    if self.all then
      self.all = false

      if c == 8 or c == 127 or (c >= 32 and c < 127) then
        self.text, self.caret = "", 1

        if c == 8 or c == 127 then
          changed(self)
          return true
        end
      end
    end

    if c == 8 or c == 127 then
      if self.caret > 1 then
        self.text = self.text:sub(1, self.caret - 2)
                    .. self.text:sub(self.caret)
        self.caret = self.caret - 1
        changed(self)
      end
      return true
    end

    if c == 27 then                       -- Escape empties it
      if self.text ~= "" then
        self.text, self.caret = "", 1
        changed(self)
        return true
      end

      return false
    end

    if c == 10 or c == 13 then
      if self.on_enter then self.on_enter(self, self.text) end
      return true
    end

    if c >= 32 and c < 127 then
      self.text = self.text:sub(1, self.caret - 1) .. string.char(c)
                  .. self.text:sub(self.caret)
      self.caret = self.caret + 1
      changed(self)
      return true
    end

    return false
  end

  --
  -- Select-all, copy, cut and paste - the whole field, except for a paste,
  -- which goes in at the caret like a typed character does.
  --
  -- A pasted newline is dropped rather than honoured. This widget is one
  -- line by construction and there is nowhere for a second one to go;
  -- taking the first line is what every single-line field does with a
  -- multi-line paste, and it is better than refusing.
  --
  function v:edit(kind)
    if kind == "selectall" then
      if self.text == "" then return false end
      self.all = true
      return true
    end

    if kind == "copy" or kind == "cut" then
      if self.text == "" then return false end
      if not wmproto.copy(self.text) then return false end

      if kind == "cut" then
        self.text, self.caret, self.all = "", 1, false
        changed(self)
      end

      return true
    end

    if kind == "paste" then
      local text = wmproto.paste()

      if not text or text == "" then return false end

      if self.all then
        self.text, self.caret, self.all = "", 1, false
      end

      text = text:match("^[^\n]*") or ""
      self.text = self.text:sub(1, self.caret - 1) .. text
                  .. self.text:sub(self.caret)
      self.caret = self.caret + #text
      changed(self)

      return true
    end

    return false
  end

  -- The caret where the click was, clamped to the end of the text: clicking
  -- past the last character puts it after the last character, which is what
  -- every text field does and what nobody notices until it does not.
  function v:mouse(action, x, y)
    if action == "press" then
      local col = (x - 10) // GW

      if col < 0 then col = 0 end
      self.caret = math.min(col + 1, #self.text + 1)
      self.all = false
    end

    return true
  end

  return v
end

--
-- **A path as a row of targets**: `/ > home > Desktop`, each name going there
-- when pressed, and the last - where you already are - drawn full and going
-- nowhere. Tracker's, moved here on 18 September when the Open and Save
-- window became its second user.
--
-- `ui.md` 16.8d has the two rules, both measured rather than assumed: a
-- segment's target runs to the start of the next one, separator included,
-- because a press three pixels into ` > ` once did nothing and looked exactly
-- like a broken handler; and widths come from `gfx.measure`, because the
-- faces are proportional and counting characters misplaces every span after
-- the first.
--
--   ui.trail{ x =, y =, w =, text = "/home", on_visit = function(path) end }
--
function ui.trail(spec)
  local v = ui.view(spec)
  local SEPARATOR = " > "

  v.h = v.h > 0 and v.h or GH
  v.text = v.text or "/"

  local function parts_of(path)
    local parts = { { text = "/", path = "/" } }
    local at = ""

    for name in tostring(path or "/"):gmatch("[^/]+") do
      at = at .. "/" .. name
      parts[#parts + 1] = { text = name, path = at }
    end

    return parts
  end

  function v:draw(g)
    local parts = parts_of(self.text)
    local x = 0

    self.spans = {}

    for i, part in ipairs(parts) do
      if i > 1 then
        g:text(x, 0, SEPARATOR, theme.text_dim)
        x = x + gfx.measure(SEPARATOR)
      end

      local width = gfx.measure(part.text)
      local last = (i == #parts)

      g:text(x, 0, part.text, last and theme.text or theme.text_dim)

      if not last then
        local reach = x + width + gfx.measure(SEPARATOR)

        self.spans[#self.spans + 1] = { from = x, to = reach, path = part.path }
      end

      x = x + width

      if x > self.w then break end
    end
  end

  function v:on_click(x, _)
    for _, span in ipairs(self.spans or {}) do
      if x >= span.from and x < span.to then
        if self.on_visit then self.on_visit(span.path) end
        return
      end
    end
  end

  return v
end

--------------------------------------------------------------------------
-- A sidebar: a window's navigation, as the mockups draw it.
--
--   ui.sidebar{ x, y, w, h,
--               items = { { id = "appearance", name = "Appearance",
--                           icon = "appearance" }, { gap = true }, ... },
--               selected = "appearance",
--               on_select = function(self, id) ... end }
--
-- **Measured off `docs/preferences.html` at 1:1** (`roadmap.md` 5zp): rows
-- on a 35-pixel pitch inset 8 from each side, a gap of 12 where the list
-- asks for one, a line icon 15 across at 10 from the row's edge and the
-- word 10 after it. The chosen row is a rounded fill in `line_soft` with a
-- radius of 7, its words still dark and its icon in the accent - a quiet
-- selection, because the accent-filled bar a file list uses shouts in a
-- list whose job is only to say where you are.
--
-- **The arrows choose.** This is navigation, and a page that waits for
-- Enter is a window whose categories cannot be browsed - the same bargain
-- `ui.list`'s `arrows_choose` makes, built in here because a sidebar that
-- did not would be a list.
--
-- The background is the window's own, drawn by whoever holds this: a
-- sidebar is a column of a window rather than a box inside one.
--
local SIDE_PITCH, SIDE_INSET, SIDE_GAP, SIDE_R = 35, 8, 12, 7
local SIDE_ICON, SIDE_WORD = 10, 35

function ui.sidebar(spec)
  local v = ui.view(spec)

  v.focusable = true
  v.items = v.items or {}

  --
  -- **`pitch`, and a gap with a `rule`**, for Tracker's places
  -- (`docs/tracker2.html`): its rows are 33 apart where Preferences' are
  -- 35, and its groups are parted by a hairline 9 below the last row, 10
  -- in from either side, rather than by space alone - 19 in all.
  --
  local PITCH = v.pitch or SIDE_PITCH
  local RULE_GAP = 19

  -- Where each row is, worked out once from the list.
  local function rows(self)
    local out, y = {}, 0

    for _, it in ipairs(self.items) do
      if it.gap then
        y = y + (it.rule and RULE_GAP or SIDE_GAP)
      else
        out[#out + 1] = { item = it, y = y }
        y = y + PITCH
      end
    end

    return out
  end

  -- The item under a point, for a right-click or a drop; nil between rows.
  function v:item_at(y)
    for _, r in ipairs(rows(self)) do
      if y >= r.y and y < r.y + PITCH then return r.item, r.y end
    end

    return nil
  end

  -- Where an item's row is, by its id - for a caller that says where
  -- things are (Tracker tells the display harness where a new place went).
  function v:row_of(id)
    for _, r in ipairs(rows(self)) do
      if r.item.id == id then return r.y, PITCH end
    end

    return nil
  end

  local function index_of(self, list)
    for i, r in ipairs(list) do
      if r.item.id == self.selected then return i end
    end

    return nil
  end

  local function choose(self, id)
    if id == self.selected then return end

    self.selected = id
    if self.on_select then self.on_select(self, id) end
  end

  function v:draw(g)
    local w = self.w - 2 * SIDE_INSET
    local y = 0

    -- The hairlines between groups, where a gap asks for one.
    for _, it in ipairs(self.items) do
      if it.gap then
        if it.rule then
          g:fill(SIDE_INSET + 10, y + 9, self.w - 2 * (SIDE_INSET + 10), 1,
                 theme.line_soft)
        end

        y = y + (it.rule and RULE_GAP or SIDE_GAP)
      else
        y = y + PITCH
      end
    end

    for _, r in ipairs(rows(self)) do
      local on = r.item.id == self.selected

      if on then
        g:fill_round(SIDE_INSET, r.y, w, PITCH, theme.line_soft, SIDE_R)
      end

      --
      -- **A ring only once the keyboard is in use.** The drawing has none:
      -- the fill already says which row is chosen, and a ring around it
      -- whenever the window has the focus - which is always, since this is
      -- the first thing in it - draws a box the mockup does not. After an
      -- arrow it is there, because that is when a person is looking for
      -- where the keys will go; a press with the pointer puts it away.
      --
      if self.focused and self.keyed and on then
        g:frame_round(SIDE_INSET, r.y, w, PITCH, theme.ring, SIDE_R)
      end

      if r.item.icon then
        g:line_icon(SIDE_INSET + SIDE_ICON, r.y + (PITCH - 15) // 2,
                    r.item.icon, on and theme.accent or theme.text_dim)
      end

      -- The chosen one's words in `label`, the drawing's weight 500; a row
      -- with nowhere to go (`quiet`) dim.
      local face = on and "label" or "text"

      g:text(SIDE_INSET + SIDE_WORD, r.y + (PITCH - gfx.height(face)) // 2,
             r.item.name or "", r.item.quiet and theme.text_dim or theme.text,
             nil, face)
    end
  end

  function v:key(c)
    local list = rows(self)
    local i = index_of(self, list) or 1

    if c == -1 and i > 1 then choose(self, list[i - 1].item.id) return true end
    if c == -2 and i < #list then
      choose(self, list[i + 1].item.id)
      return true
    end

    return c == -1 or c == -2
  end

  function v:mouse(action, x, y)
    if action ~= "press" then return true end

    for _, r in ipairs(rows(self)) do
      if y >= r.y and y < r.y + PITCH
         and x >= SIDE_INSET and x < self.w - SIDE_INSET
         and not r.item.quiet then
        choose(self, r.item.id)
        break
      end
    end

    return true
  end

  return v
end

--------------------------------------------------------------------------
-- The drawings' layout, as widgets (`roadmap.md` 5zp, `docs/apps.html`).
--
-- Diego, 24 September 2026: "The spacing of elements in the ui is key to a
-- nice design. I see some labels in apps that have no margin or spacing and
-- too close to other elements. Make sure all widgets are spaced and have the
-- correct margin as the mockups."
--
-- **Those labels sat at pixels each application picked**, and there were
-- as many spacings as applications. So the header, the page and the cards of
-- rows are widgets here, carrying the drawings' numbers, and an application
-- says what goes in them rather than where. Every number below was measured
-- off `docs/preferences.html` or `docs/tracker2.html` rendered at one pixel
-- to one, beside a screendump.
--
ui.layout = {
  head      = 46,   -- a header, its rule the last pixel
  head_in   = 18,   -- the subject, in from the left
  head_edge = 10,   -- controls, in from either edge
  head_gap  = 4,    -- between two controls
  page_top  = 22,   -- a page of cards: from the header to the first name
  page_side = 26,   --   and in from either side
  page_foot = 26,   --   and below the last card
  group     = 19,   -- a group's name: 12.5 at 1.55
  to_card   = 26,   -- from that name's top to its card
  between   = 20,   -- from a card to the next group's name
  card_r    = 10,   -- a card's corner
  row_pad   = 11,   -- a row: above and below what is in it
  row_in    = 14,   --   in from the card's edges, and between its parts
  row_min   = 48,   --   and never shorter
  line      = 21,   -- a row's name: 13.5 at 1.55
  note      = 17,   -- a note under it: 12 at 1.4
}

local L = ui.layout

--
-- **An icon button**: 26 square, no border, a line icon at 15 in the dim
-- colour - `tracker2.html`'s `.ico`. Pressed, a quiet fill under it.
--
function ui.iconbutton(spec)
  local v = ui.view(spec)

  v.w = v.w > 0 and v.w or 26
  v.h = v.h > 0 and v.h or 26
  v.focusable = true

  function v:draw(g)
    if self.pressed then
      g:fill_round(0, 0, self.w, self.h, theme.line_soft, 6)
    end

    if self.focused and self.keyed then
      g:frame_round(0, 0, self.w, self.h, theme.ring, 6)
    end

    g:line_icon((self.w - 15) // 2, (self.h - 15) // 2, self.icon or "more",
                self.pressed and theme.text or theme.text_dim)
  end

  function v:key(c)
    if c == 10 or c == 13 or c == 32 then
      if self.on_click then self.on_click(self) end
      return true
    end

    return false
  end

  function v:mouse(action, x, y)
    local inside = x >= 0 and x < self.w and y >= 0 and y < self.h

    if action == "press" then
      self.pressed = true
    elseif action == "move" then
      self.pressed = inside
    elseif action == "release" then
      local fire = self.pressed and inside

      self.pressed = false
      if fire and self.on_click then self.on_click(self) end
    end

    return true
  end

  return v
end

--
-- **A header**: 46 with its rule, the subject in the title's face 18 in,
-- what the window is looking at beside it in the dim `ui` face, and the
-- controls - `left` before the subject, `right` against the far edge - 10
-- in from the edges and 4 apart, each centred in the band.
--
--   ui.header{ x = 0, y = 0, w = W, title = "Processes",
--              sub = "21 processes . 28 threads",
--              right = { ui.button{ text = "End" }, ui.iconbutton{} } }
--
-- `title` and `sub` are fields; set them and the next paint says them.
-- Placed from its own width every time it is drawn, so a window that is
-- resized keeps its controls in the corner without a `follow` of their own.
--
function ui.header(spec)
  --
  -- **The default `follow` goes in before the view is made**, because
  -- `ui.view` fills in its own and turns the list into flags as it builds -
  -- so a default set afterwards was never read, and a header stayed the
  -- width it opened at when its window was tiled narrower, with its last
  -- button off the edge.
  --
  spec.follow = spec.follow or { "left", "right", "top" }

  local v = ui.view(spec)

  v.h = L.head
  v.left = v.left or {}
  v.right = v.right or {}

  for _, c in ipairs(v.left) do v:add(c) end
  for _, c in ipairs(v.right) do v:add(c) end

  local function centre(c) return (L.head - 1 - c.h) // 2 end

  --
  -- `edge` is `{ left, right }` where a drawing's header has insets of its
  -- own - Tracker's pane is 6 and 8 (`docs/tracker2.html`) - and a control's
  -- `space` is room before it beyond the gap, for the drawing's place
  -- button, which stands 6 clear of the arrows.
  --
  local edge_l = v.edge and v.edge[1] or L.head_edge
  local edge_r = v.edge and v.edge[2] or L.head_edge

  function v:measure()
    local x = edge_l

    -- A hidden control takes no room on either side: Tracker's place
    -- button and its search field share one slot, one of them hidden.
    for _, c in ipairs(self.left) do
      if not c.hidden then
        x = x + (c.space or 0)
        c.x, c.y = x, centre(c)
        x = x + c.w + L.head_gap
      end
    end

    self.title_x = (#self.left > 0) and (x + L.head_edge - L.head_gap)
                   or L.head_in

    local r = self.w - edge_r

    for i = #self.right, 1, -1 do
      local c = self.right[i]

      if not c.hidden then
        r = r - c.w
        c.x, c.y = r, centre(c)
        r = r - L.head_gap
      end
    end

    self.room = r - L.head_edge
  end

  function v:draw(g)
    g:fill(0, 0, self.w, L.head - 1, theme.sunken)
    g:fill(0, L.head - 1, self.w, 1, theme.line_soft)

    local x = self.title_x or L.head_in
    local title = tostring(self.title or "")

    if title ~= "" then
      g:text(x, (L.head - 1 - gfx.height("title")) // 2, title, theme.text,
             nil, "title")
      x = x + gfx.measure(title, "title") + 8
    end

    local sub = tostring(self.sub or "")

    if sub ~= "" then
      --
      -- Cut to what is left before the controls, with an ellipsis, rather
      -- than run under them - a sentence that disappears behind a button is
      -- the "too close to other elements" this widget exists to end.
      --
      local room = (self.room or self.w) - x

      if gfx.measure(sub) > room then
        local n = fits(sub, nil, room - gfx.measure("..."))
        sub = n and (sub:sub(1, n) .. "...") or ""
      end

      g:text(x, (L.head - 1 - gfx.height()) // 2, sub, theme.text_dim)
    end
  end

  return v
end

--
-- **A slider**: the drawings' level - a rail 4 high in `track`, the part up
-- to the value in the accent, and a white knob 16 across with a faint edge.
-- `value` is 0 to `max` (100 unless said); the arrows step by a twentieth,
-- a press or a drag puts the knob where the pointer is.
--
function ui.slider(spec)
  local v = ui.view(spec)

  v.w = v.w > 0 and v.w or 160
  v.h = v.h > 0 and v.h or 20
  v.max = v.max or 100
  v.value = v.value or 0
  v.focusable = true

  local KNOB = 16

  local function span(self) return self.w - KNOB end

  local function set(self, value)
    value = math.max(0, math.min(self.max, value // 1))

    if value == self.value then return end

    self.value = value
    if self.on_change then self.on_change(self, value) end
  end

  function v:draw(g)
    local y = self.h // 2 - 2
    local at = KNOB // 2 + span(self) * self.value // self.max

    g:fill_round(KNOB // 2, y, span(self), 4, theme.track, 2)
    g:fill_round(KNOB // 2, y, at - KNOB // 2, 4, theme.accent, 2)

    local ky = (self.h - KNOB) // 2

    g:fill_round(at - KNOB // 2, ky + 1, KNOB, KNOB, 0x30000000, KNOB // 2)
    g:fill_round(at - KNOB // 2, ky, KNOB, KNOB, 0xffffffff, KNOB // 2)
    g:frame_round(at - KNOB // 2, ky, KNOB, KNOB,
                  self.focused and self.keyed and theme.ring or theme.line_soft,
                  KNOB // 2)
  end

  function v:key(c)
    local step = math.max(1, self.max // 20)

    if c == -3 or c == -1 then set(self, self.value + step) return true end
    if c == -4 or c == -2 then set(self, self.value - step) return true end

    return false
  end

  function v:mouse(action, x)
    if action == "press" or action == "move" then
      set(self, (x - KNOB // 2) * self.max // math.max(1, span(self)))
    end

    return true
  end

  return v
end

--
-- **A page of cards**: groups of rows, each a name above a rounded card,
-- every row the drawings' 11 + what is in it + 11 and 48 at the least, its
-- name on the left and its control against the right.
--
--   ui.cards{ x = 0, y = L.head, w = W, h = H - L.head,
--             groups = {
--               { name = "Output", rows = {
--                   { label = "Master", control = ui.slider{ value = 70 } },
--                   { label = "Mute", control = ui.switch{} },
--                   { label = "Rate", note = "what the card runs at",
--                     value = "44100 Hz" } } } },
--             foot = "Test tone plays a second of A." }
--
-- A row's `control` is placed; with `fill = true` it takes the width left
-- after the row's name. `value` is words on the right in the dim `ui` face.
-- `width` caps the column, centred, as Preferences' 470 does.
--
-- Call `set(groups, foot)` to show other rows: a page is rebuilt, not
-- edited, because the rows are what the application knows.
--
function ui.cards(spec)
  -- Before the view is made, for the reason `ui.header` gives.
  spec.follow = spec.follow or { "left", "right", "top", "bottom" }

  local v = ui.view(spec)
  v.cards = {}

  function v:column()
    local room = self.w - 2 * L.page_side
    local width = self.width and math.min(self.width, room) or room

    return L.page_side + (room - width) // 2, width
  end

  function v:set(groups, foot)
    for i = #self.children, 1, -1 do self.children[i] = nil end

    self.groups, self.foot = groups or self.groups or {}, foot or self.foot
    self.cards = {}

    local cx, width = self:column()
    local y = L.page_top

    for gi, group in ipairs(self.groups) do
      if gi > 1 then y = y + L.between end

      if group.name and group.name ~= "" then
        self:add(ui.label{ x = cx + 3,
                           y = y + (L.group - gfx.height("heading")) // 2,
                           w = width - 3, text = group.name,
                           role = "heading" })
        y = y + L.to_card
      end

      local card = { y = y, rules = {} }
      local rows = group.rows or {}
      local pad = group.compact and 8 or L.row_pad
      local least = group.compact and 40 or L.row_min

      y = y + 1

      for i, row in ipairs(rows) do
        local c = row.control
        local right = cx + width - 1 - L.row_in
        local left = cx + 1 + L.row_in
        local words = (row.label and row.label ~= "" and L.line or 0)
                      + (row.note and L.note or 0)
        local name_w = row.label and gfx.measure(row.label, "label") or 0
        local ch = c and c.h or (row.value and gfx.height() or 0)
        local h = math.max(least - ((i < #rows) and 1 or 0),
                           2 * pad + math.max(words, ch))
        local taken = 0

        if c then
          if c.fill then
            local from = left + (name_w > 0 and (row.name_w or name_w)
                                 + L.row_in or 0)
            c.x, c.w = from, right - from
          else
            c.x = right - c.w
          end

          c.y = y + (h - c.h) // 2
          self:add(c)
          taken = c.fill and (right - c.x) or c.w
        elseif row.value then
          taken = gfx.measure(tostring(row.value))
          self:add(ui.label{ x = right - taken, y = y + (h - ch) // 2,
                             w = taken + 2, text = tostring(row.value),
                             color = theme.text_dim, role = "ui" })
        end

        local room = right - (taken > 0 and taken + L.row_in or 0) - left
        local top = y + (h - words) // 2

        if row.label and row.label ~= "" then
          self:add(ui.label{ x = left,
                             y = top + (L.line - gfx.height("label")) // 2,
                             w = c and c.fill and (row.name_w or name_w)
                                 or room,
                             text = row.label, role = "label" })
        end

        if row.note then
          self:add(ui.label{ x = left,
                             y = top + L.line + (L.note - gfx.height()) // 2,
                             w = room, text = row.note,
                             color = theme.text_dim, role = "ui" })
        end

        y = y + h

        if i < #rows then
          card.rules[#card.rules + 1] = y
          y = y + 1
        end
      end

      y = y + 1
      card.h = y - card.y
      self.cards[#self.cards + 1] = card
    end

    -- Where the last card ends, for a window that puts something of its own
    -- under the cards - a list, a log - at the drawings' spacing.
    self.content_h = y

    --
    -- The page's note, wrapped at word boundaries to the column: a label is
    -- clipped at its width, and a sentence that stops mid-word with no sign
    -- it did is the kind of thing this widget exists to prevent.
    --
    if self.foot and self.foot ~= "" then
      local line, ly = "", y + 10
      local step = gfx.height() + 3

      local function flush()
        if line ~= "" then
          self:add(ui.label{ x = cx + 3, y = ly, w = width - 3, text = line,
                             color = theme.text_dim, role = "ui" })
          ly = ly + step
          line = ""
        end
      end

      for word in tostring(self.foot):gmatch("%S+") do
        local try = (line == "") and word or (line .. " " .. word)

        if line ~= "" and gfx.measure(try) > width - 3 then
          flush()
          line = word
        else
          line = try
        end
      end

      flush()
    end

    self.built_w = self.w
  end

  --
  -- Built again when the width changes, because the column and every
  -- control against its right edge move with it.
  --
  function v:measure()
    if self.built_w ~= self.w and self.groups then self:set() end
  end

  function v:draw(g)
    local cx, width = self:column()

    g:fill(0, 0, self.w, self.h, theme.window)

    for _, c in ipairs(self.cards) do
      g:fill_round(cx, c.y, width, c.h, theme.sunken, L.card_r)
      g:frame_round(cx, c.y, width, c.h, theme.line_soft, L.card_r)

      for _, at in ipairs(c.rules) do
        g:fill(cx + 1, at, width - 2, 1, theme.line_soft)
      end
    end
  end

  if v.groups then v:set(v.groups, v.foot) end

  return v
end

--------------------------------------------------------------------------

function ui.list(spec)
  local v = ui.view(spec)
  --
  -- **How tall a row is, asked rather than remembered.**
  --
  -- Every one of these was `GH`, which is `gfx.font.h` as it stood when
  -- this file loaded - and in an application that is *before* the desktop
  -- has said what its faces are, because the faces arrive in the reply to
  -- the window this kit is about to open. So a list drew rows 16 pixels
  -- apart and put 23-pixel text in them, and the rows above and below the
  -- selected one were cut into. Diego photographed it in the Appearance
  -- panel's font list on 20 September: "each row should be able to show the
  -- contents without any overlapping on the other rows below or above".
  --
  -- Exactly `gfx.height()` and no padding: under the 8x16 bitmap it is 16,
  -- which is what `GH` was, so nothing that was measured against that face
  -- moves - and under a proportional face it is however tall that face is,
  -- which is the whole of the fix.
  --
  --
  -- **And then fixed, the way that fix should have gone** (`roadmap.md`
  -- 5x). A row that is as tall as its face grows when the face does, and
  -- the list's neighbours do not - so on 22 September a theme that padded
  -- rows, and faces at 16, moved and broke windows that place their widgets
  -- at fixed positions. A row is `theme.metrics.row` in every look - 24
  -- then, 32 since the drawings' sizes (0.10.149) - and the words are
  -- centred in it by the face in force, which is what
  -- keeps Diego's 20 September photograph from coming back - the rows no
  -- longer overlap because the faces the looks carry fit them
  -- (`tools/test_theme.lua`).
  --
  local function row_h() return ROW end

  v.h = v.h > 0 and v.h or (row_h() * 6)

  -- The same number, for a caller that places something by rows.
  function v:row_height() return row_h() end
  v.w = v.w > 0 and v.w or 200
  v.focusable = true
  v.items = v.items or {}
  v.selected = v.selected or 1
  v.top = 1

  --
  -- An optional checkbox down the left.
  --
  -- `checks` is a set keyed by the item's own text, so the caller reads the
  -- answer straight out of it without walking anything, and the set survives
  -- the list being rebuilt in a different order. Absent, and this is the
  -- plain list it has always been - which is the point of it being a field
  -- on the list rather than a widget of its own. A preferences window that
  -- wants "these, of those" should not need a second kind of list, and the
  -- three that already exist should not pay for one.
  --
  v.checks = v.checks or nil

  function v:draw(g)
    local flat = theme.flat

    --
    -- **In a flat look, the drawings' list** (`docs/apps.html`): a card
    -- with the cards' corner and rule, the chosen row a rounded fill in
    -- `line_soft` with the words left as they are - the sidebar's chosen
    -- row, since both say "this one" - and the ring round the card when
    -- the keyboard is in it.
    --
    if flat then
      g:fill_round(0, 0, self.w, self.h, theme.sunken, 10)
      g:frame_round(0, 0, self.w, self.h,
                    self.focused and self.keyed and theme.ring
                    or theme.line_soft, 10)
    else
      -- A well: content lives in here, and the bevel says so. The focus
      -- ring goes inside it rather than over it, so a focused field is
      -- still visibly a field.
      g:sunken(0, 0, self.w, self.h, "sunken")

      if self.focused and self.keyed then
        g:frame(1, 1, self.w - 2, self.h - 2, "ring")
      end
    end

    local rows = (self.h - 4) // row_h()

    --
    -- Follow the selection when it *moves*, and not on every pass.
    --
    -- This used to run unconditionally, which made the scrollbar useless the
    -- moment anything was selected: pick the last wallpaper in a list, drag
    -- the bar up, and the next repaint sees `selected` below the window and
    -- pulls `top` straight back down. Scrolled to the end and stuck there.
    --
    -- The two are different intentions. Moving the selection with the
    -- keyboard should bring the view along - that is what this is for.
    -- Moving the view with the bar is a decision about the *view*, and it
    -- should hold until the selection moves again.
    --
    if self.selected ~= self.followed then
      if self.selected < self.top then self.top = self.selected end
      if self.selected > self.top + rows - 1 then
        self.top = self.selected - rows + 1
      end

      self.followed = self.selected
    end

    -- And never past the end: the keyboard can only move the selection, but
    -- the bar and the wheel move `top` directly.
    if self.top > #self.items - rows + 1 then
      self.top = #self.items - rows + 1
    end

    if self.top < 1 then self.top = 1 end

    self.rows = rows
    self.bar = draw_scrollbar(g, self.w, self.h, #self.items, rows, self.top)

    -- The rows stop where the bar starts, or the last column of every long
    -- item would be drawn underneath it.
    local room = self.w - 4 - (self.bar and SCROLL_W + 2 or 0)

    for i = 0, rows - 1 do
      local n = self.top + i
      local item = self.items[n]

      if item then
        local y = 2 + i * row_h()
        local on = (n == self.selected)
        local bg = on and (flat and theme.line_soft or theme.accent)
                   or theme.sunken

        if on and flat then
          g:fill_round(4, y, room - 4, row_h(), bg, 6)
        elseif on then
          g:fill(2, y, room, row_h(), bg)
        end

        -- The row's words centred in it; the selection above fills the
        -- whole row, as a row is one thing.
        local tx = flat and 12 or 8

        if self.checks then
          local box = 16
          local by = y + (ROW - box) // 2

          g:sunken(4, by, box, box, "sunken")

          if self.checks[tostring(item)] then
            g:fill(6, by + 2, box - 4, box - 4, theme.good)
          end

          tx = 4 + box + 6
        end

        y = y + centred(ROW)

        --
        -- **A row a caller draws**, when it has fields rather than a name:
        -- the Open and Save window's Name, Size and Kind. Everything else a
        -- list does - which row, scrolling, the selection - stays here.
        --
        if self.draw_item then
          self:draw_item(g, item, tx, y, room - tx, on)
        else
          g:text(tx, y, tostring(item),
                 (on and not flat) and theme.text_on or theme.text, bg)
        end
      end
    end
  end

  function v:key(c)
    --
    --
    -- **`arrows_choose` - moving the selection chooses it.** Off by
    -- default, because a list of files is a list you arrow through to reach
    -- the one you want, and opening each on the way past would be
    -- unbearable. On for a list that *is* navigation, where the arrows are
    -- how you look at things: Preferences' sidebar, where a page that waits
    -- for Enter is a window whose categories cannot be browsed at all.
    --
    -- Opt-in rather than the default, so `ui.list`'s every other caller -
    -- Tracker, the Open panel, the launcher editor - behaves exactly as it
    -- did.
    --
    -- **Not `follow`, which this was called for one build.** A view already
    -- has a `follow`: the list of edges it keeps its distance to when its
    -- parent is resized. Setting it to `true` made `#self.follow` a length
    -- of a boolean, and Preferences died on its first arrow key. A name the
    -- kit already uses for something else is not a name.
    --
    local function moved(self_)
      if self_.arrows_choose and self_.on_select then
        self_.on_select(self_, self_.items[self_.selected], self_.selected)
      end

      return true
    end

    -- Arrows arrive already decoded, as negative codes. See `dispatch`.
    if c == -1 then
      self.selected = math.max(1, self.selected - 1)
      return moved(self)
    end

    if c == -2 then
      self.selected = math.min(#self.items, self.selected + 1)
      return moved(self)
    end

    -- Space, because a checklist you can reach with the arrows and cannot
    -- tick with the keyboard is a checklist you have to use the mouse for.
    if c == 32 and self.checks then
      local key = tostring(self.items[self.selected])

      if key then
        self.checks[key] = (not self.checks[key]) or nil

        if self.on_toggle then self.on_toggle(self, key, self.checks[key]) end
      end

      return true
    end

    if c == 10 or c == 13 then
      if self.on_select then
        self.on_select(self, self.items[self.selected], self.selected)
      end

      if self.on_open then
        self.on_open(self, self.items[self.selected], self.selected)
      end

      return true
    end

    return false
  end

  --
  -- Press picks the row, release on that same row chooses it.
  --
  -- Two steps rather than one so that pressing on the wrong row and sliding
  -- to the right one before letting go does what you meant, which is the
  -- same bargain the button makes.
  --
  function v:mouse(action, x, y)
    local rows = self.rows or ((self.h - 4) // row_h())

    --
    -- The bar first, because it sits over the right-hand end of every row
    -- and a click there is not a click on an item.
    --
    local to = ui.scrollbar_mouse(self, action, x, y, self.w, self.h,
                                  #self.items, rows, self.top)

    if to then
      self.top = to

      return true
    end

    local row = (y - 2) // row_h()
    local n = self.top + row

    if row < 0 or n > #self.items then return true end

    --
    -- The box is its own target. Pressing it toggles and does not select,
    -- because a checklist is read down the boxes and a selection moving
    -- under your eye while you tick things is noise.
    --
    if self.checks and action == "press" and x < 4 + 16 + 2 then
      local key = tostring(self.items[n])

      self.checks[key] = (not self.checks[key]) or nil

      if self.on_toggle then self.on_toggle(self, key, self.checks[key]) end

      return true
    end

    if action == "press" or action == "move" then
      self.selected = n
    elseif action == "release" and n == self.selected then
      if self.on_select then
        self.on_select(self, self.items[n], n)
      end

      --
      -- **A second click on the same row opens it** - `on_open`, as Enter
      -- does - the rule the tree has (`ui.md` 16.8c) and Tracker's list
      -- has always had. A list with no `on_open` behaves exactly as before.
      --
      if self.on_open then
        local now = sys.ticks()

        if n == self.opened_row and (now - (self.opened_at or 0)) < ui_again() then
          self.opened_row = nil
          self.on_open(self, self.items[n], n)
        else
          self.opened_row, self.opened_at = n, now
        end
      end
    end

    return true
  end

  return v
end

--
-- Editable text, over several lines.
--
--   ui.editor{ x =, y =, w =, h =, text = "..." }
--
-- An array of lines and a cursor, which is what a text editor is until it
-- is a good one. No undo, no selection, no syntax colouring: each is worth
-- having and none is worth delaying the thing that lets the machine change
-- itself without a rebuild.
--
-- The full-screen `edit` came first and is still there; this is the same
-- idea as a view, so it can sit in a window beside anything else. What it
-- gains by being a widget is that the window manager owns its pixels, so
-- an editor that hangs is a window you can still move.
--
function ui.editor(spec)
  local v = ui.view(spec)

  v.focusable = true
  v.read_only = spec.read_only or false
  v.lines = {}
  v.cy = 1
  v.cx = 1
  v.top = 1
  v.dirty = false

  --
  -- Where a selection began, as `{y, x}`, or nil for none.
  --
  -- **A selection is two carets, and the second one is the cursor.** That
  -- is the whole model: no separate "selecting" flag, no start-and-length,
  -- and no third state where a selection exists but the caret is somewhere
  -- else. Dragging moves the cursor and leaves the anchor; every key that
  -- moves the cursor on its own drops the anchor, which is why clicking or
  -- arrowing deselects without anything having to say so.
  --
  -- Nothing here reads the shift key, because there is no shift key to
  -- read: keys arrive as a byte stream with the arrows decoded out of
  -- `ESC [ A`-`D`, and shift-plus-arrow produces the same four bytes as an
  -- arrow. So selection is made with the pointer or with select-all, which
  -- is what the two of them can express.
  --
  v.anchor = nil

  for line in ((spec.text or "") .. "\n"):gmatch("([^\n]*)\n") do
    v.lines[#v.lines + 1] = line
  end

  if #v.lines > 1 and v.lines[#v.lines] == "" then
    v.lines[#v.lines] = nil
  end

  if #v.lines == 0 then v.lines[1] = "" end

  --
  -- Four digits and a space, unless the caller says not to.
  --
  -- Numbers are what an editor showing a *file* wants - a compiler names a
  -- line and you go to it. A pane showing a report is the other case: the
  -- numbers are noise, and they push the text right by five columns for
  -- nothing. `gutter = false` is for that, and `machine` is the first
  -- caller of it.
  --
  local GUTTER = (spec.gutter == false) and 0 or 5

  --
  -- **How far in the text starts**, 2 inside a well by default. `plain`
  -- is the drawings' page of text (`docs/apps.html`'s Editor): no well and
  -- no ring, just the text on the sunken colour from edge to edge, with
  -- `inset` of room around it - a window whose whole body is the document
  -- has no use for a box drawn around the document.
  --
  local IN_X = spec.inset and spec.inset[1] or 2
  local IN_Y = spec.inset and spec.inset[2] or 2

  function v:content()
    return table.concat(self.lines, "\n") .. "\n"
  end

  -- The other direction, so an editor can be given a different file without
  -- being rebuilt. Splitting is the same three lines the constructor uses,
  -- and having them in one place is why this is a method rather than
  -- something every caller writes out.
  function v:set(body)
    self.lines = {}

    for line in (tostring(body or "") .. "\n"):gmatch("([^\n]*)\n") do
      self.lines[#self.lines + 1] = line
    end

    if #self.lines > 1 and self.lines[#self.lines] == "" then
      self.lines[#self.lines] = nil
    end

    if #self.lines == 0 then self.lines[1] = "" end

    -- Back to the top, because the cursor was somewhere in a file that is
    -- no longer open and would otherwise sit past the end of this one.
    self.cy, self.cx, self.top = 1, 1, 1
    self.dirty = false
  end

  --
  -- The selection in reading order, or nil when there is none.
  --
  -- Anchor and cursor can be either way round - dragging upwards is as
  -- ordinary as dragging down - so every reader wants them sorted, and
  -- sorting them in one place is why this exists. An anchor sitting exactly
  -- on the cursor is *not* a selection: that is what a plain click leaves
  -- behind, and reporting it as an empty one would make copy send nothing
  -- rather than do nothing.
  --
  local function selection(self)
    local a = self.anchor

    if not a then return nil end

    local y1, x1, y2, x2 = a[1], a[2], self.cy, self.cx

    if y1 > y2 or (y1 == y2 and x1 > x2) then
      y1, x1, y2, x2 = y2, x2, y1, x1
    end

    if y1 == y2 and x1 == x2 then return nil end

    return y1, x1, y2, x2
  end

  --
  -- Which characters of line `n` the selection covers, as an inclusive
  -- pair, plus whether the newline at the end of it is in there too.
  --
  -- The newline matters for drawing and only for drawing: a selection that
  -- runs through three lines should not look like three separate
  -- selections, so a line whose end is inside the range gets one extra
  -- highlighted cell standing for the break. Nothing about the text itself
  -- depends on it.
  --
  local function span(self, n)
    local y1, x1, y2, x2 = selection(self)

    if not y1 or n < y1 or n > y2 then return nil end

    local from = (n == y1) and x1 or 1
    local to   = (n == y2) and (x2 - 1) or #self.lines[n]

    return from, to, n < y2
  end

  --
  -- Where you land `n` bytes after `(y, x)`, counting a line break as one.
  --
  -- Only the clipboard needs this, and it needs it because the clipboard
  -- has a size limit and the limit has to be *shown* rather than
  -- mentioned. Counting the break is what makes the answer agree with what
  -- `v:selected` produced, which joined its lines with one.
  --
  local function advance(self, y, x, n)
    while true do
      local room = #self.lines[y] - (x - 1)

      if n <= room then return y, x + n end

      n = n - room - 1

      if n < 0 or y >= #self.lines then
        return y, #self.lines[y] + 1
      end

      y, x = y + 1, 1
    end
  end

  --
  -- The selected text, or nil when nothing is selected.
  --
  -- Joined with "\n" and never with a trailing one, because a selection is
  -- a run of characters rather than a set of lines - copying the middle of
  -- a paragraph and pasting it should not introduce a break that was not
  -- in it.
  --
  function v:selected()
    local y1, x1, y2, x2 = selection(self)

    if not y1 then return nil end

    if y1 == y2 then
      return self.lines[y1]:sub(x1, x2 - 1)
    end

    local out = { self.lines[y1]:sub(x1) }

    for n = y1 + 1, y2 - 1 do
      out[#out + 1] = self.lines[n]
    end

    out[#out + 1] = self.lines[y2]:sub(1, x2 - 1)

    return table.concat(out, "\n")
  end

  --
  -- Take it out, leaving the caret where it was.
  --
  -- The two ends join into one line, which is what makes a multi-line
  -- delete a single edit rather than a loop that has to get its indices
  -- right as the table shrinks underneath it.
  --
  function v:delete_selected()
    local y1, x1, y2, x2 = selection(self)

    if not y1 or self.read_only then return false end

    self.lines[y1] = self.lines[y1]:sub(1, x1 - 1)
                     .. self.lines[y2]:sub(x2)

    for _ = y1 + 1, y2 do
      table.remove(self.lines, y1 + 1)
    end

    self.cy, self.cx = y1, x1
    self.anchor = nil
    self.dirty = true

    return true
  end

  --
  -- Put text in at the caret, however many lines it is.
  --
  -- One path for a word and for a paragraph, because a paste is a paste.
  -- The caret lands after what was inserted, which is where the next thing
  -- typed belongs.
  --
  function v:insert(text)
    if self.read_only then return false end

    local parts = {}

    for part in (tostring(text or "") .. "\n"):gmatch("([^\n]*)\n") do
      parts[#parts + 1] = part
    end

    if #parts == 0 then return false end

    local line = self.lines[self.cy]
    local head, tail = line:sub(1, self.cx - 1), line:sub(self.cx)

    if #parts == 1 then
      self.lines[self.cy] = head .. parts[1] .. tail
      self.cx = self.cx + #parts[1]
    else
      self.lines[self.cy] = head .. parts[1]

      for i = 2, #parts do
        table.insert(self.lines, self.cy + i - 1, parts[i])
      end

      self.cy = self.cy + #parts - 1
      self.cx = #parts[#parts] + 1
      self.lines[self.cy] = self.lines[self.cy] .. tail
    end

    self.dirty = true

    return true
  end

  --
  -- Select-all, copy, cut and paste, arriving from the window manager's
  -- prefix by way of `window:dispatch_edit`.
  --
  -- **A read-only pane answers copy and select-all and refuses the other
  -- two**, which is the useful half and exactly the half that makes sense:
  -- a report of what the machine is should be something you can take away,
  -- and nothing you can edit. Refusing by returning false rather than by
  -- pretending means the window falls back to its own `on_edit` and can do
  -- something else with the keystroke if it wants to.
  --
  function v:edit(kind)
    if kind == "selectall" then
      self.anchor = { 1, 1 }
      self.cy = #self.lines
      self.cx = #self.lines[self.cy] + 1
      return true
    end

    if kind == "copy" or kind == "cut" then
      local text = self:selected()

      if not text then return false end

      local bytes, dropped = wmproto.copy(text)

      if not bytes then return false end

      --
      -- More was selected than a clipboard holds, so the highlight moves
      -- back to what actually left.
      --
      -- **The screen says it rather than a message box.** A selection is
      -- already a picture of a range of text; shrinking it to the range
      -- that was copied makes the limit something you can see, in the one
      -- place you were already looking. A dialog saying "1900 of 4212
      -- bytes" would be the same fact, later, and in the way.
      --
      -- A cut then removes exactly the shortened run, which is why this
      -- happens before the delete rather than after it.
      --
      if dropped > 0 then
        local y1, x1 = selection(self)

        self.anchor = { y1, x1 }
        self.cy, self.cx = advance(self, y1, x1, bytes)
      end

      if kind == "cut" then return self:delete_selected() end

      return true
    end

    if kind == "paste" then
      if self.read_only then return false end

      local text = wmproto.paste()

      if not text or text == "" then return false end

      self:delete_selected()

      return self:insert(text)
    end

    return false
  end

  --
  -- The monospace cell, read fresh each time rather than captured.
  --
  -- This widget numbers its lines in a gutter, puts a block caret *on* a
  -- character and clips to a column count. All three are arithmetic on a
  -- cell that is the same width for every glyph, so in a proportional face
  -- none of it lands: the caret sits beside the character it is on and the
  -- gutter walks away from the text. An editor is monospace by
  -- construction, not by preference.
  --
  -- `GW`/`GH` are the *interface* font's and answer for that one only,
  -- which is the same thing the terminal had wrong. Read per repaint
  -- because the font is a setting and a window open while it changes has
  -- to follow it.
  --
  local function cell()
    return math.max(1, gfx.measure("0", "mono")), gfx.height("mono")
  end

  local function rows(self)
    local _, ch = cell()

    return (self.h - 2 * IN_Y) // ch
  end

  local function scroll_into_view(self)
    if self.cy < self.top then self.top = self.cy end

    if self.cy > self.top + rows(self) - 1 then
      self.top = self.cy - rows(self) + 1
    end

    if self.top < 1 then self.top = 1 end
  end

  local function clamp(self)
    if self.cy < 1 then self.cy = 1 end
    if self.cy > #self.lines then self.cy = #self.lines end
    if self.cx < 1 then self.cx = 1 end
    if self.cx > #self.lines[self.cy] + 1 then
      self.cx = #self.lines[self.cy] + 1
    end
  end

  function v:draw(g)
    local GW, GH = cell()

    scroll_into_view(self)

    -- A well: content lives in here, and the bevel says so. The focus
    -- ring goes inside it rather than over it, so a focused field is
    -- still visibly a field.
    if self.plain then
      g:fill(0, 0, self.w, self.h, theme.sunken)
    else
      g:sunken(0, 0, self.w, self.h, "sunken")

      if self.focused then
        g:frame(1, 1, self.w - 2, self.h - 2, "ring")
      end
    end

    local columns = (self.w - 2 * IN_X) // GW - GUTTER

    for row = 0, rows(self) - 1 do
      local n = self.top + row
      local line = self.lines[n]

      if line then
        local y = IN_Y + row * GH
        local x0 = IN_X + GUTTER * GW

        if GUTTER > 0 then
          g:text(IN_X, y, ("%4d "):format(n), theme.line, theme.sunken,
                 "mono")
        end

        local vis = line:sub(1, columns)
        local from, to, eol = span(self, n)

        if not from then
          g:text(x0, y, vis, theme.text, theme.sunken, "mono")
        else
          --
          -- Three pieces, and the middle one is the caret's own colours.
          --
          -- **A selection is a widened cursor**, so it is drawn as one: the
          -- same `ring` ground and `sunken` text the block caret below
          -- uses. That is not a shortcut - it is the reason no new theme
          -- token appears here. Every palette has already had to make those
          -- two readable against each other, because a caret sits on a
          -- character in all of them.
          --
          if from > columns then from = columns + 1 end
          if to > columns then to = columns end

          g:text(x0, y, vis:sub(1, from - 1), theme.text, theme.sunken,
                 "mono")

          if to >= from then
            g:text(x0 + (from - 1) * GW, y, vis:sub(from, to), theme.sunken,
                   theme.ring, "mono")
          end

          g:text(x0 + to * GW, y, vis:sub(to + 1), theme.text, theme.sunken,
                 "mono")

          -- The line break, so a run through several lines reads as one
          -- shape rather than as a ragged stack.
          if eol and to < columns then
            g:fill(x0 + to * GW, y, GW, GH, theme.ring)
          end
        end
      end
    end

    -- The cursor as a block on the character it is on, which is what makes
    -- the column obvious in indented code.
    if self.focused and not selection(self) and self.cy >= self.top
       and self.cy <= self.top + rows(self) - 1 then
      local px = IN_X + (GUTTER + math.min(self.cx, columns + 1) - 1) * GW
      local py = IN_Y + (self.cy - self.top) * GH
      local under = self.lines[self.cy]:sub(self.cx, self.cx)

      g:fill(px, py, GW, GH, theme.ring)

      if under ~= "" then
        g:text(px, py, under, theme.sunken, theme.ring, "mono")
      end
    end
  end

  function v:key(c)
    --
    -- A key that moves the caret drops the selection, and a key that
    -- changes text replaces it. Both before anything else looks at the
    -- line, because a delete moves the caret and the line under it.
    --
    local moving = (c == -1 or c == -2 or c == -3 or c == -4
                    or c == 1 or c == 5)
    local typing = (c == 10 or c == 13 or c >= 32)
    local erasing = (c == 8 or c == 127)

    if self.anchor and not self.read_only then
      if typing or erasing then
        local had = self:delete_selected()

        -- Backspace and Delete are *done* once the selection is gone -
        -- taking a further character would eat one nobody selected.
        if had and erasing then return true end
      end
    end

    if moving or typing or erasing then self.anchor = nil end

    local line = self.lines[self.cy]

    --
    -- **A read-only editor is still an editor**, and that is the point: it
    -- scrolls, it has a caret you can put on a character, and the text is
    -- text rather than a laid-out picture of text. What it will not do is
    -- change under a keystroke meant to navigate it - which for a pane
    -- reporting what the machine *is* would be a lie the moment somebody
    -- leaned on the keyboard.
    --
    -- Movement is handled below either way; only the keys that would
    -- insert, split or delete are refused here.
    --
    local editing = (c == 10 or c == 13 or c == 8 or c == 127 or c >= 32)

    if self.read_only and editing then
      return true                       -- swallowed, so the window keeps it
    end

    if c == -1 then self.cy = self.cy - 1; clamp(self); return true end
    if c == -2 then self.cy = self.cy + 1; clamp(self); return true end

    if c == -4 then                                     -- left
      if self.cx == 1 then
        if self.cy > 1 then
          self.cy = self.cy - 1
          self.cx = #self.lines[self.cy] + 1
        end
      else
        self.cx = self.cx - 1
      end
      return true
    end

    if c == -3 then                                     -- right
      if self.cx > #line then
        if self.cy < #self.lines then
          self.cy = self.cy + 1
          self.cx = 1
        end
      else
        self.cx = self.cx + 1
      end
      return true
    end

    if c == 1 then self.cx = 1 return true end          -- ^A
    if c == 5 then self.cx = #line + 1 return true end  -- ^E

    if c == 10 or c == 13 then                          -- Enter
      local rest = line:sub(self.cx)

      self.lines[self.cy] = line:sub(1, self.cx - 1)
      table.insert(self.lines, self.cy + 1, rest)
      self.cy = self.cy + 1
      self.cx = 1
      self.dirty = true
      return true
    end

    if c == 8 or c == 127 then                          -- Backspace
      if self.cx > 1 then
        self.lines[self.cy] = line:sub(1, self.cx - 2) .. line:sub(self.cx)
        self.cx = self.cx - 1
        self.dirty = true
      elseif self.cy > 1 then
        -- Joining onto the end of the line above, which is where the
        -- cursor has to land or the join is invisible.
        local above = self.lines[self.cy - 1]

        self.cx = #above + 1
        self.lines[self.cy - 1] = above .. line
        table.remove(self.lines, self.cy)
        self.cy = self.cy - 1
        self.dirty = true
      end
      return true
    end

    if c == 9 then c = 32 end                           -- Tab: a space

    if c >= 32 and c < 127 then
      self.lines[self.cy] = line:sub(1, self.cx - 1) .. string.char(c)
                            .. line:sub(self.cx)
      self.cx = self.cx + 1
      self.dirty = true
      return true
    end

    return false
  end

  --
  -- Clicking puts the cursor where the click was, clamped to the end of
  -- that line - which is what every editor does and what nobody notices
  -- until it does not.
  --
  function v:mouse(action, x, y)
    -- The same cell the drawing used, or a click lands on a different
    -- character from the one under the pointer. That disagreement is the
    -- whole reason `cell()` is a function rather than two captured numbers.
    local GW, GH = cell()

    if action == "press" or action == "move" then
      local row = (y - IN_Y) // GH

      self.cy = self.top + row
      clamp(self)

      local col = (x - IN_X) // GW - GUTTER

      if col < 0 then col = 0 end
      self.cx = math.min(col + 1, #self.lines[self.cy] + 1)

      --
      -- The press is the anchor; the drag is the cursor.
      --
      -- Which is why this is two lines rather than a selecting flag: the
      -- window holds the grab from press to release - see `dispatch_mouse`
      -- - so a `move` reaching this widget at all *means* the button is
      -- down, and there is nothing to remember. A press that goes nowhere
      -- leaves anchor and cursor equal, which `selection` reports as no
      -- selection, so a plain click deselects for free.
      --
      if action == "press" then
        self.anchor = { self.cy, self.cx }
      end
    end

    return true
  end

  return v
end

--
-- A picture.
--
--   ui.image{ x =, y =, w =, h =, asset = "test-pattern.png" }
--   ui.image{ x =, y =, w =, h =, asset = "/home/holiday.png" }
--
-- The picture is *named*, not carried. A decoded image is megabytes and a
-- message is two kilobytes, so the window manager loads it and this asks
-- how big it is - which is the same division as everything else here: the
-- compositor owns pixels, an application says what it wants drawn.
--
-- **A leading slash means a file rather than something compiled into the
-- image**, and that is the whole of the difference from this side. The
-- widget did not change to gain it and neither did the message: where the
-- compositor looks for a name was never the application's business, which
-- is exactly why it could be extended without touching a call site.
--
-- Bigger than its box is normal - a photograph is - so it pans rather than
-- scaling. **The primitive that would change that exists** since 15
-- September: `s:stretch`, nearest neighbour, in C where the per-pixel loop
-- belongs. Drawing through it here is a change to this widget - and to what
-- the `image` op carries - rather than a gap in the system.
--
function ui.image(spec)
  local v = ui.view(spec)

  v.focusable = true
  v.ox = 0
  v.oy = 0

  local iw, ih = 0, 0

  --
  -- Which picture this shows, changed after the fact.
  --
  -- The size has to be asked for rather than known, because the picture
  -- lives in the compositor - so a widget that could only be given one at
  -- construction meant an application that could only ever show one, which
  -- is what Photo was. Panning is reset with it: the corner of the last
  -- picture is not a place in this one.
  --
  function v:set(asset)
    self.asset = asset
    self.ox, self.oy = 0, 0

    local size = asset and fs.send("/app/wm", { type = "image_size",
                                               asset = asset })

    iw, ih = size and size.w or 0, size and size.h or 0
    self.image_w, self.image_h = iw, ih

    return iw > 0
  end

  v:set(v.asset)

  local function clamp(self)
    local most_x = iw - self.w
    local most_y = ih - self.h

    if most_x < 0 then most_x = 0 end
    if most_y < 0 then most_y = 0 end

    if self.ox > most_x then self.ox = most_x end
    if self.oy > most_y then self.oy = most_y end
    if self.ox < 0 then self.ox = 0 end
    if self.oy < 0 then self.oy = 0 end
  end

  --
  -- `ground` is a colour to fill behind the picture instead of the sunken
  -- well, and `centre` puts a picture smaller than the widget in its middle
  -- rather than its top-left corner: `docs/apps.html`'s Photo, a picture on
  -- a dark canvas, which is how every viewer shows one.
  --
  function v:draw(g)
    if self.ground then
      g:fill(0, 0, self.w, self.h, self.ground)
    else
      g:sunken(0, 0, self.w, self.h, "sunken")
    end

    if self.image_w == 0 then
      g:text(6, 6, "no picture called " .. tostring(self.asset), theme.bad)
      g:text(6, 6 + gfx.font.h + 4,
             "PNG or JPEG, and it has to be one", theme.text_dim)
      return
    end

    --
    -- Straight to the window manager as one command, with the source
    -- rectangle in it: a picture is one blit however big it is, and
    -- chopping it into pieces here would put every piece through a message.
    --
    -- Clipped by hand rather than by the graphics context, because the
    -- context clips *commands* and this one carries its own source
    -- rectangle - so the thing to clip is where it reads from.
    --
    local ax, ay = g.ox, g.oy
    local x0 = (ax > g.cx) and ax or g.cx
    local y0 = (ay > g.cy) and ay or g.cy
    local x1 = math.min(ax + self.w, g.cx + g.cw)
    local y1 = math.min(ay + self.h, g.cy + g.ch)

    if x1 <= x0 or y1 <= y0 then return end

    --
    -- **`fit` draws the whole picture at the widget's size**, through the
    -- compositor's scaler, instead of showing as much of it as fits. A
    -- cover is five hundred pixels and Music's design draws it at 78 and at
    -- 44; panning is right for a photograph and wrong for a sleeve.
    --
    -- The clip is the same rectangle either way, and what changes is what
    -- the source rectangle means: the whole picture, drawn into whatever
    -- part of the widget is still on screen. Scaling it by the clipped
    -- fraction would be the same mistake `stretch` refuses to make in C -
    -- an edge cut by a border is not a whole pixel of the source.
    --
    --
    -- **`contain` shrinks a picture larger than the widget until the whole
    -- of it fits, keeping its shape**, and centres it - a photograph viewer's
    -- first view of a picture, which is the whole picture. Smaller ones are
    -- left at their own size for `centre` below: enlarging a small picture
    -- only shows its pixels.
    --
    local cw, ch = self.image_w, self.image_h

    if self.contain and cw > 0 and ch > 0 and (cw > self.w or ch > self.h) then
      local dw, dh

      if cw * self.h > ch * self.w then
        dw, dh = self.w, math.max(1, ch * self.w // cw)
      else
        dw, dh = math.max(1, cw * self.h // ch), self.h
      end

      g.ops[#g.ops + 1] = {
        op = "image", asset = self.asset,
        sx = 0, sy = 0, w = cw, h = ch,
        x = ax + (self.w - dw) // 2, y = ay + (self.h - dh) // 2,
        dw = dw, dh = dh, alpha = self.alpha,
      }

      return
    end

    if self.fit and iw > 0 and ih > 0 then
      g.ops[#g.ops + 1] = {
        op = "image", asset = self.asset,
        sx = 0, sy = 0, w = iw, h = ih,
        x = ax, y = ay, dw = self.w, dh = self.h,
        alpha = self.alpha,
      }

      return
    end

    --
    -- Where the picture's own top-left corner is on the screen, and then
    -- the part of it that is inside the clip: the source rectangle is that
    -- part, measured from the corner. Panned, the corner is up and to the
    -- left of the widget; centred, it is inside it.
    --
    local iw_, ih_ = self.image_w, self.image_h
    local px, py = ax - self.ox, ay - self.oy

    if self.centre then
      if iw_ < self.w then px = ax + (self.w - iw_) // 2 end
      if ih_ < self.h then py = ay + (self.h - ih_) // 2 end
    end

    local vx0, vy0 = math.max(x0, px), math.max(y0, py)
    local vx1, vy1 = math.min(x1, px + iw_), math.min(y1, py + ih_)

    if vx1 <= vx0 or vy1 <= vy0 then return end

    g.ops[#g.ops + 1] = {
      op = "image", asset = self.asset,
      sx = vx0 - px, sy = vy0 - py,
      w = vx1 - vx0, h = vy1 - vy0,
      x = vx0, y = vy0,
    }
  end

  function v:mouse(action, x, y)
    if action == "press" then
      self.from_x, self.from_y = x, y
      self.was_x, self.was_y = self.ox, self.oy
    elseif action == "move" and self.from_x then
      self.ox = self.was_x - (x - self.from_x)
      self.oy = self.was_y - (y - self.from_y)
      clamp(self)
    end

    return true
  end

  function v:key(c)
    local step = 32

    if c == -1 then self.oy = self.oy - step; clamp(self); return true end
    if c == -2 then self.oy = self.oy + step; clamp(self); return true end
    if c == -3 then self.ox = self.ox + step; clamp(self); return true end
    if c == -4 then self.ox = self.ox - step; clamp(self); return true end

    return false
  end

  return v
end

--
-- A block of text that wraps, in a few styles.
--
--   ui.text{ x =, y =, w =, h =, blocks = {
--     { style = "title", text = "Kosmos" },
--     { style = "body",  text = "a long paragraph ..." },
--   } }
--
-- Styles rather than a markup language, and a short list of them rather
-- than a general one: this exists because two applications wanted a
-- paragraph that fits its box, and the moment it grows a parser it stops
-- being a widget and becomes a document viewer. That is a different thing
-- and it can be built on this.
--
-- Wrapping is on words, and a word longer than the line is broken rather
-- than allowed to run off the edge - which the clip would hide, so the
-- symptom would be text silently missing rather than text that looks
-- wrong.
--
function ui.text(spec)
  local v = ui.view(spec)

  v.blocks = v.blocks or {}
  v.scroll = 0
  v.content = 0
  v.focusable = true

  local STYLES = {
    title  = { colour = theme.text,     gap = 6, under = true },
    head   = { colour = theme.text,     gap = 6 },
    body   = { colour = theme.text_dim, gap = 2 },
    accent = { colour = theme.good,     gap = 2 },
  }

  -- One string into as many lines as it takes.
  local function wrap(text, columns)
    local lines = {}
    local line = ""

    for word in tostring(text):gmatch("%S+") do
      while #word > columns do
        if line ~= "" then lines[#lines + 1] = line; line = "" end
        lines[#lines + 1] = word:sub(1, columns)
        word = word:sub(columns + 1)
      end

      if line == "" then
        line = word
      elseif #line + 1 + #word <= columns then
        line = line .. " " .. word
      else
        lines[#lines + 1] = line
        line = word
      end
    end

    if line ~= "" then lines[#lines + 1] = line end
    if #lines == 0 then lines[1] = "" end

    return lines
  end

  --
  -- Where the scroll is allowed to be, given how tall the text turned out.
  --
  -- `content` is measured *by drawing*, so it describes the last pass -
  -- which is why this is called at the top of the next one rather than
  -- pretending to know the height in advance. It is scroll-independent:
  -- `draw` records `y + self.scroll`, which is the total height and not
  -- what happened to be on screen.
  --
  local function clamp(self)
    local most = (self.content or 0) - self.h + 8

    if most < 0 then most = 0 end
    if self.scroll > most then self.scroll = most end
    if self.scroll < 0 then self.scroll = 0 end
  end

  function v:draw(g)
    --
    -- **Clamped here, and not only when a key or the pointer moved it.**
    --
    -- Scrolling used to be clamped by the two handlers that scroll, which
    -- is every path but the one that mattered: a program setting `scroll`
    -- directly. `logview` did - it asked for a position past the end on
    -- every refresh so the newest line would stay in view, with a comment
    -- saying the widget "clamps an over-large scroll to the real bottom on
    -- the way past". Nothing did. The view scrolled a billion pixels down,
    -- every line fell outside it, and that window drew nothing at all on
    -- every machine it had run on - while its own status line truthfully
    -- counted the eighty-eight lines it was not showing.
    --
    -- So the widget clamps here - **against the height the previous draw
    -- measured**, which is all `content` is. A caller asking for the bottom
    -- this way therefore lands one draw behind whatever it has just added,
    -- and on the first draw, before anything has been measured, at the top.
    -- Log View did exactly that: it opened at the top of the log and stayed
    -- there until the log changed. It follows the log itself now, and a
    -- widget that wanted to stick to the bottom would have to measure before
    -- it clamps rather than after.
    --
    clamp(self)

    local columns = (self.w - 8) // GW

    if columns < 1 then return end

    local y = 4 - self.scroll

    for _, b in ipairs(self.blocks) do
      local style = STYLES[b.style or "body"] or STYLES.body

      for _, line in ipairs(wrap(b.text, columns)) do
        -- Only what is inside. The clip would hide the rest anyway; not
        -- emitting it keeps a long document from becoming a long message.
        if y > -GH and y < self.h then
          g:text(4, y, line, style.colour)
        end

        y = y + GH
      end

      if style.under then
        if y > -2 and y < self.h then
          g:fill(4, y - 2, self.w - 8, 1, theme.line)
        end
        y = y + 4
      end

      y = y + style.gap
    end

    -- How tall the whole thing is, discovered by drawing it. Kept so that
    -- scrolling can stop at the bottom rather than running off into
    -- nothing, which is what an unclamped scroll does and what makes a
    -- document look like it has been lost.
    self.content = y + self.scroll
  end

  --
  -- Scrolling. Arrows a line, page keys a screen, and dragging inside it
  -- moves with the pointer - which is what a document does everywhere and
  -- is three lines here because the pointer already grabs.
  --

  function v:key(c)
    if c == -1 then self.scroll = self.scroll - GH; clamp(self); return true end
    if c == -2 then self.scroll = self.scroll + GH; clamp(self); return true end

    -- Space and backspace, which is how every document reader has paged
    -- since before there were mice.
    if c == 32 then
      self.scroll = self.scroll + self.h - GH
      clamp(self)
      return true
    end

    if c == 8 or c == 127 then
      self.scroll = self.scroll - self.h + GH
      clamp(self)
      return true
    end

    return false
  end

  function v:mouse(action, x, y)
    if action == "press" then
      self.drag_from = y
      self.drag_scroll = self.scroll
    elseif action == "move" and self.drag_from then
      self.scroll = self.drag_scroll + (self.drag_from - y)
      clamp(self)
    end

    return true
  end

  return v
end

--------------------------------------------------------------------------
-- Replicants.
--
-- `ui.md` 16.8, and the most BeOS thing here. In BeOS you could drag a view
-- out of one application and drop it into another and it kept working - a
-- clock, a CPU meter, a mini player. It was implemented with `BArchivable`
-- and by loading a binary add-on into the destination process, which was
-- fragile and an enormous attack surface: the thing you dropped was native
-- code with the full run of its host.
--
-- Here a view is Lua source plus a state table plus a list of what it needs,
-- and all three are ordinary values that cross a boundary the way any value
-- does:
--
--   { source = "...the view's code...",
--     state  = { format = "24h" },
--     needs  = { "/dev/cpu" } }
--
-- The host loads the source into an environment built from `needs` and
-- nothing else. A replicant that asked for /dev/cpu cannot read your files -
-- not because it is checked when it tries, but because there is no name in
-- its world that reaches them.
--
-- **The honest limit.** A replicant runs *inside* its host's process, so
-- this is a restriction in the language and not one the kernel enforces:
-- the address space is the host's, and Lua is what stands between them. It
-- is strictly more than BeOS offered - which was nothing - and strictly less
-- than a separate process would be. Something that needs the stronger
-- guarantee should be an application in a window, which is a different
-- thing wanting a different mechanism.
--------------------------------------------------------------------------

--
-- A namespace with exactly the paths `needs` asked for.
--
-- Prefix matching, so "/dev/cpu" grants that node and anything under it and
-- nothing beside it: "/dev/cpuboard" does not match, because the test is on
-- a path component and not on a string.
--
function ui.restricted(needs)
  local allowed = {}

  for _, path in ipairs(needs or {}) do
    allowed[#allowed + 1] = path
  end

  local function permitted(path)
    for _, prefix in ipairs(allowed) do
      if path == prefix or path:sub(1, #prefix + 1) == prefix .. "/" then
        return true
      end
    end
    return false
  end

  local function guard(fn)
    return function(path, ...)
      if not permitted(path) then
        -- The same sentence the namespace itself gives, and for the same
        -- reason: nothing was denied, there is simply no such path here.
        return nil, "no such path: " .. tostring(path)
      end
      return fn(path, ...)
    end
  end

  return {
    read    = guard(fs.read),
    list    = guard(fs.list),
    getattr = guard(fs.getattr),
    query   = guard(fs.query),
  }
end

--
-- A view built from one of those descriptions.
--
-- The source must return a factory: a function taking the state table and
-- returning something with a `draw(self, gc)`. Optionally a `tick(self)`,
-- called once a pass, which is how a clock is a clock.
--
function ui.replicant(spec)
  local v = ui.view{ x = spec.x or 0, y = spec.y or 0,
                     w = spec.w or 160, h = spec.h or 40 }

  --
  -- What a replicant may see. Deliberately small and deliberately explicit:
  -- adding to this list is granting something to every replicant that will
  -- ever run, so it is a list and not a metatable onto _G.
  --
  -- **`measure` and `height` were granted on 20 September**, deliberately
  -- and for a reason rather than for convenience: a replicant that can only
  -- see `gfx.font` can only lay text out by counting cells, which is wrong
  -- for every face that is not the bitmap - the clock centred itself with
  -- `#text * gfx.font.w` and drifted the moment the desktop took IBM Plex.
  -- Changing that line to measure broke every replicant at once, because
  -- the function was not in here, and the display suite caught it.
  --
  -- Both are pure: they read the faces this process has already loaded and
  -- tell you how wide or tall a string would be. Nothing is drawn, nothing
  -- is changed, and a replicant already draws through the host's `gc` with
  -- the host's faces - so this grants an *answer* about what is about to
  -- happen anyway.
  --
  local env = {
    gfx = { font = gfx.font, measure = gfx.measure, height = gfx.height },
    fs = ui.restricted(spec.needs),
    ticks = sys.ticks,
    theme = theme,

    math = math, string = string, table = table,
    tostring = tostring, tonumber = tonumber,
    ipairs = ipairs, pairs = pairs, select = select,
    type = type, error = error, pcall = pcall,
  }

  local chunk, err = load(spec.source, "=replicant", "t", env)

  if not chunk then
    return nil, "replicant: " .. tostring(err)
  end

  local ok, factory = pcall(chunk)

  if not ok or type(factory) ~= "function" then
    return nil, "replicant: the source did not return a factory"
  end

  local made
  ok, made = pcall(factory, spec.state or {})

  if not ok or type(made) ~= "table" or type(made.draw) ~= "function" then
    return nil, "replicant: the factory did not produce a view"
  end

  v.instance = made

  function v:draw(g)
    -- Its failure is its own. A replicant that raises stops drawing and
    -- leaves everything around it alone, which is the property that makes
    -- dropping a stranger's view into your window a reasonable thing to do.
    local drew, why = pcall(made.draw, made, g, self.w, self.h)

    if not drew then
      g:fill(0, 0, self.w, self.h, theme.sunken)
      g:text(2, 2, "replicant: " .. tostring(why):sub(1, 40), theme.bad)
    end
  end

  function v:tick()
    if made.tick then pcall(made.tick, made) end
  end

  return v
end

--------------------------------------------------------------------------
-- A window: the root view, the conversation with the window manager, and
-- the properties it publishes.
--
-- The publishing is the part worth explaining. `roadmap.md` M7 asks that an
-- application be manipulable from the shell "without its author having done
-- anything", and this is where that is paid for: a window registers itself
-- with /app and answers reads and writes for its own properties. An
-- application that calls `ui.window` is scriptable; one that does not, is
-- not. Nobody writes scripting code either way.
--
-- In BeOS this was the same bargain and it worked for the same reason - an
-- application was scriptable because its author used BApplication, not
-- because they supported scripting. The difference here is that the
-- properties are a namespace rather than a message hierarchy, so the shell
-- needs no special verb: `cat /app/gallery/title` is the ordinary read.
--------------------------------------------------------------------------

local window = {}
window.__index = window

--
-- How much of a message a batch of commands may fill.
--
-- **By size, not by count.** This was sixteen commands per message, which
-- is fine for a window of buttons and wrong for a window of text: sixteen
-- `text` commands carrying seventy-character log lines is over two
-- kilobytes, the send raises, and the application dies mid-repaint. What
-- that looks like is a window that draws once and then stops, with no error
-- anywhere - the compositor owns the pixels, so the window stays exactly as
-- it was.
--
-- 1400 of the 2048 leaves room for the message's own keys and the
-- serialiser's framing. The estimate below is deliberately generous for the
-- same reason: being wrong in the cheap direction costs an extra message,
-- and being wrong in the other direction costs the application.
--
local BATCH_BYTES = 1200

local function op_cost(o)
  --
  -- Deliberately generous. A `text` command carries five keys and their
  -- values - the verb, two coordinates, a colour and usually a background -
  -- and every one of them costs a name and a tag in the message as well as
  -- its own bytes. Guessing low here does not cost a message, it costs the
  -- application: the send raises and the window stops repainting, with the
  -- pixels still on screen because the compositor owns them.
  --
  local cost = 96

  if o.s then cost = cost + #o.s end

  -- An `image` carries an asset name instead of a string, and a window full
  -- of icons is a hundred of them. Not counting it is how a batch goes over
  -- 2048 bytes and the whole frame silently fails to draw.
  if o.asset then cost = cost + #tostring(o.asset) end
  if o.role then cost = cost + #tostring(o.role) end

  -- A size is one more key and its number, and the generous base above is
  -- not a licence to stop counting: this is what the send would raise on.
  if o.px then cost = cost + 16 end
  if o.dw then cost = cost + 32 end

  -- Six coordinates and their names, which is the widest command here.
  if o.x1 then cost = cost + 96 end

  return cost
end

--
-- A list of drawing commands, sent to a window in message-sized pieces.
--
-- A message is 2048 bytes. Three menu items already come close and seven go
-- over, so a menu that sent its commands in one go drew *nothing at all*
-- past a certain length - the send raised, the compositor kept the pixels it
-- had, and what appeared was an empty panel with no error anywhere. The
-- window painter has always batched; the menu painter did not, and this is
-- that code in one place so there is no second one to forget.
--
-- `more` holds the damage back until the last piece, so a window is never
-- composited half-drawn.
--
local function send_ops(handle, ops)
  local at = 1

  while at <= #ops do
    local batch, bytes = {}, 0

    while at <= #ops do
      local cost = op_cost(ops[at])

      -- Always at least one, so a single enormous command is sent on its
      -- own and refused by the serialiser with its own message rather than
      -- looping here for ever.
      if #batch > 0 and bytes + cost > BATCH_BYTES then break end

      batch[#batch + 1] = ops[at]
      bytes = bytes + cost
      at = at + 1
    end

    local last = at > #ops

    if not fs.send("/app/wm", { type = "draw", window = handle,
                                ops = batch,
                                more = (not last) or nil }) then
      return false
    end
  end

  return true
end

-- The three faces the desktop is using, applied in this process.
--
-- A window that draws its own pixels rasterizes its own glyphs, so it needs
-- to be told which faces to use - twice: once when it opens, and again
-- whenever somebody changes them. Both paths call this.
--
-- A face that will not load leaves the previous one in place rather than
-- raising: a font is a preference, and an application that dies because
-- somebody picked an odd one is worse than an application with the old
-- font.
local sized_faces = {}

local function apply_fonts(fonts)
  if type(fonts) ~= "table" then return end

  -- **Four roles, which this loop said three of.** The compositor applies
  -- `title` as well, so an application that asked for it measured against a
  -- face it had never loaded - the 8x16 bitmap - while the compositor drew
  -- the desktop's title face. Found on 15 September, writing the size below.
  for _, role in ipairs(theme.roles) do
    local want = fonts[role]

    if type(want) == "table" and want.font then
      gfx.use_font(want.font, tonumber(want.px) or 16, role)
      theme.fonts[role] = { font = want.font, px = tonumber(want.px) or 16 }
    end
  end

  -- The faces asked for by size were cut from the old fonts - and their
  -- slots are given back with them, since this cache is the only thing in
  -- a process holding one (`gfx.release_faces`). Clearing it alone left the
  -- slots taken, and a process told of new faces a few times ran out.
  sized_faces = {}
  gfx.release_faces()
end

--
-- **A role's font at a size of its own.**
--
-- `gfx.face` keeps a pool of seven beyond the five roles and answers `nil,
-- "no room for another face"` rather than throwing one out - so a size that
-- cannot be served falls back to the role itself, which draws at the
-- desktop's size rather than in the bitmap font. Remembered per role and
-- size, and forgotten when the fonts change.
--
function ui.sized(role, px)
  role = role or "ui"

  local want = theme.fonts[role]

  if not want or not px or px == want.px then return role end

  local key = role .. "@" .. px
  local got = sized_faces[key]

  if got == nil then
    got = gfx.face(want.font, px) or false
    sized_faces[key] = got
  end

  return got or role
end

function ui.window(spec)
  spec = spec or {}

  --
  -- `direct = true` asks for a window whose pixels this process draws
  -- itself, into memory both it and the compositor can see.
  --
  -- `gfx.md` 19.4. The ordinary path sends drawing commands and the
  -- compositor owns every pixel, which is what lets a hung application keep
  -- a window. This is for the cases where that is the wrong trade - a video
  -- frame, a rendered scene - where the whole surface changes every frame
  -- and describing it costs more than copying it.
  --
  -- Two buffers in one region: this process draws into the one the
  -- compositor is not showing, and `commit` swaps them. No locks, because
  -- neither side ever touches the buffer the other is using.
  --
  local shared_cap = nil
  local region = nil

  if spec.direct then
    local w_ = spec.w or 400
    local h_ = spec.h or 240
    local bytes = gfx.bytes(w_, h_)
    local pages = (bytes * 2 + 4095) // 4096

    shared_cap = sys.memory(pages)

    if shared_cap then
      local at = sys.memory_map(shared_cap)

      if at then
        region = {
          [1] = gfx.wrap{ at = at, w = w_, h = h_ },
          [2] = gfx.wrap{ at = at + bytes, w = w_, h = h_ },
          draw_into = 2,
        }
      end
    end
  end

  local reply, err = fs.send("/app/wm", {
    type = "open",
    title = spec.title or "window",
    w = spec.w or 400, h = spec.h or 240,
    x = spec.x, y = spec.y,

    -- Part of the desktop rather than something running on it: no close
    -- box, no minimise, no maximise. The Deskbar is the only one, because
    -- it is how a hidden window comes back and how anything is started.
    pinned = spec.pinned or nil,

    -- The window everything else sits on: undecorated, screen-sized, at the
    -- bottom of the stack and never raised. The desktop is one of these.
    backdrop = spec.backdrop or nil,

    --
    -- **Full screen**: undecorated too, at the origin, and in front of
    -- everything including the Deskbar. The application makes its buffers
    -- the size of the screen and asks for this; the window manager does not
    -- resize it into place, because a window that draws its own pixels
    -- cannot be resized at all (`wm.lua`, on `fullscreen`).
    --
    fullscreen = spec.fullscreen or nil,

    -- And its opposite: a strip across the top, undecorated and pinned,
    -- which takes room away from the screen rather than sitting over it.
    strip = spec.strip or nil,

    --
    -- In the middle of the screen, and asked for rather than computed here.
    --
    -- An application does not know how big the screen is when it opens - it
    -- has no framebuffer and no business having one - so "centre me" is the
    -- only form this request can take. The window manager is the process
    -- that knows both numbers, and it is also the one that knows a strip
    -- along the top has taken a piece of the screen away.
    --
    -- It also turns off the cascade. A window that asked for the middle
    -- means the middle: stepping it down and across to avoid whatever is
    -- already there would put it somewhere it did not ask for, which for
    -- the launcher pad is the entire bug - it appeared in a different place
    -- every time because it was being placed *around* the windows already
    -- open.
    --
    centre = spec.centre or nil,

    -- Whether something dragged from another window may be let go over
    -- this one. It decides whether the desktop outlines this window while
    -- a drag is overhead - see `wm.lua` - so saying yes and then ignoring
    -- the drop is a promise the window does not keep.
    drops = spec.drops or nil,

    --
    -- **A menu bar, for a window that draws its own pixels.** The kit cannot
    -- draw one into memory the application owns, so the window manager
    -- draws the strip above it and says when a title is pressed; the menus
    -- themselves are this kit's, opened by `direct_event`. Only the titles
    -- go: the items stay here, where their `on_choose` can run.
    --
    menubar = (spec.direct and spec.menubar) and (function()
      local titles = {}

      for i, m in ipairs(spec.menubar) do titles[i] = tostring(m.title) end

      return titles
    end)() or nil,

    -- Always, and it is the kit saying "I understand `button`" rather than
    -- the application asking for anything.
    --
    -- The window manager will not send a right press to a window that has
    -- not said this, because a program that has never heard of one reads it
    -- as a left press and acts on it - `handlers.open` in `wm.lua` has the
    -- argument. Every window built here goes through `dispatch_mouse`,
    -- which reads `button` and drops a right press no view claimed, so it
    -- is safe here in a way it is not for a hand-written event loop. An
    -- application that wants one writes `on_context` and nothing else.
    context = true,
  }, shared_cap)

  if not reply then
    return nil, err
  end

  -- Whatever the desktop looks like right now, before the first paint. The
  -- palette is mutated in place as always, so every widget this window is
  -- about to build reads the right colours from the start.
  if reply.palette then theme.apply(reply.palette) end
  if reply.desktop then theme.override { desktop = reply.desktop } end
  apply_fonts(reply.fonts)

  local w = setmetatable({
    handle = reply.window,
    root = ui.view{ x = 0, y = 0, w = reply.w, h = reply.h },

    -- Menus this window has open, innermost last. See `push_menu`.
    menus = {},

    -- A direct window's menu bar - titles and items - for `direct_event`.
    direct_menus = spec.direct and spec.menubar or nil,

    -- Where this window's content begins on the screen. The window manager
    -- says so in the reply because it clamps what it was asked for, and
    -- anything that places a menu needs it.
    origin_x = reply.x or 0,
    origin_y = reply.y or 0,

    --
    -- And how big it actually is, for the same reason and it was missing.
    --
    -- The size in the reply is the size the compositor granted, which is
    -- not always the size that was asked for - it clamps to the screen and
    -- to a floor. `root` was built from it and nothing else was, so an
    -- application that laid out from its own numbers drew short and left
    -- whatever it did not reach showing. The bar across the top asked for
    -- 26 rows, was granted 32, painted 26, and the six nobody painted read
    -- as a border under it.
    --
    w = reply.w,
    h = reply.h,
    -- Nil unless the application asked for a particular colour, so that
    -- `paint` can fall back to the palette *at the moment it draws*.
    -- Resolving it here instead captured the colour once, at creation, and
    -- a window then kept its original background through a theme change
    -- while every widget inside it followed - which looks like the theme
    -- half worked, and is the one thing that was wrong when it did.
    background = spec.background,
    focus = 1,
    running = true,
    title = spec.title or "window",
    x = spec.x or 0,
    y = spec.y or 0,
    properties = {},
    dirty = false,
    region = region,
    ticking = {},
    tick_every = spec.tick_every or 0,
  }, window)

  --
  -- **The root view points back at its window**, so a control can find the
  -- window it is in by walking `parent` - which is how `ui.dropdown` opens a
  -- menu without every application having to hand it one.
  --
  -- The chain stops at `root`, because `win:add` places children under it
  -- and `root.parent` is nothing. Nothing needed to cross that gap until a
  -- widget wanted to draw outside itself; a menu belongs to the window and
  -- is drawn in the window's coordinates, so a control that opens one has to
  -- reach it.
  --
  w.root.window = w

  --
  -- What every window exposes, before the application adds anything.
  --
  -- A getter and a setter, so a property is a live view of the thing and
  -- not a copy that drifts. Writing `title` really renames the window,
  -- because the setter is what renaming is.
  --
  w:publish("title",
            function() return w.title end,
            function(v) w:retitle(v) end)

  w:publish("x", function() return w.x end,
                 function(v) w:move(tonumber(v) or w.x, w.y) end)
  w:publish("y", function() return w.y end,
                 function(v) w:move(w.x, tonumber(v) or w.y) end)
  w:publish("width",  function() return w.root.w end)
  w:publish("height", function() return w.root.h end)

  -- Whatever the application declared, the same way, with no ceremony:
  -- ui.window{ ..., properties = { brush = { get = ..., set = ... } } }
  for name, p in pairs(spec.properties or {}) do
    w:publish(name, p.get, p.set)
  end

  local ep = sys.endpoint()

  if ep then
    local registered = fs.send("/app", { type = "register",
                                         name = spec.title or "app" }, ep)
    if registered then
      w.control = ep
      w.name = registered.name
    else
      sys.destroy(ep)
    end
  end

  return w
end

--
-- A property. `set` may be nil, and then it is read-only - which is the
-- honest answer for `width` while windows cannot be resized.
--
--
-- The buffer to draw into, for a window opened with `direct = true`.
--
-- Never the one being shown. Asking for it again after a `commit` gives the
-- other one, which is the whole of the double buffering as far as an
-- application is concerned.
--
function window:surface()
  if not self.region then return nil end

  return self.region[self.region.draw_into]
end

--
-- This frame is finished; show it.
--
-- The damage rectangle is not optional in spirit - without one the
-- compositor has to blit the whole surface, which is the cost this whole
-- arrangement exists to avoid. Omitting it means "all of it" and is there
-- for the first frame, not for every frame.
--
function window:commit(damage)
  if not self.region then return false end

  damage = damage or { x = 0, y = 0, w = self.root.w, h = self.root.h }

  local reply = fs.send("/app/wm", {
    type = "commit", window = self.handle,
    x = damage.x, y = damage.y, w = damage.w, h = damage.h,
  })

  if not reply then
    self.running = false
    return false
  end

  self.region.draw_into = reply.draw_into or
                          ((self.region.draw_into == 1) and 2 or 1)

  return true
end

function window:publish(name, get, set)
  self.properties[name] = { get = get, set = set }
end

function window:move(x, y)
  self.x, self.y = x, y

  -- And the origin with it, or a menu opened after a move appears where the
  -- window used to be.
  self.origin_x, self.origin_y = x, y

  fs.send("/app/wm", { type = "move", window = self.handle, x = x, y = y })
end

--
-- **A window asking for its own size**, which is what a window that folds
-- needs: Music's mini player is this window with its list put away.
--
-- The window manager has served the request since 2 September and refuses
-- only a window that draws its own pixels - those buffers are the
-- application's own region, sized for exactly those dimensions, and cannot be
-- grown from this side. **Nothing in the userland ever asked**, so this is the
-- half that was missing rather than a new feature.
--
-- **The size is taken from the reply**, not from the `resize` event that
-- follows it. The event lays the view tree out again and leaves `self.w` and
-- `self.h` as they were, so a window that resized itself and then read its own
-- width got the old one.
--
function window:resize(w, h)
  local reply, why = fs.send("/app/wm", { type = "resize",
                                          window = self.handle, w = w, h = h })

  if not reply then return false, why end

  self.w, self.h = reply.w, reply.h
  self.root:resize(reply.w, reply.h)

  return true, reply.w, reply.h
end

--
-- One request from the shell, or from anything else holding this window's
-- endpoint. Non-blocking, drained every pass of the loop below, so a slow
-- reader cannot slow the interface down.
--
local function serve_properties(self)
  if not self.control then return false end

  local changed = false

  while true do
    local req, who = sys.receive(self.control, true)

    if not req then return changed end

    local name = tostring(req.path or ""):match("([^/]+)$")
    local p = name and self.properties[name]
    local reply

    if req.type == "list" then
      local names = {}
      for key in pairs(self.properties) do names[#names + 1] = key end
      table.sort(names)
      reply = { ok = true, entries = names }

    elseif not p then
      reply = { ok = false, error = "no such property" }

    elseif req.type == "read" then
      reply = { ok = true, value = tostring(p.get()) }

    elseif req.type == "getattr" then
      reply = { ok = true, attrs = { kind = "property",
                                     size = #tostring(p.get()),
                                     writable = p.set ~= nil } }

    elseif req.type == "write" then
      if not p.set then
        reply = { ok = false, error = name .. " is read-only" }
      else
        -- A trailing newline is what `write` from a shell sends and never
        -- what a property means.
        local ok, err = pcall(p.set, (tostring(req.value):gsub("\n$", "")))
        reply = ok and { ok = true } or { ok = false, error = tostring(err) }
        changed = true
      end

    else
      reply = { ok = false, error = "no such operation on a property" }
    end

    pcall(sys.reply, who, reply)
  end
end

function window:add(child)
  local added = self.root:add(child)

  -- Which window this widget is in. Absent until now, and a menu bar
  -- cannot open a menu without it: a menu is a window placed on the
  -- *screen*, so a widget that opens one has to be able to ask where its
  -- own window is.
  if type(child) == "table" then
    child.window = self
  end

  if type(child) == "table" and child.tick then
    self.ticking = self.ticking or {}
    self.ticking[#self.ticking + 1] = child

    -- Half a second, in counter ticks. Read the first time a window has
    -- anything that ticks at all, so an ordinary window never asks /dev/cpu
    -- a question it has no use for. Without this the default was "every
    -- pass", which is a full repaint per yield and would drown the window
    -- manager in messages about a clock that changes once a second.
    if not self.tick_every or self.tick_every == 0 then
      local cpu = fs.read("/dev/cpu")
      -- Once a second. Anything that ticks here is showing a number a
      -- person reads - a clock, a meter - and a person cannot read two a
      -- second, so redrawing twice as often is twice the work for nothing.
      self.tick_every = (cpu and cpu.counter_hz or 62500000)
    end
  end

  return added
end

--
-- **Put the keyboard on a widget.**
--
-- The window keeps `focus` as an index into `root:focusables()`, because
-- that is what a click sets and what Tab steps through. An application has
-- a *view*, not an index, so it had to walk the list itself - and Tracker
-- did, in a function of its own, under a comment saying there was no
-- `win:focus(v)` in the kit and that adding one for a single caller would
-- be a widget change made for an application.
--
-- **That argument expired the day there were two callers**, and the way it
-- expired is why this is here rather than in Tracker. The second caller was
-- written as `win:focus(search)` - the name the field already has - which
-- is `self.focus(self, v)` on a *number*, so pressing Find ended Tracker
-- every time. Diego, on the ThinkCentre M700: "the tracker closed on me a
-- couple of times".
--
-- Nothing could have caught it but running it: Lua resolves the call at the
-- moment of the press, so the file loads, the window opens, and the button
-- is there and wrong.
--
-- Returns false when the view is not focusable or not in this window, which
-- is a question worth being able to ask rather than a silent nothing.
--
function window:focus_on(view)
  for i, v in ipairs(self.root:focusables()) do
    if v == view then
      self.focus = i
      return true
    end
  end

  return false
end

function window:close()
  if self.closed then return end

  self.closed = true
  self.running = false

  fs.send("/app/wm", { type = "close", window = self.handle })

  if self.control then
    fs.send("/app", { type = "unregister", name = self.name })
    sys.destroy(self.control)
    self.control = nil
  end
end

--
-- **A ring once the keyboard is in use, and not before** (`roadmap.md` 5zp).
--
-- The drawings never show a focus ring, and a window that opened with one
-- round its first control - which is every window, since something always
-- holds the focus - drew a box the page does not. So a widget draws its
-- ring only while `keyed`: set by the window on any key, cleared on a
-- press with the pointer. Tab still moves the focus the moment it is
-- pressed, and the ring appears with it; a person using the mouse never
-- sees one. A field's border and a caret are not rings and do not wait.
--
local function apply_focus(self)
  local list = self.root:focusables()

  for i, v in ipairs(list) do
    v.focused = (i == self.focus)
    v.keyed = self.keyed or false
  end

  return list
end

--------------------------------------------------------------------------
-- The menu bar.
--
-- A row of titles across the top of a window. Clicking one opens its menu
-- as a window below it, which is what lets a dropdown fall outside the
-- frame and over whatever is behind.
--
-- **It is an ordinary widget in the window's own view tree**, not a band
-- the window manager reserves. The survey that preceded this found that a
-- menu bar in the client rectangle would push every application's content
-- down by its height and break all of them at once - so it does not push
-- anything: an application that wants one adds it at y = 0 and lays its own
-- content out below, and an application that does not is untouched.
--------------------------------------------------------------------------

--
-- A menu's items, which may be a list or a function that returns one.
--
-- A list is built once and read every time the menu opens, so anything it
-- says about the *state* of the program - which view mode is in force,
-- which column it is sorted on, how big the icons are - would be however it
-- was when the window was made. A function is called when the menu opens,
-- which is the only moment the answer is known to be current.
--
local function menu_items(of)
  local items = of and of.items

  if type(items) == "function" then
    local ok, made = pcall(items)

    return (ok and type(made) == "table") and made or {}
  end

  return items or {}
end

ui.menu_items = menu_items

function ui.menubar(spec)
  local v = ui.view(spec)

  v.h = v.h > 0 and v.h or ROW
  v.menus = v.menus or {}
  v.follow = { left = true, right = true, top = true }

  -- Where each title starts and ends, worked out once per draw and read by
  -- the hit test. Two functions agreeing about geometry by coincidence is
  -- how a control ends up drawn in one place and clickable in another.
  local function spans(self)
    local out, x = {}, 4

    for i, m in ipairs(self.menus) do
      local w = gfx.measure(tostring(m.title or "")) + 16

      out[i] = { x = x, w = w }
      x = x + w
    end

    return out
  end

  function v:draw(g)
    -- The same gradient the window tab above it gets, from the same pair of
    -- functions, so the two pieces of chrome at the top of a window are lit
    -- from the same direction. The groove underneath still separates the
    -- bar from the content; the gradient is what stops it reading as a
    -- painted rectangle.
    local top, bottom = theme.chrome(theme.raised)

    theme.vgradient(g, 0, 0, self.w, self.h - 2, top, bottom, 0, self.h - 2)
    g:groove(0, self.h - 2, self.w, 2)

    for i, m in ipairs(self.menus) do
      local s = spans(self)[i]
      local open = (self.open_index == i)

      if open then g:fill(s.x, 0, s.w, self.h - 2, "accent") end

      g:text(s.x + 8, centred(self.h - 2), tostring(m.title or ""),
             open and theme.text_on or theme.text,
             open and theme.accent or theme.raised)
    end
  end

  function v:mouse(action, x, y)
    if action ~= "press" then return true end

    local win = self.window

    if not win then return true end

    for i, s in ipairs(spans(self)) do
      if x >= s.x and x < s.x + s.w then
        -- Screen coordinates: the menu is a window of its own, so it is
        -- placed on the screen rather than inside this one. `win.origin`
        -- is where this window's content begins.
        self.open_index = i
        win:open_menu(win.origin_x + self.x + s.x,
                      win.origin_y + self.y + self.h,
                      menu_items(self.menus[i]))
        return true
      end
    end

    return true
  end

  return v
end

--------------------------------------------------------------------------
-- Menus.
--
-- `roadmap.md` M13 item 2. A menu is a *window* - undecorated, above
-- everything, owned by whoever opened it - so it drops over other windows
-- and outside its own frame, which is the thing the milestone is about.
--
-- **Not a nested run loop, and that is the one decision worth defending.**
-- The obvious shape is `local choice = win:menu(...)` blocking until the
-- user picks, and it would wedge every window that is also a server: the
-- Terminal answers `write` for the programs it runs from inside its own
-- pass, so a Terminal with a menu open would stop delivering output until
-- the menu closed. Asynchronous costs a callback and keeps one loop.
--
-- **One flat view, hit-tested by arithmetic.** Not a parent with an
-- item-view per row: `view:hit` returns the deepest child and the press
-- grabs it until release, so a row that captured the press would eat the
-- drag that a menu is driven by. The panel is one rectangle that works out
-- which row a coordinate is in.
--
-- Its events arrive on the *owner's* queue tagged with the menu's handle,
-- so an application still polls one window. See `wm.lua`'s pointer routing.
--------------------------------------------------------------------------

local MENU_PAD = 8

-- Room on the right for the marker that says "there is more this way".
local MENU_ARROW = 12

-- A menu item's picture, drawn at its own size because nothing here scales
-- one. A menu with any pictures in it gives every row their height, so the
-- names still line up down the left whether a row has a picture or not.
local MENU_ICON = 32

--
-- Room on the left for the mark that says which of a set of choices is in
-- force - "as icons" against "as list", one of three icon sizes, which
-- column a listing is sorted on.
--
-- **`mark = false` is not the same as no mark**, and that is the whole of
-- why this reads `~= nil`. An item that *can* be marked and is not still
-- gives the menu its column, so the names line up and the mark appears
-- beside a row rather than shifting every row when it moves.
--
-- A diamond rather than a tick, because every one of these is a choice
-- among several rather than something switched on, and because it is five
-- rectangles where a tick would be a new verb in this file.
--
local MENU_MARK = 12

local function menu_pictured(items)
  for _, it in ipairs(items) do
    if it.icon then return true end
  end

  return false
end

local function menu_marked(items)
  for _, it in ipairs(items) do
    if it.mark ~= nil then return true end
  end

  return false
end

local function menu_metrics(items)
  local widest = 0
  local deep = false
  local pictured = menu_pictured(items)

  for _, it in ipairs(items) do
    -- Measured, not counted. A character count is a width only while every
    -- glyph is the same width, and the interface font need not be.
    local n = gfx.measure(tostring(it.text or ""))

    if n > widest then widest = n end
    if it.submenu then deep = true end
  end

  -- A row of the fixed layout, or tall enough for its 32-pixel icon.
  local row = pictured and math.max(ROW, MENU_ICON + 4) or ROW

  return widest + MENU_PAD * 2 + 12 + (deep and MENU_ARROW or 0)
                + (pictured and MENU_ICON + 6 or 0)
                + (menu_marked(items) and MENU_MARK or 0),
         #items * row + 4, row
end

--
-- Draw one menu into its own window.
--
-- The same graphics context and the same batching as any window, because it
-- *is* a window. `gc:raised` gives it the edge that says it is sitting on
-- top of what is behind it.
--
function window:paint_menu(m)
  if not m then return end

  local g = new_gc()

  g.cw, g.ch = m.w, m.h
  g:raised(0, 0, m.w, m.h, "raised")

  local marks  = menu_marked(m.items)
  local mark_w = marks and MENU_MARK or 0
  local text_x = MENU_PAD + 4 + mark_w
                 + (menu_pictured(m.items) and MENU_ICON + 6 or 0)

  for i, it in ipairs(m.items) do
    local y = 2 + (i - 1) * m.row

    if it.separator then
      g:groove(MENU_PAD, y + m.row // 2, m.w - MENU_PAD * 2, 2)
    else
      -- A greyed item is drawn and never lit: it is there to say the thing
      -- exists and is not ready, which a missing item cannot say.
      local hot = (i == m.hot) and not it.disabled
      local bg  = hot and theme.accent or theme.raised

      if hot then g:fill(2, y, m.w - 4, m.row, "accent") end

      --
      -- The mark, when this item is the one in force. Seven stacked rows,
      -- widest in the middle, which is a diamond at this size and is built
      -- the same way as the submenu arrow below.
      --
      if it.mark then
        local ink = hot and theme.text_on or theme.text
        local my = y + (m.row - 7) // 2

        for k = 0, 6 do
          local run = 7 - 2 * math.abs(k - 3)

          g:fill(MENU_PAD + 2 + (7 - run) // 2, my + k, run, 1, ink)
        end
      end

      if it.icon then
        g:icon(MENU_PAD + 2 + mark_w, y + (m.row - MENU_ICON) // 2,
               it.icon .. ".png", MENU_ICON)
      end

      g:text(text_x, y + centred(m.row), tostring(it.text or ""),
             it.disabled and theme.text_dim
             or (hot and theme.text_on or theme.text), bg)

      --
      -- The submenu marker, built out of fills because there is no line and
      -- no triangle a window can draw. Five stacked rows, widest in the
      -- middle: at this size that reads as an arrow and costs five
      -- rectangles, where a real triangle would cost a new op in the
      -- compositor and a new verb in this file.
      --
      if it.submenu then
        local ax = m.w - MENU_ARROW - 4
        local ay = y + (m.row - 5) // 2
        local ink = hot and theme.text_on or theme.text

        for k = 0, 4 do
          local run = 3 - math.abs(k - 2)

          if run > 0 then g:fill(ax, ay + k, run, 1, ink) end
        end
      end
    end
  end

  send_ops(m.handle, g.ops)
end

--
-- Open a menu, and push it on the stack.
--
-- A stack rather than one menu, because a submenu is a menu: the same
-- record, the same window, the same routing. What makes it a submenu is
-- only that something above it in the stack is still open.
--
function window:push_menu(x, y, items)
  local w, h, row = menu_metrics(items)

  local reply = fs.send("/app/wm", { type = "open", kind = "menu",
                                     owner = self.handle,
                                     x = x, y = y, w = w, h = h })

  if not reply or not reply.ok then return nil end

  -- Where it actually went, which is not always where it was asked for: the
  -- window manager pulls a menu back onto the screen and says so in the
  -- reply. A caller that did not read this could not place a submenu beside
  -- its parent.
  local m = { handle = reply.window, items = items, row = row,
              x = reply.x, y = reply.y, w = reply.w, h = reply.h,
              hot = nil }

  self.menus[#self.menus + 1] = m
  self:paint_menu(m)

  return m
end

--
-- **A direct window's menu events**, for an application that runs its own
-- loop rather than `window:run` - the Super Nintendo does, because it paces
-- itself by frames. Hand every event here first; true means it was the
-- menu bar's or a menu's, and taken.
--
-- `menubar` is the window manager saying a title in the strip it draws was
-- pressed, and where under it the menu goes; a mouse event tagged `menu` is
-- a menu this window has open, handled exactly as a kit window handles it.
--
function window:direct_event(ev)
  if ev.type == "menubar" then
    local m = self.direct_menus and self.direct_menus[ev.index]

    if m then self:open_menu(ev.x, ev.y, menu_items(m)) end

    return true
  end

  if ev.type == "mouse" and ev.menu then
    self:menu_mouse(ev)
    return true
  end

  return false
end

-- Everything from `from` upward, closed. `close_menus()` closes the lot.
function window:close_menus(from)
  from = from or 1

  for i = #self.menus, from, -1 do
    fs.send("/app/wm", { type = "close", window = self.menus[i].handle })
    self.menus[i] = nil
  end

  return true
end

-- The old single-menu names, kept because the menu bar and anything else
-- that only ever wants one still reads better this way.
function window:open_menu(x, y, items)
  self:close_menus()

  return self:push_menu(x, y, items)
end

function window:close_menu()
  return self:close_menus()
end

--
-- A mouse event that arrived tagged with a menu handle.
--
-- Highlighting follows the pointer while a button is held, which is how a
-- menu is used: press on the title, slide down, release on the item. An item
-- with a submenu opens it on the way past rather than on a click, which is
-- what every menu does and is why sliding along a row of them works.
--
function window:menu_mouse(ev)
  local at, m = nil, nil

  for i, one in ipairs(self.menus) do
    if one.handle == ev.menu then at, m = i, one break end
  end

  if not m then return false end

  local row = nil

  if ev.x >= 0 and ev.x < m.w and ev.y >= 2 then
    local i = (ev.y - 2) // m.row + 1

    if i >= 1 and i <= #m.items and not m.items[i].separator then
      row = i
    end
  end

  local item = row and m.items[row]

  --
  -- A right press on a row asks about it rather than choosing it.
  --
  -- The menu closes first, because whatever answers is going to put a
  -- window on the screen and a menu left standing over it is a menu nobody
  -- can get rid of. Then `on_menu_context` is told which item, and a window
  -- that has no such handler gets a closed menu and nothing else - which is
  -- the right answer for every menu in the system except the Deskbar's.
  --
  if ev.button == "right" then
    if ev.action ~= "press" then return false end

    self:close_menus()

    if item and self.on_menu_context then
      pcall(self.on_menu_context, self, item)
      return true
    end

    return true
  end

  if ev.action == "release" then
    --
    -- Releasing on something that opens a submenu is not a choice. The
    -- submenu is already open and the pointer is on its way there; closing
    -- everything here would make a menu impossible to reach by sliding,
    -- which is how they are used.
    --
    if item and item.submenu then return false end

    -- `disabled`: drawn, and a press on it does nothing - the menu stays.
    if item and item.disabled then return true end

    self:close_menus()

    if item and item.on_choose then
      pcall(item.on_choose, item)
    end

    return true
  end

  if row == m.hot then return false end

  m.hot = row

  -- Anything this menu opened is no longer what the pointer is over.
  self:close_menus(at + 1)
  self:paint_menu(m)

  if item and item.submenu then
    -- Beside the parent and level with the row, overlapping by the border
    -- so the two read as one shape rather than two windows.
    self:push_menu(m.x + m.w - 2, m.y + 2 + (row - 1) * m.row, item.submenu)
  end

  return true
end

function window:paint()
  --
  -- A window whose pixels the application draws has nothing to send. Its
  -- views, if it has any, would be drawing into the compositor's copy -
  -- which is not the one on screen.
  --
  if self.region then return end

  apply_focus(self)

  local g = new_gc()
  g.cw, g.ch = self.root.w, self.root.h

  --
  -- **The clear, which a window whose views cover it does not need.**
  --
  -- Every frame starts by filling the window and then drawing over it, and
  -- the compositor holds the damage back until the last batch, so a repaint
  -- is never seen half-drawn. **A drag is the exception**: it damages the
  -- window on every step of the pointer and composites whatever is in the
  -- surface at that instant, which for a window repainting many times a
  -- second lands in the gap between the clear and the contents. Diego saw it
  -- on Music on 16 September - "when dragging its all flickery the window" -
  -- and noticed the other windows do not, which is what named the cause:
  -- they repaint when something happens, and Music repaints to move a clock
  -- and a meter.
  --
  -- So `background = false` says the views paint every pixel themselves.
  -- Then the worst a mid-drag composite can catch is a window with some of
  -- its parts updated, rather than an empty one.
  --
  if self.background ~= false then
    g.ops[#g.ops + 1] = { op = "fill", x = 0, y = 0,
                          w = self.root.w, h = self.root.h,
                          color = self.background or theme.window }
  end

  self.root:paint(g)

  local at = 1

  while at <= #g.ops do
    local batch = {}
    local bytes = 0

    while at <= #g.ops do
      local cost = op_cost(g.ops[at])

      -- Always at least one, so a single enormous command is sent on its
      -- own and refused by the serialiser with its own message rather than
      -- looping here for ever.
      if #batch > 0 and bytes + cost > BATCH_BYTES then
        break
      end

      batch[#batch + 1] = g.ops[at]
      bytes = bytes + cost
      at = at + 1
    end

    -- The window manager holds the damage back until the last batch, so the
    -- screen never shows this frame half-drawn. Without it every repaint
    -- flickered: the first message clears the background and the widgets
    -- arrive in the next two.
    local last = at > #g.ops

    local ok = fs.send("/app/wm", { type = "draw", window = self.handle,
                                    ops = batch,
                                    more = (not last) or nil })

    if not ok then
      -- The window manager went away, which is not this program's fault
      -- and not something it can do anything about.
      self.running = false
      return
    end
  end
end

--
-- One key, to whoever should have it.
--
-- Tab is the window's and never the widget's: a control that could swallow
-- Tab is a control you can get stuck in. Everything else is offered to the
-- focused widget first, and what it does not want falls back to the window.
--
local function dispatch(self, c)
  self.keyed = true

  if c == 9 then                                  -- Tab
    local list = self.root:focusables()

    if #list > 0 then
      self.focus = (self.focus % #list) + 1
    end

    return true
  end

  local list = apply_focus(self)
  local target = list[self.focus]

  if target and target.key and target:key(c) then
    return true
  end

  if self.on_key then return self.on_key(self, c) end

  return false
end

--
-- One mouse event, to the view under it.
--
-- The press decides who gets everything until the release, mirroring the
-- grab the window manager keeps on the window: without it, sliding off a
-- button before letting go would deliver the release to whatever the
-- pointer had wandered onto, and both widgets would be half-operated.
--
-- A press also moves the focus, so clicking a control and then typing does
-- what it looks like it should. That is the one place clicking and Tab have
-- to agree.
--
--------------------------------------------------------------------------
-- Select-all, copy, cut and paste.
--
-- **The window's part is routing, and that is deliberately all of it.**
-- The window manager decided which keys mean this; a widget decides what
-- its selection is and what pasting into it does; and in between there is
-- this, which knows only that the focused widget is the one being asked.
--
-- A widget joins in by having an `edit` method taking one of these four
-- names. Everything without one is unaffected, which is most of the kit -
-- a button has no selection and a paste into it means nothing.
--
-- The clipboard itself is never touched here. `ui.editor` sends its own
-- `wmproto.copy`, because the widget is what knows the bytes and this is
-- what would have to be told them.
--------------------------------------------------------------------------

local EDITS = {
  selectall = true, copy = true, cut = true, paste = true,
}

--
-- Hand one of them to whatever has the focus.
--
-- Falls back to the window's own `on_edit`, the same way key dispatch falls
-- back to `on_key`: a window that draws its own content rather than filling
-- itself with widgets should still be able to answer a copy.
--
function window:dispatch_edit(kind)
  local list = apply_focus(self)
  local target = list[self.focus]

  if target and target.edit and target:edit(kind) then
    return true
  end

  if self.on_edit then return self.on_edit(self, kind) end

  return false
end

--
-- The right button, which never touches focus, the grab, or a widget.
--
-- It is a different question from a click - "tell me about what is under the
-- pointer" rather than "press it" - so routing it through the same path
-- would press whatever it landed on, which is the bug this whole opt-in
-- exists to prevent. A window says what to do about it by having
-- `on_context`; one that does not gets nothing, which is why `ui.lua` can
-- ask the window manager for right presses on behalf of every window it
-- opens without changing how any of them behave.
--
-- The release is dropped too. A context menu opens on the press and
-- everything after that is the menu's, which the window manager routes by
-- handle - so there is nothing for a right release to mean here, and a
-- handler that received one would have to know to ignore it.
--
local function dispatch_context(self, ev)
  if ev.action ~= "press" then return false end

  --
  -- The view under it first, then the window. Same order and same reason as
  -- a drop: a view answers in its own coordinates and does not have to know
  -- where in the window it sits, and a window can answer everywhere without
  -- giving every widget a handler.
  --
  local target, lx, ly = self.root:hit(ev.x, ev.y)

  if target and target.on_context then
    return target.on_context(target, lx, ly) and true or false
  end

  if self.on_context then
    return self.on_context(self, ev.x, ev.y) and true or false
  end

  return false
end

local function dispatch_mouse(self, ev)
  if ev.button == "right" then
    return dispatch_context(self, ev)
  end

  if ev.action == "press" then
    local target, lx, ly = self.root:hit(ev.x, ev.y)

    self.grab = nil

    if target and target.mouse then
      self.grab = { view = target, dx = ev.x - lx, dy = ev.y - ly }
    end

    -- A press puts the rings away: see `apply_focus`.
    self.keyed = false

    if target and target.focusable then
      for i, v in ipairs(self.root:focusables()) do
        if v == target then
          self.focus = i
          break
        end
      end
    end
  end

  local g = self.grab

  if not g then return false end

  local handled = g.view:mouse(ev.action, ev.x - g.dx, ev.y - g.dy)

  if ev.action == "release" then
    self.grab = nil
  end

  return handled and true or false
end

--
-- Where something was let go, to whatever is under it.
--
-- Hit-tested exactly as a press is, so a view answers `drop` in its own
-- coordinates and does not have to know where in the window it sits. A view
-- that has no `drop` does not stop the search: the drop belongs to whoever
-- can take it, and a label lying over a list should not swallow it.
--
-- The window's own `on_drop` is the fallback, so a window can take drops
-- anywhere on it without giving every widget a handler.
--
local function dispatch_drop(self, ev)
  local target, lx, ly = self.root:hit(ev.x, ev.y)

  while target do
    if target.drop then
      return target:drop(ev.kind, ev.payload, lx, ly) and true or false
    end

    if not target.parent then break end

    -- Up one, and the point comes with it: a child sits at `(x, y)` in its
    -- parent, so the parent's coordinates are the child's plus that.
    lx, ly = lx + target.x, ly + target.y
    target = target.parent
  end

  if self.on_drop then
    return self.on_drop(self, ev.kind, ev.payload, ev.x, ev.y) and true or false
  end

  return false
end

--
-- Starting a drag, and calling it off.
--
-- `payload` is a string both ends agree about and the desktop never reads -
-- `wm.lua` says why it is a string rather than a table. `label` is what the
-- pointer carries while it is held, and is the only feedback there is that
-- a drag is happening at all, so a caller that leaves it out gets none.
--
--
-- A new name for a window, which is also a new label in the Deskbar.
--
-- A method rather than only a published property, because an application
-- retitling *itself* should not have to go out through the namespace and
-- back to do it. The property's setter is this function, so the two cannot
-- disagree about what renaming means - which they did, briefly, when
-- writing `win.title` looked like it worked and only set a field.
--
function window:retitle(text)
  self.title = tostring(text)

  return fs.send("/app/wm", { type = "retitle", window = self.handle,
                              title = self.title })
end

function ui.drag(win, kind, payload, label)
  return fs.send("/app/wm", { type = "drag", window = win.handle,
                              kind = kind, payload = payload,
                              label = label })
end

function ui.undrag(win)
  return fs.send("/app/wm", { type = "drag", window = win.handle })
end

--
-- What became of a drop, sent back to whoever started the drag.
--
-- Only the window that was just handed one may say this, and only once -
-- the desktop holds that right and takes it away after. Without the answer
-- the source cannot know its directory has changed, because nothing else
-- tells it.
--
function ui.dropped(win, ok, count, err)
  return fs.send("/app/wm", { type = "dropped", window = win.handle,
                              ok = ok and true or false,
                              count = count or 0, error = err })
end

function window:run()
  local decode_key = ui.key_decoder()

  self:paint()

  while self.running do
    --
    -- How long this window is prepared to wait.
    --
    -- The window manager holds the answer until something happens or this
    -- runs out, so between events the process is blocked rather than
    -- running. Asking with no wait at all - which is what this used to do -
    -- meant every window span for ever, and a desktop with four of them
    -- open sat at ninety-six per cent doing nothing.
    --
    --
    -- How long this window is prepared to wait.
    --
    -- A window with nothing to do sleeps until its next tick, which is a
    -- second, and the machine idles. A window that is also a *server* -
    -- the terminal - cannot: every `write` from a program it is running
    -- blocks in `sys.call` until this loop wakes up and answers, so a
    -- second of sleep is a second per line of output. `ls` came out one
    -- line at a time, which is not slow, it is a window answering its
    -- children once a second.
    --
    -- `poll_wait_ticks` is in scheduler ticks and overrides that. It is not the
    -- default because it is a real cost: a window that wakes a hundred
    -- times a second is a window that is running a hundred times a second.
    --
    --
    -- **Three numbers, and two of them were the wrong unit.**
    --
    -- `wait` on the wire is a timeout, so it is scheduler ticks - the same
    -- as `sys.sleep` and `sys.receive` and `fs.wait_input`, and the same as
    -- `poll_wait_ticks`'s own comment above. `tick_every` is *not*: it is
    -- compared against `sys.ticks()` further down, which is the counter, so
    -- it is counter units at every call site that sets it. Feeding it
    -- straight into `wait` mixed them.
    --
    -- The lazy default mixed them the other way. `counter_hz // 4` is a
    -- quarter of a second in counter units and was sent as a *tick* count:
    -- fifteen million ticks, seventeen hours. It looked like it worked only
    -- because the window manager was adding it to a counter timestamp, so
    -- the two errors cancelled - and the moment either was fixed alone, the
    -- desktop would have hung or spun.
    --
    -- So both convert here, and nothing downstream has to know.
    --
    local wait = self.poll_wait_ticks

    if not wait then
      if self.tick_every and self.tick_every > 0 then
        wait = math.max(1, self.tick_every // ui_per_tick())
      else
        wait = math.max(1, ui_tick_hz() // 4)     -- a quarter of a second
      end
    end

    local reply = wmproto.poll(self.handle, wait)

    --
    -- The window manager went away, which is the ordinary end of an
    -- application here: Control-C stops the manager and everything it
    -- started stops with it.
    --
    -- Leaving by `return` was wrong and took a while to show itself. The
    -- registration in /app outlived the process, so the *next* run of the
    -- same program registered as "gallery2" and anything written to
    -- /app/gallery went to an endpoint whose process no longer existed.
    -- Nothing failed loudly; the name was simply taken by a ghost.
    --
    if not reply then break end

    -- Whoever is scripting this window, before its own events: a property
    -- write is somebody asking for something and an event is something
    -- that already happened.
    local changed = serve_properties(self)

    --
    -- For a window that is also a server.
    --
    -- The terminal is one: it answers `write` for the programs it runs, and
    -- it has to do that every pass rather than on the tick, or a program
    -- that prints a screenful would deliver one line a second.
    --
    if self.on_frame and self:on_frame() then
      changed = true
    end

    for _, ev in ipairs(reply.events) do
      if ev.type == "close" then
        --
        -- The desktop asking, not telling. An application that gets this
        -- far has a second to leave; one that never reads its events is
        -- ended instead, which is the only thing that works on something
        -- that has stopped listening.
        --
        if self.on_close then self.on_close(self) end

        if self.running then self:close() end

        return
      elseif ev.type == "theme" then
        --
        -- The desktop changed its appearance and every window is being told.
        --
        -- The palette table is *mutated*, never replaced: every widget
        -- above reads `theme.text` at the moment it draws, so changing the
        -- fields of the one table changes what the next repaint looks like
        -- across all of them. Swapping in a new table would leave every
        -- one of those references pointing at the old one, and the theme
        -- would change only for windows opened afterwards.
        --
        -- The application is told nothing and does nothing. That is the
        -- point: an application that had to know about themes would be an
        -- application that could get them wrong.
        --
        if ev.palette then theme.apply(ev.palette) end
        if ev.desktop then theme.override { desktop = ev.desktop } end

        -- A window that draws its own pixels draws its own text, so it
        -- needs the faces as well as the colours. One that sends commands
        -- is unaffected: the compositor drew that text and has already
        -- changed.
        apply_fonts(ev.fonts)

        changed = true
      elseif ev.type == "resize" then
        --
        -- The window was resized - by the grip, or by whoever asked.
        --
        -- `view:resize` walks the tree applying follow modes, which is the
        -- whole of layout here and is why this is three lines rather than a
        -- relayout pass: a widget that said it follows the right edge moves
        -- with the right edge, and one that said nothing stays put.
        --
        -- The compositor has already thrown away the old surface and filled
        -- the new one, so this repaints unconditionally rather than only
        -- when something moved. There is nothing underneath to keep.
        --
        -- **And the window's own size with it.** This laid the tree out and
        -- left `self.w` and `self.h` as they were, so after a drag on the
        -- grip the window's own numbers were the ones it opened with - and
        -- `window.width` reads the root view instead, which is why nothing
        -- caught it. Found writing `window:resize`, where the same two
        -- fields are taken from the reply.
        --
        self.w, self.h = ev.w, ev.h
        self.root:resize(ev.w, ev.h)

        if self.on_resize then pcall(self.on_resize, self, ev.w, ev.h) end

        changed = true
      elseif ev.type == "moved" then
        --
        -- The window was moved - by a drag, or by whoever asked. Menus are
        -- windows placed on the *screen*, so anything that opens one needs
        -- this or it opens where the window used to be.
        --
        self.origin_x, self.origin_y = ev.x, ev.y
        self.x, self.y = ev.x, ev.y
      elseif ev.type == "mouse" and ev.menu then
        -- Tagged with a menu handle by the window manager, so it belongs to
        -- the open menu rather than to any widget in this window.
        if self:menu_mouse(ev) then changed = true end
      elseif ev.type == "mouse" then
        if dispatch_mouse(self, ev) then changed = true end
      elseif ev.type == "drop" then
        -- Something was let go over this window. The desktop found it; what
        -- was carried is a string this window and whoever sent it agree
        -- about, and the desktop never looked inside it.
        if dispatch_drop(self, ev) then changed = true end
      elseif ev.type == "dropped" then
        -- The other end of that, back at the source: what the destination
        -- did with what it was handed.
        if self.on_dropped then
          self.on_dropped(self, ev.ok, ev.count, ev.error)
          changed = true
        end
      elseif ev.type == "key" then
        -- One byte in, up to *two* codes out - see `ui.key_decoder`: an
        -- Escape that turned out not to start a sequence resolves both
        -- itself and the byte that disproved it.
        local a, b = decode_key(ev.code)

        if a and dispatch(self, a) then changed = true end
        if b and dispatch(self, b) then changed = true end
      elseif EDITS[ev.type] then
        -- Select-all, copy, cut, paste, from the window manager's prefix.
        --
        -- **Not a key**, even though a key is what produced it. The keys
        -- are the window manager's - it is the process that decided this
        -- machine has no Control-C to spare - and what arrives here is the
        -- intent it decided on. So a widget implements `edit`, and never
        -- has to know which keys a board happens to have.
        if self:dispatch_edit(ev.type) then changed = true end
      elseif self.on_event then
        --
        -- **Anything else, handed to the application.**
        --
        -- Until this existed the chain above ended in silence: an event the
        -- kit did not recognise was dropped, so the window manager had no
        -- way to tell an application anything the kit had not been taught.
        --
        -- That is how the Windows key ended up wedging the desktop. With no
        -- way to *post* to the Deskbar, the compositor called it instead -
        -- and a synchronous call from the key path to a process that does
        -- not answer stops the whole machine reading the keyboard. `post`
        -- was always the right mechanism; there was just nowhere for it to
        -- land.
        --
        -- Returning true means it was handled and the window should redraw.
        --
        if self.on_event(self, ev) then changed = true end
      end
    end

    -- A replicant with a `tick` is something that changes on its own - a
    -- clock is the archetype - so the window repaints on a slow clock of its
    -- own rather than only when a key arrives.
    if self.ticking and #self.ticking > 0 then
      local now = sys.ticks()

      if now - (self.last_tick or 0) > self.tick_every then
        self.last_tick = now

        for _, r in ipairs(self.ticking) do r:tick() end

        changed = true
      end
    end

    if changed then self:paint() end

    sys.yield()
  end

  -- However the loop ended. `close` tells the window manager and the
  -- registry, and both calls are harmless if the thing being told is
  -- already gone.
  self:close()
end

return ui
