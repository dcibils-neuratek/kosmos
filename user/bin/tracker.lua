-- kosmos: application
-- Tracker: the file manager.
--
--   wm tracker            opens at /home
--   wm tracker:/bin       or wherever
--
--   click a row       select it
--   click it again    open it: a directory is entered, a file is opened
--                     in the editor
--   click a heading   sort by that column; again reverses it
--   Backspace         up one level
--
-- The name is BeOS's and so is the job. What used to be called `tracker`
-- here was a replicant host and is now `adopt`.
--
-- It knows nothing about disks. Every question it asks is the ordinary
-- filesystem protocol through its own namespace, so it browses `/home` on
-- the disk, `/data` in memory and `/bin` in the image with the same code,
-- and would browse a directory served from another machine without
-- noticing which it was.
--
-- **There is no Modified column**, and the absence is deliberate rather
-- than unfinished. A file's `mtime` is `sys.ticks()`, the counter since
-- this machine started, so across a reboot it means nothing at all.
-- `design.md` gives dates to `/dev/clock`, which does not exist yet.
-- Printing the number anyway would be a column that looks like a date and
-- is not one.

local ui    = use("/lib/ui.lua")
local files = use("/lib/files.lua")
local types = use("/lib/filetypes.lua")
local theme = ui.theme

local W, H = 700, 460

-- The menu bar's height, which everything below it is offset by. A menu bar
-- is an ordinary widget in this window rather than a band the desktop
-- reserves, so the offset is this program's business - see `ui.menubar`.
local BAR_H = gfx.font.h + 8

--
-- `wm tracker:/bin icons` - a path, then the words that change how it opens.
--
-- `icons` is here rather than only in the View menu because the desktop
-- needs it: the desktop is this program with `desktop`, and a desktop that
-- opened as a list of filenames and had to be told to draw icons would be a
-- desktop that flickered through the wrong thing on every boot.
--
local words = {}

for w in args:gmatch("%S+") do words[#words + 1] = w end

local where = "/home"

for _, w in ipairs(words) do
  if w:sub(1, 1) == "/" then where = w end
end

local as_icons  = false
local backdrop  = false

for _, w in ipairs(words) do
  if w == "icons"   then as_icons = true end
  if w == "desktop" then as_icons, backdrop = true, true end
end

--
-- The desktop is this program with the frame taken off.
--
-- BeOS had no desktop program: the desktop *was* a Tracker window, borderless
-- and screen-sized, at the bottom of the stack. That is the right shape here
-- for a stronger reason than lineage. The window manager knows nothing about
-- files and should not learn - drawing icons would mean teaching it what a
-- directory is, what a file type is, and how to start a program, all of which
-- are already in this file. So the compositor gained a *place* to put a
-- window and no new knowledge, and everything else is here.
--
if backdrop then
  local screen = fs.read("/dev/screen") or {}

  W, H = screen.width or 1024, screen.height or 768

  -- `/home/Desktop` if there is one, the way every system that has a desktop
  -- folder names it. No magic beyond that: if it is not there, the desktop
  -- shows `/home`, which is a real directory rather than an empty rectangle
  -- pretending to be one.
  if where == "/home" and files.entries("/home/Desktop") then
    where = "/home/Desktop"
  end
end

local win, err = ui.window{
  title = "Tracker", w = W, h = H,
  x = backdrop and 0 or 80, y = backdrop and 0 or 60,
  backdrop = backdrop or nil,
}

if not win then
  print("tracker: " .. tostring(err))
  return
end

local GW, GH = gfx.font.w, gfx.font.h

local entries  = {}
local selected = 0

-- Declared here and defined with the toolbar, because the menu bar names
-- the same actions and a menu item and a button doing the same thing should
-- be one function rather than two that drift apart.
local new_folder, delete_selected

-- And the one that changes directory, because the places tree calls it and
-- is built above it.
local show
local sort_by  = "name"
local scroll   = 1        -- the first row shown; the bar moves this

--
-- "list" or "icons", and the View menu changes it.
--
-- The same entries either way - the mode is a *layout*, not a different
-- reading of the directory. That is what makes the desktop possible later:
-- a desktop is this view in icon mode with no frame around it.
--
local mode = as_icons and "icons" or "list"
local reversed = false

local here   = ui.label{ x = 12, y = 10 + BAR_H, w = W - 24, text = where }
local status = ui.label{ x = 12, y = H - 30, w = W - 24, text = "" }

--------------------------------------------------------------------------
-- The columns.
--
-- A view that draws rows itself rather than an `ui.list` of strings,
-- because a file manager's list has *fields* - and a list that formats them
-- into one string cannot sort by one of them or line them up when a name is
-- long. Every fill and every glyph here is still a C primitive; what Lua
-- decides is which row goes where, which is a few dozen decisions a
-- repaint.
--------------------------------------------------------------------------

local COLUMNS = {
  { key = "name", title = "Name", x = 4,   w = 210 },
  { key = "size", title = "Size", x = 220, w = 90  },
  { key = "kind", title = "Kind", x = 316, w = 90  },
}

local function sorted()
  local out = {}

  for i, e in ipairs(entries) do out[i] = e end

  table.sort(out, function(a, b)
    -- Directories stay together whatever the sort, because a directory is a
    -- place and a file is a thing, and interleaving them means reading the
    -- whole list to find where you can go next.
    local a_dir = (a.kind == "directory")
    local b_dir = (b.kind == "directory")

    if a_dir ~= b_dir then return a_dir end

    local x, y

    if sort_by == "size" then
      x, y = a.size, b.size
      if x == y then x, y = a.name, b.name end
    elseif sort_by == "kind" then
      x, y = a.kind, b.kind
      if x == y then x, y = a.name, b.name end
    else
      x, y = a.name, b.name
    end

    if reversed then return x > y end

    return x < y
  end)

  return out
end

--
-- A pane of places on the left, the listing on the right, a grip between.
--
-- The tree is *lazy*: a node's children are read the first time it is
-- opened and kept after that. A tree that loaded eagerly would walk every
-- filesystem on the machine to draw a pane four rows tall, and one of them
-- is a disk.
--
local PLACES_W = 150

local function subdirs(node)
  local out = {}

  for _, e in ipairs(files.entries(node.path) or {}) do
    if e.kind == "directory" then
      local full = files.join(node.path, e.name)

      out[#out + 1] = { text = e.name, path = full, children = subdirs }
    end
  end

  return out
end

--
-- The roots are the mounts, because those are the places this machine
-- actually has - `/bin` in the image, `/data` in memory, `/home` on the
-- disk. Naming them here rather than reading `/` keeps the pane in a
-- sensible order and out of the way of a root that lists something else.
--
local places = ui.tree{
  x = 12, y = 58 + BAR_H, w = PLACES_W, h = H - 128 - BAR_H,
  follow = { "left", "top", "bottom" },
  roots = {
    { text = "home",   path = "/home",   children = subdirs },
    { text = "system", path = "/system", children = subdirs },
    { text = "data",   path = "/data",   children = subdirs },
    { text = "bin",    path = "/bin" },
    { text = "lib",    path = "/lib" },
    { text = "dev",    path = "/dev" },
  },
  on_select = function(_, node) show(node.path) end,
}

local split = ui.splitter{
  x = 12 + PLACES_W, y = 58 + BAR_H, w = 6, h = H - 128 - BAR_H,
  follow = { "left", "top", "bottom" },
}

local rows = ui.view{ x = 12 + PLACES_W + 6, y = 58 + BAR_H,
                      w = W - 24 - PLACES_W - 6,
                      h = H - 128 - BAR_H,
                      follow = { "left", "right", "top", "bottom" } }

--
-- The grip moves the boundary, and the two panes are told their new size
-- rather than working it out: `view:resize` is what applies a follow mode,
-- and a view whose width changed without it would keep drawing at the old
-- one until the window itself was resized.
--
function split:on_move(dx)
  local w = math.min(math.max(80, places.w + dx), self.parent.w - 160)

  places.w = w
  self.x = 12 + w
  rows.x = 12 + w + 6
  rows.w = self.parent.w - 24 - w - 6
end

rows.focusable = true

--
-- How many columns of icons fit, and where one goes.
--
-- Worked out in one place because the drawing and the hit test both need it
-- and disagreeing about it is the bug where you click one icon and open
-- another - the same reason `boxes_x` exists in the window manager.
--
local CELL_W, CELL_H = 84, 56

local function cell_of(self, i)
  local across = math.max(1, (self.w - 4) // CELL_W)
  local col = (i - 1) % across
  local row = (i - 1) // across

  return 2 + col * CELL_W, 2 + row * CELL_H, across
end

local function draw_icons(self, g, list)
  local per_row = math.max(1, (self.w - 4) // CELL_W)
  local rows_fit = math.max(1, self.h // CELL_H)
  local total = math.ceil(#list / per_row)

  if scroll > total - rows_fit + 1 then scroll = total - rows_fit + 1 end
  if scroll < 1 then scroll = 1 end

  self.per = rows_fit * per_row
  self.first = (scroll - 1) * per_row + 1
  self.bar = ui.scrollbar(g, self.w, self.h, total * per_row,
                          rows_fit * per_row, self.first)

  for i = 0, self.per - 1 do
    local n = self.first + i
    local e = list[n]

    if not e then break end

    local x, y = cell_of(self, i + 1)
    local on = (n == selected)

    if on then g:fill(x, y, CELL_W - 4, CELL_H - 2, theme.accent) end

    -- The label's background, and on the desktop it is the desktop.
    --
    -- `g:text` fills behind the glyphs rather than drawing them onto what
    -- is already there, so this has to be the actual colour underneath or
    -- every name sits in a rectangle of the wrong grey.
    local bg = on and theme.accent
               or (backdrop and theme.desktop or theme.sunken)

    local ink = on and theme.text_on
                or (backdrop and theme.desktop_text or theme.text)

    files.icon(g, x + (CELL_W - 4 - files.ICON) // 2, y + 2, e,
               files.join(where, e.name))

    -- Trimmed to the cell rather than clipped, so a long name ends in a
    -- readable way instead of half a glyph.
    local label = files.label(e)
    local room = (CELL_W - 8) // GW

    if #label > room then label = label:sub(1, math.max(1, room - 1)) .. "~" end

    g:text(x + (CELL_W - 4 - gfx.measure(label)) // 2, y + files.ICON + 6,
           label, ink, bg)
  end
end

function rows:draw(g)
  -- On the desktop this view *is* the desktop, so it paints the desktop
  -- colour and has no frame: a one-pixel line around the edge of the screen
  -- is a line around the edge of the screen.
  g:fill(0, 0, self.w, self.h, backdrop and theme.desktop or theme.sunken)

  if not backdrop then
    g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)
  end

  local shown = sorted()
  self.shown = shown

  if mode == "icons" then
    -- No heading: there are no columns to sort by when there are no
    -- columns. The View menu still sorts, and the order shows.
    self.top_row = 0
    draw_icons(self, g, shown)
    return
  end

  -- The heading, which is also what you click to sort.
  g:fill(1, 1, self.w - 2, GH + 4, theme.raised)

  for _, c in ipairs(COLUMNS) do
    local mark = (sort_by == c.key) and (reversed and " v" or " ^") or ""
    g:text(c.x, 3, c.title .. mark, theme.text_dim, theme.raised)
  end

  local top = GH + 6
  local per = (self.h - top - 2) // GH

  self.per_page = per
  self.top_row  = top

  local list = shown

  --
  -- The scroll position is remembered rather than worked out from the
  -- selection every time.
  --
  -- It used to be `selected - per + 1`, which means the list can only be
  -- moved by moving the selection - and a scrollbar moves the list without
  -- touching it. So: keep `scroll`, and only push it far enough that the
  -- selection stays visible.
  --
  if selected > 0 then
    if selected < scroll then scroll = selected end
    if selected > scroll + per - 1 then scroll = selected - per + 1 end
  end

  if scroll > #list - per + 1 then scroll = #list - per + 1 end
  if scroll < 1 then scroll = 1 end

  local first = scroll
  self.first = first
  self.per = per

  -- The bar covers the rows and the heading alike, so it starts below the
  -- heading rather than at the top of the well.
  self.bar = ui.scrollbar(g, self.w, self.h, #list, per, first)

  for i = 0, per - 1 do
    local n = first + i
    local e = list[n]

    if not e then break end

    local y  = top + i * GH
    local on = (n == selected)
    local bg = on and theme.accent or theme.sunken
    local fg = on and theme.text_on or theme.text

    if on then
      g:fill(1, y, self.w - 2 - (self.bar and ui.SCROLL_W + 2 or 0), GH, bg)
    end

    g:text(COLUMNS[1].x, y, files.label(e), fg, bg)
    g:text(COLUMNS[2].x, y,
           (e.kind == "directory") and "--" or tostring(e.size), fg, bg)
    g:text(COLUMNS[3].x, y,
           (e.kind == "directory") and "folder"
           or (types.kind_of(e.name) or "file"), fg, bg)
  end
end

local show                     -- defined below; the handlers call it

local function open_selected()
  local e = rows.shown and rows.shown[selected]

  if not e then return end

  if e.kind == "directory" then
    show(files.join(where, e.name))
  else
    -- Which program opens it is `/lib/filetypes.lua`'s answer, not
    -- Tracker's. Tracker does not need to know what an editor is - only
    -- that opening a file is somebody else's job and that something knows
    -- whose.
    local full = files.join(where, e.name)
    local opener = types.opener(full)

    if not opener then
      status.text = e.name .. ": nothing claims a ."
                    .. tostring(types.kind_of(full) or "?") .. " file"
      return
    end

    local ok, why = fs.send("/dev/wm", { type = "launch",
                                         program = opener,
                                         args = full })

    status.text = ok and ("opened " .. e.name .. " in " .. opener)
                  or ("could not open it: " .. tostring(why))
  end
end

function rows:mouse(action, x, y)
  --
  -- The bar first, and it takes moves and releases as well as presses -
  -- everything else in this view only cares about a press, which is why the
  -- early return below has to come after this rather than before it.
  --
  if self.bar and x >= self.w - ui.SCROLL_W - 2 then
    local per = self.per or 1
    local total = self.shown and #self.shown or 0

    if action == "press" then
      local to = ui.scrollbar_click(x, y, self.w, self.h, total, per, scroll)

      if to == scroll then
        self.bar_drag = { y = y, top = scroll }
      elseif to then
        scroll = to
      end
    elseif action == "move" and self.bar_drag then
      local d = self.bar_drag
      local _, size = ui.thumb(self.h, total, per, d.top)
      local room = (self.h - 4) - (size or 0)

      if room > 0 then
        scroll = math.min(math.max(1, d.top + ((y - d.y) * (total - per)) // room),
                          math.max(1, total - per + 1))
      end
    elseif action == "release" then
      self.bar_drag = nil
    end

    return true
  end

  if action ~= "press" then return false end

  if mode == "icons" then
    local across = math.max(1, (self.w - 4) // CELL_W)
    local col = (x - 2) // CELL_W
    local row = (y - 2) // CELL_H

    if col < 0 or col >= across or row < 0 then return true end

    local n = (self.first or 1) + row * across + col

    if not (self.shown and self.shown[n]) then return true end

    if n == selected then open_selected() else selected = n end

    return true
  end

  if y < (self.top_row or 0) then
    for _, c in ipairs(COLUMNS) do
      if x >= c.x - 4 and x < c.x + c.w then
        if sort_by == c.key then reversed = not reversed
        else sort_by, reversed = c.key, false end

        return true
      end
    end

    return true
  end

  local n = (self.first or 1) + (y - self.top_row) // GH

  if not (self.shown and self.shown[n]) then return true end

  -- The second click on an already-selected row opens it. A double click
  -- would be the BeOS answer and this kit has no notion of one; adding it
  -- to serve a single caller would be a widget change made for an
  -- application, which is the wrong way round.
  if n == selected then open_selected() else selected = n end

  return true
end

function rows:key(c)
  if c == -2 then selected = math.min(selected + 1, #(self.shown or {}))
  elseif c == -1 then selected = math.max(selected - 1, 1)
  elseif c == 13 or c == 10 then open_selected()
  elseif c == 8 or c == 127 then
    if where ~= "/" then show(files.parent(where)) end
  else
    return false
  end

  return true
end

--------------------------------------------------------------------------

function show(path)
  local found, why = files.entries(path)

  if not found then
    status.text = tostring(why)
    return
  end

  where, entries = path, found
  selected = (#found > 0) and 1 or 0
  here.text = path

  status.text = (#found == 0) and "empty"
                or ("%d item%s"):format(#found, #found == 1 and "" or "s")
end

--
-- The chrome, and the one thing that decides whether there is any.
--
-- A desktop has no menu bar, no toolbar, no places pane and no status line -
-- it is the icons and nothing else. Rather than a `backdrop` test at eight
-- call sites, the widgets that make up the frame go through here and the
-- test is in one place.
--
local function chrome(widget)
  if not backdrop then win:add(widget) end
end

chrome(here)

local function button(x, w, text, fn)
  chrome(ui.button{ x = x, y = 28 + BAR_H, w = w, h = 24, text = text,
                    on_click = fn })
end

button(12,  50, "Up",     function()
  if where ~= "/" then show(files.parent(where)) end
end)

button(70,  60, "Home",   function() show("/home") end)
button(138, 76, "Refresh", function() show(where) end)

--
-- Named, because the menu and the toolbar do the same things and the same
-- thing should be one function rather than two that drift apart.
--
function new_folder()
  -- Named by counting rather than by asking. A dialog for a name needs a
  -- panel of its own, and the thing that makes a folder useful is that it
  -- exists.
  local n = 1
  local name

  repeat
    name = (n == 1) and "new folder" or ("new folder " .. n)
    n = n + 1
  until not fs.getattr(files.join(where, name))

  local ok, why = fs.send(files.join(where, name), { type = "mkdir" })

  if ok then show(where) else status.text = tostring(why) end
end

function delete_selected()
  local e = rows.shown and rows.shown[selected]

  if not e then
    status.text = "nothing is selected"
    return
  end

  local ok, why = fs.send(files.join(where, e.name), { type = "delete" })

  if ok then
    show(where)
    status.text = "deleted " .. e.name
  else
    status.text = tostring(why)
  end
end

button(222, 96, "New folder", new_folder)
button(326, 70, "Delete", delete_selected)

--------------------------------------------------------------------------
-- Copying, which is the thing a file manager is for and this one could not
-- do.
--
-- One path, held until it is pasted. Not a copy of the *bytes*: a file that
-- was copied and then changed before pasting should paste what it is now,
-- and holding the contents would paste what it was. That is also why there
-- is no size limit on copying and only on pasting - copying here costs a
-- string.
--------------------------------------------------------------------------

local clipboard = nil

local function chosen()
  return rows.shown and rows.shown[selected]
end

local function do_copy()
  local e = chosen()

  if not e then status.text = "nothing is selected" return end

  clipboard = files.join(where, e.name)
  status.text = "copied " .. e.name
end

local function do_paste()
  if not clipboard then
    status.text = "nothing has been copied"
    return
  end

  local name = clipboard:match("([^/]+)$") or "copy"
  local to = files.join(where, name)

  -- Pasting into the directory a file came from would otherwise ask the
  -- filesystem to copy a file onto itself, which is a truncation.
  local n = 2

  while fs.getattr(to) do
    to = files.join(where, ("%s (%d)"):format(name, n))
    n = n + 1
  end

  local put, why = files.copy(clipboard, to)

  if put then
    show(where)
    status.text = ("pasted %s, %d bytes"):format(name, put)
  else
    status.text = tostring(why)
  end
end

local function do_open()
  if not chosen() then status.text = "nothing is selected" return end

  open_selected()
end

local function sort_on(key)
  if sort_by == key then reversed = not reversed else sort_by, reversed = key, false end
end

--------------------------------------------------------------------------
-- The menu bar.
--
-- Added after the actions it names, because a menu is a list of functions
-- and the functions have to exist. Its position is the top of the window;
-- where it sits in the view tree does not decide where it is drawn.
--------------------------------------------------------------------------

win:add(ui.menubar{
  x = 0, y = 0, w = W,
  menus = {
    { title = "File",
      items = {
        { text = "Open",       on_choose = do_open },
        { text = "New folder", on_choose = function() new_folder() end },
        { separator = true },
        { text = "Copy",       on_choose = do_copy },
        { text = "Paste",      on_choose = do_paste },
        { separator = true },
        { text = "Delete",     on_choose = function() delete_selected() end },
      } },
    { title = "Go",
      items = {
        { text = "Up",      on_choose = function()
            if where ~= "/" then show(files.parent(where)) end
          end },
        { text = "Home",    on_choose = function() show("/home") end },
        { text = "Refresh", on_choose = function() show(where) end },
      } },
    { title = "View",
      items = {
        { text = "as icons", on_choose = function()
            mode, scroll = "icons", 1
          end },
        { text = "as list",  on_choose = function()
            mode, scroll = "list", 1
          end },
        { separator = true },
        { text = "By name", on_choose = function() sort_on("name") end },
        { text = "By size", on_choose = function() sort_on("size") end },
        { text = "By kind", on_choose = function() sort_on("kind") end },
      } },
  },
})

chrome(places)
chrome(split)

-- The one widget the desktop is made of, and on the desktop it is the whole
-- window: no insets, because there is no frame to be inset from.
if backdrop then
  rows.x, rows.y, rows.w, rows.h = 0, 0, W, H
end

win:add(rows)
chrome(status)

show(where)
win:run()
