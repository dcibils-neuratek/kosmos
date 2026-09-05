-- kosmos: application
-- Tracker: the file manager.
--
--   wm tracker            opens at /home
--   wm tracker:/bin       or wherever
--
--   click a row       select it
--   click it again    open it: a directory is entered, a file is opened
--                     in the editor
--   drag empty space  a rubber band, which selects what it touches
--   drag a selection  onto a folder here, or into another Tracker window,
--                     to move it there
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

local W, H = 780, 520

--
-- The bands the window is divided into, measured from the top.
--
-- Named rather than repeated, because five widgets and two resize handlers
-- have to agree about them and the version where they did not is what put
-- the status line through the middle of the places tree.
--
local TOOLBAR_Y = 6                -- under the menu bar
local TOOLBAR_H = 24
local PATH_Y    = TOOLBAR_Y + TOOLBAR_H + 8
local CONTENT_Y = PATH_Y + gfx.font.h + 8
local FOOT_H    = 26               -- the status line at the bottom

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

  --
  -- The desktop shows `/home/Desktop`, and nothing else ever.
  --
  -- Not `/home`: what is on the desktop should be what you put on the
  -- desktop. A backdrop showing a home directory is showing you every dot
  -- file and every half-finished thing you have, which is not a desktop,
  -- it is a directory that happens to be behind your windows.
  --
  -- Created if it is not there, rather than fallen back from. A desktop
  -- folder that only exists once you think to make one is a folder nobody
  -- makes, and the fallback this replaced put `/home` on the backdrop on
  -- every machine that had never had one.
  --
  where = "/home/Desktop"

  if not fs.getattr(where) then
    local ok, why = fs.send(where, { type = "mkdir" })

    if not ok then
      print("tracker: no desktop folder: " .. tostring(why))
    end
  end
end

local win, err = ui.window{
  title = "Tracker", w = W, h = H,
  x = backdrop and 0 or 80, y = backdrop and 0 or 60,
  backdrop = backdrop or nil,

  -- Files dragged out of another window land here. The desktop outlines
  -- this window while one is overhead because of this flag, so it is a
  -- promise: `rows:drop` below is what keeps it.
  drops = true,
}

if not win then
  print("tracker: " .. tostring(err))
  return
end

local GW, GH = gfx.font.w, gfx.font.h

local entries  = {}

--
-- Two things, and they are not the same thing.
--
-- `selected` is the *cursor*: one row, moved by the arrow keys, and what
-- Enter opens. `marked` is the *selection*: a set of names, and what Delete
-- and Copy act on. A plain click sets both to one row, so in the ordinary
-- case they agree and nothing has to think about the difference.
--
-- Kept by name rather than by row, so sorting by size does not select
-- different files than were selected by name a moment earlier.
--
-- **No shift-click and no control-click**, because there are no modifiers to
-- click with: `wm.lua` records that a virtio keyboard gives Control plus a
-- letter and nothing else - no Shift state, no Alt, no Super. So the gesture
-- is the one BeOS's Tracker used anyway, and the one that needs no modifier:
-- drag a rectangle over what you want.
--
local rename_field, rename_of   -- the box a new name is typed in

local selected = 0
local marked   = {}
local band     = nil       -- { x0, y0, x1, y1 } while a rectangle is drawn
local followed = nil       -- the cursor the view last scrolled to

--
-- A press on a row is not acted on until the button comes up.
--
-- Until then it might be the start of a drag, and a click that had already
-- opened a directory cannot be taken back. So the press remembers what it
-- landed on and the release decides what it was - which is also how the
-- second click on the cursor row still opens.
--
-- `DRAG_SLOP` is how far the pointer has to move before it counts as a
-- drag rather than a hand that is not quite steady. Four pixels is the
-- number every desktop has settled on and there is no reason to differ.
--
local pending  = nil       -- { n, x, y, cursor } between press and release
local carrying = false     -- a drag of our own is in progress

local DRAG_SLOP = 4

--
-- Where a row's file actually is.
--
-- Almost always `where` plus the name, and not always: a **query** puts
-- files from all over a volume in one window, and each of those carries its
-- own path. One function, because every operation - open, copy, delete,
-- drag - has to agree, and the one that guessed would act on a file in the
-- current directory that happens to share a name with the one shown.
--
local function path_of(e)
  return e and (e.path or files.join(where, e.name)) or nil
end

local function marked_count()
  local n = 0

  for _ in pairs(marked) do n = n + 1 end

  return n
end

-- Everything marked, in the order the view shows it, which is the order a
-- person expects an operation to happen in.
local function marked_entries(shown)
  local out = {}

  for _, e in ipairs(shown or {}) do
    if marked[e.name] then out[#out + 1] = e end
  end

  return out
end

-- The cursor lands on one row and the selection becomes exactly it.
local function mark_only(n, shown)
  selected = n
  marked = {}

  local e = shown and shown[n]

  if e then marked[e.name] = true end
end

-- Declared here and defined with the toolbar, because the menu bar names
-- the same actions and a menu item and a button doing the same thing should
-- be one function rather than two that drift apart.
local new_folder, delete_selected

-- The same reason, for the ones the key handler names: it is defined above
-- them because it belongs with the view, and they belong with the menu.
local select_all, select_none, do_rename

-- And the one the mouse handler starts, which is written under the view it
-- drags out of rather than in the middle of deciding what a press was.
local start_drag

-- And the one that changes directory, because the places tree calls it and
-- is built above it.
local show, visit
local go_back, go_forward, go_up
local sort_by  = "name"
local scroll   = 1        -- the first row shown; the bar moves this

--------------------------------------------------------------------------
-- Where this window has been.
--
-- Two stacks, which is what Back and Forward are - and the reason they are
-- stacks rather than one list with a cursor is Forward: going somewhere new
-- has to *throw away* where you could have gone, or Forward offers a branch
-- nobody took.
--
-- `visit` remembers, `show` does not. That distinction is the whole of it:
-- Refresh, and the redraw after a file is moved or renamed, are the same
-- directory again and must not each be a step you can go Back through.
--------------------------------------------------------------------------
local went      = {}       -- where Back goes
local ahead     = {}       -- where Forward goes

--------------------------------------------------------------------------
-- The search box, which is two different things wearing one hat.
--
-- **A plain word filters what is already on screen**, by name, as you type.
-- It costs nothing - the entries are in hand - so it happens on every
-- keystroke and there is no button to press.
--
-- **`name:value` is a query**, and a query is a message. It asks the
-- filesystem which files carry that attribute and shows the answer as a
-- folder: files from all over the volume, in one window, each with its own
-- path. That is BeOS's live query and it is what M7's index was built for -
-- `qbench` measures that the cost is the size of the answer rather than the
-- size of the disk. It runs on Enter rather than on every keystroke,
-- because the first costs a round trip and the second does not.
--
-- **The result is refreshed rather than pushed**, and the difference is
-- worth naming. A real live query blocks in `fs.watch` until the answer
-- changes, and this window cannot: it is already blocked in the desktop's
-- `poll`, and there is no way to wait on two things at once. So while a
-- query is showing the window wakes twice a second and asks again. What is
-- missing to do it properly is a select - or a second thread - and neither
-- exists yet.
--
-- **A query over names is not available and the reason is interesting.**
-- The index is over attributes, and a file's name is not one of them. BeOS
-- indexed `name` precisely so that `name = "*.jpg"` could be a query rather
-- than a walk, and until this filesystem does the same, a name search is
-- either the local filter above or a directory walk in Lua.
--------------------------------------------------------------------------
local filter    = nil      -- a name substring, while one is typed
local found     = nil      -- the rows a query returned, or nil
local asked     = nil      -- { field, value }, so it can be run again

--
-- "list" or "icons", and the View menu changes it.
--
-- The same entries either way - the mode is a *layout*, not a different
-- reading of the directory. That is what makes the desktop possible later:
-- a desktop is this view in icon mode with no frame around it.
--
local mode = as_icons and "icons" or "list"
local reversed = false

--
-- Which edges each of these is pinned to, and the reason it matters.
--
-- A widget with no `follow` keeps the position it was given, which is right
-- for a button and wrong for everything that is meant to sit against an
-- edge. Without these the status line stayed at the *old* window's bottom -
-- a band of background painted straight across the middle of the places
-- tree - and the path never widened. `view:resize` is what applies them.
--
local here   = ui.label{ x = 12, y = PATH_Y + BAR_H, w = W - 24, text = where,
                         follow = { "left", "right", "top" } }

-- The left of the status line carries what just happened; the right carries
-- how many things there are, which is the one number always worth a place
-- of its own. Two labels rather than one string, so a message never pushes
-- the count off the end.
local status = ui.label{ x = 12, y = H - FOOT_H + 4, w = W - 200, text = "",
                         follow = { "left", "right", "bottom" } }
local count  = ui.label{ x = W - 180, y = H - FOOT_H + 4, w = 168, text = "",
                         follow = { "right", "bottom" } }

--
-- Where a new name is typed. Not there until Rename asks for it.
--
-- It used to sit under the list permanently, because the kit had no way to
-- hide a view - which was true and was the wrong thing to work around. It
-- has one now (`view.hidden`), and it is three lines in `ui.lua` that every
-- panel benefits from.
--
rename_field = ui.field{ x = 12, y = H - FOOT_H - 30, w = 300, text = "",
                         hidden = true, follow = { "left", "bottom" } }

--
-- The search box, top right, which is where every file manager puts it.
--
-- `follow` pins it to the right edge rather than the left, so widening the
-- window widens the gap between the buttons and the box instead of leaving
-- it stranded in the middle.
--
local search = ui.field{ x = W - 232, y = TOOLBAR_Y + BAR_H, w = 220,
                         h = TOOLBAR_H, text = "", hint = "Search",
                         follow = { "right", "top" } }

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

--
-- What the view shows, which is not always what the directory holds.
--
-- Three sources, in order of how much they cost: a query's answer if one
-- has been run, the directory filtered by a typed name, or the directory.
-- One function, so sorting, drawing, hit-testing and every operation see
-- the same list - the version where the drawing filtered and the hit test
-- did not is a file manager that deletes the wrong file.
--
local function visible()
  if found then return found end

  if not filter or filter == "" then return entries end

  local out = {}
  local want = filter:lower()

  for _, e in ipairs(entries) do
    if e.name:lower():find(want, 1, true) then out[#out + 1] = e end
  end

  return out
end

local function sorted()
  local out = {}

  for i, e in ipairs(visible()) do out[i] = e end

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
  x = 12, y = CONTENT_Y + BAR_H, w = PLACES_W,
  h = H - CONTENT_Y - BAR_H - FOOT_H - 6,
  follow = { "left", "top", "bottom" },
  roots = {
    { text = "home",   path = "/home",   children = subdirs },
    { text = "system", path = "/system", children = subdirs },
    { text = "data",   path = "/data",   children = subdirs },
    { text = "bin",    path = "/bin" },
    { text = "lib",    path = "/lib" },
    { text = "dev",    path = "/dev" },
  },
  on_select = function(_, node) visit(node.path) end,
}

local split = ui.splitter{
  x = 12 + PLACES_W, y = CONTENT_Y + BAR_H, w = 6,
  h = H - CONTENT_Y - BAR_H - FOOT_H - 6,
  follow = { "left", "top", "bottom" },
}

local rows = ui.view{ x = 12 + PLACES_W + 6, y = CONTENT_Y + BAR_H,
                      w = W - 24 - PLACES_W - 6,
                      h = H - CONTENT_Y - BAR_H - FOOT_H - 6,
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

--
-- Where row `n` is on screen, or nil if it is not.
--
-- One function for both layouts, and it exists so the rubber band and the
-- drawing agree about where a thing is. Working it out twice is how a file
-- ends up highlighted in one place and hit in another.
--
local function box_of(self, n)
  local first = self.first or 1
  local i = n - first

  if i < 0 or i >= (self.per or 0) then return nil end

  if mode == "icons" then
    local x, y = cell_of(self, i + 1)

    return x, y, CELL_W - 4, CELL_H - 2
  end

  local w = self.w - 2 - (self.bar and ui.SCROLL_W + 2 or 0)

  return 1, (self.top_row or 0) + i * GH, w, GH
end

--
-- Which row is at a point, or nil for none.
--
-- The same reason `box_of` exists: a press, a drop and the drawing all have
-- to agree about where a thing is, and three copies of this arithmetic is
-- three chances for one of them to be off by a row.
--
local function at_point(self, x, y)
  if mode == "icons" then
    local across = math.max(1, (self.w - 4) // CELL_W)
    local col = (x - 2) // CELL_W
    local row = (y - 2) // CELL_H

    if col < 0 or col >= across or row < 0 then return nil end

    return (self.first or 1) + row * across + col
  end

  if y < (self.top_row or 0) then return nil end

  return (self.first or 1) + (y - self.top_row) // GH
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
    local on = marked[e.name] or false

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

    files.icon(g, x + (CELL_W - 4 - files.ICON) // 2, y + 2, e, path_of(e))

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

  --
  -- The rectangle being dragged, drawn last of all - see the end of this
  -- function - so it sits over the rows it is selecting.
  --
  local function draw_band()
    if not band then return end

    local x0 = math.min(band.x0, band.x1)
    local y0 = math.min(band.y0, band.y1)
    local x1 = math.max(band.x0, band.x1)
    local y1 = math.max(band.y0, band.y1)

    -- An outline and not a wash: a filled rectangle over the names would
    -- hide what is being selected, which is the one thing it is for. Four
    -- fills, which is four C spans.
    g:frame(x0, y0, math.max(1, x1 - x0), math.max(1, y1 - y0), theme.ring)
  end

  if mode == "icons" then
    -- No heading: there are no columns to sort by when there are no
    -- columns. The View menu still sorts, and the order shows.
    self.top_row = 0
    draw_icons(self, g, shown)
    draw_band()
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
  -- Follow the cursor when it *moves*, not on every pass: unconditionally
  -- the scrollbar is useless in one direction, because the next repaint
  -- drags the view back to wherever the cursor is. Same bug `ui.list` and
  -- `procs` had.
  if selected > 0 and selected ~= followed then
    if selected < scroll then scroll = selected end
    if selected > scroll + per - 1 then scroll = selected - per + 1 end

    followed = selected
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
    local on = marked[e.name] or false
    local bg = on and theme.accent or theme.sunken
    local fg = on and theme.text_on or theme.text

    if on then
      g:fill(1, y, self.w - 2 - (self.bar and ui.SCROLL_W + 2 or 0), GH, bg)
    end

    g:text(COLUMNS[1].x, y, files.label(e), fg, bg)
    g:text(COLUMNS[2].x, y,
           (e.kind == "directory") and "--" or files.size(e.size), fg, bg)
    g:text(COLUMNS[3].x, y,
           (e.kind == "directory") and "folder"
           or (types.kind_of(e.name) or "file"), fg, bg)
  end

  draw_band()
end

local function open_selected()
  local e = rows.shown and rows.shown[selected]

  if not e then return end

  if e.kind == "directory" then
    visit(path_of(e))
  else
    -- Which program opens it is `/lib/filetypes.lua`'s answer, not
    -- Tracker's. Tracker does not need to know what an editor is - only
    -- that opening a file is somebody else's job and that something knows
    -- whose.
    local full = path_of(e)
    local opener = types.opener(full)

    if not opener then
      status.text = e.name .. ": nothing claims a ."
                    .. tostring(types.kind_of(full) or "?") .. " file"
      return
    end

    local ok, why = fs.send("/app/wm", { type = "launch",
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
  do
    local per = self.per or 1
    local total = self.shown and #self.shown or 0
    local to = ui.scrollbar_mouse(self, action, x, y, self.w, self.h,
                                  total, per, scroll)

    if to then
      scroll = to

      return true
    end
  end

  --
  -- The rubber band, while one is being drawn.
  --
  -- Before the press-only guard below, because a band is the one thing here
  -- that cares about `move` and `release`. Everything the rectangle touches
  -- is marked, recomputed each time rather than accumulated: dragging back
  -- over something should unmark it, which is what a rectangle means.
  --
  if band then
    if action == "move" then
      self.box_of = self.box_of or box_of
      band.x1, band.y1 = x, y

      local x0 = math.min(band.x0, band.x1)
      local y0 = math.min(band.y0, band.y1)
      local x1 = math.max(band.x0, band.x1)
      local y1 = math.max(band.y0, band.y1)

      marked = {}

      for n, e in ipairs(self.shown or {}) do
        local ex, ey, ew, eh = self:box_of(n)

        if ex and ex < x1 and ex + ew > x0 and ey < y1 and ey + eh > y0 then
          marked[e.name] = true
        end
      end

      return true
    end

    if action == "release" then
      band = nil
      return true
    end
  end

  --
  -- A press that landed on a row, still undecided.
  --
  -- Far enough and it was a drag; the button coming up first and it was a
  -- click. Both branches are here rather than in the press, because the
  -- press cannot tell which it is yet - and opening a directory on the way
  -- to dragging a file out of it is not a thing that can be undone.
  --
  if pending then
    if action == "move" then
      if carrying then return true end

      if math.abs(x - pending.x) < DRAG_SLOP
         and math.abs(y - pending.y) < DRAG_SLOP then
        return false
      end

      start_drag(self)

      return true
    end

    if action == "release" then
      if not carrying then
        --
        -- A click after all. The cursor row opens; any other row collapses
        -- the selection onto itself, which is what a plain click means.
        --
        -- A double click would be the BeOS answer and this kit has no
        -- notion of one; adding it to serve a single caller would be a
        -- widget change made for an application, which is the wrong way
        -- round.
        --
        if pending.cursor then
          open_selected()
        else
          mark_only(pending.n, self.shown)
        end
      end

      pending, carrying = nil, false

      return true
    end
  end

  if action ~= "press" then return false end

  --
  -- The heading, which is also what you click to sort. Above the rows and
  -- only in the list layout - there are no columns to sort by when there
  -- are no columns.
  --
  if mode ~= "icons" and y < (self.top_row or 0) then
    for _, c in ipairs(COLUMNS) do
      if x >= c.x - 4 and x < c.x + c.w then
        if sort_by == c.key then reversed = not reversed
        else sort_by, reversed = c.key, false end

        return true
      end
    end

    return true
  end

  local n = at_point(self, x, y)
  local e = n and self.shown and self.shown[n]

  if not e then
    -- Empty space: start a rectangle rather than doing nothing.
    band = { x0 = x, y0 = y, x1 = x, y1 = y }
    marked = {}
    selected = 0
    return true
  end

  --
  -- Pressing something already in the selection leaves the selection alone,
  -- so a drag takes all of it. Pressing something outside the selection
  -- takes it over at once, so what is about to be dragged is visible before
  -- the pointer moves.
  --
  pending = { n = n, x = x, y = y, cursor = (n == selected) }

  if not marked[e.name] then mark_only(n, self.shown) end

  return true
end

--------------------------------------------------------------------------
-- Dragging what is selected out of this window.
--
-- The paths go to the desktop as one string and come back out at whichever
-- window the pointer was over - see `wm.lua`. Nothing in between reads
-- them: this window and whatever catches them are the two that share the
-- format, and one path per line is the whole of it.
--
-- A name with a newline in it would break that, and nothing in this system
-- makes one. Worth saying rather than defending against, because the
-- defence - a length-prefixed frame - would be a format two programs have
-- to agree about where a line does the job today.
--------------------------------------------------------------------------

function start_drag(self)
  local list = marked_entries(self.shown)

  if #list == 0 then return end

  local paths = {}

  for i, e in ipairs(list) do paths[i] = path_of(e) end

  --
  -- What the pointer carries. Long names are cut, because the badge is
  -- drawn beside the cursor and a forty-character one would be a bar across
  -- the screen.
  --
  local label

  if #list == 1 then
    label = list[1].name

    if #label > 24 then label = label:sub(1, 23) .. "~" end
  else
    label = #list .. " items"
  end

  local ok, why = ui.drag(win, "files", table.concat(paths, "\n"), label)

  if not ok then
    -- Almost always the message being full, which is a real limit and is
    -- said rather than silently dropping the tail of the selection.
    status.text = "cannot drag that many at once: " .. tostring(why)
    pending = nil
    return
  end

  carrying = true
  status.text = "dragging " .. label
end

--
-- And catching one, which is the other half.
--
-- Where it landed decides where it goes: a directory row takes them into
-- itself, and anything else - a file, the space below the last row, the
-- heading - means this directory. That is BeOS's rule and every file
-- manager's since, and it is the one that makes a drop into a window you
-- are already looking at mean something.
--
function rows:drop(kind, payload, x, y)
  if kind ~= "files" then return false end

  --
  -- A query result is a view of files that are elsewhere, so there is no
  -- "here" to put something in. Refused with a sentence rather than
  -- silently moving them into whichever directory the query was run from,
  -- which is a place the person is not looking at.
  --
  if found then
    ui.dropped(win, false, 0, "a query is not a folder to drop into")
    status.text = "a query is not a folder to drop into"
    return true
  end

  local into = where
  local n = at_point(self, x, y)
  local e = n and self.shown and self.shown[n]

  if e and e.kind == "directory" then into = path_of(e) end

  local moved, skipped, failed, why = 0, 0, 0, nil

  for path in payload:gmatch("[^\n]+") do
    local name = path:match("([^/]+)$")

    if name and files.parent(path) == into then
      -- Already where it was dropped. Not an error and not a copy: dropping
      -- a file back into its own directory should do nothing at all.
      skipped = skipped + 1
    elseif name then
      local ok, err = files.move(path, files.join(into, name))

      if ok then moved = moved + 1 else failed, why = failed + 1, err end
    end
  end

  show(where)

  if moved > 0 then
    marked = {}

    -- Selected where they landed, if that is here. Somewhere else and there
    -- is nothing in this window to select.
    if into == where then
      for path in payload:gmatch("[^\n]+") do
        local name = path:match("([^/]+)$")

        if name then marked[name] = true end
      end
    end
  end

  --
  -- The window they came from is told, and only this window may say so -
  -- the desktop handed it that right with the drop and takes it back now.
  -- Without this the source goes on showing files it no longer holds.
  --
  ui.dropped(win, failed == 0, moved, why)

  if failed > 0 then
    status.text = ("moved %d, then: %s"):format(moved, tostring(why))
  elseif moved > 0 then
    status.text = ("moved %d item%s into %s"):format(
                    moved, moved == 1 and "" or "s", into)
  elseif skipped > 0 then
    status.text = "already there"
  end

  return true
end

function rows:key(c)
  --
  -- Control plus a letter is the only modifier this hardware gives - see
  -- `wm.lua` - so the shortcuts are the ones that fit in it. Control-W is
  -- the window manager's and is not available.
  --
  if c == 1 then                       -- Control-A
    select_all()
    return true
  elseif c == 27 then                  -- Escape
    select_none()
    return true
  elseif c == 18 then                  -- Control-R
    do_rename()
    return true
  end

  if c == -2 then
    mark_only(math.min(selected + 1, #(self.shown or {})), self.shown)
  elseif c == -1 then
    mark_only(math.max(selected - 1, 1), self.shown)
  elseif c == 13 or c == 10 then open_selected()
  elseif c == 8 or c == 127 then
    go_up()
  else
    return false
  end

  return true
end

--------------------------------------------------------------------------

--
-- How many things are on screen, and how many there are to be on screen.
--
-- Both, when they differ, because "3 items" in a directory of ninety is a
-- window that looks empty for a reason the person typed a moment ago and
-- may have forgotten.
--
local function recount()
  local shown = #visible()

  if found then
    count.text = ("%d found"):format(shown)
  elseif filter and filter ~= "" then
    count.text = ("%d of %d"):format(shown, #entries)
  elseif shown == 0 then
    count.text = "empty"
  else
    count.text = ("%d item%s"):format(shown, shown == 1 and "" or "s")
  end
end

function show(path)
  local listed, why = files.entries(path)

  if not listed then
    status.text = tostring(why)
    return
  end

  -- A directory listing replaces a query's answer: the two are different
  -- windows onto the filesystem and showing one over the other would be a
  -- list nobody could account for.
  where, entries, found, asked = path, listed, nil, nil
  selected = (#listed > 0) and 1 or 0
  here.text = path

  recount()
  status.text = ""
end

--
-- The same, and remembered. See the two stacks above: `visit` is a step you
-- can go Back through and `show` is not.
--
function visit(path)
  if path == where then return end

  local from = where

  --
  -- A move clears the search; a refresh does not. Somewhere new is a new
  -- question, and carrying a filter into a directory you have just opened
  -- is a window that looks empty for a reason two directories ago.
  --
  search.text, search.caret = "", 1
  filter = nil

  show(path)

  -- Only if it worked. A path that could not be listed leaves `where` alone,
  -- and pushing it would put a place you never reached into the history.
  if where == path then
    went[#went + 1] = from
    ahead = {}
  end
end

function go_back()
  local to = table.remove(went)

  if not to then status.text = "nowhere back" return end

  local from = where

  show(to)
  ahead[#ahead + 1] = from
end

function go_forward()
  local to = table.remove(ahead)

  if not to then status.text = "nowhere forward" return end

  local from = where

  show(to)
  went[#went + 1] = from
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
  chrome(ui.button{ x = x, y = TOOLBAR_Y + BAR_H, w = w, h = TOOLBAR_H,
                    text = text, on_click = fn })
end

--
-- Back and Forward first, where every browser and every file manager has
-- put them since 1995. Arrows rather than words because two words is a
-- third of the toolbar for something that is understood at a glance.
--
button(12, 28, "<", go_back)
button(44, 28, ">", go_forward)

function go_up()
  if where ~= "/" then visit(files.parent(where)) end
end

button(80,  44, "Up",   go_up)
button(128, 56, "Home", function() visit("/home") end)

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
  local list = marked_entries(rows.shown)

  if #list == 0 then
    status.text = "nothing is selected"
    return
  end

  --
  -- Every marked file, and the first failure stops it.
  --
  -- Stopping rather than carrying on, because the reason one delete fails -
  -- a directory that is not empty, a read-only store - is usually the reason
  -- the next one will, and a list of twelve identical complaints is not more
  -- informative than one. What was already deleted stays deleted; there is
  -- no undo here yet and this does not pretend otherwise.
  --
  local done = 0

  for _, e in ipairs(list) do
    local ok, why = fs.send(path_of(e), { type = "delete" })

    if not ok then
      show(where)
      status.text = ("deleted %d, then %s: %s"):format(done, e.name,
                                                       tostring(why))
      return
    end

    done = done + 1
  end

  show(where)
  status.text = (done == 1) and ("deleted " .. list[1].name)
                or ("deleted " .. done .. " items")
end

button(192, 96, "New folder", new_folder)
button(296, 62, "Delete", delete_selected)

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

local clipboard = nil       -- a list of paths, held until it is pasted
local cut_from  = false     -- whether pasting them should remove the originals

local function chosen()
  return rows.shown and rows.shown[selected]
end

local function do_copy()
  local list = marked_entries(rows.shown)

  if #list == 0 then status.text = "nothing is selected" return end

  -- A list of paths now, not one. `do_paste` walks it, so copying three
  -- files and pasting them elsewhere is one gesture rather than three.
  clipboard = {}

  for i, e in ipairs(list) do clipboard[i] = path_of(e) end

  status.text = (#list == 1) and ("copied " .. list[1].name)
                or ("copied " .. #list .. " items")
end

local function do_paste()
  if not clipboard or #clipboard == 0 then
    status.text = "nothing has been copied"
    return
  end

  local done, bytes = 0, 0

  for _, from in ipairs(clipboard) do
    local name = from:match("([^/]+)$") or "copy"
    local to = files.join(where, name)

    -- Pasting into the directory a file came from would otherwise ask the
    -- filesystem to copy a file onto itself, which is a truncation.
    local n = 2

    while fs.getattr(to) do
      to = files.join(where, ("%s (%d)"):format(name, n))
      n = n + 1
    end

    local put, why = files.copy(from, to)

    if not put then
      show(where)
      status.text = ("pasted %d, then %s: %s"):format(done, name,
                                                      tostring(why))
      return
    end

    done = done + 1
    bytes = bytes + put

    if cut_from then
      -- A move is a copy and then a delete, and the delete only happens
      -- once the copy has actually landed. Cutting a file and losing it
      -- because the destination was full is the one failure a file manager
      -- must not have.
      local gone, gwhy = fs.send(from, { type = "delete" })

      if not gone then
        status.text = ("copied %s but could not remove the original: %s")
                      :format(name, tostring(gwhy))
      end
    end
  end

  local only = (done == 1)
               and (clipboard[1]:match("([^/]+)$") or "it") or nil

  -- A cut is spent once it is pasted. A copy is not: pasting the same
  -- things into three directories is a thing people do.
  if cut_from then clipboard, cut_from = nil, false end

  show(where)
  status.text = only and ("%s %s, %d bytes"):format(
                           cut_from and "moved" or "pasted", only, bytes)
                or ("pasted %d items, %d bytes"):format(done, bytes)
end

--
-- Cut is copy with a flag. The originals go when the paste lands, and not
-- before: a move that removes the source first and then fails to write the
-- destination has destroyed the file, which is the one thing a file manager
-- must never do.
--
local function do_cut()
  do_copy()

  if clipboard and #clipboard > 0 then
    cut_from = true
    status.text = (#clipboard == 1)
                  and ("cut " .. (clipboard[1]:match("([^/]+)$") or "it"))
                  or ("cut " .. #clipboard .. " items")
  end
end

function select_all()
  marked = {}

  for _, e in ipairs(rows.shown or {}) do marked[e.name] = true end

  status.text = marked_count() .. " selected"
end

function select_none()
  marked, selected = {}, 0
  status.text = ""
end

--
-- Renaming, in a field that appears under the list.
--
-- Not a dialog: `new_folder` above explains why there is none - a panel of
-- its own is a lot of machinery for one question - and that argument holds
-- here too. What it does instead is show the name where it can be edited,
-- take Enter as yes and Escape as no, and go away again.
--
-- One at a time, deliberately. Renaming several things at once means a
-- pattern, and a pattern is a different feature with different mistakes in
-- it.
--
--
-- Focus, set by hand.
--
-- The window keeps `focus` as an index into `root:focusables()`, which is
-- what a click sets. There is no `win:focus(v)` in the kit, and adding one
-- for a single caller would be a widget change made for an application.
--
local function focus_on(v)
  for i, w in ipairs(win.root:focusables()) do
    if w == v then win.focus = i return true end
  end

  return false
end

function do_rename()
  local list = marked_entries(rows.shown)

  if #list == 0 then status.text = "nothing is selected" return end

  if #list > 1 then
    status.text = "rename takes one thing at a time"
    return
  end

  rename_of = list[1].name
  rename_field.text = rename_of
  rename_field.caret = #rename_of + 1
  rename_field.hidden = false

  focus_on(rename_field)
  status.text = "new name for " .. rename_of .. ", then Enter"
end

--------------------------------------------------------------------------
-- Searching, which is the two things the box at the top does.
--------------------------------------------------------------------------

--
-- `name:value` and nothing else. A single word is a filter; anything with a
-- colon in it is a question for the filesystem.
--
-- Deliberately not an expression language. BeOS's query grammar had `and`,
-- `or`, comparisons and wildcards, and every one of those is a thing the
-- server would have to be taught - `fs.query` matches a value exactly,
-- which is what the index can answer without walking. A grammar in front of
-- an engine that cannot honour it would be a search box that silently
-- returns the wrong answer.
--
local function run_query(text)
  local field, value = text:match("^%s*([%w_]+)%s*:%s*(.-)%s*$")

  if not field or value == "" then
    status.text = "a query looks like kind:note"
    return
  end

  local paths, why = fs.query(where, { [field] = value })

  if not paths then
    status.text = "query: " .. tostring(why)
    return
  end

  --
  -- Paths, turned into rows.
  --
  -- Each carries its own `path`, which is what makes a query result a real
  -- folder rather than a list of strings: `path_of` hands it back, so Open,
  -- Copy, Delete and a drag all act on the file where it actually is.
  --
  local rows_out = {}

  for _, path in ipairs(paths) do
    local attrs = fs.getattr(path)

    if attrs then
      rows_out[#rows_out + 1] = {
        name = path:match("([^/]+)$") or path,
        path = path,
        kind = attrs.kind,
        size = attrs.size or 0,
      }
    end
  end

  found, asked = rows_out, { field = field, value = value }
  marked, selected = {}, (#rows_out > 0) and 1 or 0
  scroll = 1

  recount()

  --
  -- Nothing found is the ordinary answer today, and saying why is better
  -- than an empty window.
  --
  -- The index is over attributes and **nothing in this system writes one
  -- yet**: `filetypes.kind_of` already prefers `attrs.type` over the
  -- extension and no file has ever had it set. `attr` at the prompt can set
  -- one; a panel here that shows and edits them is what would make queries
  -- mean something, and it is the next thing this window wants.
  --
  if #rows_out == 0 then
    status.text = ("nothing here carries %s = %s"):format(field, value)
  else
    status.text = ("%s is %s, anywhere under %s"):format(field, value, where)
  end
end

function search:on_change(text)
  --
  -- Typing is free while the answer is in hand. A colon means a question
  -- for somebody else, and that waits for Enter.
  --
  -- The filter is dropped as soon as one appears, so the window shows the
  -- directory while a query is being typed rather than the four files whose
  -- names happen to contain "kind".
  --
  if text:find(":") then
    filter, found, asked = nil, nil, nil
    recount()
    status.text = "press Enter to ask"
    return
  end

  filter = (text ~= "") and text or nil
  found, asked = nil, nil
  selected = (#visible() > 0) and 1 or 0
  scroll = 1

  recount()
end

function search:on_enter(text)
  if text:find(":") then run_query(text) else self:on_change(text) end
end

--
-- A query's answer, asked for again.
--
-- Twice a second while one is showing, because a window cannot block on the
-- filesystem and on the desktop at the same time - see the note on the
-- search box above. `poll_wait` is what makes this a wake rather than a
-- spin: the desktop holds the reply until something happens or the wait
-- runs out, so between asks this process is not running at all.
--
local QUERY_WAIT = 125            -- scheduler ticks; TICK_HZ is 250

--
-- And a clock, because `poll_wait` is a *ceiling* rather than a period.
--
-- The desktop answers the poll the moment anything happens, so a pointer
-- moving across this window returns from it many times a second - and
-- without this, each of those would be a query. That is a message per
-- pointer movement, which is precisely what `wm.lua` refuses to do for
-- window drags and for the same reason.
--
-- The frequency is read rather than assumed: `sys.ticks` is CNTFRQ_EL0,
-- 62.5 MHz under QEMU's TCG and 24 MHz when the same machine runs on this
-- Mac's own cores under `hvf`. A constant here would ask twice a second in
-- one case and once every five in the other.
--
local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local ask_every  = counter_hz // 2
local asked_at   = 0

function win:on_frame()
  if not asked then
    win.poll_wait = nil
    return false
  end

  win.poll_wait = QUERY_WAIT

  local now = sys.ticks()

  if now - asked_at < ask_every then return false end

  asked_at = now

  local paths = fs.query(where, { [asked.field] = asked.value })

  if not paths then return false end

  -- Rebuilt only when the *set* changed, so a query that is answering the
  -- same thing costs a message and no repaint.
  if #paths == #found then
    local same = true

    for i, path in ipairs(paths) do
      if found[i] == nil or found[i].path ~= path then same = false break end
    end

    if same then return false end
  end

  local keep = asked

  run_query(keep.field .. ":" .. keep.value)

  return true
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
  follow = { "left", "right", "top" },
  menus = {
    { title = "File",
      items = {
        { text = "Open",       on_choose = do_open },
        { text = "New folder", on_choose = function() new_folder() end },
        { separator = true },
        { text = "Rename",     on_choose = function() do_rename() end },
        { separator = true },
        { text = "Cut",        on_choose = do_cut },
        { text = "Copy",       on_choose = do_copy },
        { text = "Paste",      on_choose = do_paste },
        { separator = true },
        { text = "Select all", on_choose = select_all },
        { text = "Select none", on_choose = select_none },
        { separator = true },
        { text = "Delete",     on_choose = function() delete_selected() end },
      } },
    { title = "Go",
      items = {
        { text = "Back",    on_choose = go_back },
        { text = "Forward", on_choose = go_forward },
        { separator = true },
        { text = "Up",      on_choose = go_up },
        { text = "Home",    on_choose = function() visit("/home") end },
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

--
-- Enter commits the rename; an empty name or the same name is a no.
--
-- `fs.send` with a `rename` type, and if the filesystem has no such
-- operation the message says so rather than this pretending it worked.
--
function rename_field:on_enter(text)
  local from = rename_of

  rename_of = nil
  self.text = ""
  self.hidden = true

  if not from then return end

  text = (text or ""):gsub("^%s+", ""):gsub("%s+$", "")

  if text == "" or text == from then
    status.text = "not renamed"
    return
  end

  if text:find("/") then
    status.text = "a name cannot contain a slash"
    return
  end

  if fs.getattr(files.join(where, text)) then
    status.text = text .. " already exists"
    return
  end

  local ok, why = fs.send(files.join(where, from),
                          { type = "rename", to = text })

  if ok then
    show(where)
    marked = { [text] = true }
    status.text = from .. " is now " .. text
  else
    status.text = "rename: " .. tostring(why)
  end
end

--
-- What became of a drag that left this window.
--
-- The destination did the moving and this is the only way to hear about
-- it: the files are gone from here and nothing else would say so. A window
-- that showed a file it no longer has is a file manager that lies, and it
-- lies until you happen to press Refresh.
--
function win:on_dropped(ok, count, err)
  if ok and count > 0 then
    show(where)
    status.text = ("moved %d item%s"):format(count, count == 1 and "" or "s")
  elseif ok then
    status.text = "nothing moved"
  else
    -- Refreshed even so. A drop that moved four of six and then failed has
    -- still changed this directory.
    show(where)
    status.text = tostring(err or "the drop was refused")
  end
end

win:add(rows)
chrome(search)
chrome(rename_field)
chrome(status)
chrome(count)

show(where)
win:run()
