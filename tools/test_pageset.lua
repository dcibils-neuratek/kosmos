-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Kosmos Write's pages on the host (`user/lib/pageset.lua`, `docs/write.md`
-- W2): paragraphs set into lines and lines onto pages, against a measure
-- whose every character is half an em wide - so where each line breaks and
-- stands can be worked out here by hand, and is.

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
local pageset = use("/Kosmos/Libraries/pageset.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then
    checks = checks + 1
  else
    fails = fails + 1
    print("  " .. what)
  end
end

local function near(a, b) return math.abs(a - b) < 1e-6 end

-- Every character half an em; a line 0.8 em above its baseline and 0.2
-- below, with no gap.
local measured = 0
local measure = {
  width = function(look, text)
    measured = measured + 1
    return utf8.len(text) * 0.5 * look.size_pt
  end,
  line = function(look)
    return 0.8 * look.size_pt, 0.2 * look.size_pt, 0
  end,
}

local function doc_of(body, extra)
  local t = { format = "kosmos-write", version = 1, body = body }
  for k, v in pairs(extra or {}) do t[k] = v end
  return assert(writedoc.check(t))
end

local function para(style, text, fields)
  local p = { style = style, runs = { { text = text } } }
  for k, v in pairs(fields or {}) do p[k] = v end
  return p
end

local PT = writedoc.pt
local LEFT, TOP = PT(25), PT(25)
local PAGE_W, PAGE_H = PT(210), PT(297)
local COLUMN = PAGE_W - 2 * LEFT
local BOTTOM = PAGE_H - PT(25)
local BODY = 11 * 0.5                     -- a Body character's width

-- A document like `doc` with another body: an edit's result.
local function with_body(doc, body)
  local out = {}
  for k, v in pairs(doc) do out[k] = v end
  out.body = body
  return out
end

-- A piece's size, from its look.
local function looks_size(set, piece) return set.looks[piece.look].size_pt end

-- Every line of a set, in order, with its page.
local function each_line(set)
  local out = {}
  for _, page in ipairs(set.pages) do
    for _, line in ipairs(page.lines) do out[#out + 1] = { page = page, line = line } end
  end
  return out
end

-- A line's text, its pieces in order.
local function text_of(line)
  local parts = {}
  for i, pc in ipairs(line.pieces) do parts[i] = pc.text end
  return table.concat(parts)
end

-- **Nothing lost or moved**: every piece's text is its paragraph's bytes at
-- its `at`.
local function faithful(doc, set)
  for _, e in ipairs(each_line(set)) do
    local plain = richtext.plain(doc.body[e.line.para])

    for _, pc in ipairs(e.line.pieces) do
      if not pc.soft and plain:sub(pc.at, pc.at + #pc.text - 1) ~= pc.text then
        return false, ("%q is not paragraph %d's bytes at %d"):format(
          pc.text, e.line.para, pc.at)
      end
    end
  end
  return true
end

-- 1. A new document: one page, one empty line in Body, the number 1 under it.
do
  local set = pageset.set(writedoc.new(), measure)
  local page = set.pages[1]

  check(#set.pages == 1 and #page.lines == 1 and #page.lines[1].pieces == 0,
        "a new document is not one page with one empty line")
  check(near(page.lines[1].baseline_pt, TOP + 0.8 * 11),
        "an empty line does not stand where Body's first line would")
  check(near(page.width_pt, PAGE_W) and near(page.height_pt, PAGE_H),
        "an A4 page is not 595.28 by 841.89 points")
  check(page.footer and page.footer.piece.text == "1"
        and near(page.footer.baseline_pt, PAGE_H - PT(6))
        and near(page.footer.piece.x_pt + page.footer.piece.width_pt / 2,
                 PAGE_W / 2),
        "the page number is not 1, centred, 6 mm from the foot")
end

-- 2. A short paragraph: one line, one piece, at the margin.
do
  local set = pageset.set(doc_of{ para("Body", "Hello world") }, measure)
  local line = set.pages[1].lines[1]

  check(#line.pieces == 1 and line.pieces[1].text == "Hello world"
        and near(line.x_pt, LEFT) and near(line.width_pt, 11 * BODY),
        "a short paragraph is not one piece at the left margin")
end

-- 3. Breaking at spaces: no line wider than the column, the text whole, one
-- space dropped at each break and nothing else.
do
  local words = {}
  for i = 1, 120 do words[i] = ("w"):rep(1 + (i * 7) % 9) end
  local text = table.concat(words, " ")
  local doc = doc_of{ para("Body", text) }
  local set = pageset.set(doc, measure)
  local lines, widest = {}, 0

  for _, e in ipairs(each_line(set)) do
    lines[#lines + 1] = text_of(e.line)
    widest = math.max(widest, e.line.width_pt)
  end

  check(#lines > 5, "120 words made only " .. #lines .. " lines")
  check(widest <= COLUMN + 1e-9, "a line is wider than the column: " .. widest)
  check(table.concat(lines, " ") == text,
        "the lines, put back together, are not the paragraph")
  check(faithful(doc, set))

  -- Greedy: no line could have taken the next line's first word.
  local greedy = true

  for i = 1, #lines - 1 do
    local next_word = lines[i + 1]:match("^%S+")
    if (#lines[i] + 1 + #next_word) * BODY <= COLUMN then greedy = false end
  end

  check(greedy, "a line ended before the column was full")
end

-- 4. A word longer than a line, broken between characters.
do
  local long = ("x"):rep(200)
  local set = pageset.set(doc_of{ para("Body", long) }, measure)
  local per = math.floor(COLUMN / BODY)
  local lines = each_line(set)

  check(#lines == 3 and #text_of(lines[1].line) == per
        and #text_of(lines[2].line) == per
        and #text_of(lines[3].line) == 200 - 2 * per,
        ("a 200-character word is not broken into %d, %d and %d"):format(
          per, per, 200 - 2 * per))
end

-- 5. A line break inside a paragraph, and a word in two looks kept whole.
do
  local doc = doc_of{ { style = "Body", runs = {
    { text = "first\nsecond " .. ("y"):rep(71) .. " bo" }, { text = "ld", weight = "Bold" },
    { text = " end" } } } }
  local set = pageset.set(doc, measure)
  local lines = each_line(set)

  check(text_of(lines[1].line) == "first" and lines[1].line.forced
        and text_of(lines[2].line):sub(1, 6) == "second",
        "a line break did not end its line")
  check(text_of(lines[3].line):sub(1, 4) == "bold",
        "a word in two looks was broken between them: "
        .. text_of(lines[3].line))
  check(#set.looks == 3, "looks are not each listed once: " .. #set.looks)
  check(faithful(doc, set))
end

-- 6. The four alignments.
do
  local long = ("word "):rep(60) .. "end"
  local set = pageset.set(doc_of{
    para("Body", "right", { align = "right" }),
    para("Body", "centre", { align = "center" }),
    para("Body", long, { align = "justify" }),
  }, measure)
  local lines = each_line(set)
  local right, centre = lines[1].line, lines[2].line

  check(near(right.x_pt + right.width_pt, LEFT + COLUMN),
        "a right-aligned line does not end at the right margin")
  check(near(centre.x_pt - LEFT, LEFT + COLUMN - (centre.x_pt + centre.width_pt)),
        "a centred line is not centred")

  local justified, last = true, lines[#lines].line

  for i = 3, #lines - 1 do
    local l = lines[i].line
    local p = l.pieces[#l.pieces]
    if not near(p.x_pt + p.width_pt, LEFT + COLUMN) or l.extra_space_pt <= 0 then
      justified = false
    end
  end

  check(justified, "a justified line does not reach the right margin")
  check(last.extra_space_pt == 0 and near(last.x_pt, LEFT),
        "a justified paragraph's last line was widened")
end

-- 7. Indents, line spacing, the space between paragraphs, none at the top.
do
  local long = ("word "):rep(40)
  local set = pageset.set(doc_of{
    para("Heading 1", "Heading"),
    para("Body", long, { indent_first_mm = 10 }),
    para("Heading 1", "Next"),
    para("Quote", "quoted"),
  }, measure)
  local lines = each_line(set)
  local h1, b1, b2 = lines[1].line, lines[2].line, lines[3].line

  check(near(h1.baseline_pt, TOP + 0.8 * 18),
        "a heading at the head of a page has its space before it")
  check(near(b1.x_pt, LEFT + PT(10)) and near(b2.x_pt, LEFT),
        "a first-line indent is not on the first line alone")
  check(near(b2.baseline_pt - b1.baseline_pt, 11 * 1.2),
        "Body's lines are not 1.2 lines apart")

  -- Heading's after (6) and Body's line (18 tall) before it.
  check(near(b1.baseline_pt - h1.baseline_pt, 18 - 0.8 * 18 + 6 + 0.8 * 11),
        "a paragraph does not stand its style's space after the one before")

  local body_last = lines[#lines - 2].line
  local next_head = lines[#lines - 1].line

  check(near(next_head.baseline_pt - body_last.baseline_pt,
             11 * 1.2 - 0.8 * 11 + 8 + 18 + 0.8 * 18),
        "a heading does not stand its space before it")
  check(near(lines[#lines].line.x_pt, LEFT + PT(10)),
        "Quote is not 10 mm in")
end

-- 8. Tabs: to every half inch from the text's left edge.
do
  local doc = doc_of{ para("Body", "a\tb\tc") }
  local set = pageset.set(doc, measure)
  local line = each_line(set)[1].line
  local xs = {}

  for _, pc in ipairs(line.pieces) do xs[#xs + 1] = pc.x_pt - LEFT end

  check(#line.pieces == 3 and near(xs[2], 36) and near(xs[3], 72),
        "tabs do not go to 36 and 72 points: " .. table.concat(xs, ","))
  check(faithful(doc, set))
end

-- 9. Onto pages: every line inside the margins, in order, and the numbers.
-- And the properties pagination promises, over documents of many shapes:
-- no paragraph leaves one line alone at the foot of a page or the head of
-- the next, and a heading is on the page with the first line of what it
-- heads.
do
  local seed = 1

  local function rand(n)
    seed = (seed * 1103515245 + 12345) % 2147483648
    return seed % n
  end

  local inside, ordered, alone, kept, numbered = true, true, true, true, true
  local total_pages = 0

  for _ = 1, 40 do
    local body = {}

    for i = 1, 30 + rand(40) do
      if rand(6) == 0 then
        body[i] = para(rand(2) == 0 and "Heading 1" or "Heading 2", "Heading " .. i)
      else
        body[i] = para("Body", ("text "):rep(5 + rand(200)))
      end
    end

    local doc = doc_of(body)
    local set = pageset.set(doc, measure)
    local last_para = 0
    local where = {}            -- paragraph -> { page = count of its lines }

    total_pages = total_pages + #set.pages

    for _, page in ipairs(set.pages) do
      numbered = numbered and page.footer.piece.text == tostring(page.number)

      for _, line in ipairs(page.lines) do
        local top_of = line.baseline_pt - line.ascent_pt

        if top_of < TOP - 1e-6 or top_of + line.height_pt > BOTTOM + 1e-6 then
          inside = false
        end

        if line.para < last_para then ordered = false end
        last_para = line.para

        where[line.para] = where[line.para] or {}
        where[line.para][page.number] = (where[line.para][page.number] or 0) + 1
      end
    end

    for n, pages in pairs(where) do
      local count = 0
      for _ in pairs(pages) do count = count + 1 end

      if count > 1 then
        for _, lines_here in pairs(pages) do
          if lines_here < 2 then alone = false end
        end
      end

      if doc.body[n].style:find("^Heading") and where[n + 1] then
        local heading_page = next(pages)
        local first_next = math.huge
        for pg in pairs(where[n + 1]) do first_next = math.min(first_next, pg) end
        if heading_page ~= first_next then kept = false end
      end
    end
  end

  check(total_pages > 80, "forty documents filled only " .. total_pages .. " pages")
  check(inside, "a line stands outside the top and bottom margins")
  check(ordered, "paragraphs are not in order down the pages")
  check(alone, "a paragraph left one line alone at the foot or head of a page")
  check(kept, "a heading was left on a page without what it heads")
  check(numbered, "a page's number is not its place")
end

-- 10. The page as it lies: Letter on its side.
do
  local set = pageset.set(doc_of({ para("Body", "x") },
    { paper = { name = "Letter", landscape = true } }), measure)

  check(near(set.pages[1].width_pt, PT(279.4))
        and near(set.pages[1].height_pt, PT(215.9)),
        "Letter on its side is not 792 by 612 points")
end

-- 11. **What a hundred pages cost**, with this measure: a thousand
-- paragraphs of three hundred characters, set, the widths asked once each.
do
  local body = {}
  local words = ("the quick brown fox jumps over the lazy dog "):rep(7)

  for i = 1, 1000 do
    body[i] = para(i % 25 == 1 and "Heading 1" or "Body", words:sub(1, 300))
  end

  local doc = doc_of(body)

  measured = 0

  local t0 = os.clock()
  local set = pageset.set(doc, measure)
  local ms = (os.clock() - t0) * 1000

  check(#set.pages >= 60, "a thousand paragraphs set onto only "
        .. #set.pages .. " pages")
  check(measured < 200, "the measure was asked " .. measured
        .. " times; a width is asked once")
  print(("  a hundred pages: %d pages set in %.0f ms on this Mac, the measure "
         .. "asked %d times"):format(#set.pages, ms, measured))
end

-- 12. **Which face a look is set in** (`faces.lua`), from a catalogue
-- shaped like the image's: Plex Sans in five weights and two slants, Plex
-- Serif with no SemiBold, and a family with no italic.
do
  local faces = use("/Kosmos/Libraries/faces.lua")
  local list = {}

  for _, w in ipairs{ { "Regular", 400 }, { "Medium", 500 },
                      { "SemiBold", 600 }, { "Bold", 700 } } do
    list[#list + 1] = { file = "IBMPlexSans-" .. w[1] .. ".ttf",
                        family = "IBM Plex Sans", weight = w[2], italic = false }
  end

  list[#list + 1] = { file = "IBMPlexSans-Italic.ttf", family = "IBM Plex Sans",
                      weight = 400, italic = true }
  list[#list + 1] = { file = "IBMPlexSans-BoldItalic.ttf",
                      family = "IBM Plex Sans", weight = 700, italic = true }
  list[#list + 1] = { file = "IBMPlexSerif-Regular.ttf",
                      family = "IBM Plex Serif", weight = 400, italic = false }
  list[#list + 1] = { file = "IBMPlexSerif-Bold.ttf",
                      family = "IBM Plex Serif", weight = 700, italic = false }
  list[#list + 1] = { file = "Oswald-Regular.ttf", family = "Oswald",
                      weight = 400, italic = false }

  local cat = faces.catalogue(list)

  local function picked(face, weight, italic)
    local f, exact = faces.pick(cat, { face = face, weight = weight,
                                       italic = italic })
    return f and f.file, exact
  end

  check(table.concat(cat.names, ",") == "IBM Plex Sans,IBM Plex Serif,Oswald",
        "the catalogue's families are not the fonts' own, in order")
  check(select(1, picked("IBM Plex Sans", "SemiBold", false))
        == "IBMPlexSans-SemiBold.ttf" and select(2, picked("IBM Plex Sans",
        "SemiBold", false)) == true, "an exact face was not found exactly")
  check(picked("IBM Plex Serif", "SemiBold", false) == "IBMPlexSerif-Bold.ttf",
        "SemiBold in a family without it is not the nearest, heavier, weight")
  check(picked("IBM Plex Serif", "Medium", false) == "IBMPlexSerif-Regular.ttf",
        "Medium in a family without it is not Regular, the nearer")
  check(picked("IBM Plex Sans", "SemiBold", true) == "IBMPlexSans-BoldItalic.ttf",
        "an italic was given up for a weight")
  check(picked("Oswald", "Regular", true) == "Oswald-Regular.ttf"
        and select(2, picked("Oswald", "Regular", true)) == false,
        "a slant a family lacks did not fall back upright and say so")
  check(picked("Comic Sans", "Bold", false) == "IBMPlexSans-Bold.ttf"
        and select(2, picked("Comic Sans", "Bold", false)) == false,
        "a family this machine lacks is not set in IBM Plex Sans, said")

  -- The measure: the font's units scaled by the size in points, each file
  -- opened once.
  local opens = 0
  local m = faces.measure(cat, function(file)
    opens = opens + 1
    return {
      metrics = function() return 1000, 780, -220, 0 end,
      advance = function(_, text) return #text * 600, 0 end,
    }
  end)
  local look = { face = "IBM Plex Sans", weight = "Regular", italic = false,
                 size_pt = 10 }
  local a, d, g = m.line(look)

  check(near(m.width(look, "abcd"), 4 * 6) and near(a, 7.8) and near(d, 2.2)
        and g == 0, "the measure is not the font's units at 10 points")
  m.width({ face = "IBM Plex Sans", weight = "Regular", italic = false,
            size_pt = 30 }, "x")
  m.width({ face = "Comic Sans", weight = "Regular", italic = false,
            size_pt = 30 }, "x")
  check(opens == 1, "a face's file was opened " .. opens .. " times")
end

-- 13. **A caret on the page** (W4b): the cache sets only what changed;
-- where a place stands, the place under a point, Up and Down, Home and End.
do
  local long = ("word "):rep(40) .. "end"
  local doc = doc_of{ para("Body", "Hello world"), para("Body", long),
                      para("Body", ""), para("Body", long, { align = "justify" }) }
  local cache = pageset.cache()

  measured = 0
  local set = pageset.set(doc, measure, cache)
  local first = measured
  local lines_of_1 = set.pages[1].lines[1]

  measured = 0
  local again = pageset.set(doc, measure, cache)
  check(measured == 0 and again.pages[1].lines[1] == lines_of_1,
        "a set with its cache set an unchanged document again")

  local body2 = richtext.type(doc.body, { para = 2, at = 1 }, "new ")
  local edited = with_body(doc, body2)
  local set2 = pageset.set(edited, measure, cache)
  check(set2.pages[1].lines[1] == lines_of_1 and first > 0,
        "an edit to one paragraph set the others again")
  check(set2.pages[1].lines[2].pieces[1].text:sub(1, 8) == "new word",
        "the edited paragraph was not set again")

  -- Where places stand, on the first line and at a wrap.
  local function at(place) return pageset.locate(set, measure, place) end

  check(near(at({ para = 1, at = 1 }).x_pt, LEFT)
        and near(at({ para = 1, at = 7 }).x_pt, LEFT + 6 * BODY)
        and near(at({ para = 1, at = 12 }).x_pt, LEFT + 11 * BODY),
        "a place on a line does not stand at its characters' advance")

  local second = set.pages[1].lines[3]               -- paragraph 2's second line
  local here = at({ para = 2, at = second.from })

  check(here.line == second and near(here.x_pt, LEFT)
        and near(here.baseline_pt, second.baseline_pt),
        "a place at a wrap does not stand at the start of the next line")
  check(at({ para = 2, at = second.from - 1 }).line == set.pages[1].lines[2],
        "the space a wrap dropped does not stand at the end of the line before")
  check(near(at({ para = 3, at = 1 }).x_pt, LEFT),
        "an empty paragraph's place is not at its line's start")

  -- A justified line: a place after its first space is that much further.
  local just = nil
  for _, l in ipairs(set.pages[1].lines) do
    if l.para == 4 and l.extra_space_pt > 0 and not just then just = l end
  end
  check(just ~= nil and near(at({ para = 4, at = just.from + 5 }).x_pt,
                             just.x_pt + 5 * BODY + just.extra_space_pt),
        "a place in a justified line does not count its widened space")

  -- The place under a point: a character's middle decides.
  local function hit(x, y) return pageset.hit(set, measure, 1, x, y) end
  local base = set.pages[1].lines[1].baseline_pt

  check(hit(LEFT + 6 * BODY + 1, base).at == 7
        and hit(LEFT + 6 * BODY - 1, base).at == 7
        and hit(LEFT + 5 * BODY + 1, base).at == 6,
        "a point does not find the place nearest it")
  check(hit(0, base).at == 1 and hit(PAGE_W, base).at == 12
        and hit(LEFT, 0).para == 1,
        "a point beyond a line or above the page does not find its end")

  -- Up and Down keep their column; Home and End are the line's.
  local down, column = pageset.vertical(set, measure, { para = 1, at = 4 }, 1)
  check(down.para == 2 and near(at(down).x_pt, LEFT + 3 * BODY) and near(column, LEFT + 3 * BODY),
        "Down did not keep the column")
  local up = pageset.vertical(set, measure, { para = 1, at = 4 }, -1)
  check(up.para == 1 and up.at == 4, "Up from the first line moved")
  -- A selection from the middle of the first line into the second
  -- paragraph's second line: a rectangle a line, the first from its start
  -- place, the middle whole, the last to its end place.
  local rects = pageset.selection(set, measure, { para = 2, at = second.from + 2 },
                                  { para = 1, at = 7 })
  check(#rects == 3 and near(rects[1].x_pt, LEFT + 6 * BODY)
        and near(rects[1].w_pt, 5 * BODY)
        and near(rects[3].x_pt, LEFT) and near(rects[3].w_pt, 1 * BODY + BODY)
        and near(rects[2].y_pt, set.pages[1].lines[2].baseline_pt
                                 - set.pages[1].lines[2].ascent_pt),
        "a selection's rectangles are not its lines'")

  local home, last = pageset.line_ends(set, measure, { para = 2, at = second.from + 3 })
  check(home.at == second.from and last.at > second.from
        and near(at(last).x_pt, second.x_pt + second.width_pt),
        "Home and End are not the line's ends")
end

-- 14. **A page break before a paragraph** (Add Page) and **the header's
-- words** (W4d).
do
  local doc = doc_of({ para("Body", "first"), para("Body", "second",
                                                    { page_break_before = true }),
                       para("Body", "third") },
                     { header = { on = true, from_top_mm = 9, text = "The header" } })
  local set = pageset.set(doc, measure)

  check(#set.pages == 2 and set.pages[2].lines[1].para == 2
        and set.pages[2].lines[2].para == 3,
        "a page break before a paragraph did not start a page with it")

  local first_at_top = doc_of({ para("Body", "only", { page_break_before = true }) })
  check(#pageset.set(first_at_top, measure).pages == 1,
        "a page break at the head of the first page made an empty page")

  local h = set.pages[1].header
  check(h and h.piece.text == "The header" and set.pages[2].header
        and near(h.baseline_pt, PT(9) + 0.8 * 9)
        and near(h.piece.x_pt + h.piece.width_pt / 2, PAGE_W / 2),
        "the header's words are not centred 9 mm from the top on every page")

  local none = doc_of({ para("Body", "x") }, { header = { on = false, text = "Hidden" } })
  check(pageset.set(none, measure).pages[1].header == nil,
        "a header that is off was drawn")

  local raw = writedoc.check{ format = "kosmos-write", version = 1, body = {},
                              header = { text = "a\nb\27[2J" .. ("x"):rep(300) } }
  check(raw.header.text:sub(1, 6) == "ab[2Jx" and #raw.header.text == 200,
        "a header's words from a file were not cleaned and held to 200 bytes")
end

-- 15. **Lists and drop caps** (W4e).
do
  local LIST = PT(pageset.LIST_MM)
  local long = ("word "):rep(40) .. "end"
  local doc = doc_of{
    para("Body", "one", { list = "number" }), para("Body", "two", { list = "number" }),
    para("Body", long, { list = "number" }), para("Body", "plain"),
    para("Body", "again", { list = "number" }), para("Body", "dot", { list = "bullet" }),
  }
  local cache = pageset.cache()
  local set = pageset.set(doc, measure, cache)
  local markers, starts = {}, {}

  for _, line in ipairs(set.pages[1].lines) do
    if line.marker then markers[#markers + 1] = line.marker.text end
  end

  check(table.concat(markers, " ") == "1. 2. 3. 1. \u{2022}",
        "list markers are not counted, restarted and bulleted: " .. table.concat(markers, " "))

  local third = {}
  for _, line in ipairs(set.pages[1].lines) do
    if line.para == 3 then third[#third + 1] = line end
  end

  check(#third > 1 and near(third[1].marker.x_pt, LEFT)
        and near(third[1].x_pt, LEFT + LIST) and near(third[2].x_pt, LEFT + LIST),
        "a list's lines do not hang in from its marker")
  check(faithful(doc, set))

  -- An item put in at the top: every number after it moves, though the
  -- paragraphs were set from the cache.
  local body = richtext.split(doc.body, { para = 1, at = 1 }, nil)
  local renumbered = pageset.set(with_body(doc, body), measure, cache)
  local again = {}
  for _, line in ipairs(renumbered.pages[1].lines) do
    if line.marker then again[#again + 1] = line.marker.text end
  end
  check(again[4] == "4.", "a list was not numbered again around a new item: "
        .. table.concat(again, " "))

  -- A drop cap three lines tall.
  local capped = doc_of{ para("Body", ("Words "):rep(60), { drop_cap_lines = 3 }) }
  local cset = pageset.set(capped, measure)
  local lines = cset.pages[1].lines
  local cap = lines[1].pieces[1]

  check(cap.cap and cap.text == "W" and near(cap.x_pt, LEFT)
        and looks_size(cset, cap) > 11,
        "the first character is not a drop cap at the margin")

  local beside = LEFT + cap.width_pt + PT(pageset.CAP_GAP_MM)

  check(near(lines[1].pieces[2].x_pt, beside) and near(lines[2].x_pt, beside)
        and near(lines[3].x_pt, beside) and near(lines[4].x_pt, LEFT),
        "the cap's three lines are not beside it, or the fourth not back at the margin")
  check(near(lines[1].height_pt, 11 * 1.2)
        and near(cap.drop_pt, lines[2].height_pt + lines[3].height_pt),
        "the cap made its line taller, or does not stand down by two lines")
  check(faithful(capped, cset))
  check(near(pageset.locate(cset, measure, { para = 1, at = 1 }).x_pt, LEFT)
        and near(pageset.locate(cset, measure, { para = 1, at = 2 }).x_pt,
                 LEFT + cap.width_pt),
        "the caret does not stand before and after the cap")
end

-- 16. **Hyphenation, ligatures and facing pages** (W4e).
do
  local hyphen = dofile("user/lib/hyphen.lua")
  local function read(name)
    local f = assert(io.open("assets/hyphenation/" .. name, "rb"))
    local t = f:read("a")
    f:close()
    return t
  end
  local en = hyphen.parse(read("hyph-en-us.pat.txt"), read("hyph-en-us.hyp.txt"), 2, 3)

  -- Fifteen four-letter words and "hyphenation": 74 characters, a space and
  -- eleven more is past the column's 82, and "hyphen-" brings it to 82.
  local text = ("abcd "):rep(15) .. "hyphenation."
  local on = doc_of({ para("Body", text) }, { hyphenation = true })
  local set = pageset.set(on, measure, nil, { hyphenate = en })
  local l1, l2 = set.pages[1].lines[1], set.pages[1].lines[2]
  local last = l1.pieces[#l1.pieces]

  check(last.soft and last.text == "-" and text_of(l1):sub(-7) == "hyphen-"
        and text_of(l2) == "ation.",
        "a word at the line's end was not broken at its last break that fits: "
        .. text_of(l1):sub(-10) .. " | " .. text_of(l2))
  check(faithful(on, set))
  check(pageset.locate(set, measure, { para = 1, at = l2.from }).line == l2,
        "the place after a hyphen's break is not at the next line's start")

  local off = doc_of({ para("Body", text) })
  check(text_of(pageset.set(off, measure, nil, { hyphenate = en }).pages[1].lines[2])
        == "hyphenation.", "a word was hyphenated with the switch off")

  -- Ligatures: the switch reaches the measure.
  local asked = {}
  local recording = {
    width = function(look, t, lig) asked[#asked + 1] = lig return measure.width(look, t) end,
    line = measure.line,
  }
  pageset.set(doc_of({ para("Body", "office") }, { ligatures = true }), recording)
  check(#asked > 0 and asked[1] == true, "the ligatures switch did not reach the measure")

  -- Facing pages: a left-hand page's lines stand over by the margins'
  -- difference, and a place found and a point hit agree across it.
  local facing = doc_of({ para("Body", "one"), para("Body", "two", { page_break_before = true }) },
                        { facing = true, margins_mm = { left = 20, right = 40, top = 25, bottom = 25 } })
  local fset = pageset.set(facing, measure)
  local shift = PT(40) - PT(20)

  check(fset.pages[1].shift_pt == 0 and near(fset.pages[2].shift_pt, shift),
        "a left-hand page is not shifted by its margins' difference")
  local here = pageset.locate(fset, measure, { para = 2, at = 2 })
  check(near(here.x_pt, PT(20) + shift + BODY),
        "a place on a left-hand page is not where it is drawn")
  local back = pageset.hit(fset, measure, 2, here.x_pt + 0.1, here.baseline_pt)
  check(back.para == 2 and back.at == 2, "a point on a left-hand page did not find its place")
end

-- 17. **Pictures** (W5): a picture's paragraph is one line as tall as the
-- picture, scaled down to the column when wider, placed as it aligns; and
-- a picture's name outside `pictures/` is not one.
do
  local wide = doc_of{ { style = "Body", align = "center",
                         picture = { name = "pictures/1.png", width_mm = 400, height_mm = 200 } },
                       para("Body", "after") }
  local set = pageset.set(wide, measure)
  local line = set.pages[1].lines[1]
  local pc = line.pieces[1]

  check(pc.picture == "pictures/1.png" and near(pc.width_pt, COLUMN)
        and near(pc.height_pt, COLUMN / 2) and near(pc.x_pt, LEFT)
        and near(line.baseline_pt, TOP + COLUMN / 2),
        "a picture wider than the column was not scaled to it, in proportion")

  local narrow = doc_of{ { style = "Body", align = "center",
                           picture = { name = "pictures/2.jpg", width_mm = 50, height_mm = 30 } } }
  local npc = pageset.set(narrow, measure).pages[1].lines[1].pieces[1]
  check(near(npc.width_pt, PT(50)) and near(npc.x_pt, LEFT + (COLUMN - PT(50)) / 2),
        "a narrower picture did not keep its size, centred")

  local bad = doc_of{ { style = "Body", picture = { name = "../etc/x.png", width_mm = 5, height_mm = 5 },
                        runs = { { text = "words" } } } }
  check(bad.body[1].picture == nil and richtext.plain(bad.body[1]) == "words",
        "a picture named outside pictures/ was kept")

  local held = doc_of{ { style = "Body", picture = { name = "pictures/3.png", width_mm = 5000,
                                                     height_mm = 0 } } }
  check(held.body[1].picture and held.body[1].picture.width_mm == 1000
        and held.body[1].picture.height_mm == 1,
        "a picture's sizes were not held to their range, as every number is")
end

-- 18. **Tables** (W5b): a row a line, its columns sharing the column, a
-- cell's text set as a paragraph's in its room, the header row bold over
-- its tint and again at the head of each page the table runs on to; a
-- place in a cell found, hit, and moved through by Up and Down; an edit in
-- one cell setting that cell and no other.
do
  local PAD = pageset.CELL_PAD_PT
  local LINE = (0.8 + 0.2) * 11 * 1.2           -- a Body line

  local function cell(text) return { style = "Body", runs = { { text = text } } } end

  local tdoc = doc_of{
    { style = "Body", table = { columns = 3, header = true, rows = {
      { cell("Name"), cell("Size"), cell("Kind") },
      { cell("one"), cell("two"), cell("three") },
      { cell(("word "):rep(30)), cell("x"), cell("y") },
    } } },
    para("Body", "after"),
  }
  local set = pageset.set(tdoc, measure)
  local lines = set.pages[1].lines
  local w = COLUMN / 3

  check(#lines == 4 and lines[1].cells and #lines[1].cells == 3 and lines[1].row == 1
        and lines[3].row == 3 and lines[4].para == 2,
        "a table's rows are not a line each, before the paragraph after it")
  check(near(lines[1].cells[2].x_pt, LEFT + w) and near(lines[1].cells[2].width_pt, w),
        "a table's columns do not share the column equally")

  local h1 = lines[1].cells[1].lines[1]
  check(near(h1.pieces[1].x_pt, LEFT + PAD) and near(lines[1].height_pt, LINE + 2 * PAD)
        and near(lines[1].baseline_pt - lines[1].ascent_pt, TOP)
        and near(h1.baseline_pt, TOP + PAD + 0.8 * 11),
        "a cell's text does not stand inside its cell, the room round it")
  check(set.looks[h1.pieces[1].look].weight == "Bold"
        and set.looks[lines[2].cells[1].lines[1].pieces[1].look].weight == "Regular",
        "the header row's text is not bold, or the rows below it are")

  local tints, rules = 0, 0
  for _, a in ipairs(lines[1].art) do
    if a.kind == "rect" and a.fill == pageset.HEADER_TINT then tints = tints + 1 end
    if a.kind == "rule" then rules = rules + 1 end
  end
  check(tints == 1 and rules == 2 + 4 and #lines[2].art == 6,
        "a row's rules or the header's tint are not what a table draws")

  -- A long cell wraps in its own room, and its row is as tall as it.
  local room = w - 2 * PAD
  local per_line = math.floor((room + BODY) / (5 * BODY)) -- "word " fits this many
  local long = lines[3].cells[1].lines
  check(#long > 1 and near(lines[3].height_pt, #long * LINE + 2 * PAD)
        and #lines[3].cells[2].lines == 1,
        ("a long cell did not wrap in its room (%d lines, %d words a line)")
        :format(#long, per_line))

  -- A place in cell (2, 2), found and hit.
  local place = { para = 1, at = 3, row = 2, col = 2 }
  local here = pageset.locate(set, measure, place)
  local row2 = TOP + lines[1].height_pt
  check(here and near(here.x_pt, LEFT + w + PAD + 2 * BODY)
        and near(here.baseline_pt, row2 + PAD + 0.8 * 11),
        "a place in a cell is not where its text is")
  local back = pageset.hit(set, measure, 1, here.x_pt + 0.1, here.baseline_pt - 2)
  check(back.para == 1 and back.row == 2 and back.col == 2 and back.at == 3,
        "a point in a cell did not find its place there")
  local edge = pageset.hit(set, measure, 1, LEFT + 2 * w + 1, row2 + 1)
  check(edge.row == 2 and edge.col == 3 and edge.at == 1,
        "a point at a cell's left did not find that cell's start")

  -- Up and Down: the cell above and below, then out of the table, and into
  -- it at the cell under the caret.
  local down = pageset.vertical(set, measure, { para = 1, at = 1, row = 1, col = 2 }, 1)
  check(down.row == 2 and down.col == 2, "Down from a cell did not reach the cell below")
  local out = pageset.vertical(set, measure, { para = 1, at = 1, row = 3, col = 2 }, 1)
  check(out.para == 2 and out.row == nil, "Down from the last row did not leave the table")
  local x_in = pageset.locate(set, measure, { para = 2, at = 1 }).x_pt
  local up = pageset.vertical(set, measure, { para = 2, at = 1 }, -1)
  check(up.para == 1 and up.row == 3 and up.col == 1,
        "Up into a table did not reach its last row's cell under the caret")
  local _, kept_x = pageset.vertical(set, measure, { para = 2, at = 1 }, -1, x_in + w)
  local up2 = pageset.vertical(set, measure, { para = 2, at = 1 }, -1, x_in + w)
  check(up2.col == 2 and near(kept_x, x_in + w), "Up into a table did not keep its column")

  -- Home and End in a cell, and a selection inside one.
  local home, finish = pageset.line_ends(set, measure, { para = 1, at = 2, row = 2, col = 3 })
  check(home.row == 2 and home.col == 3 and home.at == 1 and finish.at == 6,
        "Home and End in a cell did not stay in it")
  local marks = pageset.selection(set, measure, { para = 1, at = 1, row = 2, col = 1 },
                                  { para = 1, at = 4, row = 2, col = 1 })
  check(#marks == 1 and near(marks[1].x_pt, LEFT + PAD) and near(marks[1].w_pt, 3 * BODY),
        "a selection in a cell is not its text's")

  -- **An edit in one cell sets that cell and no other**: the cells it did
  -- not touch keep their lines from the setting before.
  local cache = pageset.cache()
  local before = pageset.set(tdoc, measure, cache)
  local body2 = richtext.type(tdoc.body, { para = 1, at = 4, row = 2, col = 2 }, "!")
  local after = pageset.set(with_body(tdoc, body2), measure, cache)
  local b1, a1 = before.pages[1].lines[2], after.pages[1].lines[2]
  check(a1.cells[1].lines == b1.cells[1].lines and a1.cells[2].lines ~= b1.cells[2].lines
        and text_of(a1.cells[2].lines[1]) == "two!",
        "an edit in one cell set again cells it did not touch, or missed its own")

  -- **A long table runs on**: broken between rows, never inside one, and
  -- its header row again at the head of the next page - a caret never in
  -- that copy.
  local rows = { { cell("Head"), cell("Two") } }
  for r = 2, 80 do rows[r] = { cell("row " .. r), cell("x") } end
  local long_doc = doc_of{ { style = "Body", table = { columns = 2, header = true, rows = rows } } }
  local lset = pageset.set(long_doc, measure)
  local p2 = lset.pages[2] and lset.pages[2].lines

  check(#lset.pages >= 2 and p2[1].repeated and p2[1].row == 1 and p2[2].row > 2,
        "a table running on to a page did not head it with its header row")

  local fits = true
  for _, pg in ipairs(lset.pages) do
    for _, l in ipairs(pg.lines) do
      if l.baseline_pt - l.ascent_pt + l.height_pt > BOTTOM + 1e-6 then fits = false end
    end
  end
  check(fits, "a table row ran past the foot of a page")

  local first = pageset.locate(lset, measure, { para = 1, at = 1, row = 1, col = 1 })
  check(first.page == 1, "a caret in the header row stood in its copy on a later page")
  local hit2 = pageset.hit(lset, measure, 2, LEFT + 1, p2[2].baseline_pt - 1)
  check(hit2.row == p2[2].row, "a point on a later page's row did not find that row")
end

-- 19. **Text boxes** (W7a): a table of one cell, as wide as it says and
-- placed as its paragraph aligns, with more room round its text, its fill
-- and its border; a line break in it a line of its own; and a place in it
-- found and hit as a cell's.
do
  local PAD = pageset.BOX_PAD_PT
  local LINE = (0.8 + 0.2) * 11 * 1.2
  local raw = richtext.new_box("Body", 60)
  raw.table.rows[1][1] = { style = "Body", runs = { { text = "Note\nsecond" } } }
  raw.table.box.fill = "#eef3fb"
  local bdoc = doc_of{ para("Body", "before"), raw, para("Body", "after") }
  local set = pageset.set(bdoc, measure)
  local row = set.pages[1].lines[2]
  local W60 = PT(60)

  check(row.cells and #row.cells == 1 and near(row.cells[1].width_pt, W60)
        and near(row.x_pt, LEFT + (COLUMN - W60) / 2),
        "a text box is not as wide as it says, centred")
  local lines = row.cells[1].lines
  check(#lines == 2 and text_of(lines[1]) == "Note" and text_of(lines[2]) == "second"
        and near(lines[1].pieces[1].x_pt, row.x_pt + PAD)
        and near(row.height_pt, 2 * LINE + 2 * PAD),
        "a text box's line break, or the room round its text, is wrong")

  local fills, rules = 0, 0
  for _, a in ipairs(row.art) do
    if a.kind == "rect" and a.fill == "#eef3fb" then fills = fills + 1 end
    if a.kind == "rule" and a.colour == pageset.BOX_RULE then rules = rules + 1 end
  end
  check(fills == 1 and rules == 4, "a text box is not filled and bordered as it says")

  local here = pageset.locate(set, measure, { para = 2, at = 6, row = 1, col = 1 })
  local back = here and pageset.hit(set, measure, 1, here.x_pt + 0.1, here.baseline_pt - 1)
  check(here and back and back.row == 1 and back.at == 6 and here.line == lines[2],
        "a place after a box's line break is not on its second line")

  local plain = richtext.new_box("Body", 500)
  plain.align = "left"
  plain.table.box.border = false
  local pset = pageset.set(doc_of{ plain }, measure)
  local prow = pset.pages[1].lines[1]
  check(#prow.art == 0 and near(prow.cells[1].width_pt, COLUMN) and near(prow.x_pt, LEFT),
        "a box wider than the column, unbordered and unfilled, is not the column's width and bare")
end

-- 20. **Shapes** (W7b): a shape's paragraph is one line as tall as the
-- shape, scaled to the column when wider and placed as it aligns, the
-- shape its `art` - a rectangle, a rounded one, an ellipse, or a polygon
-- whose every point can be seen from its centre, which is what lets the
-- screen fill it as triangles from there.
do
  local function shape(kind, w, h, fields)
    local p = { style = "Body", align = "center",
                shape = { kind = kind, width_mm = w, height_mm = h, fill = "#d35400" } }
    for k, v in pairs(fields or {}) do p[k] = v end
    return p
  end

  local sdoc = doc_of{ shape("star", 40, 30), shape("oval", 400, 100), shape("rounded", 20, 10,
                       { align = "left" }), shape("arrow", 50, 20), shape("triangle", 30, 30) }
  local lines = pageset.set(sdoc, measure).pages[1].lines

  local star = lines[1].art[1]
  check(#lines == 5 and star.kind == "poly" and #star.points == 20 and star.fill == "#d35400"
        and near(lines[1].x_pt, LEFT + (COLUMN - PT(40)) / 2) and near(lines[1].ascent_pt, PT(30) + 2),
        "a star is not ten points, centred, as tall as it says")
  check(near(star.points[1], star.cx_pt) and near(star.points[2], 2)
        and near(star.cx_pt, lines[1].x_pt + PT(40) / 2),
        "a star's first point is not at its top centre")

  local oval = lines[2].art[1]
  check(oval.kind == "ellipse" and near(oval.w_pt, COLUMN) and near(oval.h_pt, COLUMN / 4),
        "a shape wider than the column was not scaled to it, in proportion")

  local rounded = lines[3].art[1]
  check(rounded.kind == "rect" and near(rounded.radius_pt, PT(10) * 0.15)
        and near(rounded.x_pt, LEFT), "a rounded rectangle is not rounded, or not at the left")

  -- **Every point of a polygon is seen from its centre**: no edge between
  -- the centre and a point crosses the outline, so the fan of triangles is
  -- the shape.
  local function crosses(ax, ay, bx, by, cx, cy, dx, dy)
    local function side(px, py, qx, qy, rx, ry) return (qx - px) * (ry - py) - (qy - py) * (rx - px) end
    local d1, d2 = side(cx, cy, dx, dy, ax, ay), side(cx, cy, dx, dy, bx, by)
    local d3, d4 = side(ax, ay, bx, by, cx, cy), side(ax, ay, bx, by, dx, dy)
    return d1 * d2 < -1e-9 and d3 * d4 < -1e-9
  end

  local seen = true
  for _, line in ipairs({ lines[1], lines[4], lines[5] }) do
    local a = line.art[1]
    local n = #a.points // 2
    for i = 1, n do
      local px, py = a.points[2 * i - 1], a.points[2 * i]
      for j = 1, n do
        local k = j % n + 1
        if crosses(a.cx_pt, a.cy_pt, px, py, a.points[2 * j - 1], a.points[2 * j],
                   a.points[2 * k - 1], a.points[2 * k]) then
          seen = false
        end
      end
    end
  end
  check(seen, "a shape has a point its centre cannot see; the screen's triangles would spill")

  local here = pageset.locate(pageset.set(sdoc, measure), measure, { para = 4, at = 1 })
  check(here and near(here.x_pt, lines[4].x_pt), "a caret on a shape does not stand at its left")
end

if fails > 0 then
  print(("pageset: %d of %d checks failed"):format(fails, checks + fails))
  os.exit(1)
end

print(("pageset: %d checks pass"):format(checks))
