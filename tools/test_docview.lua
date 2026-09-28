-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Text Editor's page, the part of it that is arithmetic (`roadmap.md` 6zs):
-- where a line breaks into rows, and the steps a caret takes being whole
-- characters. Measured here by a stand-in face - one unit a character - so
-- every answer below can be worked out by hand.
--
--   build/host/lua tools/test_docview.lua

-- `docview.lua` asks for its neighbours by their places in the running
-- system; here they are the files beside it.
local function use(path)
  local name = path:match("([^/]+)$")

  return assert(loadfile("user/lib/" .. name))()
end

local chunk = assert(loadfile(arg and arg[1] or "user/lib/docview.lua", "t",
                              setmetatable({ use = use }, { __index = _G })))
local docview = chunk()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- One unit a character, whatever its bytes.
local function chars(s) return utf8.len(s) or #s end

-- How wide bytes `a` to `b` of `s` are, in that face.
local function over(s)
  return function(a, b) return chars(s:sub(a, b)) end
end

local function rows(s, width, from)
  local out = {}

  for _, r in ipairs(docview.wrap(s, width, over(s), from)) do
    out[#out + 1] = s:sub(r[1], r[2])
  end

  return table.concat(out, "|")
end

local function wraps(s, width, want, what)
  local got = rows(s, width)

  check(got == want, ("%s: %q, not %q"):format(what, got, want))
end

wraps("", 10, "", "an empty line is one empty row")
wraps("hello world", 20, "hello world", "a line that fits is one row")
wraps("hello world", 8, "hello |world", "a break after the space, which stays on its row")
wraps("aa bb cc", 4, "aa |bb |cc", "each word on a row of its own when two do not fit")
wraps("aa bb cc", 5, "aa bb |cc", "and two to a row when they just do")
wraps("abcdefghij", 4, "abcd|efgh|ij", "a word wider than a row, cut where it stops fitting")
wraps("a bcdefgh", 4, "a |bcde|fgh", "a long word after a short one starts a row, then is cut")
wraps("éééé", 2, "éé|éé", "a cut at a character, never inside one")
wraps("x\xc3", 1, "x|\xc3", "a stray byte at the end is still a place")
wraps("one  two", 4, "one  |two", "every space after a word stays with it")
check(docview.wrap("anything", nil, over("anything"))[1][2] == 8,
      "no width - lines that do not wrap - is the whole line")

-- A styled line's hanging mark is not wrapped with its words.
check(rows("## a heading that wraps", 8, 4) == "a |heading |that |wraps",
      "from the words after the mark: " .. rows("## a heading that wraps", 8, 4))
check(rows("# ", 8, 3) == "", "a mark and nothing after it is one empty row")
check(docview.wrap("# ", 8, over("# "), 3)[1][1] == 3,
      "and that row starts after the mark")

-- The pieces cover the line, in order, with nothing missing or twice.
local text = "the quick brown fox jumps over the lazy dog and keeps going"

for width = 1, 30 do
  local list = docview.wrap(text, width, over(text))
  local joined, at = {}, 1

  for _, r in ipairs(list) do
    if r[1] ~= at then joined = nil break end

    joined[#joined + 1] = text:sub(r[1], r[2])
    at = r[2] + 1
  end

  check(joined and table.concat(joined) == text and at == #text + 1,
        ("the rows at width %d are the line, each byte once"):format(width))
end

-- Steps between characters.
local s = "aé b"

check(docview.next_char(s, 1) == 2, "after a is é")
check(docview.next_char(s, 2) == 4, "after é, two bytes, is the space")
check(docview.prev_char(s, 4) == 2, "before the space is é, at its first byte")
check(docview.prev_char(s, 2) == 1, "before é is a")
check(docview.prev_char(s, 1) == 1, "and nothing is before the first")
check(docview.next_char(s, #s) == #s + 1, "after the last is the end")

if failed > 0 then
  print(("FAIL: %d of %d checks on the document's page"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the document's page (rows broken after a word's "
       .. "spaces, a word wider than a row cut at a character, every width "
       .. "covering the line exactly, and the caret's steps whole characters)")
      :format(passed))
