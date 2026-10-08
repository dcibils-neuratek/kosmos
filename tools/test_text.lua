-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What programs and windows share about text, on this computer
-- (`user/lib/text.lua`): lines, with no empty one made of a final newline;
-- `-n` and a path in either order; a count with its thousands marked; and
-- a share as a bar of characters.
--
--   build/host/lua tools/test_text.lua

use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local text = dofile("user/lib/text.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

check(#text.lines("a\nb\n") == 2 and #text.lines("a\nb") == 2,
      "two lines, with a final newline or without")

do
  local n, path = text.count_and_path("-n 3 notes.txt", 10)
  local m, other = text.count_and_path("notes.txt -n 4", 10)
  local d = text.count_and_path("notes.txt", 10)

  check(n == 3 and path == "notes.txt" and m == 4 and other == "notes.txt" and d == 10,
        "-n and a path in either order, and the caller's default")

  local q, spaced = text.count_and_path('-n 2 "/Home/My Notes/a b.txt"', 10)

  check(q == 2 and spaced == "/Home/My Notes/a b.txt",
        "a quoted path with spaces is one path: " .. tostring(spaced))
end

for _, c in ipairs({ { 0, "0" }, { 999, "999" }, { 1000, "1,000" }, { 1204, "1,204" },
                     { 2007961344, "2,007,961,344" }, { -1234567, "-1,234,567" },
                     { 1204.9, "1,204" }, { "42", "42" }, { nil, "0" } }) do
  check(text.grouped(c[1]) == c[2],
        ("%s grouped is %q, not %q"):format(tostring(c[1]), text.grouped(c[1]), c[2]))
end

check(text.meter(50, 10) == "[|||||.....]", "half a bar: " .. text.meter(50, 10))
check(text.meter(0, 4) == "[....]" and text.meter(100, 4) == "[||||]", "empty and full")
check(text.meter(250, 4) == "[||||]" and text.meter(-5, 4) == "[....]",
      "past either end, held to the bar")

-- Capitals that keep their accents.
for _, c in ipairs({
  { "Cordón", "CORDÓN" }, { "Larrañaga", "LARRAÑAGA" }, { "Muñoz", "MUÑOZ" },
  { "Ålesund", "ÅLESUND" }, { "Łódź", "ŁÓDŹ" }, { "Kraków", "KRAKÓW" },
  { "Ελλάδα", "ΕΛΛΆΔΑ" }, { "Москва", "МОСКВА" }, { "straße", "STRAßE" },
  { "Port Alder 3", "PORT ALDER 3" }, { "東京", "東京" }, { "a\xffb", "A\xffB" },
}) do
  check(text.upper(c[1]) == c[2], ("%q in capitals is %q, not %q"):format(c[1], text.upper(c[1]), c[2]))
end

if failed == 0 then
  print(("PASS: %d checks on what programs and windows share about text (lines, "
         .. "-n and a path, thousands marked, a share as a bar, capitals that keep their accents)."):format(checks))
else
  print(("FAIL: %d of %d checks on text.lua"):format(failed, checks))
  os.exit(1)
end
