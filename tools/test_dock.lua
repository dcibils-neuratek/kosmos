-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The dock's arithmetic, on the host (`user/lib/dock.lua`; `roadmap.md`, a
-- dock at the bottom): which cells there are for what is pinned and what
-- runs, where each one is, what a press hit, and what a press does.

package.path = "user/lib/?.lua;" .. package.path

local dock = dofile("user/lib/dock.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then
    checks = checks + 1
  else
    fails = fails + 1
    print("  " .. what)
  end
end

local function kinds(items)
  local out = {}

  for _, it in ipairs(items) do out[#out + 1] = it.kind == "app" and it.name or it.kind end

  return table.concat(out, ",")
end

-- 1. Names from programs.
check(dock.name("/Kosmos/Apps/groove.lua") == "groove", "a program's path is not its name")
check(dock.name("Groove") == "groove", "a name is not its own name, lowered")
check(dock.name("/Home/Apps/Doom/doom.lua") == "doom", "an installed application's name")
check(dock.name(nil) == nil and dock.name("") == nil, "nothing has a name")

-- 2. The cells: Kosmos, the pins in their order, a separator, what runs
-- unpinned in the order it started - an application once, whatever its
-- windows.
local running = {
  { handle = 4, title = "Terminal", icon = "App_Terminal", program = "/Kosmos/Apps/terminal.lua" },
  { handle = 7, title = "Log", icon = "Server_Syslog", program = "/Kosmos/Apps/logview.lua", focused = true },
  { handle = 9, title = "Terminal", icon = "App_Terminal", program = "/Kosmos/Apps/terminal.lua", hidden = true },
  { handle = 11, title = "Monitor", icon = "App_Pulse", program = "/Kosmos/Apps/sysmon.lua" },
}
local asked = {}
local items = dock.items({ "tracker", "terminal", "groove" }, running,
                         function(name) asked[#asked + 1] = name return "Icon_" .. name end)

check(kinds(items) == "kosmos,tracker,terminal,groove,separator,logview,sysmon",
      "the cells were " .. kinds(items))
check(#items[3].windows == 2 and items[3].running, "Terminal's two windows were not one running cell")
check(not items[2].running and items[2].icon == "Icon_tracker",
      "a pinned application that is not running did not ask for its picture")
check(items[3].icon == "App_Terminal", "a running application did not wear its windows' picture")
check(items[6].front and not items[3].front, "the focused window's application was not in front")

-- No separator with nothing unpinned running.
check(kinds(dock.items({ "terminal" }, { running[1] })) == "kosmos,terminal",
      "a separator with nothing after it")

-- A pin named twice is one cell.
check(kinds(dock.items({ "tracker", "tracker" }, {})) == "kosmos,tracker", "a pin twice was two cells")

-- 3. Where they go: the ends, the gaps, the Kosmos pill's width.
local width = dock.layout(items, 60)
local kosmos_w = dock.KOSMOS_IN * 2 + dock.MARK + 8 + 60

check(items[1].x == dock.PAD and items[1].w == kosmos_w, "the Kosmos button was not at the start, its width")
check(items[2].x == dock.PAD + kosmos_w + dock.GAP, "the first pin did not follow the Kosmos button")
check(items[5].w == dock.SEP, "a separator was not its own width")

local want = dock.PAD * 2 + kosmos_w + 5 * dock.CELL + dock.SEP + 6 * dock.GAP

check(width == want, ("the dock is %d wide, not %d"):format(width, want))

-- 4. What a press hit: a cell, and not an end, a gap or a separator.
check(dock.hit(items, items[3].x + 1) == items[3], "a press in Terminal's cell missed it")
check(dock.hit(items, 1) == nil, "a press on the dock's end hit something")
check(dock.hit(items, items[5].x + 2) == nil, "a press on the separator hit something")
check(dock.hit(items, items[2].x - 1) == nil, "a press in a gap hit something")

-- 5. What a press does.
local what, arg = dock.action(items[2])

check(what == "launch" and arg == "tracker", "a pin not running was not launched: " .. tostring(what))

what, arg = dock.action(items[6])
check(what == "minimise" and arg == 7, "the application in front was not put away: " .. tostring(what))

what, arg = dock.action(items[3])
check(what == "raise" and arg == 4, "an application behind did not raise its showing window: "
      .. tostring(what) .. " " .. tostring(arg))

local hidden = dock.items({}, { running[3] })[3]
what, arg = dock.action(hidden)
check(what == "raise" and arg == 9, "an application put away was not brought back")

local starting = dock.items({ "music" }, { { starting = true, program = "music", title = "Music" } })[2]
check(dock.action(starting) == "wait", "an application starting was asked for again")
check(dock.action(items[1]) == nil, "the Kosmos button is an application")

-- 6. Arranged by hand: pinning and unpinning, where a drop lands, what a
-- drop does.
local function list(t) return table.concat(t, ",") end
local pins = { "tracker", "terminal", "groove" }

check(list(dock.pin(pins, "music")) == "tracker,terminal,groove,music", "a pin did not go on the end")
check(list(dock.pin(pins, "music", 1)) == "music,tracker,terminal,groove", "a pin did not go first")
check(list(dock.pin(pins, "groove", 1)) == "groove,tracker,terminal", "a pin moved was not moved")
check(list(pins) == "tracker,terminal,groove", "pinning changed the list it was given")
check(list(dock.unpin(pins, "terminal")) == "tracker,groove", "unpinning did not take it out")
check(items[2].pinned and not items[6].pinned, "a cell did not say whether it is pinned")

-- items: kosmos, tracker, terminal, groove, separator, logview, sysmon
-- (laid out above, 60 wide words for Kosmos).
local tr, te, gr = items[2], items[3], items[4]

check(dock.drop_at(items, tr.x + 2, "groove") == 1, "a drop at the first pin's left half was not first")
check(dock.drop_at(items, te.x + te.w - 2, "tracker") == 2,
      "a drop past the middle of the second, dragging the first, was not second")
check(dock.drop_at(items, items[6].x + 10, "logview") == nil,
      "a drop among what runs unpinned landed among the pins")

local out, what = dock.drop(pins, items, "groove", tr.x + 2, 10)
check(what == "moved" and list(out) == "groove,tracker,terminal", "groove dropped first: " .. tostring(what) .. " " .. list(out))

out, what = dock.drop(pins, items, "terminal", te.x + 20, 10)
check(what == nil and list(out) == list(pins), "a drop back on its own place changed something: " .. tostring(what))

out, what = dock.drop(pins, items, "terminal", te.x + 20, -dock.REMOVE_ABOVE - 5)
check(what == "removed" and list(out) == "tracker,groove", "an icon let go above the dock was not taken out")

out, what = dock.drop(pins, items, "logview", gr.x + gr.w - 2, 10)
check(what == "kept" and list(out) == "tracker,terminal,groove,logview", "a running one dropped on the pins was not kept: " .. tostring(what) .. " " .. list(out))

out, what = dock.drop(pins, items, "groove", items[7].x + 10, 10)
check(what == "unpinned" and list(out) == "tracker,terminal", "a pin dropped after the separator was not let go")

out, what = dock.drop(pins, items, "logview", items[7].x + 10, -dock.REMOVE_ABOVE - 5)
check(what == nil, "an unpinned one let go above the dock did something")

-- 6. Magnified under the pointer: the cell under it the largest, its
-- neighbours less and the same either side, the far ones untouched; the
-- point under the pointer kept there; nothing at a peak of 1; and no jump
-- as the pointer crosses from one cell into the next.
do
  dock.sized("medium")

  local row = { { kind = "kosmos" } }

  for i = 1, 9 do row[#row + 1] = { kind = "app", name = "a" .. i } end

  dock.layout(row, 60)

  local mid = row[6]
  local px = mid.x + mid.w // 2
  local m = dock.magnify(row, px, 1.5)

  check(m[6].size == 54 and m[6].w == 75, "the cell under the pointer did not grow by half: "
        .. tostring(m[6].size) .. " " .. tostring(m[6].w))
  check(m[5].size == m[7].size and m[5].size < 54 and m[5].size > 36,
        "the neighbours did not grow less, alike: " .. tostring(m[5].size) .. " " .. tostring(m[7].size))
  check(m[2].size == 36 and m[10].size == 36, "a cell three away grew")
  check(m[1].w == row[1].w and m[1].size == nil, "the Kosmos button grew")
  check(math.abs(m[6].x + m[6].w / 2 - px) <= 1, "the cell under the pointer moved from under it: "
        .. (m[6].x + m[6].w / 2) .. " for " .. px)
  check(m[6].orig == mid and m[6].name == "a5", "a grown cell is not the cell it grew from")
  check(dock.hit(m, px) and dock.hit(m, px).orig == mid, "a press under the pointer missed the grown cell")

  local flat = dock.magnify(row, px, 1)
  local same = true

  for i, c in ipairs(flat) do
    if c.x ~= row[i].x or c.w ~= row[i].w then same = false end
  end

  check(same, "a peak of 1 moved something")

  -- Across the edge between two cells, the row's ends move by a pixel or two.
  local edge = row[6].x + row[6].w
  local a, b = dock.magnify(row, edge - 1, 1.5), dock.magnify(row, edge + 1, 1.5)

  check(math.abs(a[1].x - b[1].x) <= 3 and math.abs(a[10].x - b[10].x) <= 3,
        "the row jumped as the pointer crossed into the next cell: "
        .. a[1].x .. "/" .. b[1].x .. ", " .. a[10].x .. "/" .. b[10].x)
end

if fails == 0 then
  print(("PASS: %d checks on the dock's arithmetic (names, the cells for what is pinned "
         .. "and what runs, where each goes, what a press hit and what it does, and "
         .. "the dock arranged by hand - pinned, moved, kept, taken out, let go - and magnified under the pointer)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the dock."):format(fails, checks + fails))
os.exit(1)
