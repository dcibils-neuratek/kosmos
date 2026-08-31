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
      self.w = #tostring(self.text or "") * GW
    end
  end

  v:measure()

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

  --
  -- Press picks the row, release on that same row chooses it.
  --
  -- Two steps rather than one so that pressing on the wrong row and sliding
  -- to the right one before letting go does what you meant, which is the
  -- same bargain the button makes.
  --
  function v:mouse(action, x, y)
    local row = (y - 2) // GH
    local n = self.top + row

    if row < 0 or n > #self.items then return true end

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

    g:fill(0, 0, self.w, self.h, theme.sunken)
    g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)

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
    g:fill(0, 0, self.w, self.h, theme.sunken)

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

  return cost
end

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
    title = spec.title or "window",
    x = spec.x or 0,
    y = spec.y or 0,
    properties = {},
    dirty = false,
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
function window:publish(name, get, set)
  self.properties[name] = { get = get, set = set }
end

function window:move(x, y)
  self.x, self.y = x, y
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

function window:paint()
  apply_focus(self)

  local g = new_gc()
  g.cw, g.ch = self.root.w, self.root.h

  g.ops[#g.ops + 1] = { op = "fill", x = 0, y = 0,
                        w = self.root.w, h = self.root.h,
                        color = self.background }

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
    local wait = self.tick_every or 0

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
