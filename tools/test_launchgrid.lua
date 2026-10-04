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

-- 3. Where the tiles are: six to a row from the panel's margin.
local cw = grid.cell_w(600)

check(cw == (600 - 2 * grid.PAD) // 6, "a column is " .. cw)

local x, y = grid.place(1, 0, 600)

check(x == grid.PAD and y == grid.TOP, "the first tile is not at the top left")

x, y = grid.place(8, 0, 600)
check(x == grid.PAD + cw and y == grid.TOP + grid.CELL_H, "the eighth tile is not the second of the second row")

x, y = grid.place(8, 1, 600)
check(y == grid.TOP, "scrolled a row, the second row is not at the top")

-- 4. What a press hit.
local shown = grid.rows_shown(600)

check(shown == 5, "a 600 panel shows " .. shown .. " rows, not 5")
check(grid.hit(20, 0, 600, 600, grid.PAD + 1, grid.TOP + 1) == 1, "a press on the first tile missed it")
check(grid.hit(20, 0, 600, 600, grid.PAD + cw * 2 + 3, grid.TOP + grid.CELL_H + 3) == 9,
      "a press on the ninth tile missed it")
check(grid.hit(20, 1, 600, 600, grid.PAD + 1, grid.TOP + 1) == 7, "scrolled, a press missed the seventh")
check(grid.hit(20, 0, 600, 600, grid.PAD + 1, 30) == nil, "a press on the search hit a tile")
check(grid.hit(20, 0, 600, 600, 5, grid.TOP + 5) == nil, "a press in the margin hit a tile")
check(grid.hit(8, 0, 600, 600, grid.PAD + cw * 4, grid.TOP + grid.CELL_H + 5) == nil,
      "a press past the last tile hit something")

-- 5. The arrows: across, a row, held at the ends; down from a short row's
-- neighbour goes to the last.
check(grid.move(1, 20, "left") == 1, "left from the first went somewhere")
check(grid.move(1, 20, "right") == 2, "right did not go to the next")
check(grid.move(20, 20, "right") == 20, "right from the last went somewhere")
check(grid.move(3, 20, "down") == 9, "down did not go a row")
check(grid.move(9, 20, "up") == 3, "up did not go a row")
check(grid.move(3, 20, "up") == 3, "up from the first row went somewhere")
check(grid.move(16, 20, "down") == 20, "down to a short last row did not land on the last tile")
check(grid.move(20, 20, "down") == 20, "down from the last row went somewhere")
check(grid.move(1, 0, "down") == nil, "an empty grid has a selection")

-- 6. Kept on screen, and the wheel held at the ends.
check(grid.keep_visible(33, 0, 5) == 1, "the sixth row's tile did not scroll one row")
check(grid.keep_visible(1, 3, 5) == 0, "the first tile did not scroll back to the top")
check(grid.keep_visible(13, 1, 5) == 1, "a tile on screen moved the rows")
check(grid.scroll(0, 48, 5, 3) == 3, "three notches did not scroll three rows")
check(grid.scroll(2, 48, 5, 10) == 3, "the wheel scrolled past the last row (48 is 8 rows, 5 shown)")
check(grid.scroll(1, 48, 5, -4) == 0, "the wheel scrolled above the first row")

-- 7. Where the panel goes: centred on the anchor, GAP above it, on the
-- screen, and shorter when the screen is.
local px, py, pw, ph = grid.panel(860, 1364, 1720, 32)

check(pw == 600 and ph == 600 and px == 560 and py == 1364 - grid.GAP - 600,
      ("the panel at 1720x1440 is %d,%d %dx%d"):format(px, py, pw, ph))

px, py, pw, ph = grid.panel(640, 644, 1280, 32)
check(ph == 644 - grid.GAP - 32 - grid.GAP and py == 32 + grid.GAP,
      ("on a short screen the panel is %d tall at %d"):format(ph, py))

px = grid.panel(100, 1364, 1720, 32)
check(px == 0, "a panel near the left edge went off the screen")

if fails == 0 then
  print(("PASS: %d checks on the launcher grid's arithmetic (every application once and "
         .. "A to Z, a search, where each tile is, what a press hit, the arrows, the wheel, "
         .. "and where the panel sits)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the launcher grid."):format(fails, checks + fails))
os.exit(1)
