-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Deskbar
-- kosmos: name Launcher
-- kosmos: section none
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
-- **`/Home/Deskbar` is the menu, and the menu is the answer to "what can I
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

local ui   = use("/Kosmos/Libraries/ui.lua")
local menu = use("/Kosmos/Libraries/deskbarmenu.lua")

-- `fs` is a global this process is handed, not a library to `use` - there is
-- no `/Kosmos/Libraries/fs.lua` and asking for one is how this failed to start at all.

local W, H = 460, 300

--
-- Everything that can be started, flattened once at open.
--
-- Flattened because a launcher pad has no sections: typing `term` should
-- find the terminal wherever somebody filed it, and a person who wanted to
-- browse by section would have opened the menu instead.
--
local function everything()
  local out, order = {}, {}

  -- Each kept with the folder at the top of the menu it was found under,
  -- which the grid's categories are (`launchgrid.categories`).
  local function walk(items, where)
    for _, item in ipairs(items or {}) do
      if item.items then
        walk(item.items, where)
      elseif item.program then
        item.section = where
        out[#out + 1] = item
      end
    end
  end

  --
  -- **The menu as the Deskbar shows it**: what ships in `/Kosmos/Deskbar`,
  -- the applications installed in `/Home/Apps`, and the person's own in
  -- `/Home/Deskbar` over both. This read the last alone, which held nothing
  -- on a machine whose person had not added to the menu - so the grid, the
  -- first thing to show the count, said "Every application, 0".
  --
  local types = use("/Kosmos/Libraries/filetypes.lua")
  local sections = menu.merge_sections(
    menu.merge_sections(menu.sections(fs, "/Kosmos/Deskbar"),
                        menu.installed(types.installed(fs), types.declared)),
    menu.sections(fs, "/Home/Deskbar"))

  for _, section in ipairs(sections or {}) do
    order[#order + 1] = section.name
    walk(section.items, section.name)
  end

  return out, order
end

local all, section_order = everything()
local shown = {}

--------------------------------------------------------------------------
-- **The grid, when the bar is a dock** (`roadmap.md`, a dock at the
-- bottom, step 4; `docs/dock.html`, agreed by Diego on 3 October - "like
-- the googlebook has"). The Deskbar says where its dock is in
-- `/Running/Deskbar/anchor`, the middle of the dock's top edge; with it,
-- this is a panel above the dock - the search first, then every
-- application A to Z in round tiles - and without it, the list below.
--
-- A popup (`ui.window{ popup = true }`): no title bar, rounded and shadowed
-- as the look has windows, and closed by the window manager when a press
-- lands anywhere outside it - which is also how a second press on the
-- Kosmos button closes it. Escape closes it, Return opens the tile chosen,
-- the arrows choose, typing searches, the wheel scrolls.
--
-- What is where is `launchgrid.lua`'s arithmetic, held on the Mac by
-- `tools/test_launchgrid.lua`; this draws it.
--------------------------------------------------------------------------
local function grid_mode(ax, ay)
  local grid = use("/Kosmos/Libraries/launchgrid.lua")
  local dock = use("/Kosmos/Libraries/dock.lua")
  local theme = ui.theme
  local screen = gfx.screen()
  local sw = screen and (screen:size()) or 1920
  local px, py, pw, ph = grid.panel(ax, ay, sw, dock.STRIP_H)
  local apps = grid.everything(all)
  local typed, list, sel, top = "", apps, (#apps > 0) and 1 or nil, 0
  local rows = grid.rows_shown(ph)
  local cats = grid.categories(apps, section_order)
  local category = grid.ALL

  local win, err = ui.window{ title = "Open", w = pw, h = ph, x = px, y = py,
                              popup = true }

  if not win then
    print("launchpad: " .. tostring(err))
    return
  end

  print(("launchpad: the grid at %d,%d %dx%d, %d applications")
        :format(px, py, pw, ph, #apps))

  -- A picture for each: the launcher's own, else the one its program
  -- declares (`-- kosmos: icon`), as the Deskbar finds them.
  local icon_of = {}

  local function picture(item)
    if item.icon and item.icon ~= "" then return item.icon end

    local program = tostring(item.program)

    if icon_of[program] == nil then
      local attrs = fs.getattr(program)

      icon_of[program] = (attrs and attrs.icon) or "App_Generic"
    end

    return icon_of[program]
  end

  -- `b` over `a` by `t` of 255, opaque: the tiles and the chosen cell are
  -- the drawing's whites at a tenth and a twelfth over the panel.
  local function mix(a, b, t)
    local function ch(shift)
      return (((a >> shift) & 0xff) * (255 - t) + ((b >> shift) & 0xff) * t) // 255
    end

    return 0xff000000 | (ch(16) << 16) | (ch(8) << 8) | ch(0)
  end

  local function search(text)
    typed = text
    list = grid.filter(apps, typed, category)
    sel = (#list > 0) and 1 or nil
    top = 0
    win.dirty = true
  end

  -- A category chosen, by its pill or by Tab: the grid that folder's alone,
  -- what was typed still searching inside it.
  local function choose(cat)
    category = cat
    search(typed)
    print(("launchpad: %s, %d"):format(category, #list))
  end

  local function open(item)
    if not item then return end

    fs.send("/Running/wm", { type = "launch", program = item.program,
                             args = item.args })
    print("launchpad: opened " .. tostring(item.name))
    win:close()
  end

  -- Two points larger than they were drawn first (Diego, 3 October: "the
  -- fonts for the app new drawer is too small, lets increase the size by
  -- 2pts"): the names and the heading, the pills, and the search, which is
  -- the look's own size and two more.
  local SMALL = 14
  local small = ui.sized("ui", SMALL)
  local PILL = 15
  local pill_face = ui.sized("ui", PILL)
  local SEARCH = (theme.fonts and theme.fonts.ui and tonumber(theme.fonts.ui.px or theme.fonts.ui.size) or 18) + 2
  local search_face = ui.sized("ui", SEARCH)
  local chips = grid.chips(cats, function(s) return gfx.measure(s, pill_face) end, pw)

  do
    local said = {}

    for _, c in ipairs(chips) do said[#said + 1] = ("%s %d,%d %dx%d"):format(c.name, c.x, grid.CHIP_Y, c.w, grid.CHIP_H) end

    print("launchpad: pills " .. table.concat(said, "; "))
  end

  -- A name as wide as its cell allows, cut with an ellipsis if not.
  local function fitted(name, room)
    if gfx.measure(name, small) <= room then return name end

    while #name > 1 and gfx.measure(name .. "...", small) > room do
      name = name:sub(1, -2)
    end

    return name .. "..."
  end

  local view = ui.view{ x = 0, y = 0, w = pw, h = ph }

  function view:draw(g)
    local P = grid.PAD
    local face = theme.window

    g:fill(0, 0, self.w, self.h, face)

    -- The search: a pill, the glass, what was typed or what to type, and
    -- the caret where the next letter goes.
    local sx, sy, sw_, sh_ = P, P, self.w - 2 * P, grid.SEARCH_H
    local tx = sx + 18 + 19 + 12
    local ty = sy + (sh_ - gfx.height(search_face)) // 2

    g:fill_round(sx, sy, sw_, sh_, theme.raised, sh_ // 2)
    g:line_icon(sx + 18, sy + (sh_ - 19) // 2, "search", theme.text_dim, 19)

    if typed == "" then
      g:text(tx, ty, "Search applications", theme.text_dim, theme.raised, "ui", SEARCH)
      g:fill(tx - 2, sy + 14, 2, sh_ - 28, theme.accent)
    else
      g:text(tx, ty, typed, theme.text, theme.raised, "ui", SEARCH)
      g:fill(tx + gfx.measure(typed, search_face) + 1, sy + 14, 2, sh_ - 28, theme.accent)
    end

    -- The categories: All, then the menu's folders, the chosen one lit.
    for _, c in ipairs(chips) do
      local on = (c.name == category)
      local back = on and theme.accent or theme.raised

      g:fill_round(c.x, grid.CHIP_Y, c.w, grid.CHIP_H, back, grid.CHIP_H // 2)
      g:text(c.x + grid.CHIP_IN, grid.CHIP_Y + (grid.CHIP_H - gfx.height(pill_face)) // 2, c.name,
             on and theme.text_on or theme.text, back, "ui", PILL)
    end

    -- What the grid holds, and in what order.
    local head = (typed ~= "") and ("%d of %d"):format(#list, #apps)
                 or (category == grid.ALL) and ("Every application, %d"):format(#apps)
                 or ("%s, %d"):format(category, #list)

    g:text(P + 8, grid.HEAD_Y, head, theme.text_dim, face, "ui", SMALL)
    g:text(self.w - P - 8 - gfx.measure("A to Z", small), grid.HEAD_Y, "A to Z",
           theme.text_dim, face, "ui", SMALL)

    -- The tiles that show: a round tile, its picture, its name under it -
    -- and the row after the last whole one, cut by the panel's edge, which
    -- says there is more below without anything to say it.
    local tile, chosen = mix(face, 0xffd6e4ff, 26), mix(face, 0xffffffff, 20)

    for i = top * grid.COLS + 1, math.min(#list, (top + rows + 1) * grid.COLS) do
      local item = list[i]
      local cx, cy, cw, ch = grid.place(i, top, self.w)
      local behind = face

      if i == sel then
        g:fill_round(cx + 2, cy, cw - 4, ch - 4, chosen, 14)
        behind = chosen
      end

      local x0 = cx + (cw - grid.TILE) // 2

      g:fill_round(x0, cy + 6, grid.TILE, grid.TILE, tile, grid.TILE // 2)
      g:icon(x0 + (grid.TILE - grid.ICON) // 2, cy + 6 + (grid.TILE - grid.ICON) // 2,
             picture(item) .. ".png", grid.ICON)

      local name = fitted(grid.title(item), cw - 8)

      g:text(cx + (cw - gfx.measure(name, small)) // 2, cy + 6 + grid.TILE + 6, name,
             theme.text, behind, "ui", SMALL)
    end

    if #list == 0 then
      g:text(P + 8, grid.TOP + 8, "Nothing here is called that", theme.text_dim, face)
    end
  end

  function view:mouse(action, x, y)
    if action ~= "press" then return false end

    local cat = grid.chip_hit(chips, x, y)

    if cat then
      choose(cat)
      return true
    end

    local i = grid.hit(#list, top, self.w, self.h, x, y)

    if i then open(list[i]) end

    return true
  end

  --
  -- **A tile's right press**: Open, and Add to Dock - the Deskbar's `pin`,
  -- by the application's name (Diego, 3 October: "how do i add or remove
  -- apps from the dock?"). Not for a page, which is the browser at an
  -- address rather than an application of its own.
  --
  function view:on_context(x, y)
    local i = grid.hit(#list, top, self.w, self.h, x, y)
    local item = i and list[i]

    if not item then return false end

    local rows = { { text = "Open", on_choose = function() open(item) end } }
    local name = tostring(item.program or ""):match("([^/]+)%.lua$")

    if name and (item.args or "") == "" then
      rows[#rows + 1] = { text = "Add to Dock", on_choose = function()
        local ok, why = fs.write("/Running/Deskbar/pin", name)

        print("launchpad: added to the dock " .. name .. (ok and "" or (": " .. tostring(why))))
      end }
    end

    win:open_menu(win.origin_x + x, win.origin_y + y, rows)
    return true
  end

  function view:wheel(n, _, _)
    top = grid.scroll(top, #list, rows, -n)
    return true
  end

  local ARROW = { [-1] = "up", [-2] = "down", [-3] = "right", [-4] = "left" }

  --
  -- The keys are the grid's: it is the one thing in the window, focused,
  -- and it takes Tab - which a window keeps for moving the focus unless
  -- what has it says `takes_tab` - to go round the categories.
  --
  view.focusable = true
  view.takes_tab = true

  function view:key(c)
    if c == 27 then
      win:close()
    elseif c == 9 then
      choose(grid.next_category(cats, category))
    elseif c == 10 or c == 13 then
      open(sel and list[sel])
    elseif c == 8 or c == 127 then
      search(typed:sub(1, (utf8.offset(typed, -1) or 1) - 1))
    elseif ARROW[c] then
      sel = grid.move(sel, #list, ARROW[c])

      if sel then top = grid.keep_visible(sel, top, rows) end
    elseif c >= 32 then
      search(typed .. utf8.char(c))
    else
      return false
    end

    win.dirty = true
    return true
  end

  win:add(view)
  win:run()
end

do
  local anchor = fs.read("/Running/Deskbar/anchor")
  local ax, ay = tostring(anchor or ""):match("^(%-?%d+),(%-?%d+)$")

  if ax then
    grid_mode(tonumber(ax), tonumber(ay))
    return
  end
end

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
    local name = menu.shown(item)

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
    names[i] = menu.shown(item)
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
  fs.send("/Running/wm", { type = "launch",
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
-- The span is read from `/Devices/cpu` rather than assumed, because `sys.ticks`
-- is the counter and the two clocks differ by a quarter of a million on one
-- board and four million on another. Half a second is slow enough for a
-- hand that is not in a hurry and far short of two deliberate clicks.
--------------------------------------------------------------------------
local CLICK_SPAN = ((fs.read("/Devices/cpu") or {}).counter_hz or 62500000) // 2

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
