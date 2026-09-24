-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Tracker
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
-- the disk, `/ramfs` in memory and `/bin` in the image with the same code,
-- and would browse a directory served from another machine without
-- noticing which it was.
--
-- **There is no Modified column**, and the absence is deliberate rather
-- than unfinished. A file's `mtime` is `sys.ticks()`, the counter since
-- this machine started, so across a reboot it means nothing at all.
-- Printing the number anyway would be a column that looks like a date and
-- is not one.
--
-- **What is in the way is not the clock.** `/dev/clock` exists and answers
-- with the epoch - it was dropped when `devices` moved from Lua to C and
-- restored when every clock in the system started saying "no clock". What
-- is in the way is that `diskfs` stamps `sys.ticks()` rather than asking
-- it, and a server is spawned with the capabilities it was handed: giving
-- it a wall clock is a mount, and a decision about which servers get one,
-- rather than a line here.

local ui    = use("/lib/ui.lua")
local files = use("/lib/files.lua")
local types = use("/lib/filetypes.lua")
local layout = use("/lib/iconlayout.lua")
local iconsize = use("/lib/iconsize.lua")
local placelib = use("/lib/places.lua")
local sidebar  = use("/lib/sidebar.lua")
local theme = ui.theme

local W, H = 780, 520

--
-- The bands the window is divided into, measured from the top.
--
-- Named rather than repeated, because five widgets and two resize handlers
-- have to agree about them and the version where they did not is what put
-- the status line through the middle of the places tree.
--
-- **One band, where there were three** (`roadmap.md` 5zg). A menu bar, a
-- row of buttons and a trail of every path segment each took a strip across
-- the top; the header is the one strip that replaced them, and the files
-- start immediately under it.
--
-- **The kit's header since 0.10.149** (`docs/tracker2.html`): 46 with its
-- rule, back and forward and the place as a pill on the left, and search,
-- a new folder, the view and the dots as icons on the right. It was a row
-- 33 tall of words in boxes - "<", ">", Find, New, View, "..." - which the
-- comment below it called a decision rather than a shortfall; the kit has
-- icon buttons now, and the drawing was always icons.
--
local L = ui.layout
local CONTENT_Y = L.head
local FOOT_H    = 26               -- the status line, when there is one

--
-- **The sidebar, as `docs/tracker2.html` draws it** (`roadmap.md` 5zt): 200
-- wide on the sidebar's own colour with a rule down its right edge, a head
-- of its own 46 tall - the search, "Files", a menu - and the places under
-- it, 33 apart, grouped by hairlines. The header is the file pane's only,
-- and the files sit on the pane with nothing drawn round them. It was a
-- tree of Places, System and Drives in a framed well 210 wide, with the
-- files in another, both 12 in from the window's edges.
--
local SIDE_W = 200

-- Nothing is offset by a menu bar any more. Kept as a name rather than
-- deleted at thirty call sites, and zero because there is no bar.
local BAR_H = 0

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

  --
  -- Three things the desktop always has, put back whenever they are missing.
  --
  -- **The cheat sheet**, because it is the quickest way into this system
  -- and the desktop is where a person looks first. Written again whenever
  -- it differs from the copy in the image, so it describes the system it
  -- is on rather than whichever one first put it there.
  --
  -- **Drive**, a launcher for Tracker at `/`, the one place every file is
  -- under - BeOS put the boot volume's icon on the desktop for the same
  -- reason. A launcher rather than a special case in this file, so it is
  -- moved, renamed and removed like anything else, and comes back at the
  -- next start if it was removed.
  --
  -- **The Trash**, where a delete puts things - see `files.TRASH`.
  --
  local sheet = sys.asset("cheatsheet.html")
  local sheet_path = files.join(where, "cheatsheet.html")

  if sheet and fs.read(sheet_path) ~= sheet then
    local ok, why = fs.write(sheet_path, sheet)

    if not ok then print("tracker: no cheat sheet: " .. tostring(why)) end
  end

  local drive = files.join(where, "Drive")

  if not fs.getattr(drive) then
    local ok, why = fs.write(drive, "")

    if ok then
      ok, why = fs.setattr(drive, { kind = "launcher", type = "launcher",
                                    program = "/bin/tracker.lua",
                                    args = "/", icon = "Device_Harddisk" })
    end

    if not ok then print("tracker: no Drive: " .. tostring(why)) end
  end

  if not fs.getattr(files.TRASH) then
    local ok, why = fs.send(files.TRASH, { type = "mkdir" })

    if not ok then print("tracker: no Trash: " .. tostring(why)) end
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

-- The desktop's real size, which is the screen less the strip across the
-- top. The window manager decides that and says so in its reply.
if backdrop then W, H = win.w, win.h end

--
-- **The face's own height, asked once the window has said what its faces
-- are** - an icon's label is set in it - and the list's rows at the fixed
-- layout's height, as every list is (`docs/tracker2.html`'s list: a row of
-- words with 6 above and below). Both were `gfx.font.h`, the 16-pixel
-- bitmap the kit loads before a window exists, so under the look's faces
-- the rows of the list were 16 apart with 21-pixel words in them.
--
local GW, GH = gfx.font.w, gfx.height()
local LROW = ui.metrics.row

--
-- How big the icons are here, which is a choice and is kept (`roadmap.md`
-- 5za).
--
-- **The desktop and a window keep different ones**, in the same file under
-- different keys, because they are the same program and not the same place:
-- a desktop of large pictures over a photograph and a window of small ones
-- you can see two hundred of is the pair of things people actually want.
--
-- `resize_cells` is forward-declared below and recomputes the grid, which
-- is the only thing in this program that a size decides.
--
local resize_cells

local icons = iconsize.new("/home/.tracker",
                           backdrop and "desktop_icon_px" or "window_icon_px",
                           function() resize_cells() end)

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

--
-- The last segment of a path, which is what a header says you are in.
--
-- `files.lua` has `parent` and `label` and no name-of-a-path, because
-- nothing had wanted one: an entry already carries its own name, and a path
-- was only ever shown whole. A header that shows one segment wants this.
--
local function last_part(path)
  return (tostring(path):match("([^/]+)/?$")) or "/"
end

-- The button that says where you are and opens the path as a menu, the one
-- that turns the header into a search field, and the menu of path segments
-- it opens. Declared here because `visit` retitles the first, the header
-- defines them, and the menu is written next to the other menus.
local place_button, search, search_on, trail_menu, more_menu, view_menu
local header

-- The place button's picture: the drawing's house for Home, the Trash's
-- bin, a drive for anything under `/drives`, and a folder for the rest.
local function place_icon(path)
  if path == "/home" then return "home" end
  if path == files.TRASH or path:sub(1, #files.TRASH + 1) == files.TRASH .. "/"
  then
    return "trash"
  end
  if path == "/drives" or path:match("^/drives/") then return "drive" end

  return "folder"
end
local place_pending             -- what a place dropped on Places will be,
                                -- while the same box asks for its name

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
local dragged  = nil       -- where that drag was pressed, until it lands

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
--
-- **The trail**, as `docs/drives.html` draws it:
-- `Drives > Kingston DataTraveler > KOSMOS HOME`, each part clickable.
--
-- A view rather than a label, because a label has `measure` and `draw` and
-- no `mouse` at all - and this needs to know *which* part was clicked. It
-- keeps the `text` field a label had, so `here.text = path` in `show` and
-- `chrome(here)` below are both unchanged: what differs is that the view
-- splits its own text and remembers where each part landed.
--
-- Laid out with `gfx.measure` rather than a glyph count, for the reason that
-- binding exists: the face is proportional, and a column computed from
-- character widths lands in the wrong place (`testing.md` 18.85).
--
--
-- The trail is the kit's now (`ui.trail`), shared with the Open and Save
-- window; its rules are `ui.md` 16.8d.
--
--
-- **The trail is a menu now, not a band.** `ui.trail` is still what the
-- Open and Save window uses and its rules are still `ui.md` 16.8d; what
-- changed here is that a row of segments across the whole width was most of
-- a line spent on something looked at once a minute. `trail_menu` builds
-- the same segments as menu items and `place_button` opens them.
--

-- The left of the status line carries what just happened; the right carries
-- how many things there are, which is the one number always worth a place
-- of its own. Two labels rather than one string, so a message never pushes
-- the count off the end.
local status = ui.label{ x = SIDE_W + 12, y = H - FOOT_H + 4,
                         w = W - SIDE_W - 200, text = "", color = "text_dim",
                         follow = { "left", "right", "bottom" } }
local count  = ui.label{ x = W - 180, y = H - FOOT_H + 4, w = 168, text = "",
                         color = "text_dim",
                         follow = { "right", "bottom" } }

--
-- Where a new name is typed. Not there until Rename asks for it.
--
-- It used to sit under the list permanently, because the kit had no way to
-- hide a view - which was true and was the wrong thing to work around. It
-- has one now (`view.hidden`), and it is three lines in `ui.lua` that every
-- panel benefits from.
--
rename_field = ui.field{ x = SIDE_W + 12, y = H - FOOT_H - 34, w = 300,
                         text = "",
                         hidden = true, follow = { "left", "bottom" } }

--
-- The search box, top right, which is where every file manager puts it.
--
-- `follow` pins it to the right edge rather than the left, so widening the
-- window widens the gap between the buttons and the box instead of leaving
-- it stranded in the middle.
--
--
-- **The search box is a press away rather than always on screen.**
--
-- It was a field in the toolbar, which is where every file manager puts
-- one - and a field that is always there is a field that is always taking
-- room from the thing being searched. The magnifier swaps it for the place
-- button, which is the one control it can take the room from without
-- costing anything: you are either looking at where you are or typing what
-- you want.
--
-- **The queries are untouched.** `kind:note` is Tracker's own idea and the
-- thing it has that a file manager usually does not; simplifying a window
-- is not a reason to lose a feature (`roadmap.md` 5zg).
--
search = ui.field{ w = 220, text = "", hint = "Search", hidden = true }

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
  { key = "name", title = "Name", x = 10,  w = 210 },
  { key = "size", title = "Size", x = 226, w = 90  },
  { key = "kind", title = "Kind", x = 322, w = 90  },
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
-- The places on the left, the listing on the right.
--
-- **Home, Desktop and the Trash; then Documents, Music and Pictures where
-- they exist, and the places a person made; then the drives** - the
-- drawing's groups, less its Recent, which would be a row that leads
-- nowhere: nothing here keeps a list of recent files yet. The system's
-- mounts are one press away through the place button's menu, which starts
-- at `/`.
--
-- `/lib/sidebar.lua` still answers for the drives and is what the Open and
-- Save window draws; Tracker lists them itself because its sidebar is the
-- drawing's list and not a tree.
--
local side = sidebar.new()
local place_by_id = {}

local function place_items()
  local items, by = {}, {}

  local function add(name, path, icon, extra)
    local it = { id = path or ("#" .. name), name = name, path = path,
                 icon = icon }

    for k, v in pairs(extra or {}) do it[k] = v end

    items[#items + 1] = it
    by[it.id] = it
  end

  add("Home", "/home", "home")
  add("Desktop", "/home/Desktop", "folder")
  add("Trash", files.TRASH, "trash")

  local mine = {}

  for _, f in ipairs({ { "Documents", "document" }, { "Music", "music" },
                       { "Pictures", "pictures" } }) do
    local path = "/home/" .. f[1]

    if fs.getattr(path) then mine[#mine + 1] = { f[1], path, f[2] } end
  end

  --
  -- The places a person made, the drawing's `MyPhotos on PHOTOS 2024`: a
  -- place whose drive is away stays, dim and going nowhere (`quiet`), which
  -- is `drives.html`'s "Unplug the drive and MyPhotos stays in Places,
  -- greyed out".
  --
  for _, p in ipairs(placelib.read(fs)) do
    local volumes = p.attrs.volume and side.volumes() or {}
    local path = placelib.resolve(p.attrs, volumes)

    mine[#mine + 1] = { p.name, path, "folder", { place = p,
                                                  quiet = (path == nil) } }
  end

  if #mine > 0 then items[#items + 1] = { gap = true, rule = true } end

  for _, m in ipairs(mine) do add(m[1], m[2], m[3], m[4]) end

  local drives = side.volumes() or {}

  if #drives > 0 then items[#items + 1] = { gap = true, rule = true } end

  for _, v in ipairs(drives) do
    add(v.name, files.join("/drives", v.name), "drive")
  end

  return items, by
end

local places = ui.sidebar{
  x = 0, y = L.head + 2, w = SIDE_W - 1, h = H - L.head - 2, pitch = 33,
  follow = { "left", "top", "bottom" },
  on_select = function(_, id)
    local it = place_by_id[id]

    if it and it.path then visit(it.path) end
  end,
}

-- The chosen row is the place you are in, when you are in one.
local function mark_place(path)
  places.selected = place_by_id[path] and path or nil
end

--
-- **Places and Drives read again**: after a place is made or removed, and on
-- Refresh. Only those two groups, so a folder somebody opened in System is
-- still open afterwards.
--
local function refresh_places()
  side.refresh()
  places.items, place_by_id = place_items()
  mark_place(where)
end

--
-- **A drive or a folder dropped on the sidebar becomes a place**, once it
-- has a name. Anywhere on the sidebar rather than only on the Places
-- heading: the trail taught that the pixels between targets should not be
-- dead, and there is nothing else a drop here could mean.
--
-- The name is asked in the same box Rename uses, offered as the folder's
-- own and there to be typed over.
--
function places:drop(kind, payload, _, _)
  if kind ~= "files" then return false end

  local first = tostring(payload or ""):match("[^\n]+")

  if not first then return true end

  if tostring(payload):find("\n.") then
    ui.dropped(win, false, 0, "one place at a time")
    status.text = "one place at a time"
    return true
  end

  local attrs, why = placelib.from_path(first, side.volumes(true))

  if not attrs then
    ui.dropped(win, false, 0, why)
    status.text = why
    return true
  end

  place_pending = attrs
  rename_of = nil
  rename_field.text = placelib.suggest(first)
  rename_field.caret = #rename_field.text + 1
  rename_field.hidden = false
  win:focus_on(rename_field)

  ui.dropped(win, true, 0, nil)
  status.text = "a name for this place, then Enter"
  return true
end

--
-- **A right-click on a place takes it out of Places**, into the Trash like
-- every other delete in Tracker - so the wrong one is one drag back. It is
-- the shortcut that goes, never what it points at. Home and Desktop are not
-- files anybody made, and say so.
--
function places:on_context(_, y)
  local it = self:item_at(y)

  if not it then return true end

  if not it.place then
    status.text = it.name .. " is built in, not a place you made"
    return true
  end

  local name, why = files.free_name(files.TRASH, it.place.name)
  local ok = false

  if name then
    ok, why = files.move(it.place.file, files.join(files.TRASH, name))
  end

  status.text = ok and (it.place.name .. " is out of Places, and in the Trash")
                or ("could not remove it: " .. tostring(why))

  refresh_places()
  return true
end

local rows = ui.view{ x = SIDE_W, y = CONTENT_Y, w = W - SIDE_W,
                      h = H - CONTENT_Y - FOOT_H,
                      follow = { "left", "right", "top", "bottom" } }

--
-- The pane's ground to the window's bottom, under the foot line as well,
-- so the files and what is said about them are one white page.
--
local pane_ground = ui.view{ x = SIDE_W, y = CONTENT_Y, w = W - SIDE_W,
                             h = H - CONTENT_Y,
                             follow = { "left", "right", "top", "bottom" } }

function pane_ground:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
end

rows.focusable = true

--
-- How many columns of icons fit, and where one goes.
--
-- Worked out in one place because the drawing and the hit test both need it
-- and disagreeing about it is the bug where you click one icon and open
-- another - the same reason `boxes_x` exists in the window manager.
--
--
-- The cell the icons in force want - `iconsize.cell`, which is where that
-- arithmetic lives and where it is tested.
--
-- Held in two locals rather than asked for at each call site, because the
-- drawing, the hit test, the free-cell layout and a drop all read them and
-- four copies of one sum is four chances for one to be off.
--
local CELL_W, CELL_H = 0, 0

function resize_cells()
  CELL_W, CELL_H = iconsize.cell(icons:size(), GH)
end

resize_cells()

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
    if backdrop then
      local r = self.rects and self.rects[n]

      if not r then return nil end

      return r.x, r.y, CELL_W - 4, CELL_H - 2
    end

    local x, y = cell_of(self, i + 1)

    return x, y, CELL_W - 4, CELL_H - 2
  end

  local w = self.w - 2 - (self.bar and ui.SCROLL_W + 2 or 0)

  return 1, (self.top_row or 0) + i * LROW, w, LROW
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
    -- On the desktop an icon is wherever it was put, so the point is tested
    -- against each one, the last drawn first because that one is on top.
    if backdrop then
      local rects = self.rects or {}

      for n = #rects, 1, -1 do
        local r = rects[n]

        if x >= r.x and x < r.x + CELL_W - 4
           and y >= r.y and y < r.y + CELL_H - 2 then
          return n
        end
      end

      return nil
    end

    local across = math.max(1, (self.w - 4) // CELL_W)
    local col = (x - 2) // CELL_W
    local row = (y - 2) // CELL_H

    if col < 0 or col >= across or row < 0 then return nil end

    return (self.first or 1) + row * across + col
  end

  if y < (self.top_row or 0) then return nil end

  return (self.first or 1) + (y - self.top_row) // LROW
end

local function draw_icons(self, g, list)
  if backdrop then
    --
    -- The desktop has no grid and no scrolling: an icon is where it was
    -- dragged, from its `desktop_x` and `desktop_y`, and one never dragged
    -- takes the next free cell - `iconlayout.place` says which.
    --
    local items = {}

    for i, e in ipairs(list) do
      local a = e.attrs or {}

      items[i] = { x = a.desktop_x, y = a.desktop_y }
    end

    self.rects = layout.place(items, CELL_W, CELL_H, self.w, self.h, 2)
    self.per, self.first, self.bar = #list, 1, nil
  else
    local per_row = math.max(1, (self.w - 4) // CELL_W)
    local rows_fit = math.max(1, self.h // CELL_H)
    local total = math.ceil(#list / per_row)

    if scroll > total - rows_fit + 1 then scroll = total - rows_fit + 1 end
    if scroll < 1 then scroll = 1 end

    self.per = rows_fit * per_row
    self.first = (scroll - 1) * per_row + 1
    self.bar = ui.scrollbar(g, self.w, self.h, total * per_row,
                            rows_fit * per_row, self.first)
  end

  for i = 0, self.per - 1 do
    local n = self.first + i
    local e = list[n]

    if not e then break end

    local x, y

    if backdrop then
      x, y = self.rects[n].x, self.rects[n].y
    else
      x, y = cell_of(self, i + 1)
    end
    local on = marked[e.name] or false

    if on then g:fill(x, y, CELL_W - 4, CELL_H - 2, theme.accent) end

    --
    -- The label's background - and on the desktop there is none.
    --
    -- `g:text` fills behind the glyphs rather than drawing them onto what is
    -- already there, so in a window this has to be the actual colour
    -- underneath or every name sits in a rectangle of the wrong grey. On the
    -- desktop what is underneath is somebody's picture, so the name is drawn
    -- with no box at all and a shadow under it instead, which is what reads
    -- on a dark photograph and on a light one.
    --
    local plain = backdrop and not on
    local bg = on and theme.accent or (not backdrop and theme.sunken or nil)

    local ink = on and theme.text_on
                or (backdrop and theme.desktop_text or theme.text)

    local px = icons:size()

    files.icon(g, x + (CELL_W - 4 - px) // 2, y + 2, e, path_of(e), px)

    -- Two lines of the cell's width rather than one, so a name reads in
    -- full up to twice as long, and past that the second line keeps its
    -- end - where the extension is - rather than half a glyph.
    local room = (CELL_W - 8) // GW
    local first, second = layout.label(files.label(e), room)

    local function label(text, ly)
      local lx = x + (CELL_W - 4 - gfx.measure(text)) // 2

      -- The shadow first, a pixel down and across, then the name over it.
      if plain then g:text(lx + 1, ly + 1, text, 0xff000000) end

      g:text(lx, ly, text, ink, bg)
    end

    label(first, y + px + 6)

    if second then label(second, y + px + 6 + GH) end
  end
end

function rows:draw(g)
  --
  -- On the desktop this view *is* the desktop, and it draws nothing behind
  -- the icons: cleared to transparent so that whatever the compositor paints
  -- under it shows through, and no frame, because a one-pixel line around
  -- the edge of the screen is a line around the edge of the screen.
  --
  -- It filled with the desktop colour before. That is the same colour the
  -- compositor paints when nobody has chosen a picture, so it looked right
  -- and a wallpaper was never visible - see `compose_rect` in `wm.lua` for
  -- the half of that which is not about colour.
  --
  g:fill(0, 0, self.w, self.h, backdrop and 0x00000000 or theme.sunken)

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

  --
  -- The heading, which is also what you click to sort: a row of the list's
  -- height with the columns' names dim in it and a hairline under it, on
  -- the list's own ground in a flat look and the raised face in the others.
  --
  local flat = theme.flat
  local ground = flat and theme.sunken or theme.raised
  local wy = (LROW - gfx.height()) // 2

  g:fill(1, 1, self.w - 2, LROW - 1, ground)
  g:fill(1, LROW, self.w - 2, 1, theme.line_soft)

  for _, c in ipairs(COLUMNS) do
    local mark = (sort_by == c.key) and (reversed and " v" or " ^") or ""
    g:text(c.x, wy, c.title .. mark, theme.text_dim, ground)
  end

  local top = LROW + 1
  local per = math.max(1, (self.h - top - 2) // LROW)

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

    local ry = top + i * LROW
    local y  = ry + wy
    local on = marked[e.name] or false

    -- Chosen: a pale band with the words as they were in a flat look (the
    -- drawing's `.row.on`), the accent with white words in the others.
    local bg = on and (flat and theme.line_soft or theme.accent)
               or theme.sunken
    local fg = (on and not flat) and theme.text_on or theme.text

    local wide = self.w - 2 - (self.bar and ui.SCROLL_W + 2 or 0)

    if on then
      g:fill(1, ry, wide, LROW, bg)
    elseif flat and n % 2 == 0 then
      -- Every other row a shade off the page, as the drawing's list is.
      bg = theme.mix(theme.sunken, theme.window, 300)
      g:fill(1, ry, wide, LROW, bg)
    end

    --
    -- A folder or a file, as the drawing marks them before the name: a
    -- small rounded block, the accent's for a folder and the rail's grey for
    -- anything else.
    --
    g:fill_round(COLUMNS[1].x, ry + (LROW - 13) // 2, 16, 13,
                 (e.kind == "directory") and theme.mix(theme.accent,
                                                       theme.sunken, 250)
                 or theme.track, 2)

    g:text(COLUMNS[1].x + 24, y, files.label(e), fg, bg)
    g:text(COLUMNS[2].x, y,
           (e.kind == "directory") and "--" or files.size(e.size), fg, bg)
    --
    -- The attributes go with the name, or the Kind column can only ever
    -- read an extension.
    --
    -- `kind_of` takes them and prefers them - it always has - and this call
    -- left them out, so a launcher read as `file` and every typed attribute
    -- the disk grows would have been invisible here. Launchers were the
    -- first thing to notice: thirty-eight of them in the Deskbar's folder,
    -- all saying "file", all 0 B, with nothing on screen to say what they
    -- were.
    --
    g:text(COLUMNS[3].x, y,
           (e.kind == "directory") and "folder"
           or (types.kind_of(e.name, e.attrs) or "file"), fg, bg)
  end

  draw_band()
end

--
-- A launcher opened: what its attributes say to start, started.
--
-- The window manager is asked exactly as the Deskbar asks it, so a launcher
-- starts nothing the Deskbar could not, and the window manager's check on
-- the program's name is the only check there is. Read when it is opened
-- rather than when it was listed, so a launcher changed with `attr` does
-- the new thing without a refresh.
--
local function start_launcher(path, name)
  local a = fs.getattr(path) or {}
  local program = tostring(a.program or "")

  if program == "" then
    status.text = name .. ": a launcher that names no program"
    return
  end

  local ok, why = fs.send("/app/wm", { type = "launch", program = program,
                                       args = tostring(a.args or "") })

  status.text = ok and ("started " .. program)
                or ("could not start " .. program .. ": " .. tostring(why))
end

local function open_selected()
  local e = rows.shown and rows.shown[selected]

  if not e then return end

  if e.kind == "launcher" then
    start_launcher(path_of(e), e.name)
  elseif e.kind == "directory" and backdrop then
    -- The desktop does not wander off into a folder, because it is the
    -- desktop. A folder opened from it opens in a Tracker window of its
    -- own, which is what BeOS did and what a person reaching for one means.
    local ok, why = fs.send("/app/wm", { type = "launch", program = "tracker",
                                         args = path_of(e) })

    status.text = ok and ("opened " .. e.name)
                  or ("could not open it: " .. tostring(why))
  elseif e.kind == "directory" then
    visit(path_of(e))
  else
    -- How it opens is `/lib/filetypes.lua`'s answer, not Tracker's.
    -- Tracker does not need to know what an editor is - only that opening
    -- a file is somebody else's job and that something knows whose. A Lua
    -- file is a program and runs, which needs its opening comment to say
    -- whether it is an application; nothing else is read.
    local full = path_of(e)
    local source = types.kind_of(full) == "lua" and fs.read(full) or nil
    local how = types.how_to_open(full, nil,
                                  type(source) == "string" and source or nil)

    if not how then
      status.text = e.name .. ": nothing claims a ."
                    .. tostring(types.kind_of(full) or "?") .. " file"
      return
    end

    local ok, why = fs.send("/app/wm", { type = "launch",
                                         program = how.program,
                                         args = how.args })

    if not ok then
      status.text = "could not open it: " .. tostring(why)
    elseif how.program == full then
      status.text = "started " .. e.name
    elseif how.program == "terminal" then
      status.text = "running " .. e.name .. " in a Terminal"
    else
      status.text = "opened " .. e.name .. " in " .. how.program
    end
  end
end

--
-- Right-click a launcher: edit what it starts.
--
-- The same window the Deskbar's menu opens on a right press, because it is
-- the same kind of file - a desktop icon and a menu row are two views of one
-- launcher, and having two ways to edit it would be two ways to disagree.
--
-- Only a launcher answers. Right-clicking anything else says so rather than
-- opening a window about a file that has nothing to edit; a general context
-- menu for every kind of file is a bigger idea and is not this one.
--
-- `dispatch_context` in `ui.lua` hit-tests to this view and hands local
-- coordinates, so `at_point` is the same function a left press uses and the
-- desktop's free-placed icons are found the same way.
--
function rows:on_context(x, y)
  local n = at_point(self, x, y)
  local e = n and self.shown and self.shown[n]

  if not e then
    --
    -- Nothing under it: how big the icons here are, which is the one thing
    -- the background of a view full of icons has to say. On the desktop it
    -- is the *only* way to it, because a desktop has no menu bar - and a
    -- press on the background asking about the background is what every
    -- desktop has meant by a right click since there were two buttons.
    --
    if mode == "icons" then
      win:open_menu(win.origin_x + self.x + x, win.origin_y + self.y + y,
                    icons:items())

      return true
    end

    status.text = "nothing there"
    return true
  end

  if e.kind ~= "launcher" then
    status.text = e.name .. " is not a launcher"
    return true
  end

  local ok, why = fs.send("/app/wm", { type = "launch",
                                       program = "/bin/launcheredit.lua",
                                       args = path_of(e) })

  status.text = ok and ("editing " .. e.name)
                or ("could not open it: " .. tostring(why))

  return true
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

  -- Where the press was, kept past the release: a drop back onto the
  -- desktop moves its icons by how far the pointer went, and the release
  -- reaches this window before the drop does - see `rows:drop`.
  dragged = { x = pending and pending.x or 0, y = pending and pending.y or 0 }

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

  --
  -- On the desktop, icons dragged from the desktop and let go on it are
  -- *moved*, and the files stay where they are. Dropping a file into the
  -- directory it is already in does nothing anywhere else; on a desktop
  -- what a person means by it is "put it here". Only onto another
  -- directory's icon is it a move of the file, by the rule below.
  --
  local from = dragged

  dragged = nil

  if backdrop and from then
    local at = at_point(self, x, y)
    local onto = at and self.shown and self.shown[at]

    if not (onto and onto.kind == "directory" and not marked[onto.name]) then
      local dx, dy = x - from.x, y - from.y

      for i, it in ipairs(self.shown or {}) do
        local r = self.rects and self.rects[i]

        if marked[it.name] and r then
          local nx, ny = r.x + dx, r.y + dy

          it.attrs = it.attrs or {}
          it.attrs.desktop_x, it.attrs.desktop_y = nx, ny

          fs.setattr(path_of(it), { desktop_x = nx, desktop_y = ny })
        end
      end

      ui.dropped(win, true, 0, nil)
      return true
    end
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
      -- Into the Trash under a name it does not hold yet: a second
      -- `notes.txt` thrown away is `notes 2.txt`, not a refusal.
      local target = (into == files.TRASH) and files.free_name(into, name)
                     or name
      local ok, err = files.move(path, files.join(into, target or name))

      if ok then
        moved = moved + 1

        -- Onto the desktop, a file lands where it was let go and the next
        -- one under it, rather than in whichever cell happened to be free.
        if backdrop and into == where then
          fs.setattr(files.join(into, name), {
            desktop_x = x - CELL_W // 2,
            desktop_y = y - icons:size() // 2 + (moved - 1) * CELL_H,
          })
        end
      else
        failed, why = failed + 1, err
      end
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

  -- The header says where you are, which is the innermost segment and not
  -- the whole path: the whole path is what pressing it opens.
  if place_button then
    -- A place's own name where it is one - "Home", as the sidebar and the
    -- drawing say it - and the folder's name everywhere else.
    local known = place_by_id[path]

    place_button.text = known and known.name
                        or ((path == "/") and "/" or last_part(path))
    place_button.icon = place_icon(path)
    place_button:fit()

    -- The header places its controls from their widths, and this one's
    -- just changed.
    if header then header:measure() end
  end

  -- And the sidebar marks the place you are in, when you are in one.
  mark_place(path)

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


--
-- Back and Forward first, where every browser and every file manager has
-- put them since 1995 - as the drawing's arrows, the kit's icon buttons.
--
local back_button = ui.iconbutton{ icon = "back", on_click = go_back }
local forward_button = ui.iconbutton{ icon = "forward", on_click = go_forward }

function go_up()
  if where ~= "/" then visit(files.parent(where)) end
end

--
-- **Where you are, as one button.** It opens the whole path as a menu, so
-- Up and Home are in it - the innermost segment is what it says and every
-- one above is a row. Up was a button of its own and Home another, and both
-- are one press from here rather than nought, which is the trade the whole
-- header is: three bands of chrome for one.
--
-- A pill with the place's picture and a chevron (`ui.button`'s `icon` and
-- `chevron`), in the label face, as `docs/tracker2.html` draws it.
--
place_button = ui.button{ text = "Home", icon = "home", chevron = true,
                          role = "label", space = 6 }

place_button.on_click = function()
  -- On the screen: `open_menu` opens a window of its own and the window
  -- manager places windows on the screen - and the button is in the
  -- header, which starts at the sidebar's edge.
  win:open_menu(win.origin_x + SIDE_W + place_button.x, win.origin_y + L.head,
                trail_menu())
end

--
-- **The magnifier, in the sidebar's head**, which turns that head into the
-- field and back - `docs/tracker2.html`: "Pressing it turns the header into
-- the field". "Files" and the menu give it their room while it is open.
--
-- **Focus follows it**, because a search box that appears and does not take
-- the keyboard is a box you have to click after asking for it - which is
-- the kind of half-done control that makes a window feel slow without
-- anything being slow.
--
local side_menu                    -- the sidebar's menu, below

local function toggle_search()
  search_on = not search_on
  search.hidden = not search_on
  side_menu.hidden = search_on

  if search_on then
    win:focus_on(search)
  else
    search.text = ""
    show(where)
  end

  win:paint()
end

--
-- The right of the header: what makes something, what changes how it is
-- shown, and everything else - three icons, as drawn. The drawing's fourth,
-- a close, is the title bar's.
--
-- **View opens its own menu rather than toggling.** The drawing has an icon
-- for the layout; one button that opens the six rows - as icons, as list,
-- and the sort and sizes under them - is the same thing with every mark in
-- one place.
--
local find_button = ui.iconbutton{ x = SIDE_W - 8 - 26 - 1 - 4 - 26, y = 10,
                                   icon = "search", on_click = toggle_search }
local new_button = ui.iconbutton{ icon = "newfolder",
                                  on_click = function() new_folder() end }
local view_button = ui.iconbutton{ icon = "menu" }
local more_button = ui.iconbutton{ icon = "more" }

view_button.on_click = function()
  -- `view_menu` is the function the menu bar used to hand `ui.menu_items`,
  -- which resolves a menu's `items` when it is one. Called directly here,
  -- because what `open_menu` wants is the items and not the menu.
  win:open_menu(win.origin_x + SIDE_W + view_button.x, win.origin_y + L.head,
                view_menu())
end

more_button.on_click = function()
  win:open_menu(win.origin_x + SIDE_W + more_button.x, win.origin_y + L.head,
                more_menu())
end

header = ui.header{
  x = SIDE_W, y = 0, w = W - SIDE_W, title = "", edge = { 6, 8 },
  left = { back_button, forward_button, place_button },
  right = { new_button, view_button, more_button },
}

--
-- **The sidebar's head**: "Files" in the title face at the left, 18 in as
-- every header's title is, and the magnifier and a menu at the right - the
-- menu of what concerns Tracker and its places rather than the files in
-- front of you, which is the dots'. The drawing centred the word between
-- the two icons; Diego, of Preferences' the same: "it should be aligned to
-- the left to the content as the rest of the apps".
--
side_menu = ui.iconbutton{ x = SIDE_W - 8 - 26 - 1, y = 10, icon = "menu" }

search.x, search.y = 10, (L.head - 1 - 31) // 2
search.w = find_button.x - 4 - 10

local side_ground = ui.view{ x = 0, y = 0, w = SIDE_W, h = H,
                             follow = { "left", "top", "bottom" } }

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
end

local side_head = ui.view{ x = 0, y = 0, w = SIDE_W, h = L.head }

function side_head:draw(g)
  if search_on then return end

  local word = "Files"

  g:text(L.head_in, (self.h - 1 - gfx.height("title")) // 2, word,
         theme.text, nil, "title")
end

--
-- Where the header's controls and the files are, in points inside the
-- window - for the display harness, which drops on a place row and opens
-- View by its position, and held a copy of this layout that went stale
-- every time the header moved.
--
if not backdrop then
  header:measure()
  print(("tracker: content at %d, view at %d,%d"):format(
        CONTENT_Y, SIDE_W + view_button.x + view_button.w // 2,
        view_button.y + view_button.h // 2))
end

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
  -- Stopping rather than carrying on, because the reason one fails - a
  -- read-only store, a name the Trash has run out of - is usually the reason
  -- the next one will, and a list of twelve identical complaints is not more
  -- informative than one.
  --
  -- **Into the Trash, unless it is already there.** A delete anywhere else
  -- is a move, and taking it back is dragging it out again. Only inside the
  -- Trash is it for good, which is where there being no undo stops being
  -- the whole story. The Trash itself goes nowhere.
  --
  local done, trashed = 0, 0

  for _, e in ipairs(list) do
    local from = path_of(e)
    local ok, why

    if from == files.TRASH then
      ok, why = nil, "the Trash does not go in the Trash"
    elseif files.in_trash(from) then
      ok, why = files.remove(from)
    else
      local name

      name, why = files.free_name(files.TRASH, e.name)

      if name then
        ok, why = files.move(from, files.join(files.TRASH, name))
      end

      if ok then trashed = trashed + 1 end
    end

    if not ok then
      show(where)
      status.text = ("%d done, then %s: %s"):format(done, e.name,
                                                    tostring(why))
      return
    end

    done = done + 1
  end

  show(where)

  if trashed == 0 then
    status.text = (done == 1) and ("deleted " .. list[1].name)
                  or ("deleted " .. done .. " items")
  else
    status.text = (done == 1) and (list[1].name .. " is in the Trash")
                  or (done .. " items are in the Trash")
  end
end

--
-- Everything in the Trash, gone for good.
--
-- No confirmation, for the reason there is no dialog anywhere in this
-- window - see `new_folder`. What makes that bearable is that this is now
-- the only delete that cannot be taken back, and it is asked for by name.
--
local function empty_trash()
  local names = fs.list(files.TRASH) or {}

  for i, name in ipairs(names) do
    local ok, why = files.remove(files.join(files.TRASH, name))

    if not ok then
      show(where)
      status.text = ("emptied %d, then %s: %s"):format(i - 1, name,
                                                       tostring(why))
      return
    end
  end

  show(where)
  status.text = (#names == 0) and "the Trash was already empty"
                or ("emptied the Trash of %d item%s"):format(
                     #names, (#names == 1) and "" or "s")
end

--
-- New folder and Delete were buttons here, beside the place. New folder is
-- the `+` in the header - it is the one action that *makes* something and
-- worth a press of its own - and Delete is in the `...` menu with the rest
-- of File, where it already was and where the right button has always had
-- it. A button that removes things is not one to leave under a hand that is
-- aiming at a path.
--

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

  win:focus_on(rename_field)
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
-- search box above. `poll_wait_ticks` is what makes this a wake rather than a
-- spin: the desktop holds the reply until something happens or the wait
-- runs out, so between asks this process is not running at all.
--
local QUERY_WAIT = 125            -- scheduler ticks; TICK_HZ is 250

--
-- And a clock, because `poll_wait_ticks` is a *ceiling* rather than a period.
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
    win.poll_wait_ticks = nil
    return false
  end

  win.poll_wait_ticks = QUERY_WAIT

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

-- Edit, beside Open: a Lua file opens by running now, and this is the way to
-- change one. Whatever handles the file's type - the editor, for a `.lua`.
local function do_edit()
  local e = chosen()

  if not e then status.text = "nothing is selected" return end

  if e.kind == "directory" or e.kind == "launcher" then
    status.text = e.name .. ": not a file to edit"
    return
  end

  local program = types.opener(path_of(e)) or "editor"
  local ok, why = fs.send("/app/wm", { type = "launch", program = program,
                                       args = path_of(e) })

  status.text = ok and ("editing " .. e.name .. " in " .. program)
                or ("could not edit it: " .. tostring(why))
end

local function sort_on(key)
  if sort_by == key then reversed = not reversed else sort_by, reversed = key, false end
end

--
-- The View menu, worked out when it opens rather than when the window is
-- made, because every row of it says what is in force: which layout, which
-- column the listing is sorted on, and how big the icons are. A list built
-- once would show how things were the moment Tracker started.
--
-- The sizes are only offered in icon view, since a list has no icons in it
-- and a menu that offers a choice which changes nothing is worse than one
-- that does not offer it.
--
function view_menu()
  local items = {
    { text = "as icons", mark = (mode == "icons"),
      on_choose = function() mode, scroll = "icons", 1 end },
    { text = "as list", mark = (mode == "list"),
      on_choose = function() mode, scroll = "list", 1 end },
    { separator = true },
    { text = "By name", mark = (sort_by == "name"),
      on_choose = function() sort_on("name") end },
    { text = "By size", mark = (sort_by == "size"),
      on_choose = function() sort_on("size") end },
    { text = "By kind", mark = (sort_by == "kind"),
      on_choose = function() sort_on("kind") end },
  }

  if mode == "icons" then
    items[#items + 1] = { separator = true }

    for _, it in ipairs(icons:items()) do items[#items + 1] = it end
  end

  return items
end

--------------------------------------------------------------------------
-- The menu bar.
--
-- Added after the actions it names, because a menu is a list of functions
-- and the functions have to exist. Its position is the top of the window;
-- where it sits in the view tree does not decide where it is drawn.
--------------------------------------------------------------------------

--
-- **One header instead of a menu bar, a toolbar and a trail.**
--
-- Diego, 23 September 2026, with GNOME's Files beside it: "right now is too
-- complicated. i want to simplify it like the one in the image."
-- `docs/tracker2.html` is what was drawn and `roadmap.md` 5zg the
-- agreement.
--
-- **Nothing was deleted.** Three menus of twenty items became one `...`
-- menu of the same twenty, in the same order, with the same marks - and
-- every one of them was already on the right button too. What changed is
-- how much of the window is spent saying so: a menu bar, a row of buttons
-- and a trail of every path segment were three bands of chrome above the
-- files, and they are one band now.
--
-- The trail is the part worth arguing about and it is the part that gains
-- most. It was a row of clickable segments across the whole width, which on
-- `Drives > Kingston DataTraveler > KOSMOS HOME > photos` is most of a line
-- for something looked at once a minute. It is a button saying where you
-- are, and pressing it opens the same segments as a menu.
--
local more_items = {
  { text = "Open",        on_choose = do_open },
  { text = "Edit",        on_choose = do_edit },
  { text = "Rename",      on_choose = function() do_rename() end },
  { separator = true },
  { text = "Cut",         on_choose = do_cut },
  { text = "Copy",        on_choose = do_copy },
  { text = "Paste",       on_choose = do_paste },
  { separator = true },
  { text = "Select all",  on_choose = select_all },
  { text = "Select none", on_choose = select_none },
  { separator = true },
  { text = "Delete",      on_choose = function() delete_selected() end },
  { text = "Empty Trash", on_choose = function() empty_trash() end },
  { separator = true },
  { text = "Refresh",     on_choose = function() refresh_places() show(where) end },
}

--
-- The `...` menu: everything File and Go held, in the order they held it.
-- View is a button of its own beside it, because its rows carry marks and
-- are asked for far more often than Empty Trash.
--
function more_menu()
  local out = {}

  for _, it in ipairs(more_items) do out[#out + 1] = it end

  return out
end

--
-- Where you are, as a menu: every segment of the path, innermost last, each
-- one a place to go back to. The same list the trail drew across the window.
--
function trail_menu()
  local out, at = {}, where

  while true do
    -- A copy per item: `at` is reassigned every turn of the loop, so a
    -- closure over it would send every segment to the last one.
    local step = at

    table.insert(out, 1, { text = (step == "/") and "/" or last_part(step),
                           mark = (step == where),
                           on_choose = function() visit(step) end })

    if at == "/" then break end

    local up = files.parent(at)

    if up == at then break end

    at = up
  end

  return out
end


-- The one widget the desktop is made of, and on the desktop it is the whole
-- window: no insets, because there is no frame to be inset from.
if backdrop then
  rows.x, rows.y, rows.w, rows.h = 0, 0, W, H

  -- And again whenever the window manager changes it, which it does when
  -- the strip across the top starts after the desktop, or goes away.
  function win:on_resize(w, h)
    rows.x, rows.y, rows.w, rows.h = 0, 0, w, h
  end
end

--
-- Enter commits the rename; an empty name or the same name is a no.
--
-- `fs.send` with a `rename` type, and if the filesystem has no such
-- operation the message says so rather than this pretending it worked.
--
--
-- **A place being named**, when a drop on the sidebar asked. Refused with a
-- sentence for the same reasons a rename is, and for one of its own: a name
-- already in Places.
--
local function name_place(field, text)
  local attrs = place_pending

  place_pending = nil
  field.text = ""
  field.hidden = true

  text = (text or ""):gsub("^%s+", ""):gsub("%s+$", "")

  if text == "" then
    status.text = "no place made"
    return
  end

  if text:find("/") then
    status.text = "a name cannot contain a slash"
    return
  end

  local file = files.join(placelib.DIR, text)

  if fs.getattr(file) then
    status.text = text .. " is already in Places"
    return
  end

  if not fs.getattr(placelib.DIR) then
    fs.send(placelib.DIR, { type = "mkdir" })
  end

  local ok, why = fs.write(file, "")

  if ok then ok, why = fs.setattr(file, attrs) end

  status.text = ok and (text .. " is in Places")
                or ("could not make it: " .. tostring(why))

  refresh_places()

  --
  -- Where it went, in the window, for the display harness - which clicks it
  -- and right-clicks it, and would otherwise count rows it cannot see: a
  -- new place comes after a hairline, not at a fixed row.
  --
  for id, it in pairs(place_by_id) do
    if it.place and it.name == text then
      local y, pitch = places:row_of(id)

      if y then
        print(("tracker: place %s at %d"):format(text,
                                                 places.y + y + pitch // 2))
      end
    end
  end
end

function rename_field:on_enter(text)
  if place_pending then return name_place(self, text) end

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
  dragged = nil

  --
  -- **A drop on this window's own Places is not a move**, and the answer it
  -- sends back says nothing moved - which is true, and arrived after the
  -- drop handler asked for the place's name, so the prompt was replaced by
  -- "nothing moved" at the moment somebody was reading it. Photographed on
  -- 18 September. While a place is being named, the prompt stands.
  --
  if place_pending then return end

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

--
-- **Added in the order the keyboard should meet them**: the files first,
-- so arrows move among them from the start; then the places, the
-- sidebar's head and the header. And in the order they are drawn: the
-- grounds under everything, the status line over the pane's foot.
--
-- The sidebar's menu: here, because `empty_trash` is defined above it.
side_menu.on_click = function()
  win:open_menu(win.origin_x + side_menu.x, win.origin_y + L.head, {
    { text = "New window", on_choose = function()
        fs.send("/app/wm", { type = "launch", program = "tracker",
                             args = where })
      end },
    { separator = true },
    { text = "Empty Trash", on_choose = function() empty_trash() end },
  })
end

chrome(side_ground)
chrome(pane_ground)
win:add(rows)
chrome(places)
chrome(side_head)
chrome(find_button)
chrome(side_menu)
chrome(search)
chrome(header)
chrome(rename_field)
chrome(status)
chrome(count)

refresh_places()

show(where)
win:run()
