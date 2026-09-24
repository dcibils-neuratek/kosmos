-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Appearance
-- kosmos: needs screen
-- Edit a launcher: what it starts, with what arguments, under what picture.
--
--   launcheredit /home/Deskbar/Demos/doom
--   launcheredit /home/Desktop/Drive
--
-- Opened by right-clicking an item in the Deskbar's menu, and by
-- right-clicking an icon on the desktop. Both are the same kind of file - an
-- empty node whose attributes say what to start - so both are this window.
--
-- **It edits the file, not the menu.** A launcher is `kind=launcher` with
-- `program`, `args` and `icon`, and everything that shows one reads those.
-- So this writes those attributes and tells the Deskbar to read its tree
-- again; it does not know what a menu is, and the Deskbar does not know this
-- exists.

local ui = use("/lib/ui.lua")
local theme = ui.theme

local path = tostring(args or ""):match("^%s*(%S+)")

if not path then
  print("usage: launcheredit <path to a launcher>")
  return
end

local attrs = fs.getattr(path)

if not attrs then
  print("launcheredit: no " .. path)
  return
end

--
-- The name is the file's, and is not edited here.
--
-- Renaming is Tracker's job and it already does it; two places that rename a
-- file is two places to disagree about what happens when the new name is
-- taken. So this window is about what the launcher *does*.
--
local name = path:match("([^/]+)$") or path

--
-- Every picture the image carries, which is what there is to choose from.
--
-- `sys.asset()` with no argument lists the table the build compiled in, and
-- the icons are the `.png` in it. The test pattern is the one that is not an
-- icon - it is there so the PNG decoder has something to decode before there
-- is a filesystem - so it is the one name taken out.
--
-- Read once. The table is fixed at build time and cannot change while this
-- window is open.
--
local ICONS = {}

do
  for _, asset in ipairs(sys.asset() or {}) do
    -- A name with no folder: the 16s and 64s are the same icons again
    -- (`16x16/`, `64x64/`), and a list of each three times is not a choice.
    local base = tostring(asset):match("^([^/]+)%.png$")

    if base and base ~= "test-pattern" then ICONS[#ICONS + 1] = base end
  end

  table.sort(ICONS)
end

local W, H = 560, 540
local win, err = ui.window{ title = "Launcher", w = W, h = H, x = 180, y = 120 }

if not win then
  print("launcheredit: " .. tostring(err))
  return
end

local L = ui.layout

--------------------------------------------------------------------------
-- The window, as `docs/apps.html` draws it (`roadmap.md` 5zp): the
-- launcher's name and file in the header with Revert and Save, what it
-- starts in a card, and the picture in a card of its own above the list to
-- choose it from. It was labels and fields at x = 12 and 96, and a caption
-- for the Program field under it in the size of a label.
--------------------------------------------------------------------------

--
-- **What the field holds, said in the row's note**: the Lua file to run,
-- and it does not have to be in `/bin` - `/home/mine.lua` is as ordinary as
-- `/bin/doom.lua`. The window manager would complete a bare name, and this
-- stores the completed one on save rather than the short one, so what is in
-- the file is what runs.
--
local program = ui.field{ w = 260, text = tostring(attrs.program or "") }
local arguments = ui.field{ w = 260, text = tostring(attrs.args or "") }

local chosen = tostring(attrs.icon or "")

--------------------------------------------------------------------------
-- The picture, chosen from what there is rather than typed.
--
-- A name typed into a box is a name that can be wrong, and the only way to
-- find out was to save and look at the menu. Forty-eight names is a list
-- somebody can read, and the one thing a list cannot show is what the
-- picture *looks like* - so the chosen one is drawn in its row at the size
-- the Deskbar draws it.
--
-- `g:icon` takes the asset's own name, so the `.png` goes back on here: the
-- launcher's attribute is `App_Generic` and the file is `App_Generic.png`,
-- and every other reader of a launcher does the same join.
--------------------------------------------------------------------------

local preview = ui.view{ w = 32, h = 32 }

function preview:draw(g)
  if chosen ~= "" then g:icon(0, 0, chosen .. ".png", 32) end
end

local save, revert               -- the verbs, below

local header = ui.header{
  x = 0, y = 0, w = W, title = name, sub = path,
  right = { ui.button{ text = "Revert", on_click = function() revert() end },
            ui.button{ text = "Save", go = true,
                       on_click = function() save() end } },
}

local function picture_row()
  return { label = (chosen ~= "") and chosen or "No picture of its own",
           note = "what the Deskbar and the desktop draw", control = preview }
end

local cards = ui.cards{
  x = 0, y = L.head, w = W, h = 1,
  follow = { "left", "right", "top" },
  groups = {
    { name = "Starts", rows = {
        { label = "Program", note = "a Lua file, anywhere", control = program },
        { label = "With", note = "what follows its name",
          control = arguments } } },
    { name = "Picture", rows = { picture_row() } },
  },
}

cards.h = cards.content_h

local list_y = L.head + cards.content_h + L.between

local picture = ui.list{
  x = L.page_side, y = list_y, w = W - 2 * L.page_side,
  h = H - list_y - L.page_foot,
  items = ICONS,
  follow = { "left", "right", "top", "bottom" },
  on_select = function(_, item)
    chosen = tostring(item or "")

    -- The row names the picture as well as showing it.
    cards.groups[2].rows[1] = picture_row()
    cards:set()
    win.dirty = true
  end,
}

-- Open on the one it already has, so the list says what this launcher looks
-- like rather than starting at the top and implying the first name.
for i, one in ipairs(ICONS) do
  if one == chosen then picture.selected = i break end
end

win:add(header)
win:add(cards)
win:add(picture)

--
-- Saved, and the Deskbar told.
--
-- `setprop /app/Deskbar/menu reload` is what a person would type; this
-- writes the same property directly, which is the same thing without the
-- program in between. Deliberately not an error when no Deskbar is running -
-- editing a launcher on the desktop is an ordinary thing to do with no menu
-- open anywhere.
--
function save()
  --
  -- Typed short, stored whole - the same rule `launcher.lua` applies when
  -- it makes one at the prompt.
  --
  -- The completion belongs in the typing, not in the file. A launcher that
  -- records `doom` runs correctly and reads as broken to anybody who does
  -- not know that the window manager will put `/bin/` and `.lua` around it;
  -- one that records `/bin/doom.lua` says what it does.
  --
  local starts = program.text

  if starts ~= "" and not starts:find("/") then
    starts = "/bin/" .. starts .. ".lua"
    program.text = starts
    program.caret = #starts + 1
  end

  local ok, why = fs.setattr(path, {
    kind = "launcher",
    type = "launcher",
    program = starts,
    args = arguments.text,
    -- An empty choice means "no picture of its own", which is nil rather
    -- than an empty string: an empty string is a name, and nothing is
    -- called "".
    icon = (chosen ~= "") and chosen or nil,
  })

  if not ok then
    header.sub = "could not save: " .. tostring(why)
    win.dirty = true
    return
  end

  fs.write("/app/Deskbar/menu", "reload")

  header.sub = "saved - " .. name .. " starts " .. starts
  win.dirty = true
end

function revert()
  -- The caret with the text, or it is left pointing past the end of a
  -- shorter string and the next key typed lands nowhere.
  local function put(field, text)
    field.text = text
    field.caret = #text + 1
    field.all = false
  end

  put(program, tostring(attrs.program or ""))
  put(arguments, tostring(attrs.args or ""))

  chosen = tostring(attrs.icon or "")

  for i, one in ipairs(ICONS) do
    if one == chosen then picture.selected = i break end
  end

  cards.groups[2].rows[1] = picture_row()
  cards:set()
  header.sub = path
  win.dirty = true
end

win:run()
