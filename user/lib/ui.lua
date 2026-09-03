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

local ui = { theme = theme }

local GW, GH = gfx.font.w, gfx.font.h

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
function gc:text(x, y, s, color, bg, role)
  color, bg = shade(color), shade(bg)
  local ax, ay = self.ox + x, self.oy + y

  local GW, GH = GW, GH

  -- A role's own metrics, for the clipping below. The widget font's cell is
  -- the wrong ruler for a face that is not the widget font, and clipping by
  -- the wrong cell width drops characters that would have fitted.
  if role then
    GW = math.max(1, gfx.measure("0", role))
    GH = gfx.height(role)
  end

  if ay + GH <= self.cy or ay >= self.cy + self.ch then return end

  -- Clipped by the character rather than by the pixel: a half glyph is
  -- worse than a missing one, and the alternative is a clip rectangle in
  -- the blitter, which is `gfx`'s business and not this file's.
  local skip = 0

  if ax < self.cx then
    skip = (self.cx - ax + GW - 1) // GW
    ax = ax + skip * GW
  end

  local room = (self.cx + self.cw - ax) // GW

  if room <= 0 then return end

  local shown = s:sub(skip + 1, skip + room)

  if shown == "" then return end

  self.ops[#self.ops + 1] = { op = "text", x = ax, y = ay,
                              s = shown, color = color, bg = bg,
                              role = role }
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
function gc:icon(x, y, name, size)
  size = size or 32

  local ax, ay = self.ox + x, self.oy + y

  local x0 = (ax > self.cx) and ax or self.cx
  local y0 = (ay > self.cy) and ay or self.cy
  local x1 = math.min(ax + size, self.cx + self.cw)
  local y1 = math.min(ay + size, self.cy + self.ch)

  if x1 <= x0 or y1 <= y0 then return end

  self.ops[#self.ops + 1] = {
    op = "image", asset = name, alpha = true,
    sx = x0 - ax, sy = y0 - ay,
    w = x1 - x0, h = y1 - y0,
    x = x0, y = y0,
  }
end

-- A one-pixel frame, which is what this kit uses instead of a bevel.
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

function gc:raised(x, y, w, h, face)
  if face then self:fill(x, y, w, h, face) end

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

local SCROLL_W = 14

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

local function has_arrows(h)
  return h >= 4 + 2 * ARROW + 24
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

local function draw_scrollbar(g, w, h, total, shown, top)
  local y, size = thumb_of(h, total, shown, top)

  if not y then return false end

  local x = w - SCROLL_W - 2

  g:sunken(x, 2, SCROLL_W, h - 4, "window")
  g:raised(x + 1, y, SCROLL_W - 2, size, "raised")

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

    if n.open and n.kids then
      tree_rows(n.kids, depth + 1, out)
    end
  end

  return out
end

function ui.tree(spec)
  local v = ui.view(spec)

  v.w = v.w > 0 and v.w or 180
  v.h = v.h > 0 and v.h or (GH * 8)
  v.focusable = true
  v.roots = v.roots or {}
  v.top = 1
  v.chosen = nil

  function v:draw(g)
    g:sunken(0, 0, self.w, self.h, "sunken")

    if self.focused then
      g:frame(1, 1, self.w - 2, self.h - 2, "ring")
    end

    local rows = tree_rows(self.roots, 0, {})
    local shown = (self.h - 4) // GH

    self.rows = rows
    self.shown = shown

    if self.top > #rows - shown + 1 then self.top = #rows - shown + 1 end
    if self.top < 1 then self.top = 1 end

    self.bar = draw_scrollbar(g, self.w, self.h, #rows, shown, self.top)

    local room = self.w - 4 - (self.bar and SCROLL_W + 2 or 0)

    for i = 0, shown - 1 do
      local r = rows[self.top + i]

      if not r then break end

      local y = 2 + i * GH
      local x = 4 + r.depth * TREE_INDENT
      local on = (r.node == self.chosen)
      local bg = on and theme.accent or theme.sunken
      local fg = on and theme.text_on or theme.text

      if on then g:fill(2, y, room, GH, bg) end

      --
      -- The marker, and only on something that can be opened. Built from
      -- fills like the menu's: pointing right when shut and down when open,
      -- which is the one convention every tree in every system shares.
      --
      if r.node.children or r.node.kids then
        local ax, ay = x, y + (GH - 5) // 2

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

      g:text(x + TREE_ARROW, y, tostring(r.node.text or "?"), fg, bg)
    end
  end

  function v:mouse(action, x, y)
    local to = ui.scrollbar_mouse(self, action, x, y, self.w, self.h,
                                  #(self.rows or {}), self.shown or 1,
                                  self.top)

    if to then
      self.top = to

      return true
    end

    if action ~= "press" then return true end

    local r = self.rows and self.rows[self.top + (y - 2) // GH]

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
  local v = ui.view(spec)

  v.w = v.w > 0 and v.w or 6
  v.follow = v.follow or { left = true, top = true, bottom = true }

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

    if x >= c.x and x < c.x + c.w and y >= c.y and y < c.y + c.h then
      return c:hit(x - c.x, y - c.y)
    end
  end

  return self, x, y
end

-- Depth first, in tree order, which is the order Tab moves in.
function view:focusables(out)
  out = out or {}

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
  v.h = v.h > 0 and v.h or GH

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

  function v:measure()
    if not self.fixed_width then
      self.w = gfx.measure(tostring(self.text or ""))
    end
  end

  v:measure()

  function v:draw(g)
    g:text(0, 0, tostring(self.text or ""),
           shade(self.color) or theme.text, shade(self.bg))
  end

  return v
end

function ui.button(spec)
  local v = ui.view(spec)
  v.h = v.h > 0 and v.h or (GH + 10)
  v.w = v.w > 0 and v.w or (gfx.measure(tostring(v.text or "")) + 24)
  v.focusable = true

  function v:draw(g)
    local face = self.pressed and theme.accent or theme.raised

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
    if self.focused then
      g:frame(1, 1, self.w - 2, self.h - 2, "ring")
    end

    local label = tostring(self.text or "")
    local tx = (self.w - gfx.measure(label)) // 2
    local ty = (self.h - GH) // 2

    -- And the label moves with it, a pixel down and right, because a
    -- control that goes in takes its label with it.
    if self.pressed then tx, ty = tx + 1, ty + 1 end

    g:text(tx, ty, label,
           self.pressed and theme.text_on or theme.text, face)
  end

  function v:key(c)
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
  v.h = v.h > 0 and v.h or GH
  v.w = v.w > 0 and v.w or (gfx.measure(tostring(v.text or "")) + 3 * GW)
  v.focusable = true
  v.checked = v.checked or false

  function v:draw(g)
    local box = GH - 2
    g:sunken(0, 1, box, box, "sunken")

    if self.focused then
      g:frame(1, 2, box - 2, box - 2, "ring")
    end

    if self.checked then
      g:fill(3, 4, box - 6, box - 6, theme.good)
    end

    g:text(box + GW, 0, tostring(self.text or ""), theme.text)
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

function ui.field(spec)
  local v = ui.view(spec)
  v.h = v.h > 0 and v.h or (GH + 6)
  v.w = v.w > 0 and v.w or 200
  v.focusable = true
  v.text = v.text or ""
  v.caret = #v.text + 1

  function v:draw(g)
    -- A well: content lives in here, and the bevel says so. The focus
    -- ring goes inside it rather than over it, so a focused field is
    -- still visibly a field.
    g:sunken(0, 0, self.w, self.h, "sunken")

    if self.focused then
      g:frame(1, 1, self.w - 2, self.h - 2, "ring")
    end

    local room = (self.w - 8) // GW
    local from = math.max(1, self.caret - room + 1)
    local shown = self.text:sub(from, from + room - 1)

    g:text(4, (self.h - GH) // 2, shown, theme.text, theme.sunken)

    if self.focused then
      local cx = 4 + (self.caret - from) * GW
      g:fill(cx, (self.h - GH) // 2, 1, GH, theme.ring)
    end
  end

  function v:key(c)
    if c == 8 or c == 127 then
      if self.caret > 1 then
        self.text = self.text:sub(1, self.caret - 2)
                    .. self.text:sub(self.caret)
        self.caret = self.caret - 1
      end
      return true
    end

    if c == 10 or c == 13 then
      if self.on_enter then self.on_enter(self, self.text) end
      return true
    end

    if c >= 32 and c < 127 then
      self.text = self.text:sub(1, self.caret - 1) .. string.char(c)
                  .. self.text:sub(self.caret)
      self.caret = self.caret + 1
      return true
    end

    return false
  end

  -- The caret where the click was, clamped to the end of the text: clicking
  -- past the last character puts it after the last character, which is what
  -- every text field does and what nobody notices until it does not.
  function v:mouse(action, x, y)
    if action == "press" then
      local col = (x - 4) // GW

      if col < 0 then col = 0 end
      self.caret = math.min(col + 1, #self.text + 1)
    end

    return true
  end

  return v
end

function ui.list(spec)
  local v = ui.view(spec)
  v.h = v.h > 0 and v.h or (GH * 6)
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
    -- A well: content lives in here, and the bevel says so. The focus
    -- ring goes inside it rather than over it, so a focused field is
    -- still visibly a field.
    g:sunken(0, 0, self.w, self.h, "sunken")

    if self.focused then
      g:frame(1, 1, self.w - 2, self.h - 2, "ring")
    end

    local rows = (self.h - 4) // GH

    if self.selected < self.top then self.top = self.selected end
    if self.selected > self.top + rows - 1 then
      self.top = self.selected - rows + 1
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
        local y = 2 + i * GH
        local on = (n == self.selected)
        local bg = on and theme.accent or theme.sunken

        if on then g:fill(2, y, room, GH, bg) end

        local tx = 4

        if self.checks then
          local box = GH - 4

          g:sunken(4, y + 2, box, box, "sunken")

          if self.checks[tostring(item)] then
            g:fill(6, y + 4, box - 4, box - 4, theme.good)
          end

          tx = 4 + box + 6
        end

        g:text(tx, y, tostring(item), on and theme.text_on or theme.text, bg)
      end
    end
  end

  function v:key(c)
    -- Arrows arrive already decoded, as negative codes. See `dispatch`.
    if c == -1 then
      self.selected = math.max(1, self.selected - 1)
      return true
    end

    if c == -2 then
      self.selected = math.min(#self.items, self.selected + 1)
      return true
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
    local rows = self.rows or ((self.h - 4) // GH)

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

    local row = (y - 2) // GH
    local n = self.top + row

    if row < 0 or n > #self.items then return true end

    --
    -- The box is its own target. Pressing it toggles and does not select,
    -- because a checklist is read down the boxes and a selection moving
    -- under your eye while you tick things is noise.
    --
    if self.checks and action == "press" and x < 4 + GH + 2 then
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
  v.lines = {}
  v.cy = 1
  v.cx = 1
  v.top = 1
  v.dirty = false

  for line in ((spec.text or "") .. "\n"):gmatch("([^\n]*)\n") do
    v.lines[#v.lines + 1] = line
  end

  if #v.lines > 1 and v.lines[#v.lines] == "" then
    v.lines[#v.lines] = nil
  end

  if #v.lines == 0 then v.lines[1] = "" end

  local GUTTER = 5              -- four digits and a space

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

  local function rows(self)
    return (self.h - 4) // GH
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
    scroll_into_view(self)

    -- A well: content lives in here, and the bevel says so. The focus
    -- ring goes inside it rather than over it, so a focused field is
    -- still visibly a field.
    g:sunken(0, 0, self.w, self.h, "sunken")

    if self.focused then
      g:frame(1, 1, self.w - 2, self.h - 2, "ring")
    end

    local columns = (self.w - 4) // GW - GUTTER

    for row = 0, rows(self) - 1 do
      local n = self.top + row
      local line = self.lines[n]

      if line then
        local y = 2 + row * GH

        g:text(2, y, ("%4d "):format(n), theme.line, theme.sunken)
        g:text(2 + GUTTER * GW, y, line:sub(1, columns), theme.text,
               theme.sunken)
      end
    end

    -- The cursor as a block on the character it is on, which is what makes
    -- the column obvious in indented code.
    if self.focused and self.cy >= self.top
       and self.cy <= self.top + rows(self) - 1 then
      local px = 2 + (GUTTER + math.min(self.cx, columns + 1) - 1) * GW
      local py = 2 + (self.cy - self.top) * GH
      local under = self.lines[self.cy]:sub(self.cx, self.cx)

      g:fill(px, py, GW, GH, theme.ring)

      if under ~= "" then
        g:text(px, py, under, theme.sunken, theme.ring)
      end
    end
  end

  function v:key(c)
    local line = self.lines[self.cy]

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
    if action == "press" or action == "move" then
      local row = (y - 2) // GH

      self.cy = self.top + row
      clamp(self)

      local col = (x - 2) // GW - GUTTER

      if col < 0 then col = 0 end
      self.cx = math.min(col + 1, #self.lines[self.cy] + 1)
    end

    return true
  end

  return v
end

--
-- A picture.
--
--   ui.image{ x =, y =, w =, h =, asset = "test-pattern.png" }
--
-- The picture is *named*, not carried. A decoded image is megabytes and a
-- message is two kilobytes, so the window manager loads it and this asks
-- how big it is - which is the same division as everything else here: the
-- compositor owns pixels, an application says what it wants drawn.
--
-- Bigger than its box is normal - a photograph is - so it pans rather than
-- scaling. There is no scaler, and a nearest-neighbour one written in Lua
-- would be exactly the per-pixel loop `gfx.md` 19.2 forbids. When one
-- arrives it will be a `gfx` primitive and this widget will use it.
--
function ui.image(spec)
  local v = ui.view(spec)

  v.focusable = true
  v.ox = 0
  v.oy = 0

  local iw, ih = 0, 0
  local size = v.asset and fs.send("/dev/wm", { type = "image_size",
                                               asset = v.asset })

  if size then
    iw, ih = size.w, size.h
  end

  v.image_w, v.image_h = iw, ih

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

  function v:draw(g)
    g:sunken(0, 0, self.w, self.h, "sunken")

    if self.image_w == 0 then
      g:text(6, 6, "no picture called " .. tostring(self.asset), theme.bad)
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

    g.ops[#g.ops + 1] = {
      op = "image", asset = self.asset,
      sx = self.ox + (x0 - ax), sy = self.oy + (y0 - ay),
      w = x1 - x0, h = y1 - y0,
      x = x0, y = y0,
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

  function v:draw(g)
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
  local function clamp(self)
    local most = self.content - self.h + 8

    if most < 0 then most = 0 end
    if self.scroll > most then self.scroll = most end
    if self.scroll < 0 then self.scroll = 0 end
  end

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

  -- What a replicant may see. Deliberately small and deliberately explicit:
  -- adding to this list is granting something to every replicant that will
  -- ever run, so it is a list and not a metatable onto _G.
  local env = {
    gfx = { font = gfx.font },
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

    if not fs.send("/dev/wm", { type = "draw", window = handle,
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
local function apply_fonts(fonts)
  if type(fonts) ~= "table" then return end

  for _, role in ipairs { "ui", "text", "mono" } do
    local want = fonts[role]

    if type(want) == "table" and want.font then
      gfx.use_font(want.font, tonumber(want.px) or 16, role)
      theme.fonts[role] = { font = want.font, px = tonumber(want.px) or 16 }
    end
  end
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

  local reply, err = fs.send("/dev/wm", {
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

    -- And its opposite: a strip across the top, undecorated and pinned,
    -- which takes room away from the screen rather than sitting over it.
    strip = spec.strip or nil,
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
  -- What every window exposes, before the application adds anything.
  --
  -- A getter and a setter, so a property is a live view of the thing and
  -- not a copy that drifts. Writing `title` really renames the window,
  -- because the setter is what renaming is.
  --
  w:publish("title",
            function() return w.title end,
            function(v)
              w.title = tostring(v)
              fs.send("/dev/wm", { type = "retitle", window = w.handle,
                                   title = w.title })
            end)

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

  local reply = fs.send("/dev/wm", {
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

  fs.send("/dev/wm", { type = "move", window = self.handle, x = x, y = y })
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

function window:close()
  if self.closed then return end

  self.closed = true
  self.running = false

  fs.send("/dev/wm", { type = "close", window = self.handle })

  if self.control then
    fs.send("/app", { type = "unregister", name = self.name })
    sys.destroy(self.control)
    self.control = nil
  end
end

local function apply_focus(self)
  local list = self.root:focusables()

  for i, v in ipairs(list) do
    v.focused = (i == self.focus)
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

function ui.menubar(spec)
  local v = ui.view(spec)

  v.h = v.h > 0 and v.h or (GH + 8)
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
    g:fill(0, 0, self.w, self.h, theme.raised)
    g:groove(0, self.h - 2, self.w, 2)

    for i, m in ipairs(self.menus) do
      local s = spans(self)[i]
      local open = (self.open_index == i)

      if open then g:fill(s.x, 0, s.w, self.h - 2, "accent") end

      g:text(s.x + 8, (self.h - 2 - GH) // 2, tostring(m.title or ""),
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
                      self.menus[i].items or {})
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

local function menu_metrics(items)
  local widest = 0
  local deep = false

  for _, it in ipairs(items) do
    -- Measured, not counted. A character count is a width only while every
    -- glyph is the same width, and the interface font need not be.
    local n = gfx.measure(tostring(it.text or ""))

    if n > widest then widest = n end
    if it.submenu then deep = true end
  end

  local row = GH + 6

  return widest + MENU_PAD * 2 + 12 + (deep and MENU_ARROW or 0),
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

  for i, it in ipairs(m.items) do
    local y = 2 + (i - 1) * m.row

    if it.separator then
      g:groove(MENU_PAD, y + m.row // 2, m.w - MENU_PAD * 2, 2)
    else
      local hot = (i == m.hot)
      local bg  = hot and theme.accent or theme.raised

      if hot then g:fill(2, y, m.w - 4, m.row, "accent") end

      g:text(MENU_PAD + 4, y + (m.row - GH) // 2, tostring(it.text or ""),
             hot and theme.text_on or theme.text, bg)

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

  local reply = fs.send("/dev/wm", { type = "open", kind = "menu",
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

-- Everything from `from` upward, closed. `close_menus()` closes the lot.
function window:close_menus(from)
  from = from or 1

  for i = #self.menus, from, -1 do
    fs.send("/dev/wm", { type = "close", window = self.menus[i].handle })
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

  if ev.action == "release" then
    --
    -- Releasing on something that opens a submenu is not a choice. The
    -- submenu is already open and the pointer is on its way there; closing
    -- everything here would make a menu impossible to reach by sliding,
    -- which is how they are used.
    --
    if item and item.submenu then return false end

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

  g.ops[#g.ops + 1] = { op = "fill", x = 0, y = 0,
                        w = self.root.w, h = self.root.h,
                        color = self.background or theme.window }

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

    local ok = fs.send("/dev/wm", { type = "draw", window = self.handle,
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
local function dispatch_mouse(self, ev)
  if ev.action == "press" then
    local target, lx, ly = self.root:hit(ev.x, ev.y)

    self.grab = nil

    if target and target.mouse then
      self.grab = { view = target, dx = ev.x - lx, dy = ev.y - ly }
    end

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

function window:run()
  local escape = 0

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
    -- `poll_wait` is in scheduler ticks and overrides that. It is not the
    -- default because it is a real cost: a window that wakes a hundred
    -- times a second is a window that is running a hundred times a second.
    --
    local wait = self.poll_wait or self.tick_every or 0

    if wait <= 0 then
      local cpu = fs.read("/dev/cpu")
      wait = (cpu and cpu.counter_hz or 62500000) // 4
    end

    local reply = fs.send("/dev/wm", { type = "poll", window = self.handle,
                                       wait = wait })

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
      elseif ev.type == "key" then
        local c = ev.code

        -- The three bytes of an arrow, turned into one code the widgets can
        -- read. Negative, so it can never collide with a character.
        if escape == 1 then
          escape = (c == 91) and 2 or 0
          c = nil
        elseif escape == 2 then
          escape = 0
          c = ({ [65] = -1, [66] = -2, [67] = -3, [68] = -4 })[c]
        elseif c == 27 then
          escape = 1
          c = nil
        end

        if c and dispatch(self, c) then changed = true end
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
