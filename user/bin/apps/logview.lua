-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Server_Syslog
-- kosmos: section system
-- The system log, in a window.
--
--   wm logview
--
-- Everything this machine has printed, kernel and processes together, in
-- the order it happened.
--
-- One place with all of it, and that is the interesting part. The kernel's
-- boot narration goes through `kputc`; so does every `print` from every
-- program, because a program prints by asking the console server and the
-- console server calls `sys.write`, which is `kputc`. So the kernel keeps a
-- ring of what went past and this reads it.
--
-- Before this, the only complete record was the serial line - which is on
-- the other end of a cable, and is exactly what you do not have when you
-- are looking at the screen.
--
--------------------------------------------------------------------------
-- It looks like the Terminal, because what it shows is console output.
--
-- This was a `ui.text` - a document widget - with a heading above it and a
-- status line below, on the window's own colour. On the ThinkPad that was
-- two faults at once:
--
-- - **Unreadable.** `ui.text` draws body text in `text_dim`, which is for a
--   paragraph of help beside the real text, and in the BeOS palette that is
--   #808080 on the panel grey #d8d8d8.
-- - **Overlapping.** `ui.text` and `ui.label` lay text out on the bitmap
--   font's 8x16 cell and the compositor draws it in the interface face. With
--   a 20-pixel TrueType face chosen, rows sat 16 pixels apart, the heading's
--   line ran into the first row's, and both the heading and the status line
--   were cut short. At the default font the two cells are the same size,
--   which is why no screen the harness photographed ever showed it.
--
-- So it is drawn the way the console is: black in every theme,
-- `console_text`, the monospace face, and every row measured in that face on
-- every draw - the Terminal's arithmetic rather than a widget's. The title
-- bar already says what this window is, so there is no heading to collide
-- with anything.
--------------------------------------------------------------------------

local ui = use("/lib/ui.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

-- A menu bar's row on top, since the View menu arrived: the rows keep
-- their room and the window is one row taller.
local BAR = ui.metrics.row
local W, H = 620, 420 + BAR

local win, err = ui.window{ title = "Log", w = W, h = H, x = 130, y = 110 }

if not win then
  print("logview: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- Colour, by shape.
--
-- Nothing that prints here declares a severity, so the only honest way to
-- pick one out is by what it says.
--
-- **Numbers, not palette names.** The palette's `good` and `bad` are chosen
-- to read against the *window*, which in BeOS is grey: #009800 and #cb0000,
-- dark on dark once the ground is black. The console met that first, and
-- `CONSOLE_COLOURS` in `init.lua` answered it with the dark palette's values
-- because a console is dark in every theme. These are those values, so a
-- line here is the colour it would be in a Terminal.
--------------------------------------------------------------------------

local STAGE = 0xff3fb950            -- a boot stage: "[3/12] processor"
local FAULT = 0xffda3633            -- something that went wrong
local NOTE  = 0xffffc700            -- this window talking about itself

local function colour_of(line)
  --
  -- **After the stamp.** The kernel starts every line in the ring with the
  -- time it was written - `[12.345] ` - and the stage pattern was anchored to
  -- the start of the line, so since the stamps arrived it had matched nothing
  -- and the twelve stages were drawn like every other line.
  --
  local said = line:gsub("^%[%d+%.%d+%] ", "", 1)

  if said:match("^PANIC") or said:match("died") or said:match("[Ee]rror")
     or said:match("could not") then
    return FAULT
  end

  if said:match("^%[%d+/%d+%]") then return STAGE end

  return nil
end

--------------------------------------------------------------------------
-- Rows: the log cut into lines, and lines cut to the width.
--
-- Wrapped rather than clipped. The Terminal clips, because a program that
-- wants a narrow line prints one; a log line is whatever somebody wrote, and
-- a clipped one is information silently missing - the address at the end of
-- a boot line is usually what the log was opened to read.
--
-- By character, in the monospace cell, so a character of several bytes is
-- one column and never cut in half. Built when the text or the width changes
-- and kept otherwise: a repaint for a scroll should not split a log again.
--------------------------------------------------------------------------

local function build(text, columns)
  local rows = {}

  for line in (text .. "\n"):gmatch("([^\n]*)\n") do
    -- Carriage returns are for the serial line's benefit and are noise
    -- here; the screen has no idea what a carriage return is.
    line = line:gsub("\r", "")

    if line ~= "" then
      local colour = colour_of(line)
      local length = utf8.len(line)

      if not length then
        -- Not UTF-8, so the bytes are the characters there are.
        for at = 1, #line, columns do
          rows[#rows + 1] = { text = line:sub(at, at + columns - 1),
                              colour = colour }
        end
      else
        for k = 0, length - 1, columns do
          local from = utf8.offset(line, k + 1)
          local stop = utf8.offset(line, k + columns + 1) or (#line + 1)

          rows[#rows + 1] = { text = line:sub(from, stop - 1),
                              colour = colour }
        end
      end
    end
  end

  return rows
end

--------------------------------------------------------------------------
-- Following the log, and not while somebody is reading it.
--
-- `back` is how many rows the view sits above the newest. At 0 the window
-- follows: whatever the kernel says next is drawn at the bottom. Scrolled up
-- by any amount it is *held*: the text on screen stays what it was when the
-- reader left the bottom, however much is written meanwhile, and a note in
-- the corner says there is more. Back at the bottom - by a key, the bar or a
-- drag - it takes the newest text there is at that moment and follows again.
--
-- **Held rather than adjusted.** The other way is to go on following
-- underneath and add to `back` however many rows arrived, so the same lines
-- stay in view. That needs to know how many arrived, and the kernel does not
-- say: `sys.log` is the last 64 KB of a ring, so once the ring holds more
-- than that, every read has lost lines off the top as well as gained them at
-- the bottom, and telling the two apart means matching strings against each
-- other and hoping no line repeats. Holding is correct by construction and
-- costs one string kept.
--
-- It used to follow by setting `ui.text`'s `scroll` to `1 << 30` and letting
-- the widget clamp it. The clamp measured the height the *previous* draw had
-- found, which on the first draw was nothing, so the window opened at the top
-- of the log and stayed there until the log changed - and then sat one
-- refresh behind it.
--------------------------------------------------------------------------

local ASK = 65536       -- how much of the ring to read; see `refresh`

local fetched = nil     -- exactly what the kernel last handed over
local latest = ""       -- that, from its first whole line: the newest there is
local source = ""       -- what is on screen, which is `latest` unless held
local back = 0          -- rows between the bottom of the view and the newest

local rows, built_from, built_columns = {}, nil, 0

--
-- Pinned to all four edges, like the Terminal's, so a bigger window is a
-- bigger log rather than the same log with a border of window colour.
--
local view = ui.view{ x = 8, y = BAR + 8, w = W - 16, h = H - BAR - 20,
                      follow = { left = true, right = true,
                                 top = true, bottom = true } }

view.focusable = true

--
-- **Its own text size**, from the View menu (`/lib/textsize.lua`): Diego,
-- 22 September, "a way to increase font size in the menu of the log viewer
-- and terminal". Kept in `/home/.logview`. The rows are measured and drawn
-- in `size:face()` at `size:size()`, so a new size rewraps them.
--
local textsize = use("/lib/textsize.lua")
local size = textsize.new(ui, "/home/.logview")

win:add(ui.menubar{
  x = 0, y = 0, w = W,
  follow = { "left", "right", "top" },
  menus = { { title = "View", items = size:items() } },
})

--
-- Where everything is, for this size and this face, with `back` held inside
-- what exists. Returns the row height, how many rows fit, and the first one
-- drawn - one-based, which is what the kit's scroll bar counts in.
--
-- Measured in the *monospace* face every time it is asked, which is what the
-- old window had wrong: the face is a setting, and a window that is open
-- while it changes has to follow it.
--
-- The bar's column is kept whether or not a bar is drawn. Whether there is a
-- bar depends on how many rows there are, which depends on the width, so a
-- width that depended on the bar would be a circle.
--
local function layout(self)
  local face = size:face()
  local MW = math.max(1, gfx.measure("0", face))
  local MH = math.max(1, gfx.height(face))
  local columns = math.max(1, (self.w - 8 - ui.SCROLL_W - 4) // MW)

  if source ~= built_from or columns ~= built_columns then
    rows = build(source, columns)
    built_from, built_columns = source, columns
  end

  local shown = math.max(1, (self.h - 6) // MH)
  local most = math.max(0, #rows - shown)

  if back > most then back = most end
  if back < 0 then back = 0 end

  return MH, shown, math.max(1, #rows - shown - back + 1)
end

-- Move, and follow again on arriving at the bottom.
local function scroll_to(self, to)
  back = to

  layout(self)

  if back == 0 then source = latest end
end

function view:draw(g)
  g:fill(0, 0, self.w, self.h, "console")
  g:frame(0, 0, self.w, self.h, self.focused and theme.ring or "line")

  local MH, shown, first = layout(self)

  for i = 0, shown - 1 do
    local row = rows[first + i]

    if not row then break end

    g:text(4, 3 + i * MH, row.text, row.colour or "console_text",
           "console", "mono", size:size())
  end

  ui.scrollbar(g, self.w, self.h, #rows, shown, first)

  --
  -- Why nothing is moving, in the corner where the Terminal says what it is
  -- running. Only when there is something below to be missing: scrolled up
  -- over a log that has not changed, a view that stays still is not news.
  --
  if back > 0 and source ~= latest then
    local note = "new lines below"
    local right = self.w - ui.SCROLL_W - 2
    local x = right - 8 - gfx.measure(note, size:face())

    g:fill(x - 4, 1, right - (x - 4), MH + 4, "console")
    g:text(x, 3, note, NOTE, "console", "mono", size:size())
  end
end

--
-- Up and down a row; backspace and space a page, the way `ui.text` pages and
-- document readers have paged since before there were mice. Each answer
-- returns true, so the view is painted where it now is in the same pass: a
-- key answered by the next refresh is a key that feels dropped.
--
function view:key(c)
  local _, shown = layout(self)
  local page = math.max(1, shown - 1)

  if c == -1 then scroll_to(self, back + 1) return true end
  if c == -2 then scroll_to(self, back - 1) return true end
  if c == 8 or c == 127 then scroll_to(self, back + page) return true end
  if c == 32 then scroll_to(self, back - page) return true end

  return false
end

--
-- The pointer: the kit's scroll bar, and dragging the text itself, which is
-- what `ui.text` offered and what a pointer with no wheel has instead.
--
function view:mouse(action, x, y)
  local MH, shown, first = layout(self)
  local top = ui.scrollbar_mouse(self, action, x, y, self.w, self.h,
                                 #rows, shown, first)

  if action == "release" then self.drag_from = nil end

  if top then
    scroll_to(self, #rows - shown - top + 1)
    return true
  end

  if action == "press" then
    self.drag_from, self.drag_back = y, back
  elseif action == "move" and self.drag_from then
    -- The text moves with the pointer, so dragging down shows older lines.
    -- Rounded towards zero both ways, so a pixel of jitter is not a row.
    local d = y - self.drag_from
    local by = (d >= 0) and (d // MH) or -((-d) // MH)

    scroll_to(self, self.drag_back + by)
  end

  return true
end

--------------------------------------------------------------------------
-- Reading the ring.
--
-- A tail rather than a subscription: the kernel keeps a ring and this reads
-- the end of it twice a second. A subscription would mean the kernel calling
-- a process, which is the thing this system is arranged not to do - and a
-- log viewer that could block the kernel would be a poor trade for half a
-- second of latency.
--
-- 64 KB and not the whole ring. The ring is a quarter of a megabyte, and all
-- of it twice a second is half a megabyte a second of copying and Lua string
-- for a window that shows twenty lines. `log` at the prompt reads the whole
-- ring, because it runs once when somebody types it. If this ever matters,
-- the fix is for the kernel to say how much it has written, so this can ask
-- only when that changed.
--
-- A read that fills the request starts wherever the ring was cut, which is
-- almost never at a line, so its first line is dropped rather than drawn as
-- a fragment.
--
-- Returns whether the window has anything new to paint.
--------------------------------------------------------------------------

local function refresh()
  local text = sys.log(ASK)

  if not text or text == fetched then return false end

  fetched = text

  if #text >= ASK then
    text = text:match("\n(.*)$") or text
  end

  local behind = (source ~= latest)

  latest = text

  if back == 0 then
    source = latest
    return true
  end

  -- Held: the rows do not change, so the only thing to paint is the note,
  -- and only the first time there is something for it to say.
  return not behind
end

--
-- Every pass, and not as a view with a `tick`.
--
-- It was a 0x0 view with a `tick`, and to the kit having one means "this
-- changes on its own, like a clock": the window was marked for a repaint on
-- every tick whether or not the log had moved, so an idle Log View sent every
-- row it shows to the compositor twice a second for as long as it was open.
-- The Terminal found the same thing about itself; `terminal.lua` has the
-- account, at the end.
--
-- `on_frame` repaints only when it says something changed. The clock is
-- still half a second - `tick_every` with nothing ticking sets only how long
-- a pass waits - and the read is rate-limited here as well, because a pass
-- also runs for every key and every step of a drag, and a scroll should not
-- copy the ring.
--
local HALF = ((fs.read("/dev/cpu") or {}).counter_hz or 62500000) // 2
local due = 0

function win:on_frame()
  local now = sys.ticks()

  if now < due then return false end

  -- A little under the pass interval, so a pass that wakes a moment early is
  -- not skipped and the refresh stretched to a second.
  due = now + HALF * 3 // 4

  return refresh()
end

win.tick_every = HALF

win:add(view)

-- The log is on the first paint, not half a second after it.
refresh()

win:run()
