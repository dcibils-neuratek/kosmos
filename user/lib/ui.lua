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

function gc:text(x, y, s, color, bg)
  local ax, ay = self.ox + x, self.oy + y

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
                              s = shown, color = color, bg = bg }
end

-- A one-pixel frame, which is what this kit uses instead of a bevel.
function gc:frame(x, y, w, h, color)
  self:fill(x, y, w, 1, color)
  self:fill(x, y + h - 1, w, 1, color)
  self:fill(x, y, 1, h, color)
  self:fill(x + w - 1, y, 1, h, color)
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

  -- Which edges are pinned. Left and top by default, which is the
  -- behaviour of something that simply sits where it was put.
  v.follow = v.follow or { left = true, top = true }

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
  local saved = g:push(self.x, self.y, self.w, self.h)

  if self.draw then self:draw(g) end

  for _, c in ipairs(self.children) do
    c:paint(g)
  end

  g:pop(saved)
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
  v.w = v.w > 0 and v.w or (#tostring(v.text or "") * GW)

  function v:draw(g)
    g:text(0, 0, tostring(self.text or ""),
           self.color or theme.text, self.bg)
  end

  return v
end

function ui.button(spec)
  local v = ui.view(spec)
  v.h = v.h > 0 and v.h or (GH + 10)
  v.w = v.w > 0 and v.w or (#tostring(v.text or "") * GW + 24)
  v.focusable = true

  function v:draw(g)
    local face = self.pressed and theme.accent or theme.raised
    g:fill(0, 0, self.w, self.h, face)
    g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)

    local label = tostring(self.text or "")
    local tx = (self.w - #label * GW) // 2
    g:text(tx, (self.h - GH) // 2, label,
           self.pressed and theme.text_on or theme.text, face)
  end

  function v:key(c)
    if c == 10 or c == 13 or c == 32 then
      if self.on_click then self.on_click(self) end
      return true
    end
    return false
  end

  return v
end

function ui.checkbox(spec)
  local v = ui.view(spec)
  v.h = v.h > 0 and v.h or GH
  v.w = v.w > 0 and v.w or (#tostring(v.text or "") * GW + 3 * GW)
  v.focusable = true
  v.checked = v.checked or false

  function v:draw(g)
    local box = GH - 2
    g:fill(0, 1, box, box, theme.sunken)
    g:frame(0, 1, box, box, self.focused and theme.ring or theme.line)

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
    g:fill(0, 0, self.w, self.h, theme.sunken)
    g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)

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

  function v:draw(g)
    g:fill(0, 0, self.w, self.h, theme.sunken)
    g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)

    local rows = (self.h - 4) // GH

    if self.selected < self.top then self.top = self.selected end
    if self.selected > self.top + rows - 1 then
      self.top = self.selected - rows + 1
    end

    for i = 0, rows - 1 do
      local n = self.top + i
      local item = self.items[n]

      if item then
        local y = 2 + i * GH
        local on = (n == self.selected)
        local bg = on and theme.accent or theme.sunken

        if on then g:fill(2, y, self.w - 4, GH, bg) end

        g:text(4, y, tostring(item), on and theme.text_on or theme.text, bg)
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

    if c == 10 or c == 13 then
      if self.on_select then
        self.on_select(self, self.items[self.selected], self.selected)
      end
      return true
    end

    return false
  end

  return v
end

--------------------------------------------------------------------------
-- A window: the root view, plus the conversation with the window manager.
--------------------------------------------------------------------------

local window = {}
window.__index = window

-- How many commands go in one message.
--
-- A message is 2048 bytes and a command is a small table, so a full window
-- is several messages. Batched rather than made to fit: a limit that a
-- slightly busier window silently crosses is a limit that will be crossed.
local BATCH = 16

function ui.window(spec)
  spec = spec or {}

  local reply, err = fs.send("/dev/wm", {
    type = "open",
    title = spec.title or "window",
    w = spec.w or 400, h = spec.h or 240,
    x = spec.x, y = spec.y,
  })

  if not reply then
    return nil, err
  end

  local w = setmetatable({
    handle = reply.window,
    root = ui.view{ x = 0, y = 0, w = reply.w, h = reply.h },
    background = spec.background or theme.window,
    focus = 1,
    running = true,
  }, window)

  return w
end

function window:add(child)
  return self.root:add(child)
end

function window:close()
  self.running = false
  fs.send("/dev/wm", { type = "close", window = self.handle })
end

local function apply_focus(self)
  local list = self.root:focusables()

  for i, v in ipairs(list) do
    v.focused = (i == self.focus)
  end

  return list
end

function window:paint()
  apply_focus(self)

  local g = new_gc()
  g.cw, g.ch = self.root.w, self.root.h

  g.ops[#g.ops + 1] = { op = "fill", x = 0, y = 0,
                        w = self.root.w, h = self.root.h,
                        color = self.background }

  self.root:paint(g)

  for i = 1, #g.ops, BATCH do
    local batch = {}

    for j = i, math.min(i + BATCH - 1, #g.ops) do
      batch[#batch + 1] = g.ops[j]
    end

    local ok = fs.send("/dev/wm", { type = "draw", window = self.handle,
                                    ops = batch })

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

function window:run()
  local escape = 0

  self:paint()

  while self.running do
    local reply = fs.send("/dev/wm", { type = "poll", window = self.handle })

    if not reply then return end

    local changed = false

    for _, ev in ipairs(reply.events) do
      if ev.type == "key" then
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

    if changed then self:paint() end

    sys.yield()
  end
end

return ui
