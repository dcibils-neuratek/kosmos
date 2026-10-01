-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- `user/lib/longmenu.lua` on the Mac (`roadmap.md` 6zz j6): a list that fits
-- left as it is; one that does not grouped into submenus of as many as fit,
-- in order, named by their first and last; grouped again while there are
-- more groups than fit; every item reached once, in its place; and a long
-- name cut at a character, not inside one.

local longmenu = assert(loadfile("user/lib/longmenu.lua"))()

local failures, checks = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failures = failures + 1
    print(("not ok %d - %s"):format(checks, what))
  end
end

local function options(n, name)
  local out = {}

  for i = 1, n do out[i] = { text = (name or "Option %d"):format(i), n = i } end

  return out
end

-- Every leaf, in the order a person would meet them going down each menu.
local function leaves(items, out)
  out = out or {}

  for _, it in ipairs(items) do
    if it.submenu then leaves(it.submenu, out) else out[#out + 1] = it end
  end

  return out
end

-- The tallest menu anywhere in it, and the deepest.
local function tallest(items, depth)
  local most, deepest = #items, depth or 1

  for _, it in ipairs(items) do
    if it.submenu then
      local m, d = tallest(it.submenu, (depth or 1) + 1)

      most, deepest = math.max(most, m), math.max(deepest, d)
    end
  end

  return most, deepest
end

local function in_order(items, n)
  local got = leaves(items)

  if #got ~= n then return false end

  for i, it in ipairs(got) do
    if it.n ~= i then return false end
  end

  return true
end

do
  local list = options(3)

  check(longmenu.grouped(list, 40) == list, "three that fit are left as they are")
  check(longmenu.grouped(options(40), 40)[40].text == "Option 40", "forty that fit, too")
end

do
  local g = longmenu.grouped(options(120), 48)

  check(#g == 3 and #g[1].submenu == 48 and #g[2].submenu == 48 and #g[3].submenu == 24,
        "120 in fours of 48 is three groups of 48, 48 and 24")
  check(g[1].text == "Option 1 - Option 48" and g[3].text == "Option 97 - Option 120",
        "each named by its first and last: " .. tostring(g[3].text))
  check(in_order(g, 120), "every option reached once, in its place")
end

do
  local g = longmenu.grouped(options(3000), 40)
  local most, deepest = tallest(g)

  check(most <= 40, "no menu taller than forty: " .. most)
  check(deepest == 3, "3000 at forty is three deep: " .. deepest)
  check(in_order(g, 3000), "and all 3000 reached, in order")
  check(g[1].text == "Option 1 - Option 1600" and g[2].text == "Option 1601 - Option 3000",
        "groups of groups named by their first and last leaf: " .. tostring(g[1].text))
end

do
  local g = longmenu.grouped(options(5, "Ñandú, the long name of option number %d"), 2)

  check(g[1].text:find("^Ñandú, the long name o%.%.%. %- ") ~= nil,
        "a long name cut short: " .. g[1].text)
  check(utf8.len(g[1].text) ~= nil, "and still UTF-8: " .. g[1].text)
end

if failures == 0 then
  print(("PASS: %d checks on long menus grouped, on this machine."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on long menus grouped."):format(failures, checks))
os.exit(1)
