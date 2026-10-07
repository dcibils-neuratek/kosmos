-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The launcher as a grid: what is in it, where, and what a press or a key
-- does.
--
-- `roadmap.md`, a dock at the bottom, step 4 - drawn in `docs/dock.html`
-- and agreed by Diego on 3 October: above the dock's Kosmos button, a
-- panel with its search first, then every application A to Z in round
-- tiles - beside the sections' column since 7 October, five to a row. `launchpad` draws it when the Deskbar is a dock and
-- says where (`/Running/Deskbar/anchor`), and its list of names otherwise.
--
-- **Arithmetic here, pixels in `launchpad`**, as `dock.lua` and the
-- Deskbar divide it: which applications match, where each tile is, what a
-- press landed on and where the arrows go are answers `tools/
-- test_launchgrid.lua` checks on the Mac without booting anything.

local grid = {}

--
-- **The sections as a sidebar** (`docs/launcher.html`, agreed 7 October;
-- Diego: "the categories are good but they are small and hard to click",
-- "i want to convert the categories into menus (like a sidebar of items)
-- where i can browse with the mouse without needing to click down"). The
-- pills across the top became rows down a column at the left, each as tall
-- as a menu's row, and the grid moved beside it, five across in a panel
-- grown from 600 to 820 so the tiles keep their size.
--
grid.W        = 820     -- the panel, as the drawing has it
grid.H        = 600
grid.PAD      = 18      -- inside it, all round
grid.SEARCH_H = 50      -- the search pill
grid.SIDE_W   = 236     -- the sections' column, from the panel's left edge
grid.SIDE_TOP = 82      -- its first row, under the search
grid.ROW_H    = 46      -- a section's row: a menu's row, not a pill
grid.ROW_IN   = 10      -- a row's lit band, in from the column's edges
grid.SEP_H    = 13      -- the rule under All
grid.HEAD_Y   = 90      -- "Every application" and "A to Z", beside it
grid.TOP      = 114     -- where the first row of tiles starts
grid.COLS     = 5
grid.CELL_H   = 96      -- a tile and its name under it
grid.TILE     = 52      -- the round tile
grid.ICON     = 38      -- the picture in it
grid.GAP      = 12      -- between the panel and the dock

--
-- **Every application, A to Z, each once.** The Deskbar's menu flattened -
-- its sections are how a person browses, and a grid with a search is how a
-- person finds - by name, ignoring case; a program filed twice (in
-- Applications and in a folder of one's own) is one tile, by its program.
--
--
-- **Not the launcher itself, nor the desktop and its bar**, which the menu
-- lists because it lists everything in `/Kosmos/Apps`: nobody starts the
-- thing they are starting from, or the desktop they are looking at.
--
grid.NOT_STARTED_HERE = { launchpad = true, desktop = true, deskbar = true }

local function program_name(program)
  return tostring(program or ""):match("([^/]+)%.lua$") or tostring(program or "")
end

-- What a tile says: the application's name for a person when it declares
-- one (`kosmos: name`, the menu's `title`), else the menu's name with its
-- first letter a capital, as the Deskbar titles a program (`about` is About).
function grid.title(item)
  if type(item) == "table" then
    if item.title and item.title ~= "" then return tostring(item.title) end

    item = item.name
  end

  local name = tostring(item or "")

  return name:sub(1, 1):upper() .. name:sub(2)
end

function grid.everything(items)
  local seen, out = {}, {}

  for _, item in ipairs(items or {}) do
    local key = tostring(item.program or "") .. "\0" .. tostring(item.args or "")

    if item.program and item.program ~= "" and not seen[key]
       and not grid.NOT_STARTED_HERE[program_name(item.program)] then
      seen[key] = true
      out[#out + 1] = item
    end
  end

  table.sort(out, function(a, b)
    local x, y = grid.title(a):lower(), grid.title(b):lower()

    if x ~= y then return x < y end

    return tostring(a.name) < tostring(b.name)
  end)

  return out
end

--
-- **The categories** (Diego, 3 October: "the app launcher needs a category
-- filter", "right now we have 53 apps all at once which makes find one
-- fairly hard"): All, then each of the menu's folders that holds an
-- application, in `order` - the menu's own, Diego's (`deskbarmenu.lua`'s
-- `SECTION_ORDER`) - its folders inside folded into it, so the GL demos are
-- Demos'. `item.section` is the folder at the top of the menu; a folder
-- `order` does not name comes after, as the items have it.
--
grid.ALL = "All"

function grid.categories(items, order)
  local has, out, seen = {}, { grid.ALL }, {}

  for _, item in ipairs(items or {}) do
    if item.section and item.section ~= "" then has[item.section] = true end
  end

  for _, s in ipairs(order or {}) do
    if has[s] and not seen[s] then
      seen[s] = true
      out[#out + 1] = s
    end
  end

  for _, item in ipairs(items or {}) do
    local s = item.section

    if s and has[s] and not seen[s] then
      seen[s] = true
      out[#out + 1] = s
    end
  end

  return out
end

-- The one after `current`, round to All after the last: what Tab does.
function grid.next_category(cats, current)
  for i, c in ipairs(cats or {}) do
    if c == current then return cats[i % #cats + 1] end
  end

  return grid.ALL
end

--
-- **The sections' rows**: All first and a rule under it, then the rest -
-- each `{ name, y, h }`, its `y` from the panel's top.
--
function grid.side_rows(cats)
  local out, y = {}, grid.SIDE_TOP

  for i, name in ipairs(cats or {}) do
    out[#out + 1] = { name = name, y = y, h = grid.ROW_H }
    y = y + grid.ROW_H + 2

    if i == 1 then y = y + grid.SEP_H end
  end

  return out
end

-- The section whose row a point in the panel is on, or nil.
function grid.side_hit(rows, x, y)
  if x < 0 or x >= grid.SIDE_W then return nil end

  for _, r in ipairs(rows or {}) do
    if y >= r.y and y < r.y + r.h then return r.name end
  end

  return nil
end

--
-- **A menu's pause** (the drawing's first question, agreed): a row is taken
-- at once when the pointer comes to it going up or down, and only after a
-- moment when it is heading right, across the rows between, for the grid -
-- the way a menu keeps the submenu you are going for open. "now" or "wait",
-- from where the pointer was to where it is.
--
grid.PAUSE_MS = 90

function grid.aim(from_x, from_y, x, y)
  if not from_x then return "now" end

  local dx, dy = x - from_x, y - from_y

  if dx > 0 and dx >= math.abs(dy) then return "wait" end

  return "now"
end

function grid.filter(items, typed, category)
  local want = tostring(typed or ""):lower()
  local starts, contains = {}, {}
  local only = (category and category ~= grid.ALL) and category or nil

  for _, item in ipairs(items or {}) do
    local name = grid.title(item):lower()

    if only and item.section ~= only then
      -- Another folder's.
    elseif want == "" or name:sub(1, #want) == want then
      starts[#starts + 1] = item
    elseif name:find(want, 1, true) then
      contains[#contains + 1] = item
    end
  end

  for _, item in ipairs(contains) do starts[#starts + 1] = item end

  return starts
end

-- How wide a column is, and how many rows a panel `h` tall shows.
-- Where the grid starts across the panel: right of the sections' column.
grid.GX = grid.SIDE_W + grid.PAD

function grid.cell_w(w)
  return ((w or grid.W) - grid.GX - grid.PAD) // grid.COLS
end

function grid.rows_shown(h)
  return math.max(1, ((h or grid.H) - grid.TOP - grid.PAD // 2) // grid.CELL_H)
end

function grid.rows(n)
  return (n + grid.COLS - 1) // grid.COLS
end

--
-- Where tile `i` is - its cell's x, y, w, h in the panel - with `top` the
-- first row shown (0 for the top of the list). A tile above or below what
-- shows has a place all the same; the drawing leaves it out.
--
function grid.place(i, top, w)
  local row, col = (i - 1) // grid.COLS, (i - 1) % grid.COLS
  local cw = grid.cell_w(w)

  return grid.GX + col * cw, grid.TOP + (row - (top or 0)) * grid.CELL_H, cw, grid.CELL_H
end

-- The tile a press at `x, y` landed on, of `n`, or nil: the search, the
-- heading, the margins and the cells past the last are nothing to press.
function grid.hit(n, top, w, h, x, y)
  local cw = grid.cell_w(w)

  if x < grid.GX or x >= grid.GX + cw * grid.COLS then return nil end
  if y < grid.TOP or y >= grid.TOP + grid.rows_shown(h) * grid.CELL_H then return nil end

  local col = (x - grid.GX) // cw
  local row = (y - grid.TOP) // grid.CELL_H + (top or 0)
  local i = row * grid.COLS + col + 1

  return (i >= 1 and i <= n) and i or nil
end

--
-- **Where an arrow goes** from tile `sel` of `n`: across by one, down and
-- up by a row. Held at the ends rather than wrapping - the list is ordered,
-- and running off the bottom to reappear at the top moves away from what
-- was wanted (`launchpad`'s rule for its list). Down from a last row that
-- is short goes to the last tile.
--
function grid.move(sel, n, key)
  if n < 1 then return nil end

  sel = math.max(1, math.min(n, sel or 1))

  if key == "left" then
    return math.max(1, sel - 1)
  elseif key == "right" then
    return math.min(n, sel + 1)
  elseif key == "up" then
    return sel - grid.COLS >= 1 and sel - grid.COLS or sel
  elseif key == "down" then
    if sel + grid.COLS <= n then return sel + grid.COLS end

    return grid.rows(sel) < grid.rows(n) and n or sel
  end

  return sel
end

-- The first row to show so that tile `sel` is on screen, moved as little
-- as it can be from `top`.
function grid.keep_visible(sel, top, shown)
  local row = (sel - 1) // grid.COLS

  if row < top then return row end
  if row >= top + shown then return row - shown + 1 end

  return top
end

-- The first row after scrolling `by` rows (the wheel's notches), held so
-- the last row is never above the bottom of the panel.
function grid.scroll(top, n, shown, by)
  local most = math.max(0, grid.rows(n) - shown)

  return math.max(0, math.min(most, (top or 0) + (by or 0)))
end

--
-- **Where the panel goes**, given the Deskbar's anchor - the point above
-- the dock its bottom edge is centred on - and the screen: centred on it,
-- `GAP` above it, as tall as fits below the strip at the top, and on the
-- screen. Its x, y, w, h.
--
function grid.panel(ax, ay, sw, top_strip)
  local h = math.min(grid.H, ay - grid.GAP - (top_strip or 0) - grid.GAP)
  local w = math.min(grid.W, sw - 2 * grid.GAP)
  local x = math.max(0, math.min(sw - w, ax - w // 2))

  return x, ay - grid.GAP - h, w, h
end

return grid
