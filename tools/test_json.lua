-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- /lib/json.lua on this machine: what JSON is, read and written back, and
-- what it is not, refused with where.
--
-- Cafesa3D's scenes are glTF, which is JSON, and a scene is a file from
-- somewhere else - so the refusals matter as much as the reading: a
-- trailing comma, a single quote, a comment, a number JSON does not have, a
-- string with a raw newline in it, half of a surrogate pair, and a
-- thousand nested brackets, each refused rather than half-read.

local json = assert(loadfile("user/lib/json.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- Reading what JSON is.
local v = json.decode(' { "a" : [1, 2.5, -3e2, 0, -0.25], "b": true, "c": false, "d": null,'
                      .. ' "e": "x\\"y\\\\z\\n\\u00e9\\ud83d\\ude00", "f": {} , "g": [] } ')

check(v and v.a[1] == 1 and math.type(v.a[1]) == "integer", "an integer stays an integer")
check(v and v.a[2] == 2.5 and v.a[3] == -300 and v.a[4] == 0 and v.a[5] == -0.25,
      "numbers: a fraction, an exponent, nought, a negative")
check(v and v.b == true and v.c == false and v.d == json.null, "true, false and null")
check(v and v.e == 'x"y\\z\n\u{e9}\u{1f600}', "escapes, a \\u, and a surrogate pair as one character")
check(v and type(v.f) == "table" and next(v.f) == nil and #v.g == 0, "an empty object and array")
check(json.decode("42") == 42 and json.decode('"s"') == "s", "a value on its own is a document")

-- Written and read back.
local scene = { asset = { version = "2.0" }, nodes = { { name = "Cube", translation = { 1, 2.5, -3 } } },
                ok = true, none = json.null, s = "tab\there \"quoted\" \u{e9}" }
local text = json.encode(scene)
local back = json.decode(text)

check(back and back.nodes[1].name == "Cube" and back.nodes[1].translation[2] == 2.5
      and back.ok == true and back.none == json.null and back.s == scene.s,
      "written and read back the same: " .. text)
check(json.encode(scene) == text, "the same table writes the same bytes twice")
check(json.encode({ b = 1, a = 2 }) == '{"a":2,"b":1}', "names in order")
check(json.decode(json.encode(0.1 + 0.2)) - 0.3 < 1e-9, "a float to ten digits")
check(json.encode(scene, "  "):find("\n  ") ~= nil and json.decode(json.encode(scene, "  ")) ~= nil,
      "indented, and still JSON")

-- What JSON is not: refused, and saying where.
local refusals = {
  { '[1, 2,]', "a trailing comma" },
  { "{'a': 1}", "a single quote" },
  { '{"a": 1} // no', "a comment after" },
  { '[01]', "a leading zero" },
  { '[1.]', "a point with no digits after" },
  { '[.5]', "a point with no digits before" },
  { '[1e]', "an exponent with no digits" },
  { '[+1]', "a plus sign" },
  { '[NaN]', "NaN" },
  { '["a\nb"]', "a raw newline in a string" },
  { '["\\ud83d"]', "half of a surrogate pair" },
  { '["\\x41"]', "an escape JSON does not have" },
  { '{"a" 1}', "a missing colon" },
  { '[1 2]', "a missing comma" },
  { '"open', "a string that never ends" },
  { '', "nothing at all" },
  { '[1] [2]', "two values" },
  { ('['):rep(1000) .. (']'):rep(1000), "a thousand brackets deep" },
}

for _, r in ipairs(refusals) do
  local got, why = json.decode(r[1])

  check(got == nil and type(why) == "string" and why:find("line %d+, column %d+") ~= nil,
        "refused, with where: " .. r[2] .. " (" .. tostring(why) .. ")")
end

local _, why = json.decode('{\n  "a": [1,\n  2,]\n}')

check(why and why:find("line 3") ~= nil, "a mistake on the third line is said to be there: " .. tostring(why))

if failed > 0 then
  print(("FAIL: %d of %d checks on json.lua"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on json.lua (numbers, escapes and surrogate pairs read; written and "
       .. "read back byte for byte; eighteen things JSON is not, each refused with its line "
       .. "and column)"):format(passed))
