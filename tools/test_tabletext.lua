-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A table as text and back, on the host (`user/init/tabletext.lua`): what a
-- settings file looks like, that every value comes back as it went, and
-- that reading takes values and nothing else - no name, no call, no code -
-- and says which line it stopped at.

local tt = dofile("user/init/tabletext.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then
    checks = checks + 1
  else
    fails = fails + 1
    print("  " .. what)
  end
end

-- Deep equality, types included: 1 and 1.0 are not the same here.
local function same(a, b)
  if type(a) ~= type(b) then return false end
  if type(a) ~= "table" then return a == b and math.type(a) == math.type(b) end

  for k, v in pairs(a) do
    if not same(v, b[k]) then return false end
  end

  for k in pairs(b) do
    if a[k] == nil then return false end
  end

  return true
end

-- 1. What a settings file looks like: the mark, keys sorted, a short list
-- on one line, two spaces in.
local appearance = { palette = "night", bar = "dock", shadow = true,
                     dock_transparency = 25, scale = 1.25,
                     pins = { "tracker", "terminal", "music" } }
local text = tt.encode(appearance)

check(text == table.concat({
  "-- kosmos: table",
  "{",
  "  bar = \"dock\",",
  "  dock_transparency = 25,",
  "  palette = \"night\",",
  "  pins = { \"tracker\", \"terminal\", \"music\" },",
  "  scale = 1.25,",
  "  shadow = true,",
  "}",
  "" }, "\n"), "a settings file did not read as drawn:\n" .. tostring(text))

check(same(tt.decode(text), appearance), "a settings file did not come back as written")
check(tt.encode(appearance) == text, "the same table made a different file")

-- 2. Everything a value may be, back as it went.
local all = {
  "first", 2, 3.5, -4, 0.1, 1e300, -0.0, true, false,
  nested = { deeper = { deepest = { "x" } }, [3] = "three", [false] = "no" },
  ["a key with spaces"] = "and \"quotes\" and \\ and a\nnew line and a\ttab",
  ["end"] = "a keyword is a key in brackets",
  utf8 = "Größe, 大小, día",
  control = "\0\1\127",
  [7] = "seven", [2.5] = "a float key",
  long = { "one", "two", "three", "four", "five", "six", "seven", "eight", "nine" },
  empty = {},
  big = math.maxinteger, small = math.mininteger,
}
local back, why = tt.decode(tt.encode(all))

check(back ~= nil and same(back, all), "the table of every kind did not come back: " .. tostring(why))
check(math.type(back and back[2]) == "integer" and math.type(back and back[3]) == "float",
      "an integer and a float did not stay what they were")
check(tt.encode({ x = 2.0 }):find("x = 2.0,", 1, true) ~= nil, "a float that is whole was written as an integer")

-- 3. What cannot be stored, refused with why.
local loop = {}
loop.me = loop

for _, case in ipairs({
  { { f = print }, "a function" },
  { loop, "holds itself" },
  { { n = 0 / 0 }, "not a number" },
  { { n = math.huge }, "not a number" },
  { { [{}] = 1 }, "a key that is a table" },
  { { co = coroutine.create(function() end) }, "a thread" },
}) do
  local s, w = tt.encode(case[1])
  check(s == nil and tostring(w):find(case[2], 1, true) ~= nil,
        ("storing %s was not refused as it should be: %s"):format(case[2], tostring(w)))
end

local deep = {}
local d = deep
for _ = 1, 40 do d.x = {} d = d.x end
check(tt.encode(deep) == nil, "a table forty deep was stored")

-- 4. Read by hand: comments, blank lines, any spacing, single quotes,
-- semicolons, a trailing comma, CRLF line ends, hex.
local hand = "-- kosmos: table\r\n-- what the dock looks like\r\n\r\n{  bar='dock' ;\r\n"
             .. "  --[[ a block\r\n comment ]] gap = 6, list = {1,2,3,},\r\n  hex = 0x10, f = .5, e = 1e3 }  -- end\r\n"
local h, hw = tt.decode(hand)

check(h ~= nil and h.bar == "dock" and h.gap == 6 and same(h.list, { 1, 2, 3 })
      and h.hex == 16 and h.f == 0.5 and h.e == 1000.0,
      "a file written by hand did not read: " .. tostring(hw))

-- 5. Values only: nothing runs, and a broken file says its line.
local said = {}

for _, case in ipairs({
  { "-- kosmos: table\n{ x = print }", "line 2", "print" },
  { "-- kosmos: table\n{ x = os.exit() }", "line 2", "os" },
  { "-- kosmos: table\n{\n  a = 1,\n  b = 2\n  c = 3,\n}", "line 5", "'}' was expected" },
  { "-- kosmos: table\n{ a = \"no end }", "line 2", "does not end" },
  { "-- kosmos: table\n{ a = 1 } x = 2", "line 2", "after the table" },
  { "-- kosmos: table\n{ [{}] = 1 }", "line 2", "cannot be a key" },
  { "-- kosmos: table\n{ a = nil }", "line 2", "nil" },
  { "-- kosmos: table\n{ a = 1", "line 2", "does not end" },
  { "-- kosmos: table\nreturn { a = 1 }", "line 2", "'{'" },
  { "{ a = 1 }", "line 1", "not a stored table" },
  { "-- kosmos: table\n{ a == 1 }", "line 2", "before '='" },
  { "-- kosmos: table\n{ a = function() end }", "line 2", "function" },
}) do
  local v, w = tt.decode(case[1])
  check(v == nil and tostring(w):find(case[2], 1, true) and tostring(w):find(case[3], 1, true),
        ("%q was not refused at %s for %s: %s"):format(case[1], case[2], case[3], tostring(w)))
  said[#said + 1] = w
end

-- 6. A file is a table only when it says so, and a stray text never is.
check(tt.is("-- kosmos: table\n{}") and tt.is("-- kosmos: table\r\n{}"), "a stored table was not known")
check(not tt.is("{ a = 1 }") and not tt.is("-- kosmos: tables\n{}") and not tt.is(""),
      "text that does not say it is a table was taken for one")

if fails == 0 then
  print(("PASS: %d checks on a table as text (the file a setting is, every value "
         .. "back as it went, what cannot be stored refused, a file written by hand "
         .. "read, and nothing but values read - a broken one refused at its line)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on a table as text."):format(fails, checks + fails))
os.exit(1)
