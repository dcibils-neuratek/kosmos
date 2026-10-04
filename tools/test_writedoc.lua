-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Kosmos Write's document on the host (`docs/write.md`, W1): what a new one
-- is, that a document checked is checked once - written as text and read
-- back it is equal to itself - and that a document from a file keeps the
-- declared fields of the declared types, each to its range, and nothing
-- else; and what a hundred pages cost.

tabletext = dofile("user/init/tabletext.lua")

local loaded = {}

function use(path)
  local name = path:match("^/Kosmos/Libraries/(.+)$")
  assert(name, "no such library here: " .. path)

  if not loaded[name] then loaded[name] = dofile("user/lib/" .. name) end

  return loaded[name]
end

local richtext = use("/Kosmos/Libraries/richtext.lua")
local writedoc = use("/Kosmos/Libraries/writedoc.lua")

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

-- Out as text and back.
local function round(doc)
  return tabletext.decode(tabletext.encode(doc))
end

-- 1. A new document, as the Document panel draws one.
local new = writedoc.new()

check(new.paper.name == "A4" and new.paper.width_mm == 210
      and new.paper.height_mm == 297 and new.paper.landscape == false,
      "a new document is not A4 upright")
check(new.margins_mm.top == 25 and new.margins_mm.bottom == 25
      and new.margins_mm.left == 25 and new.margins_mm.right == 25,
      "a new document's margins are not 25 mm")
check(new.header.on and new.header.from_top_mm == 9 and new.footer.on
      and new.footer.from_bottom_mm == 6 and new.footer.page_numbers,
      "a new document's header and footer are not as drawn")
check(new.ligatures and not new.facing and not new.hyphenation,
      "a new document's switches are not as drawn")

local names = {}
for i, s in ipairs(new.styles) do names[i] = s.name end

check(table.concat(names, ",")
      == "Title,Subtitle,Heading 1,Heading 2,Body,Caption,Quote",
      "the styles are not the Format panel's, in its order: "
      .. table.concat(names, ","))
check(#new.body == 1 and new.body[1].style == "Body"
      and #new.body[1].runs == 0,
      "a new document is not one empty paragraph in Body")

-- Every style whole: each field present.
local whole = true

for _, s in ipairs(new.styles) do
  for _, k in ipairs(richtext.CHAR_KEYS) do whole = whole and s[k] ~= nil end
  for _, k in ipairs(richtext.PARA_KEYS) do whole = whole and s[k] ~= nil end
  whole = whole and s.next ~= nil
end

check(whole, "a new document's styles are not whole")

local title = writedoc.style(new, "Title")
check(title.face == "IBM Plex Sans" and title.weight == "Bold"
      and title.size_pt == 30 and title.next == "Body",
      "Title is not IBM Plex Sans Bold 30 pt, followed by Body")

-- 2. Checked once is checked: a new document, and one with something in it.
check(same(writedoc.check(new), new), "checking a new document changed it")

local doc = writedoc.new()

doc.body = {
  { style = "Title", runs = { { text = "Simple Home Styling" } } },
  { style = "Subtitle", runs = { { text = "Easy Decorating" } } },
  { style = "Body", runs = {
      { text = "To get started, " },
      { text = "write over", italic = true },
      { text = " this text.\nA second line, in the same paragraph.\tTabbed." },
  } },
  { style = "Body", align = "justify", spacing_lines = 1.5, drop_cap_lines = 3,
    runs = { { text = "Café, naïve, 東京", colour = "#C0392B", size_pt = 10.5,
               weight = "Bold", underline = true, strike = true } } },
  { style = "Quote", runs = {} },
}

local checked = assert(writedoc.check(doc))

check(same(writedoc.check(checked), checked),
      "checking a checked document changed it")
check(same(round(checked), checked),
      "a document written as text and read back is not equal to itself")
check(same(writedoc.check(round(checked)), checked),
      "a document read back and checked is not the one that was written")
check(richtext.plain(checked.body[3])
      == "To get started, write over this text.\nA second line, in the same "
         .. "paragraph.\tTabbed.",
      "a paragraph's text did not survive: " .. richtext.plain(checked.body[3]))
check(checked.body[4].runs[1].colour == "#c0392b",
      "a colour is not kept in lower case")
check(checked.body[4].runs[1].size_pt == 10.5 and checked.body[4].align == "justify"
      and checked.body[4].drop_cap_lines == 3,
      "a paragraph's own fields were not kept")

-- The text as it is in the file: the mark, and readable.
local text = tabletext.encode(checked)

check(text:sub(1, #tabletext.MARK) == tabletext.MARK,
      "a document's text does not start with the mark")
check(text:find('text = "Simple Home Styling"', 1, true) ~= nil
      and text:find('format = "kosmos-write"', 1, true) ~= nil,
      "a document's text does not read as one")

-- 3. Runs: what is the style's is left out, alike ones joined, empty ones
-- and bytes below a space dropped.
local p = writedoc.check{ format = "kosmos-write", version = 1, body = {
  { style = "Body", runs = {
      { text = "one ", face = "IBM Plex Serif", weight = "Regular" },
      { text = "two " },
      { text = "" },
      { text = "three\0\1\27[31m" },
      { text = "four", italic = false, size_pt = 11 },
  } } } }.body[1]

check(#p.runs == 1 and p.runs[1].text == "one two three[31mfour",
      "runs that look alike were not joined into one: " .. #p.runs)
check(p.runs[1].face == nil and p.runs[1].weight == nil
      and p.runs[1].italic == nil and p.runs[1].size_pt == nil,
      "a run kept fields equal to its style's")

-- 4. Fields to their ranges and their types, and nothing undeclared.
local odd = writedoc.check{ format = "kosmos-write", version = 1,
  run_this = "os.exit()",
  paper = { name = "Custom", width_mm = 5000, height_mm = -3 },
  margins_mm = { top = "25", left = 600 },
  header = { on = "yes", from_top_mm = 0 / 0 },
  body = {
    { style = "Body", spacing_lines = 99, before_pt = -5, align = "middle",
      drop_cap_lines = 2.7, list = "bullet", onclick = "x",
      runs = { { text = "x", size_pt = 1 / 0, colour = "red", weight = "Black",
                 italic = 1, face = "\27[2J", hidden = true } } },
    "not a paragraph",
    42,
  } }

check(odd.run_this == nil, "an undeclared field of a document was kept")
check(odd.paper.name == "Custom" and odd.paper.width_mm == 1000
      and odd.paper.height_mm == 50,
      "a custom paper was not held to 50 mm and a metre")
check(odd.margins_mm.top == (50 - 20) / 2 and odd.margins_mm.left == (1000 - 20) / 2,
      "margins were not held to their range and their type, defaults included")
check(odd.header.on == true and odd.header.from_top_mm == 9,
      "a header's fields of the wrong type were not the defaults")

local q = odd.body[1]

check(#odd.body == 1, "what is not a paragraph became one: " .. #odd.body)
check(q.spacing_lines == 5 and q.before_pt == nil and q.align == nil
      and q.drop_cap_lines == 2 and q.list == "bullet" and q.onclick == nil,
      "a paragraph's fields were not held to their ranges and types")

local r = q.runs[1]

check(r.size_pt == nil and r.colour == nil and r.weight == nil
      and r.italic == nil and r.face == nil and r.hidden == nil,
      "a run kept a field it should not have")

-- 5. Styles: filled from the default of their name, unknown ones on the
-- plain style, a name twice kept once, `next` to a style that exists.
local styled = writedoc.check{ format = "kosmos-write", version = 1,
  styles = {
    { name = "Title" },
    { name = "Mine", size_pt = 14, next = "Nowhere" },
    { name = "Title", size_pt = 99 },
    { size_pt = 12 },
  },
  body = { { style = "Mine", runs = { { text = "a" } } },
           { style = "Gone", runs = { { text = "b" } } } } }

check(#styled.styles == 2, "a style named twice or not at all was kept: "
      .. #styled.styles)
check(styled.styles[1].face == "IBM Plex Sans" and styled.styles[1].size_pt == 30
      and styled.styles[1].weight == "Bold" and styled.styles[1].underline == false,
      "a style missing its fields did not take its default's")
check(styled.styles[1].next == "Title",
      "Title's next is Body in a document that has no Body: "
      .. tostring(styled.styles[1].next))
check(styled.styles[2].size_pt == 14 and styled.styles[2].face == "IBM Plex Sans"
      and styled.styles[2].next == "Mine",
      "a style of its own did not take the plain one's fields, or kept a next to nothing")
check(styled.body[1].style == "Mine" and styled.body[2].style == "Title",
      "a paragraph in a missing style did not take the first, there being no Body: "
      .. styled.body[2].style)

local bare = writedoc.check{ format = "kosmos-write", version = 1, styles = {},
                             body = {} }

check(#bare.styles == #writedoc.STYLES and #bare.body == 1
      and bare.body[1].style == "Body",
      "a document with no styles and no body is not the defaults and a place to type")

-- 6. What is not a document is refused, and says why.
local function refused(t, words)
  local ok, why = writedoc.check(t)
  check(ok == nil and type(why) == "string" and why:find(words, 1, true) ~= nil,
        ("not refused with %q: %s"):format(words, tostring(why)))
end

refused("text", "not a document")
refused({ format = "kosmos-sheets", version = 1, body = {} }, "kosmos-sheets")
refused({ version = 1, body = {} }, "it says nothing")
refused({ format = "kosmos-write", version = "1", body = {} }, "which version")
refused({ format = "kosmos-write", version = 2, body = {} },
        "newer Kosmos Write (format 2; this one reads 1)")
refused({ format = "kosmos-write", version = 1 }, "no body")

-- 7. The paper: a name it knows is that paper; turned, it lies the other
-- way; and points from millimetres.
local letter = writedoc.check{ format = "kosmos-write", version = 1,
  paper = { name = "Letter", width_mm = 1, height_mm = 1, landscape = true },
  margins_mm = { left = 200, right = 200, top = 200 }, body = {} }

check(letter.paper.width_mm == 215.9 and letter.paper.height_mm == 279.4,
      "Letter is not 215.9 by 279.4 mm whatever the file says")

local w, h = writedoc.page_mm(letter)

check(w == 279.4 and h == 215.9, "a page on its side is not turned")
check(letter.margins_mm.left == (279.4 - 20) / 2
      and letter.margins_mm.top == (215.9 - 20) / 2,
      "margins were not held to the page as it lies")
check(writedoc.pt(25.4) == 72 and math.abs(writedoc.pt(210) - 595.28) < 0.01,
      "millimetres are not 72 points to the inch")

-- 8. **What a hundred pages cost**: a thousand paragraphs of three hundred
-- characters in three runs, the size of a short book, checked and written
-- as text, measured rather than trusted. A process's heap starts at 2 MB
-- and grows (`runtime/libc/malloc.c`), so this is not a ceiling; it is the
-- number that says whether the shape of a document has grown fat.
collectgarbage()
collectgarbage()

local before = collectgarbage("count")
local book = { format = "kosmos-write", version = 1, body = {} }
local words = ("the quick brown fox jumps over the lazy dog "):rep(7)

for i = 1, 1000 do
  book.body[i] = { style = i % 25 == 1 and "Heading 1" or "Body", runs = {
    { text = words:sub(1, 100) }, { text = words:sub(101, 200), italic = true },
    { text = words:sub(201, 300) } } }
end

local checked_book = writedoc.check(book)

book = nil
collectgarbage()
collectgarbage()

local held_kb = collectgarbage("count") - before
local book_text = tabletext.encode(checked_book)
local text_kb = #book_text / 1024

collectgarbage()
collectgarbage()

local with_text_kb = collectgarbage("count") - before

check(same(tabletext.decode(book_text), checked_book),
      "a hundred pages written as text did not read back the same")
check(with_text_kb < 2048,
      ("a hundred pages and their text take %.0f KB, which was 1,368")
      :format(with_text_kb))

print(("  a hundred pages: %.0f KB held, %.0f KB as text, %.0f KB both")
      :format(held_kb, text_kb, with_text_kb))

if fails > 0 then
  print(("writedoc: %d of %d checks failed"):format(fails, checks + fails))
  os.exit(1)
end

print(("writedoc: %d checks pass"):format(checks))
