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

local W, H = 560, 400

local where = args:match("^%s*(%S+)") or "/home"

local win, err = ui.window{ title = "Tracker", w = W, h = H, x = 80, y = 60 }

if not win then
  print("tracker: " .. tostring(err))
  return
end

local GW, GH = gfx.font.w, gfx.font.h

local entries  = {}
local selected = 0
local sort_by  = "name"
local reversed = false

local here   = ui.label{ x = 12, y = 10, w = W - 24, text = where }
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
  { key = "name", title = "Name", x = 4,   w = 300 },
  { key = "size", title = "Size", x = 310, w = 110 },
  { key = "kind", title = "Kind", x = 425, w = 100 },
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

local rows = ui.view{ x = 12, y = 58, w = W - 24, h = H - 128 }

rows.focusable = true

function rows:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)

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

  local list = sorted()
  self.shown  = list

  local first = math.max(1, math.min(selected - per + 1, #list - per + 1))
  first = math.max(1, first)
  self.first = first

  for i = 0, per - 1 do
    local n = first + i
    local e = list[n]

    if not e then break end

    local y  = top + i * GH
    local on = (n == selected)
    local bg = on and theme.accent or theme.sunken
    local fg = on and theme.text_on or theme.text

    if on then g:fill(1, y, self.w - 2, GH, bg) end

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
  if action ~= "press" then return false end

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

win:add(here)

local function button(x, w, text, fn)
  win:add(ui.button{ x = x, y = 28, w = w, h = 24, text = text,
                     on_click = fn })
end

button(12,  50, "Up",     function()
  if where ~= "/" then show(files.parent(where)) end
end)

button(70,  60, "Home",   function() show("/home") end)
button(138, 76, "Refresh", function() show(where) end)

button(222, 96, "New folder", function()
  -- Named by counting rather than by asking. A dialog for a name needs a
  -- panel of its own, and the thing that makes a folder useful is that it
  -- exists; renaming is the next operation to add and is not here yet.
  local n = 1
  local name

  repeat
    name = (n == 1) and "new folder" or ("new folder " .. n)
    n = n + 1
  until not fs.getattr(files.join(where, name))

  local ok, why = fs.send(files.join(where, name), { type = "mkdir" })

  if ok then show(where) else status.text = tostring(why) end
end)

button(326, 70, "Delete", function()
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
end)

win:add(rows)
win:add(status)

show(where)
win:run()
