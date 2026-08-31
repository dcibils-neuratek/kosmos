-- The window manager: windows, decoration, stacking, focus, and the
-- compositor underneath them.
--
--   wm                    an empty desktop
--   wm hello-win          start /bin/hello-win.lua in a window
--   wm hello-win,stuck    two applications, one of which hangs
--   wm gallery,setprop:/app/gallery/title=hello
--                         and one that changes the other's title
--
-- Control-C gives the screen back to the shell.
--
--------------------------------------------------------------------------
-- Why this is a separate process from the applications it draws.
--
-- The definition of done for this milestone is BeOS's test: drag a window
-- with a hung application inside it and have the window keep moving. That
-- is not a performance question, it is a question about who owns the
-- pixels. Here the application owns none of them. It sends a list of
-- drawing commands - `ui.md` 16.6, commands and not a shared buffer - and
-- this process renders them into a surface it owns and keeps.
--
-- So an application that stops answering changes nothing. Its window still
-- has its last contents, because those contents were never in its address
-- space, and this process never waits for it: every request is taken with a
-- non-blocking receive, and one that is not there is simply not there.
--
-- The BeOS app_server did the opposite - it handed the application a
-- pointer into its own buffer with a lock around it - and that is exactly
-- the arrangement that makes a hung application freeze a desktop.
--------------------------------------------------------------------------

local KEY_TAB    = 9
local KEY_ESC    = 27
local KEY_CTRL_C = 3
local KEY_PREFIX = 23      -- Control-W

local DESKTOP    = 0xff1c2530
local TAB_H      = 20
local BORDER     = 2

local FOCUSED    = 0xffffc700     -- the BeOS yellow, near enough
local UNFOCUSED  = 0xffb8b8b8
local FRAME      = 0xff2b3440
local TITLE_FG   = 0xff101010

local screen = gfx.screen()

if not screen then
  print("wm: this process was not given the screen")
  return
end

local W, H = screen:size()

-- The backbuffer. Everything is composed here and only rectangles that
-- changed are copied out - `ui.md` 16.7. Under QEMU the framebuffer is
-- ordinary cached RAM and a full copy is not expensive, so this buys
-- nothing in speed here and is not pretending to: what it buys is that a
-- frame is never seen half-composed, and it is the shape the hardware
-- needs. On a real board the scanout buffer is often uncached, where
-- drawing into it directly is the mistake that costs 10-50x.
local back = gfx.surface{ w = W, h = H }

--
-- The screen is this process's now.
--
-- Until this call the kernel console is also drawing here, and the two
-- cannot share: printing one line scrolls the whole display, which drags
-- every window up sixteen pixels and leaves a copy of its tab behind. It
-- reads as a compositor bug and is not - the console's scroll moves every
-- pixel, because as far as it knows every pixel is text.
--
-- Nothing stops being reported. The console keeps writing to the serial
-- line, which is where a system with a window manager on it is debugged
-- from, and a panic takes the screen back regardless.
--
sys.screen_take(true)

local ep = sys.endpoint()

if not ep then
  print("wm: no endpoint")
  return
end

--------------------------------------------------------------------------
-- Windows, back to front. The last one is on top and has the focus.
--------------------------------------------------------------------------

local windows = {}
local by_handle = {}
local next_handle = 1
local damage = {}
local running = true

local function add_damage(x, y, w, h)
  if w <= 0 or h <= 0 then return end
  damage[#damage + 1] = { x = x, y = y, w = w, h = h }
end

-- Everything a window occupies on screen, decoration included. Used for
-- damage, so it has to be the outside of the outermost thing drawn.
local function frame_of(win)
  return win.x - BORDER,
         win.y - TAB_H,
         win.w + BORDER * 2,
         win.h + TAB_H + BORDER
end

local function damage_window(win)
  add_damage(frame_of(win))
end

local function tab_width(win)
  return math.min(win.w + BORDER * 2, #win.title * gfx.font.w + 16)
end

--------------------------------------------------------------------------
-- The drawing commands an application may send.
--
-- A small set on purpose. Each one is a primitive that already exists in
-- C, so this table is a name-to-primitive map and not an interpreter -
-- there is nothing here that loops over pixels in Lua, which is the rule
-- `gfx.md` 19.2 exists to keep.
--
-- Anything unrecognised is skipped rather than refused. An application
-- built against a later version of this list should lose a rectangle, not
-- its window.
--------------------------------------------------------------------------

local ops = {
  fill = function(s, o)
    s:fill(o.x or 0, o.y or 0, o.w or 0, o.h or 0, o.color or 0xff000000)
  end,

  text = function(s, o)
    s:text(o.x or 0, o.y or 0, tostring(o.s or ""),
           o.color or 0xffffffff, o.bg)
  end,

  blend = function(s, o)
    s:blend(o.x or 0, o.y or 0, o.w or 0, o.h or 0, o.color or 0)
  end,
}

--------------------------------------------------------------------------
-- Compositing.
--
-- One rectangle at a time: desktop, then every window that overlaps it,
-- back to front, then the copy out. Windows are opaque, so there is no
-- blending between them and the order is the whole of the occlusion.
--------------------------------------------------------------------------

local function compose_rect(r)
  back:fill(r.x, r.y, r.w, r.h, DESKTOP)

  for i = 1, #windows do
    local win = windows[i]
    local focused = (i == #windows)

    -- The frame, then the tab, then the contents. Drawn whole and clipped
    -- by the surface primitives rather than intersected here: the clip is
    -- in C, exact, and already tested.
    local fx, fy, fw, fh = frame_of(win)
    back:fill(fx, fy, fw, fh, FRAME)

    -- The BeOS tab: as wide as its title and no wider, so the titles of
    -- several stacked windows are all readable at once. It is the most
    -- recognisable decision in the whole look and it is a functional one.
    local tw = tab_width(win)
    back:fill(fx, fy, tw, TAB_H, focused and FOCUSED or UNFOCUSED)
    back:text(fx + 8, fy + (TAB_H - gfx.font.h) // 2, win.title,
              TITLE_FG, focused and FOCUSED or UNFOCUSED)

    back:blit(win.surface, 0, 0, win.w, win.h, win.x, win.y)
  end

  screen:blit(back, r.x, r.y, r.w, r.h, r.x, r.y)
end

local function compose()
  if #damage == 0 then return end

  for _, r in ipairs(damage) do
    compose_rect(r)
  end

  damage = {}
end

--------------------------------------------------------------------------
-- The protocol.
--------------------------------------------------------------------------

local function focused_window()
  return windows[#windows]
end

local function raise(win)
  if windows[#windows] == win then return end

  for i, w_ in ipairs(windows) do
    if w_ == win then
      table.remove(windows, i)
      break
    end
  end

  windows[#windows + 1] = win
  damage_window(win)
end

local handlers = {}

handlers.open = function(req)
  local w_ = math.min(math.max(tonumber(req.w) or 320, 32), W - 8)
  local h_ = math.min(math.max(tonumber(req.h) or 200, 32), H - TAB_H - 8)

  local win = {
    handle  = next_handle,
    title   = tostring(req.title or "window"),
    x       = math.min(math.max(tonumber(req.x) or 40, BORDER), W - w_ - BORDER),
    y       = math.min(math.max(tonumber(req.y) or 40, TAB_H), H - h_ - BORDER),
    w       = w_,
    h       = h_,
    surface = gfx.surface{ w = w_, h = h_ },
    events  = {},
  }

  win.surface:fill(0, 0, w_, h_, 0xff202020)

  next_handle = next_handle + 1
  windows[#windows + 1] = win
  by_handle[win.handle] = win
  damage_window(win)

  return { ok = true, window = win.handle, w = w_, h = h_ }
end

handlers.draw = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  for _, o in ipairs(req.ops or {}) do
    local fn = ops[o.op]
    if fn then fn(win.surface, o) end
  end

  damage_window(win)
  return { ok = true }
end

handlers.retitle = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  -- Both the old tab and the new one: a shorter title leaves the tail of
  -- the longer one behind, and the tab is as wide as its text.
  damage_window(win)
  win.title = tostring(req.title or win.title)
  damage_window(win)

  return { ok = true }
end

handlers.move = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  -- The old place has to be repainted as well as the new one, or the window
  -- leaves a copy of itself behind.
  damage_window(win)
  win.x = math.min(math.max(tonumber(req.x) or win.x, BORDER), W - win.w - BORDER)
  win.y = math.min(math.max(tonumber(req.y) or win.y, TAB_H), H - win.h - BORDER)
  damage_window(win)

  return { ok = true }
end

handlers.poll = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  local out = win.events
  win.events = {}
  return { ok = true, events = out }
end

handlers.close = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  damage_window(win)
  by_handle[req.window] = nil

  for i, w_ in ipairs(windows) do
    if w_ == win then
      table.remove(windows, i)
      break
    end
  end

  win.surface:free()
  return { ok = true }
end

--
-- Everything the window manager did not claim goes to the window with the
-- focus, as an event it can collect whenever it gets round to asking.
-- Queued and not delivered: delivering would mean calling the application,
-- and calling it is what this process must never do.
--
local function to_focused(c)
  local win = focused_window()

  if not win then return end

  win.events[#win.events + 1] = { type = "key", code = c }

  if #win.events > 64 then
    -- An application that has stopped collecting its events is not going to
    -- start. Dropping the oldest is better than growing without limit in a
    -- process that everything else on the screen depends on.
    table.remove(win.events, 1)
  end
end

--------------------------------------------------------------------------
-- Input.
--
-- `ui.md` 16.7: input is never behind anything else. Here that is not a
-- thread priority but the shape of the loop - keys are read every pass,
-- before any application is served, and nothing in this loop can block.
--
-- Until there is a pointer device the keyboard does the dragging:
--
--   the pointer               click to raise, drag a title bar to move
--   Control-W then an arrow    move the focused window
--   Control-W then Tab         focus the next window
--   Control-W then Control-W   a literal Control-W to the application
--   Control-C                  give the screen back
--
-- Everything else belongs to the application. See `prefixed` below for why
-- there is a prefix key at all rather than a handful of reserved ones.
--
-- Arrows arrive as three bytes - escape, [, then A to D - which is the one
-- piece of terminal grammar this has to know, and it knows it here rather
-- than in the console because the console has no idea what a window is.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- The pointer.
--
-- The tablet reports absolute position in a range of its own - 0 to 32767
-- on this machine, whatever the display is - so the scaling happens here,
-- where the size of the screen is known. The kernel passes the range out
-- undecoded for exactly this reason.
--
-- Absolute rather than relative is the right kind of device for a virtual
-- machine: there is no acceleration curve to agree on with the host, so the
-- guest cursor cannot drift away from the real one.
--
-- The cursor is drawn onto the screen *after* the composite, not into the
-- backbuffer. It is not part of the scene - nothing is ever drawn on top of
-- it and it must not be scrolled or blitted with anything - so putting it
-- in the backbuffer would mean every window redraw fighting it. What that
-- costs is one rectangle of damage where it used to be, which is the
-- cheapest possible way to erase it.
--------------------------------------------------------------------------

local CURSOR_W, CURSOR_H = 10, 16

-- An arrow. Two colours so it stays visible on a light window and on the
-- dark desktop: white body, dark outline.
local CURSOR = {
  "X.........",
  "XX........",
  "XoX.......",
  "XooX......",
  "XoooX.....",
  "XooooX....",
  "XoooooX...",
  "XooooooX..",
  "XoooooooX.",
  "XooooooooX",
  "XoooooXXXX",
  "XooXooX...",
  "XoX.XooX..",
  "XX..XooX..",
  "X....XooX.",
  "......XX..",
}

local pointer_x, pointer_y = 0, 0
local drawn_x, drawn_y = -1, -1
local buttons = 0
local dragging = nil          -- { win, dx, dy } while a title bar is held

local function draw_cursor()
  for row = 0, CURSOR_H - 1 do
    local line = CURSOR[row + 1]

    for col = 0, CURSOR_W - 1 do
      local ch = line:sub(col + 1, col + 1)

      if ch ~= "." then
        screen:fill(pointer_x + col, pointer_y + row, 1, 1,
                    (ch == "X") and 0xff000000 or 0xffffffff)
      end
    end
  end

  drawn_x, drawn_y = pointer_x, pointer_y
end

-- Which window is under a point, front to back, decoration included.
local function window_at(x, y)
  for i = #windows, 1, -1 do
    local win = windows[i]
    local fx, fy, fw, fh = frame_of(win)

    if x >= fx and x < fx + fw and y >= fy and y < fy + fh then
      return win, fx, fy
    end
  end

  return nil
end

local function pointer_pass()
  local p = fs.pointer("/dev/console")

  if not p then return end

  local range_x = (p.max_x - p.min_x)
  local range_y = (p.max_y - p.min_y)

  if range_x <= 0 or range_y <= 0 then return end

  local nx = (p.x - p.min_x) * (W - 1) // range_x
  local ny = (p.y - p.min_y) * (H - 1) // range_y

  local was_down = (buttons & 1) ~= 0
  local is_down = (p.buttons & 1) ~= 0

  if nx ~= pointer_x or ny ~= pointer_y then
    -- Where it was has to be repainted from the backbuffer; where it is now
    -- gets the arrow after the composite.
    if drawn_x >= 0 then
      add_damage(drawn_x, drawn_y, CURSOR_W, CURSOR_H)
    end

    pointer_x, pointer_y = nx, ny
  end

  if is_down and not was_down then
    --
    -- A press. Raise whatever is under it, and if that was the title bar,
    -- start a drag.
    --
    local win, fx, fy = window_at(nx, ny)

    if win then
      raise(win)

      if ny < fy + TAB_H then
        dragging = { win = win, dx = nx - win.x, dy = ny - win.y }
      end
    end
  elseif not is_down and was_down then
    dragging = nil
  end

  if dragging and is_down then
    handlers.move{ window = dragging.win.handle,
                   x = nx - dragging.dx, y = ny - dragging.dy }
  end

  buttons = p.buttons
end

local STEP = 16
local prefix = false
local pending_escape = {}

local function move_focused(dx, dy)
  local win = focused_window()
  if not win then return end
  handlers.move{ window = win.handle, x = win.x + dx, y = win.y + dy }
end

--
-- A prefix key, rather than a set of reserved ones.
--
-- The first version took Tab for "next window" and the arrows for "move the
-- window". Both were wrong, and the gallery showed it in one screenshot:
-- Tab is how every user interface ever built moves between controls, and
-- the arrows are how every list is used. A window manager that keeps them
-- has decided that no application may have a second control.
--
-- There are no modifiers to escape into. A virtio keyboard gives Control
-- plus a letter and nothing else - no Alt, no Super, and Control plus an
-- arrow is a terminal escape sequence this system does not speak. So this
-- takes the approach screen and tmux took for exactly the same reason:
-- **one** key is reserved, and it introduces a command rather than being
-- one.
--
--   Control-W then an arrow    move the focused window
--   Control-W then Tab         focus the next window
--   Control-W then Control-W   send a literal Control-W to the application
--
-- One key out of the application's vocabulary instead of five, and the one
-- taken is the one applications want least.
--
local function prefixed(c)
  prefix = false

  if c == KEY_TAB then
    if #windows > 1 then raise(windows[1]) end
    return
  end

  if c == KEY_PREFIX then
    to_focused(KEY_PREFIX)
    return
  end

  if c == KEY_ESC then
    -- An arrow after the prefix. The escape belongs to the sequence, not to
    -- the application, so the prefix stays on until the sequence finishes.
    prefix = true
    pending_escape = { KEY_ESC }
    return
  end
end

local function key(c)
  if c == KEY_CTRL_C then
    running = false
    return
  end

  -- Halfway through an escape sequence that began after the prefix.
  if #pending_escape > 0 then
    if #pending_escape == 1 then
      if c == 91 then                                   -- '['
        pending_escape[2] = c
        return
      end

      pending_escape = {}
      prefix = false
      to_focused(c)
      return
    end

    pending_escape = {}
    prefix = false

    if c == 65 then move_focused(0, -STEP) return end   -- A, up
    if c == 66 then move_focused(0,  STEP) return end   -- B, down
    if c == 67 then move_focused( STEP, 0) return end   -- C, right
    if c == 68 then move_focused(-STEP, 0) return end   -- D, left
    return
  end

  if prefix then
    prefixed(c)
    return
  end

  if c == KEY_PREFIX then
    prefix = true
    return
  end

  to_focused(c)
end

--------------------------------------------------------------------------
-- Start whatever was asked for, each in a process of its own, each handed
-- this endpoint under a name. They cannot reach it any other way and they
-- cannot pass it anywhere this process did not.
--------------------------------------------------------------------------

-- `name` or `name:arguments`, comma separated. The colon is so a program
-- started here can be given something to work on - `wm gallery,setprop:...`
-- - without this having to understand a shell's quoting rules, which it is
-- not and should not become.
for entry in tostring(args or ""):gmatch("[^,]+") do
  entry = entry:match("^%s*(.-)%s*$")

  if entry ~= "" then
    local name, argument = entry:match("^([^:]+):(.*)$")
    name = name or entry

    local path = name:sub(1, 1) == "/" and name or ("/bin/" .. name .. ".lua")
    local ok, err = run(path, argument or "", true, { ["/dev/wm"] = ep })

    if not ok then
      print(("wm: could not start %s: %s"):format(path, tostring(err)))
    end
  end
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

add_damage(0, 0, W, H)

while running do
  -- 1. Input, always first.
  local keys = fs.keys("/dev/console") or {}

  for _, c in ipairs(keys) do
    key(c)
  end

  -- 2. Whatever the applications have asked for, and not one message more
  -- than has already arrived.
  while true do
    local req, who = sys.receive(ep, true)
    if not req then break end

    local handler = handlers[req.type]
    local reply

    if not handler then
      reply = { ok = false, error = "no such operation: " .. tostring(req.type) }
    else
      local ok, result = pcall(handler, req)
      reply = ok and result or { ok = false, error = tostring(result) }
    end

    pcall(sys.reply, who, reply)
  end

  -- 3. The pointer, before the picture: a click can raise a window and a
  -- drag can move one, and both are damage that this pass should draw.
  pointer_pass()

  -- 4. The picture, and then the cursor on top of it.
  compose()
  draw_cursor()

  sys.yield()
end

-- Given back, which repaints: the console has no scrollback, so it starts
-- again from the top rather than restoring something nobody kept.
sys.screen_take(false)

back:free()

for _, win in ipairs(windows) do
  win.surface:free()
end

sys.destroy(ep)
