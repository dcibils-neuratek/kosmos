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

-- 9. **Editing** (`richtext`'s, for Write, Present's boxes and Sheets'
-- cells): typed in the look before the caret, Return, a range taken out
-- across paragraphs, the text between two places, a step a character - and
-- nothing changed in place, so the body before an edit is the undo.
do
  local doc = writedoc.check{ format = "kosmos-write", version = 1, body = {
    { style = "Heading 1", runs = { { text = "Title" } } },
    { style = "Body", runs = { { text = "plain " }, { text = "bold", weight = "Bold" },
                               { text = " end" } } },
  } }
  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local body = doc.body
  local before_p2 = body[2]

  -- Typed inside "bold": the bold look, joined into its run.
  local b2, caret = richtext.type(body, { para = 2, at = 9 }, "XY")
  check(richtext.plain(b2[2]) == "plain boXYld end" and #b2[2].runs == 3
        and b2[2].runs[2].text == "boXYld" and b2[2].runs[2].weight == "Bold",
        "typing inside a bold word was not bold: " .. richtext.plain(b2[2]))
  check(caret.para == 2 and caret.at == 11, "the caret did not follow what was typed")
  check(body[2] == before_p2 and richtext.plain(body[2]) == "plain bold end"
        and b2[1] == body[1],
        "an edit changed the body it was given, or copied what it did not touch")

  -- At a run's start the caret takes the look before it: after "plain ".
  local b3 = richtext.type(body, { para = 2, at = 7 }, "Z")
  check(b3[2].runs[1].text == "plain Z" and b3[2].runs[1].weight == nil,
        "typing at a bold run's start took the bold look")

  -- Typed with line breaks: new paragraphs in the same style.
  local b4, c4 = richtext.type(body, { para = 2, at = 7 }, "one\r\ntwo\nthree ")
  check(#b4 == 4 and richtext.plain(b4[2]) == "plain one"
        and richtext.plain(b4[3]) == "two"
        and richtext.plain(b4[4]) == "three bold end"
        and b4[3].style == "Body" and c4.para == 4 and c4.at == 7,
        "typing line breaks did not make paragraphs: " .. #b4)

  -- Return at a heading's end: Body after it; in the middle, the same style.
  local b5, c5 = richtext.split(body, { para = 1, at = 6 }, by_name)
  check(#b5 == 3 and b5[2].style == "Body" and #b5[2].runs == 0
        and c5.para == 2 and c5.at == 1,
        "Return at a heading's end did not give a Body paragraph")
  local listed = richtext.arrange(body, { para = 2, at = 1 }, { para = 2, at = 1 },
                                  { list = "bullet" }, by_name)
  local b8 = richtext.split(listed, { para = 2, at = 15 }, by_name)
  check(b8[3].list == "bullet" and #b8[3].runs == 0,
        "Return at a list item's end did not go on with the list")

  local b6 = richtext.split(body, { para = 1, at = 3 }, by_name)
  check(richtext.plain(b6[1]) == "Ti" and richtext.plain(b6[2]) == "tle"
        and b6[2].style == "Heading 1",
        "Return inside a heading did not keep its style for both halves")

  -- A range across paragraphs: the first's style, the last's tail.
  local b7, c7 = richtext.delete(body, { para = 2, at = 3 }, { para = 1, at = 3 })
  check(#b7 == 1 and richtext.plain(b7[1]) == "Tiain bold end"
        and b7[1].style == "Heading 1" and c7.para == 1 and c7.at == 3,
        "a range across paragraphs was not taken out: "
        .. (b7[1] and richtext.plain(b7[1]) or "?"))
  check(richtext.text(body, { para = 1, at = 3 }, { para = 2, at = 6 })
        == "tle\nplain", "the text between two places is not what is there")

  -- Backspace over an accent is one character, and across paragraphs.
  local accented = richtext.type(body, { para = 2, at = 1 }, "caf\u{e9}")
  local s1 = richtext.step(accented, { para = 2, at = 6 }, false)
  local s2 = richtext.step(accented, { para = 2, at = 1 }, false)
  local s3 = richtext.step(accented, { para = 1, at = 6 }, true)
  check(s1.at == 4 and s2.para == 1 and s2.at == 6 and s3.para == 2 and s3.at == 1,
        "a step is not one character, or does not cross a paragraph's end")

  -- And an edited document is still one `check` keeps as it is.
  local edited = { format = "kosmos-write", version = 1, styles = doc.styles,
                   body = b4 }
  check(same(writedoc.check(edited).body, b4),
        "an edited body is not what checking it gives back")
end

-- 10. **Formatting** (W4c): a style, character fields and paragraph fields
-- over a range, and the look the panel shows.
do
  local doc = writedoc.check{ format = "kosmos-write", version = 1, body = {
    { style = "Body", align = "center", runs = { { text = "one two three" } } },
    { style = "Body", runs = { { text = "four" } } },
  } }
  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end
  local body = doc.body

  -- Bold on "two", splitting the run where the range starts and ends.
  local b1 = richtext.format(body, { para = 1, at = 5 }, { para = 1, at = 8 },
                             { weight = "Bold" }, by_name)
  check(#b1[1].runs == 3 and b1[1].runs[2].text == "two"
        and b1[1].runs[2].weight == "Bold" and b1[1].align == "center"
        and b1[2] == body[2],
        "bold over a word did not split its run, or lost the paragraph's own")

  -- Bold again on what is bold, then plain: back to one run.
  local b2 = richtext.format(b1, { para = 1, at = 5 }, { para = 1, at = 8 },
                             { weight = "Regular" }, by_name)
  check(#b2[1].runs == 1 and b2[1].runs[1].weight == nil,
        "a field set back to the style's was kept, or the runs not joined")

  -- Italic across two paragraphs, from the middle of the first.
  local b3 = richtext.format(body, { para = 2, at = 3 }, { para = 1, at = 9 },
                             { italic = true }, by_name)
  check(b3[1].runs[2].text == "three" and b3[1].runs[2].italic
        and b3[2].runs[1].text == "fo" and b3[2].runs[1].italic
        and b3[2].runs[2].italic == nil,
        "italic across two paragraphs did not reach just the range")

  -- A style chosen: the paragraph's own fields given up, its runs' kept.
  local b4 = richtext.restyle(b1, { para = 1, at = 2 }, { para = 1, at = 2 },
                              "Heading 1", by_name)
  check(b4[1].style == "Heading 1" and b4[1].align == nil
        and b4[1].runs[2].weight == "Bold",
        "choosing a style kept the paragraph's alignment or lost a run's bold")

  -- Alignment on both paragraphs; the first already centred.
  local b5 = richtext.arrange(body, { para = 1, at = 1 }, { para = 2, at = 1 },
                              { align = "right" }, by_name)
  check(b5[1].align == "right" and b5[2].align == "right",
        "alignment did not reach every paragraph the range touches")
  local b6 = richtext.arrange(b5, { para = 1, at = 1 }, { para = 1, at = 1 },
                              { align = "left" }, by_name)
  check(b6[1].align == nil, "an alignment equal to the style's was kept")

  -- What the panel shows at a place: the look before the caret.
  local look, layout, style = richtext.look_at(b1, { para = 1, at = 7 }, by_name)
  check(look.weight == "Bold" and look.face == "IBM Plex Serif"
        and layout.align == "center" and style == "Body",
        "the look at a place is not the text before it's")

  -- Typed with a look chosen and nothing selected.
  local b7 = richtext.type(body, { para = 2, at = 5 }, "!", { italic = true })
  check(b7[2].runs[2].text == "!" and b7[2].runs[2].italic,
        "typing with a chosen look did not take it")
end

-- 11. **Tables** (W5b): a table checked as everything is - its columns and
-- rows held to their ranges, every row its columns' cells, a cell text and
-- never a table or a picture - kept through a file; an edit in a cell is
-- that cell's, its other rows the same tables; Left and Right go cell by
-- cell and out; a range from text over a table takes it whole; a copy is
-- rows of tabbed cells; and a table reshaped keeps what it holds.
do
  local doc = writedoc.new()
  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local function cell(text) return { style = "Body", runs = { { text = text } } } end

  local raw = { style = "Body", table = { columns = 99, header = "yes", rows = {
    { cell("a"), { style = "Body", runs = { { text = "b" } },
                   table = { columns = 1, rows = { { cell("deep") } } },
                   picture = { name = "pictures/1.png", width_mm = 5, height_mm = 5 } } },
    "not a row",
    { cell("c") },
  } } }
  local p = richtext.paragraph(raw, by_name, "Body")

  check(p.table and p.table.columns == richtext.TABLE_COLUMNS and p.table.header == false
        and #p.table.rows == 2 and #p.table.rows[1] == richtext.TABLE_COLUMNS,
        "a table's columns, header or rows were not held to what a table is")
  check(p.table.rows[1][2].table == nil and p.table.rows[1][2].picture == nil
        and richtext.plain(p.table.rows[1][2]) == "b"
        and richtext.plain(p.table.rows[2][5]) == "",
        "a cell held a table or a picture, or a short row was not filled out")
  check(richtext.paragraph({ style = "Body", table = { columns = 2, rows = {} } }, by_name, "Body").table == nil,
        "a table with no rows was kept")

  local t = richtext.new_table(2, 2, "Body", true)
  t.table.rows[1][1] = cell("Name")
  local body = { { style = "Body", runs = { { text = "Intro" } } },
                 richtext.paragraph(t, by_name, "Body"),
                 { style = "Body", runs = { { text = "End" } } } }
  doc.body = body
  check(same(round(doc).body, body), "a table did not come back from its file as it went")

  -- An edit in a cell.
  local b2, at = richtext.type(body, { para = 2, at = 5, row = 1, col = 1 }, "s\nnow")
  check(richtext.plain(b2[2].table.rows[1][1]) == "Names now" and at.row == 1 and at.col == 1
        and at.at == 10 and b2[2].table.rows[2] == body[2].table.rows[2] and b2[1] == body[1],
        "typing in a cell was not that cell's, its line break a space, the rest shared")
  local b3 = richtext.delete(b2, { para = 2, at = 1, row = 1, col = 1 }, { para = 2, at = 7, row = 1, col = 1 })
  check(richtext.plain(b3[2].table.rows[1][1]) == "now", "a range in a cell was not taken from it")
  local b4 = richtext.format(b2, { para = 2, at = 1, row = 1, col = 1 }, { para = 2, at = 6, row = 1, col = 1 },
                             { italic = true }, by_name)
  check(b4[2].table.rows[1][1].runs[1].italic and not b4[2].table.rows[1][1].runs[2].italic,
        "a format in a cell did not reach just the range")
  local look = richtext.look_at(b4, { para = 2, at = 3, row = 1, col = 1 }, by_name)
  check(look.italic, "the look in a cell is not that cell's text's")

  -- Left and Right.
  local function eq(a, b)
    return a.para == b.para and a.at == b.at and a.row == b.row and a.col == b.col
  end
  check(eq(richtext.step(b2, { para = 1, at = 6 }, true), { para = 2, at = 1, row = 1, col = 1 }),
        "Right at a paragraph's end before a table did not enter its first cell")
  check(eq(richtext.step(b2, { para = 2, at = 10, row = 1, col = 1 }, true), { para = 2, at = 1, row = 1, col = 2 }),
        "Right at a cell's end did not reach the next cell")
  check(eq(richtext.step(b2, { para = 2, at = 1, row = 2, col = 1 }, false), { para = 2, at = 1, row = 1, col = 2 }),
        "Left at a cell's start did not reach the end of the cell before")
  check(eq(richtext.step(b2, { para = 2, at = 1, row = 2, col = 2 }, true), { para = 3, at = 1 }),
        "Right at the last cell's end did not leave the table")
  check(eq(richtext.step(b2, { para = 3, at = 1 }, false), { para = 2, at = 1, row = 2, col = 2 }),
        "Left into a table did not reach its last cell's end")
  check(richtext.before({ para = 2, at = 9, row = 1, col = 2 }, { para = 2, at = 1, row = 2, col = 1 }),
        "places in a table are not in order row by row")

  -- A range over a table takes it whole; one in a table's text stays there.
  local b5 = richtext.delete(b2, { para = 1, at = 3 }, { para = 3, at = 2 })
  check(#b5 == 1 and richtext.plain(b5[1]) == "Innd" and not b5[1].table,
        "a range over a table did not take it whole")
  local b6 = richtext.delete(b2, { para = 2, at = 1 }, { para = 3, at = 4 })
  check(#b6 == 2 and b6[2].table == nil and richtext.plain(b6[2]) == "",
        "a range from a table's start did not take the table")
  check(richtext.text(b2, { para = 1, at = 1 }, { para = 3, at = 4 }) == "Intro\nNames now\t\n\t\nEnd",
        "a table copied is not rows of tabbed cells")
  check(richtext.text(b2, { para = 2, at = 3, row = 1, col = 1 }, { para = 2, at = 6, row = 1, col = 1 }) == "mes",
        "a copy in a cell is not that cell's text")

  -- Reshaped: rows and columns added and taken at the end, what was there kept.
  local b7 = richtext.reshape(b2, 2, { rows = 3, columns = 3 }, by_name)
  check(#b7[2].table.rows == 3 and b7[2].table.columns == 3
        and richtext.plain(b7[2].table.rows[1][1]) == "Names now" and b7[2].table.header,
        "a table made larger lost what it held")
  local b8 = richtext.reshape(b7, 2, { rows = 0, columns = 0, header = false }, by_name)
  check(#b8[2].table.rows == 1 and b8[2].table.columns == 1 and not b8[2].table.header,
        "a table was made smaller than one cell, or kept its header")
  local nx = richtext.next_cell(b7, { para = 2, at = 1, row = 1, col = 3 }, 1)
  check(nx.row == 2 and nx.col == 1 and richtext.next_cell(b7, { para = 2, at = 1, row = 3, col = 3 }, 1) == nil,
        "the cell after the last of a row is not the next row's first, or there is one after the table")

  -- A style or a paragraph field over a whole table reaches its cells, and
  -- a style over a picture keeps the picture.
  local b9 = richtext.arrange(b2, { para = 1, at = 1 }, { para = 3, at = 1 }, { align = "center" }, by_name)
  check(b9[2].table.rows[2][2].align == "center", "a paragraph field over a table missed its cells")
  local pic = { style = "Body", runs = {}, picture = { name = "pictures/1.png", width_mm = 5, height_mm = 5 } }
  local b10 = richtext.restyle({ richtext.paragraph(pic, by_name, "Body") }, { para = 1, at = 1 },
                               { para = 1, at = 1 }, "Caption", by_name)
  check(b10[1].picture and b10[1].style == "Caption", "a style over a picture lost the picture")
end

-- 12. **Text boxes and line breaks** (W7a): a box is one cell however
-- many it was given, its width held to its range and its border on unless
-- it says off; a line break typed is a "\n" in the run, in a paragraph or
-- in a box's cell, never a new paragraph.
do
  local doc = writedoc.new()
  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local function cell(text) return { style = "Body", runs = { { text = text } } } end

  local p = richtext.paragraph({ style = "Body", table = { columns = 3, header = true,
    rows = { { cell("a"), cell("b") }, { cell("c") } },
    box = { width_mm = 5000, fill = "#ABCDEF" } } }, by_name, "Body")
  check(p.table.box and p.table.columns == 1 and #p.table.rows == 1 and not p.table.header
        and p.table.box.width_mm == 1000 and p.table.box.border == true
        and p.table.box.fill == "#abcdef" and richtext.plain(p.table.rows[1][1]) == "a",
        "a text box is not one cell, its width held and its border on")

  local body = { { style = "Body", runs = { { text = "onetwo" } } } }
  local b1, at = richtext.line_break(body, { para = 1, at = 4 })
  check(#b1 == 1 and richtext.plain(b1[1]) == "one\ntwo" and at.at == 5,
        "a line break made a paragraph, or is not where the caret was")

  local box = { richtext.paragraph(richtext.new_box("Body", 60), by_name, "Body") }
  local b2, at2 = richtext.type(box, { para = 1, at = 1, row = 1, col = 1 }, "Note")
  b2, at2 = richtext.line_break(b2, at2)
  b2, at2 = richtext.type(b2, at2, "more")
  check(richtext.plain(b2[1].table.rows[1][1]) == "Note\nmore" and at2.row == 1 and at2.at == 10
        and b2[1].table.box.width_mm == 60,
        "a line break in a box's cell is not in its text, or the box was lost")
  doc.body = b2
  check(same(round(doc).body, b2), "a text box did not come back from its file")
end

-- 13. **Shapes** (W7b): a kind this knows, sizes held to their range, a
-- fill that is a colour or the drawing's blue; a shape goes whole; a style
-- over one keeps it.
do
  local doc = writedoc.new()
  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local p = richtext.paragraph({ style = "Body", runs = { { text = "words" } },
    shape = { kind = "star", width_mm = 9000, height_mm = 0, fill = "red" } }, by_name, "Body")
  check(p.shape and p.shape.width_mm == 1000 and p.shape.height_mm == 1
        and p.shape.fill == "#2a55c9" and #p.runs == 0,
        "a shape's sizes or fill were not held, or it kept text")
  check(richtext.paragraph({ style = "Body", shape = { kind = "blob", width_mm = 5, height_mm = 5 } },
                           by_name, "Body").shape == nil, "a shape of no kind this knows was kept")

  local body = { { style = "Body", runs = { { text = "one" } } },
                 richtext.paragraph(richtext.new_shape("oval", "Body"), by_name, "Body"),
                 { style = "Body", runs = { { text = "two" } } } }
  local b1 = richtext.delete(body, { para = 1, at = 2 }, { para = 3, at = 2 })
  check(#b1 == 1 and richtext.plain(b1[1]) == "owo", "a range over a shape did not take it")
  local b2 = richtext.restyle(body, { para = 1, at = 1 }, { para = 3, at = 1 }, "Caption", by_name)
  check(b2[2].shape and b2[2].shape.kind == "oval", "a style over a shape lost the shape")
end

-- 14. **Charts** (W7c): a kind this knows, a height held to its range,
-- its first row always a header; its numbers read from its cells - a
-- thousands comma read past, words as nought; and the caret entering one
-- from beside it lands on the chart, never in a number.
do
  local doc = writedoc.new()
  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local function cell(text) return { style = "Body", runs = { { text = text } } } end

  local p = richtext.paragraph({ style = "Body", table = { columns = 3, header = false,
    chart = { kind = "pie", height_mm = 900 },
    rows = { { cell(""), cell("A"), cell("B") }, { cell("x"), cell("1,200"), cell("lots") } } } },
    by_name, "Body")
  check(p.table.chart.kind == "pie" and p.table.chart.height_mm == 250 and p.table.header,
        "a chart's height was not held, or its first row is not its header")
  check(richtext.paragraph({ style = "Body", table = { columns = 1, chart = { kind = "radar" },
          rows = { { cell("a") } } } }, by_name, "Body").table.chart == nil,
        "a chart of no kind this knows was kept as one")

  local data = richtext.chart_data(p.table)
  check(data.series[2] == "B" and data.categories[1] == "x" and data.values[1][1] == 1200
        and data.values[2][1] == 0, "a chart's numbers were not read as a person wrote them")

  local body = { { style = "Body", runs = { { text = "one" } } }, p,
                 { style = "Body", runs = { { text = "two" } } } }
  local into = richtext.step(body, { para = 1, at = 4 }, true)
  local back = richtext.step(body, { para = 3, at = 1 }, false)
  check(into.para == 2 and into.row == nil and back.para == 2 and back.row == nil,
        "the caret entered a chart's numbers from beside it")

  local b2 = richtext.reshape(body, 2, { rows = 4 }, by_name)
  check(b2[2].table.chart.kind == "pie" and #b2[2].table.rows == 4,
        "a chart reshaped is no longer a chart")
end

-- 15. **Comments** (W7d): a mark on the runs a comment is about, which
-- keeps them apart as a look does and is no style's; typed text under one
-- only inside it; where each is, across runs, paragraphs and a cell; one
-- taken away, the runs joined again; and a file holding only the comments
-- its text refers to.
do
  local doc = writedoc.new()
  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local body = { { style = "Body", runs = { { text = "one two three" } } },
                 { style = "Body", runs = { { text = "four" } } } }
  local b1 = richtext.format(body, { para = 1, at = 5 }, { para = 1, at = 8 }, { comment = 7 }, by_name)
  check(#b1[1].runs == 3 and b1[1].runs[2].text == "two" and b1[1].runs[2].comment == 7,
        "a comment's mark did not keep its words a run of their own")

  local inside = richtext.type(b1, { para = 1, at = 6 }, "w")
  local after = richtext.type(b1, { para = 1, at = 8 }, "s")
  check(inside[1].runs[2].text == "twwo" and after[1].runs[2].text == "two"
        and after[1].runs[3].text == "s three" and not after[1].runs[3].comment,
        "typing in a comment's words is not under it, or typing after them is")
  check(richtext.comment_at(b1, { para = 1, at = 8 }) == 7 and richtext.comment_at(b1, { para = 1, at = 4 }) == nil,
        "the comment at a place is not the one before the caret")

  local b2 = richtext.format(b1, { para = 1, at = 11 }, { para = 2, at = 3 }, { comment = 8 }, by_name)
  local ranges = richtext.comment_ranges(b2)
  check(#ranges == 2 and ranges[1].id == 7 and ranges[1].a.at == 5 and ranges[1].b.at == 8
        and ranges[2].a.para == 1 and ranges[2].b.para == 2 and ranges[2].b.at == 3,
        "where the comments are is not from their first words to their last, across paragraphs")

  local b3 = richtext.uncomment(b2, 7)
  check(#b3[1].runs == 2 and b3[1].runs[1].text == "one two th" and b3[2] == b2[2],
        "a comment taken away did not join its words with their neighbours again")

  local t = richtext.paragraph(richtext.new_table(1, 2, "Body"), by_name, "Body")
  local tb = richtext.type({ t }, { para = 1, at = 1, row = 1, col = 2 }, "cell")
  tb = richtext.format(tb, { para = 1, at = 1, row = 1, col = 2 }, { para = 1, at = 5, row = 1, col = 2 },
                       { comment = 3 }, by_name)
  local tr = richtext.comment_ranges(tb)
  check(#tr == 1 and tr[1].a.row == 1 and tr[1].a.col == 2 and tr[1].b.at == 5,
        "a comment in a table's cell is not found there")

  doc.body = b2
  doc.comments = { { id = 7, text = "Is it two?" }, { id = 8, text = "Spans\nlines" },
                   { id = 9, text = "about nothing" }, { id = 7, text = "again" }, { id = "x" } }
  local checked = writedoc.check(doc)
  check(#checked.comments == 2 and checked.comments[1].text == "Is it two?"
        and checked.comments[2].text == "Spans\nlines",
        "the file kept a comment nothing refers to, a number twice, or one of no number")
  check(same(writedoc.check(checked), checked), "a document with comments checked twice is not itself")
end

if fails > 0 then
  print(("writedoc: %d of %d checks failed"):format(fails, checks + fails))
  os.exit(1)
end

print(("writedoc: %d checks pass"):format(checks))
