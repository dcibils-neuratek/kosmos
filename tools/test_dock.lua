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

if fails == 0 then
  print(("PASS: %d checks on the dock's arithmetic (names, the cells for what is pinned "
         .. "and what runs, where each goes, what a press hit and what it does)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the dock."):format(fails, checks + fails))
os.exit(1)
