-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The desktop's arithmetic, on the build machine: where icons go and how a
-- name fits under one, from `user/lib/iconlayout.lua`.
--
--   build/host/lua tools/test_iconlayout.lua

local layout = dofile("user/lib/iconlayout.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("FAIL: " .. what)
  end
end

local function overlap(a, b, w, h)
  return a.x < b.x + w and a.x + w > b.x and a.y < b.y + h and a.y + h > b.y
end

local function none_overlap(rects, w, h)
  for i = 1, #rects do
    for j = i + 1, #rects do
      if overlap(rects[i], rects[j], w, h) then return false end
    end
  end

  return true
end

-- Labels ------------------------------------------------------------------
--
-- In a stand-in for a proportional face: narrow letters 4 wide, wide ones
-- 14 to 16, a space 5, the rest 9, and the widest - which the old counting
-- divided by - 16. The room is 104, what a 112 cell gives a name.

local WIDE = { i = 4, l = 4, ["."] = 4, [" "] = 5, m = 14, w = 13, M = 15,
               W = 16 }

local function face(s)
  local n = 0

  for _, c in utf8.codes(s) do n = n + (WIDE[utf8.char(c)] or 9) end

  return n
end

local ROOM = 104

local function fits(line)
  return line == nil or (utf8.len(line) ~= nil and face(line) <= ROOM)
end

local a, b = layout.label("Deskbar", ROOM, face)
check(a == "Deskbar" and b == nil,
      "\"Deskbar\" is one line - it was \"Deskb\" and \"ar\", six widest "
      .. "glyphs' room for 61 pixels of name (Diego, 24 September)")

a, b = layout.label("PSP MEMORY CARD", ROOM, face)
check(a == "PSP MEMORY" and b == "CARD",
      "a name breaks between words: " .. tostring(a) .. " / " .. tostring(b))

a, b = layout.label("cheatsheet.html", ROOM, face)
check(a == "cheatsheet" and b == ".html",
      "with no word to end on, before the extension: "
      .. tostring(a) .. " / " .. tostring(b))

a, b = layout.label("Screen Recording 2026-09-24 at 11.05.13 AM.mov", ROOM,
                    face)
check(fits(a) and fits(b) and b:find("...", 1, true) and b:sub(-5) == "M.mov",
      "a long name is shortened in the middle, its end kept: "
      .. tostring(a) .. " / " .. tostring(b))

local long = string.rep("a", 60) .. ".txt"
a, b = layout.label(long, ROOM, face)
check(#long == 64 and fits(a) and fits(b) and b:sub(-4) == ".txt"
      and b:find("...", 1, true),
      "a sixty-four character name keeps its extension on the second line")

a, b = layout.label("Café au lait menu.pdf", ROOM, face)
check(a == "Café au lait" and b == "menu.pdf",
      "UTF-8, cut between characters: " .. tostring(a) .. " / "
      .. tostring(b))

-- And no line of any of these is wider than its room, or cut inside a
-- character.
local every = true

for _, name in ipairs({ "x", "Deskbar", "Trash", "PSP MEMORY CARD",
                        "cheatsheet.html", long, "WWWWWWWWWWWWWWWWWW",
                        "a_very_long_file_name_without_spaces.txt",
                        "ñandú ñandú ñandú ñandú ñandú.ogg",
                        "Screen Recording 2026-09-24 at 11.05.13 AM.mov" }) do
  local p, q = layout.label(name, ROOM, face)

  every = every and fits(p) and fits(q)
end

check(every, "no line wider than its room, and none cut inside a character")

-- Placement ---------------------------------------------------------------

local CW, CH, M = 84, 72, 2

local r = layout.place({ {}, {}, {}, {}, {} }, CW, CH, 400, 300, M)
check(r[1].x == 2 and r[1].y == 2 and r[2].x == 2 and r[2].y == 74
      and r[4].x == 2 and r[4].y == 218 and r[5].x == 86 and r[5].y == 2,
      "new icons fill the first column downward, then the next")

r = layout.place({ {}, { x = 200, y = 100 }, {} }, CW, CH, 400, 300, M)
check(r[2].x == 200 and r[2].y == 100, "a placed icon stays where it was put")
check(none_overlap(r, CW, CH), "new icons do not land on a placed one")

r = layout.place({ { x = 2, y = 2 }, {} }, CW, CH, 400, 300, M)
check(r[2].x == 2 and r[2].y == 74,
      "a cell an icon was dragged onto is not given to the next one")

r = layout.place({ { x = 10000, y = -50 } }, CW, CH, 400, 300, M)
check(r[1].x == 400 - CW and r[1].y == 0,
      "a position off the screen is pulled back onto it")

local many = {}
for i = 1, 40 do many[i] = {} end
r = layout.place(many, CW, CH, 200, 160, M)

local inside = true
for _, rect in ipairs(r) do
  if rect.x < 0 or rect.y < 0 or rect.x + CW > 200 or rect.y + CH > 160 then
    inside = false
  end
end
check(inside, "more icons than cells stay on the screen")

if failed > 0 then
  print(("FAIL: %d of %d checks on the desktop's layout"):format(failed, checks))
  os.exit(1)
end

print(("PASS: %d checks on where desktop icons go and how a name fits "
       .. "under one, on this machine."):format(checks))
