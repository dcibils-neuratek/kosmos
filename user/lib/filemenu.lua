-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What a right click offers, by what it was pressed on (`roadmap.md` 6za).
--
-- `docs/rightclick.html` is the drawing and Diego agreed it on 27 September.
-- A right click in Tracker did one thing where it did anything - the
-- launcher editor on a launcher, the icon sizes on the empty space, a place
-- taken out of the sidebar at once - and said "roms is not a launcher" on
-- everything else. Now it opens a menu of **what applies to what was
-- clicked**, and nothing that does not: no Edit on a folder, no Empty Trash
-- anywhere but on the Trash, no Paste on a file.
--
-- **The decisions, and nothing else.** This returns items with an `id`, and
-- the caller - Tracker - says what each id does, because doing is Tracker's:
-- it holds the selection, the clipboard and the window. What is here is what
-- a person would argue with, and an argument is cheaper on the build machine
-- than in a boot (`tools/test_filemenu.lua`).
--
--   local items = filemenu.items{ what = "folder", name = "roms",
--                                 pinned = false }
--   -- { { id = "open", text = "Open" }, { separator = true }, ... }
--
-- What was pressed on, `what`:
--
--   "folder"    a folder in a window           `pinned`: already a place
--   "zip"       a zip, which opening extracts   `stem`: the folder it makes
--   "file"      a file                         `opener`: what opens it
--   "lua"       a Lua file, which Open runs
--
--   and on both, `with`: every application that opens it, the default
--   first, as `{ program = "video", name = "Video" }` (`roadmap.md` 6z)
--   "launcher"  a launcher
--   "trash"     the Trash, in a window or in the sidebar
--   "several"   more than one thing selected   `count`
--   "space"     nothing: the window itself     `here`, `paste`, `icons`,
--                                              `root` when it is `/`
--   "place"     a place a person made, in the sidebar
--   "builtin"   Home, Desktop, a standard folder, in the sidebar
--   "drive"     a drive in the sidebar         `pinned`
--
-- `in_trash` on a file, folder or several: Delete there is for good, and
-- the hint says so.

local filemenu = {}

local SEP = { separator = true }

local function item(id, text, hint, off)
  return { id = id, text = text, hint = hint, off = off or nil }
end

-- Rename, Cut and Copy, then Delete, then Info: what every file and folder
-- ends with, in that order, so the hand learns one place for each.
local function tail(t)
  return {
    SEP,
    item("rename", "Rename"),
    item("cut", "Cut"),
    item("copy", "Copy"),
    SEP,
    item("delete", "Delete", t.in_trash and "for good" or "to the Trash"),
    SEP,
    item("info", "Info"),
  }
end

local function joined(head, rest)
  local out = {}

  for _, it in ipairs(head) do out[#out + 1] = it end
  for _, it in ipairs(rest) do out[#out + 1] = it end

  return out
end

local function pin_item(t)
  return t.pinned and item("unpin", "Unpin from sidebar")
         or item("pin", "Pin to sidebar")
end

local MENUS = {}

--
-- **Compress** (`roadmap.md` 6v): a zip beside what was pressed on, named
-- after it - or `Archive.zip` for several - by a program Tracker starts and
-- watches. Not on the Trash, not inside it, and not on a zip, which is
-- compressed already.
--
local function compress(t)
  if t.in_trash then return nil end

  return item("compress", "Compress")
end

function MENUS.folder(t)
  return joined({ item("open", "Open"), SEP, pin_item(t), compress(t) },
                tail(t))
end

--
-- A zip opens by extracting it, beside it, into a folder named after it
-- (`docs/rightclick.html`, answer 1) - so Extract is the first thing on it
-- and says where the files go.
--
function MENUS.zip(t)
  return joined({ item("extract", "Extract",
                       t.stem and ("into " .. t.stem) or nil) }, tail(t))
end

--
-- **Open with**, a submenu of every application that opens the file, the
-- default first and saying so (`roadmap.md` 6z). For this once: it changes
-- nothing, since what opens a type is one setting, in Preferences' File
-- types and in Info. Not offered when nothing opens it.
--
local function open_with(t)
  local sub = {}

  for i, one in ipairs(t.with or {}) do
    sub[#sub + 1] = { id = "open_with", text = one.name,
                      hint = (i == 1) and "default" or nil,
                      program = one.program }
  end

  if #sub == 0 then return nil end

  return { id = "open_with", text = "Open with", submenu = sub }
end

-- Open names what it will open the file in, so there is no guessing; with
-- nothing that opens it, Open is there and dim - the file exists, and
-- saying nothing claims it is the menu's job.
function MENUS.file(t)
  local open = t.opener and item("open", "Open", t.opener)
               or item("open", "Open", "nothing opens it", true)
  local head = { open }

  head[#head + 1] = open_with(t)
  head[#head + 1] = SEP
  head[#head + 1] = compress(t)

  return joined(head, tail(t))
end

-- Opening a Lua file runs it, so the menu says Run; Edit beside it is the
-- only way to change one - in the IDE, since 28 September (`roadmap.md`
-- 6zs) - and is offered only here and on a launcher, where it does
-- something Open does not.
function MENUS.lua(t)
  local head = { item("open", "Run"), item("edit", "Edit", "Kosmos IDE") }

  head[#head + 1] = open_with(t)
  head[#head + 1] = SEP
  head[#head + 1] = compress(t)

  return joined(head, tail(t))
end

function MENUS.launcher(t)
  return joined({ item("open", "Open"),
                  item("edit", "Edit", "Launcher editor") }, tail(t))
end

-- The count is in the words, so what is about to happen is on the item.
function MENUS.several(t)
  local n = tostring(t.count or 2)

  local head = t.in_trash and {}
               or { item("compress", "Compress " .. n .. " items"), SEP }

  return joined(head, {
    item("cut", "Cut"),
    item("copy", "Copy"),
    SEP,
    item("delete", "Delete " .. n .. " items",
         t.in_trash and "for good" or "to the Trash"),
    SEP,
    item("info", "Info"),
  })
end

-- Empty Trash is here and nowhere else in a right click: it is the one thing
-- in these menus that cannot be taken back.
function MENUS.trash()
  return { item("open", "Open"), item("empty_trash", "Empty Trash"),
           SEP, item("info", "Info") }
end

-- The window itself. The icon sizes stay, marked as they were: on the
-- desktop, which has no header, this is the only way to them. `icon_sizes`
-- is where the caller puts them, since it owns what is in force.
function MENUS.space(t)
  local out = {
    item("new_folder", "New folder"),
    item("paste", "Paste", nil, not t.paste),
    SEP,
    item("select_all", "Select all"),
  }

  if t.icons then
    out[#out + 1] = SEP
    out[#out + 1] = { id = "icon_sizes" }
  end

  out[#out + 1] = SEP
  out[#out + 1] = item("refresh", "Refresh")

  -- Not at the root: Info there would be a walk of every drive plugged in.
  if not t.root then
    out[#out + 1] = SEP
    out[#out + 1] = item("info", t.here and ("Info on " .. t.here) or "Info")
  end

  return out
end

-- A place is a shortcut: Unpin takes the shortcut, never the folder, and a
-- menu asks first where the right click used to take it at once.
function MENUS.place()
  return { item("open", "Open"), SEP, item("unpin", "Unpin from sidebar"),
           SEP, item("info", "Info") }
end

function MENUS.builtin()
  return { item("open", "Open"), SEP, item("info", "Info") }
end

function MENUS.drive(t)
  return { item("open", "Open"), SEP, pin_item(t), SEP, item("info", "Info") }
end

--
-- The items for `t`, or nil for a `what` this does not know - which the
-- caller treats as nothing to offer rather than as an empty menu.
--
function filemenu.items(t)
  local make = MENUS[t and t.what]

  return make and make(t) or nil
end

--
-- What a single entry is, in the words `items` takes. `entry` is one of
-- `files.entries`' rows; `path` its full path; `trash` where the Trash is.
--
function filemenu.what_of(entry, path, trash)
  if path == trash then return "trash" end
  if entry.kind == "directory" then return "folder" end
  if entry.kind == "launcher" then return "launcher" end

  local ext = tostring(entry.name):sub(2):match("%.([%w]+)$")

  if ext and ext:lower() == "lua" then return "lua" end
  if ext and ext:lower() == "zip" then return "zip" end

  return "file"
end

return filemenu
