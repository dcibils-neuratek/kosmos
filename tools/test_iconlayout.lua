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

local a, b = layout.label("notes.txt", 9)
check(a == "notes.txt" and b == nil, "a name that fits is one line")

a, b = layout.label("cheatsheet.html", 9)
check(a == "cheatshee" and b == "t.html",
      "a name too long for one line carries on to the second")

local long = string.rep("a", 60) .. ".txt"
a, b = layout.label(long, 9)
check(#long == 64 and #a == 9 and #b == 9 and b:sub(1, 1) == "~"
      and b:sub(-4) == ".txt",
      "a sixty-four character name keeps its extension on the second line")

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
