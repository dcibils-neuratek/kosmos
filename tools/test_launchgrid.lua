-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The launcher grid's arithmetic, on the host (`user/lib/launchgrid.lua`;
-- `roadmap.md`, a dock at the bottom, step 4): every application once and
-- A to Z, what a search keeps and in what order, where each tile is, what a
-- press hit, where the arrows and the wheel go, and where the panel sits.

package.path = "user/lib/?.lua;" .. package.path

local grid = dofile("user/lib/launchgrid.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then
    checks = checks + 1
  else
    fails = fails + 1
    print("  " .. what)
  end
end

local function names(items)
  local out = {}

  for _, it in ipairs(items) do out[#out + 1] = it.name end

  return table.concat(out, ",")
end

-- 1. Every application, once, A to Z, ignoring case.
local menu = {
  { name = "Terminal", program = "/Kosmos/Apps/terminal.lua" },
  { name = "about Kosmos", program = "/Kosmos/Apps/about.lua" },
  { name = "Notes", program = "/Kosmos/Apps/texteditor.lua" },
  { name = "Terminal", program = "/Kosmos/Apps/terminal.lua" },       -- filed twice
  { name = "Groove", program = "/Kosmos/Apps/groove.lua" },
  { name = "Docs", program = "" },                                    -- no program
  { name = "Test", program = "/Kosmos/Apps/terminal.lua", args = "--test" },
  { name = "launchpad", program = "/Kosmos/Apps/launchpad.lua" },     -- itself
  { name = "desktop", program = "/Kosmos/Apps/desktop.lua" },         -- what it is on
}
local all = grid.everything(menu)

check(names(all) == "about Kosmos,Groove,Notes,Terminal,Test",
      "every application A to Z was " .. names(all))

check(grid.title("about") == "About" and grid.title("Groove") == "Groove",
      "a tile's name was not given its capital")

-- 2. A search: beginning before containing, A to Z within each.
check(names(grid.filter(all, "te")) == "Terminal,Test,Notes",
      "`te` kept " .. names(grid.filter(all, "te")))
check(names(grid.filter(all, "TE")) == "Terminal,Test,Notes", "a search cared about case")
check(#grid.filter(all, "") == #all, "nothing typed did not keep everything")
check(#grid.filter(all, "zzz") == 0, "a name nothing has kept something")

-- 3. Where the tiles are: five to a row, right of the sections' column
--    (7 October, `docs/launcher.html`).
local cw = grid.cell_w(820)
check(cw == (820 - grid.SIDE_W - 2 * grid.PAD) // 5, "a column is " .. cw)
local x, y = grid.place(1, 0, 820)
check(x == grid.SIDE_W + grid.PAD and y == grid.TOP,
      "the first tile is not at the top left of the grid, right of the sections")
x, y = grid.place(7, 0, 820)
check(x == grid.GX + cw and y == grid.TOP + grid.CELL_H, "the seventh tile is not the second of the second row")
x, y = grid.place(7, 1, 820)
check(y == grid.TOP, "scrolled a row, the second row is not at the top")

-- 4. What a press hit.
local shown = grid.rows_shown(600)
check(shown == 4, "a 600 panel shows " .. shown .. " rows of tiles, not 4")
check(grid.hit(20, 0, 820, 600, grid.GX + 1, grid.TOP + 1) == 1, "a press on the first tile missed it")
check(grid.hit(20, 0, 820, 600, grid.GX + cw * 2 + 3, grid.TOP + grid.CELL_H + 3) == 8,
      "a press on the eighth tile missed it")
check(grid.hit(20, 1, 820, 600, grid.GX + 1, grid.TOP + 1) == 6, "scrolled, a press missed the sixth")
check(grid.hit(20, 0, 820, 600, grid.GX + 1, 30) == nil, "a press on the search hit a tile")
check(grid.hit(20, 0, 820, 600, grid.SIDE_W - 5, grid.TOP + 5) == nil, "a press on the sections hit a tile")
check(grid.hit(8, 0, 820, 600, grid.GX + cw * 4, grid.TOP + grid.CELL_H + 5) == nil,
      "a press past the last tile hit something")

-- 5. The arrows, five across.
check(grid.move(1, 20, "left") == 1, "left from the first went somewhere")
check(grid.move(1, 20, "right") == 2, "right did not go to the next")
check(grid.move(20, 20, "right") == 20, "right from the last went somewhere")
check(grid.move(3, 20, "down") == 8, "down did not go a row")
check(grid.move(8, 20, "up") == 3, "up did not go a row")
check(grid.move(3, 20, "up") == 3, "up from the first row went somewhere")
check(grid.move(17, 22, "down") == 22, "down to a short last row did not land on the last tile")
check(grid.move(20, 20, "down") == 20, "down from the last row went somewhere")
check(grid.move(1, 0, "down") == nil, "an empty grid has a selection")

-- 6. Kept in view, and the wheel.
check(grid.keep_visible(28, 0, 5) == 1, "the sixth row's tile did not scroll one row")
check(grid.keep_visible(1, 3, 5) == 0, "the first tile did not scroll back to the top")
check(grid.keep_visible(11, 1, 5) == 1, "a tile on screen moved the rows")
check(grid.scroll(0, 40, 5, 3) == 3, "three notches did not scroll three rows")
check(grid.scroll(2, 40, 5, 10) == 3, "the wheel scrolled past the last row (40 is 8 rows, 5 shown)")
check(grid.scroll(1, 40, 5, -4) == 0, "the wheel scrolled above the first row")

-- 7. The sections: in Diego's order, each once, only those with something.
local filed = {
  { name = "Terminal", program = "/a/terminal.lua", section = "Applications" },
  { name = "GL Gears", program = "/a/glgears.lua", section = "Demos" },
  { name = "Plasma", program = "/a/plasma.lua", section = "Demos" },
  { name = "Process Viewer", program = "/a/procs.lua", section = "System" },
  { name = "Mine", program = "/a/mine.lua", section = "Games" },
}
local cats = grid.categories(filed, { "Applications", "System", "Development", "Demos", "Preferences" })
check(table.concat(cats, ",") == "All,Applications,System,Demos,Games",
      "the categories were " .. table.concat(cats, ","))
check(names(grid.filter(filed, "", "Demos")) == "GL Gears,Plasma", "Demos kept " .. names(grid.filter(filed, "", "Demos")))
check(names(grid.filter(filed, "pl", "Demos")) == "Plasma", "a search inside Demos left Demos")
check(#grid.filter(filed, "", "All") == 5 and #grid.filter(filed, "") == 5, "All was not everything")
check(grid.next_category(cats, "Demos") == "Games" and grid.next_category(cats, "Games") == "All",
      "the next section after the last was not All")

-- 8. The sections as rows down the column: All, a rule, the rest.
local rows_ = grid.side_rows(cats)
check(#rows_ == #cats and rows_[1].name == "All" and rows_[1].y == grid.SIDE_TOP
      and rows_[1].h == grid.ROW_H, "the first row is not All at the column's top")
check(rows_[2].y == rows_[1].y + grid.ROW_H + 2 + grid.SEP_H, "the rule under All is not there")
check(rows_[3].y == rows_[2].y + grid.ROW_H + 2, "a row does not follow the one before")
check(grid.side_hit(rows_, 40, rows_[4].y + 20) == cats[4], "the pointer on the fourth row missed it")
check(grid.side_hit(rows_, grid.SIDE_W + 3, rows_[4].y + 20) == nil, "the grid's edge hit a row")
check(grid.side_hit(rows_, 40, rows_[1].y + grid.ROW_H + 5) == nil, "the rule under All hit a row")

-- 9. A menu's pause: up or down is taken now, heading right for the grid waits.
check(grid.aim(nil, nil, 40, 200) == "now", "the first place the pointer was seen waited")
check(grid.aim(40, 200, 42, 240) == "now", "straight down waited")
check(grid.aim(40, 240, 41, 200) == "now", "straight up waited")
check(grid.aim(40, 200, 90, 230) == "wait", "heading right across the rows for the grid was taken at once")
check(grid.aim(120, 200, 60, 230) == "now", "heading left waited")

-- 10. Where the panel sits: above the button, on the screen, 820 wide.
local px, py, pw, ph = grid.panel(860, 1364, 1720, 32)
check(pw == 820 and ph == 600 and px == 450 and py == 1364 - grid.GAP - 600,
      ("the panel at 1720x1440 is %d,%d %dx%d"):format(px, py, pw, ph))
px, py, pw, ph = grid.panel(640, 644, 1280, 32)
check(ph == 644 - grid.GAP - 32 - grid.GAP and py == 32 + grid.GAP,
      ("on a short screen the panel is %d tall at %d"):format(ph, py))
px = grid.panel(100, 1364, 1720, 32)
check(px == 0, "a panel near the left edge went off the screen")

if fails == 0 then
  print(("PASS: %d checks on the launcher grid's arithmetic (every application once and "
         .. "A to Z, a search, where each tile is beside the sections, what a press hit, "
         .. "the sections' rows and the menu's pause, the arrows, the wheel, and where the "
         .. "panel sits)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the launcher grid."):format(fails, checks + fails))
os.exit(1)
