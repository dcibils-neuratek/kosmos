-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Deskbar
-- kosmos: section Applications
-- kosmos: needs screen
--
-- launchpad: start something by typing part of its name.
--
--   Super + space, from anywhere
--
-- A field and a list under it, the way every launcher since Spotlight has
-- worked: type, see what matches, press Return.
--
--------------------------------------------------------------------------
-- Where the names come from, and why it is not `/bin`.
--
-- `/bin` is every program in the image - a hundred and six of them, most
-- test harnesses and benchmarks that nobody starts by name. A launcher
-- offering all of them is a list you have to read rather than glance at.
--
-- **`/home/Deskbar` is the menu, and the menu is the answer to "what can I
-- start".** A thing is in it because somebody made a launcher for it, which
-- is exactly the judgement this needs and one already being made. So this
-- reads the same tree the Deskbar does, through the same library, and a
-- launcher added there appears here without anybody being told.
--
-- That is also what lets it grow. `deskbarmenu` hands back launchers with
-- their programs, arguments and icons, so a later version that searches
-- files as well has somewhere to put them: the rows are already "a name, a
-- picture, and something to do with it" rather than a list of program
-- names.
--------------------------------------------------------------------------

local ui   = use("/lib/ui.lua")
local menu = use("/lib/deskbarmenu.lua")

-- `fs` is a global this process is handed, not a library to `use` - there is
-- no `/lib/fs.lua` and asking for one is how this failed to start at all.

local W, H = 460, 300

--
-- Everything that can be started, flattened once at open.
--
-- Flattened because a launcher pad has no sections: typing `term` should
-- find the terminal wherever somebody filed it, and a person who wanted to
-- browse by section would have opened the menu instead.
--
local function everything()
  local out = {}

  local function walk(items)
    for _, item in ipairs(items or {}) do
      if item.items then
        walk(item.items)
      elseif item.program then
        out[#out + 1] = item
      end
    end
  end

  for _, section in ipairs(menu.sections(fs, "/home/Deskbar") or {}) do
    walk(section.items)
  end

  return out
end

local all = everything()
local shown = {}

--
-- In the middle of the screen, every time.
--
-- It opened wherever the cascade put it, which for a window summoned by a
-- key from anywhere is exactly wrong: a launcher pad has no place of its
-- own to be remembered, so "where it was last time" is noise, and the middle
-- is the one position that is the same whatever else is open. Spotlight has
-- always done this and so does every launcher since.
--
local win, err = ui.window{ title = "Open", w = W, h = H, centre = true }

if not win then
  print("launchpad: " .. tostring(err))
  return
end

local list = ui.list{ x = 12, y = 58, w = W - 24, h = H - 70, items = {} }

--
-- **What matches, and in what order.**
--
-- A name that *begins* with what was typed comes before one that merely
-- contains it, because typing `te` and being offered `Notes` above
-- `Terminal` is what makes a launcher feel stupid. Within each group the
-- menu's own order is kept, which is alphabetical.
--
-- Case-insensitive, and deliberately not fuzzy: `tmnl` finding Terminal is
-- a party trick that also finds four things you did not mean, and this is a
-- list meant to stop being read as soon as the first row is right.
--
local function refilter(typed)
  local want = tostring(typed or ""):lower()
  local starts, contains, names = {}, {}, {}

  for _, item in ipairs(all) do
    local name = tostring(item.name or "")

    if want == "" then
      starts[#starts + 1] = item
    else
      local at = name:lower():find(want, 1, true)

      if at == 1 then
        starts[#starts + 1] = item
      elseif at then
        contains[#contains + 1] = item
      end
    end
  end

  shown = starts

  for _, item in ipairs(contains) do
    shown[#shown + 1] = item
  end

  for i, item in ipairs(shown) do
    names[i] = tostring(item.name or "?")
  end

  list.items = names
  list.selected = 1
  list.top = 1
  win.dirty = true
end

local function launch()
  local item = shown[list.selected or 1]

  if not item then
    return
  end

  --
  -- Asked of the window manager rather than started here, for the reason
  -- `launcher.lua` gives: this process holds a screen and nothing else, and
  -- what may be started is the window manager's judgement to make.
  --
  fs.send("/app/wm", { type = "launch",
                       program = item.program,
                       args = item.args })
  win:close()
end

local field = ui.field{
  x = 12, y = 14, w = W - 24,
  on_change = function(_, text) refilter(text) end,
  on_enter  = launch,
}

--------------------------------------------------------------------------
-- The arrows belong to the list, even though the field has the focus.
--
-- A launcher pad is one control with two halves - you type in the top and
-- you choose in the bottom - and having to press Tab between them would be
-- the widget kit's structure showing through to a person. `ui.field`
-- returns false for a key it has no use for, and an unclaimed key falls back
-- to the window, so this is where an arrow lands while the field is focused.
--
-- Clamped rather than wrapped. A list of results is short and ordered by how
-- well it matched, so running off the bottom and reappearing at the top
-- moves you further from what you wanted, not nearer.
--------------------------------------------------------------------------
function win:on_key(c)
  if c == -1 or c == -2 then
    local to = (list.selected or 1) + (c == -1 and -1 or 1)

    if #list.items > 0 then
      list.selected = math.max(1, math.min(#list.items, to))
    end

    return true
  end

  return false
end

--------------------------------------------------------------------------
-- A click looks, two clicks choose.
--
-- `ui.list` calls `on_select` when a click is released on the row it was
-- pressed on, which is a *single* click - and a single click in a list of
-- results is how you look at one. Two in quick succession is how you mean
-- it, which is what every file list since the Macintosh has said and what a
-- person expects here.
--
-- The span is read from `/dev/cpu` rather than assumed, because `sys.ticks`
-- is the counter and the two clocks differ by a quarter of a million on one
-- board and four million on another. Half a second is slow enough for a
-- hand that is not in a hurry and far short of two deliberate clicks.
--------------------------------------------------------------------------
local CLICK_SPAN = ((fs.read("/dev/cpu") or {}).counter_hz or 62500000) // 2

local clicked_row, clicked_at = nil, 0

list.on_select = function(_, _, n)
  local now = sys.ticks()

  if n == clicked_row and (now - clicked_at) < CLICK_SPAN then
    launch()
    return
  end

  clicked_row, clicked_at = n, now
end

--
-- Return, for somebody who arrowed into the list itself with Tab. It starts
-- the selection outright rather than going through `on_select`, which above
-- means "one click" and would need two.
--
local list_key = list.key

list.key = function(self, c)
  if c == 10 or c == 13 then
    launch()
    return true
  end

  return list_key(self, c)
end

win:add(ui.label{ x = 12, y = 44, w = W - 24, text = "" })
win:add(field)
win:add(list)

refilter("")
win:run()
