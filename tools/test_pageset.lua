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
      if plain:sub(pc.at, pc.at + #pc.text - 1) ~= pc.text then
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
  local edited = { format = doc.format, version = doc.version, paper = doc.paper,
                   margins_mm = doc.margins_mm, header = doc.header,
                   footer = doc.footer, styles = doc.styles, body = body2 }
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
  local renumbered = pageset.set({ format = doc.format, version = doc.version,
    paper = doc.paper, margins_mm = doc.margins_mm, header = doc.header,
    footer = doc.footer, styles = doc.styles, body = body }, measure, cache)
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

if fails > 0 then
  print(("pageset: %d of %d checks failed"):format(fails, checks + fails))
  os.exit(1)
end

print(("pageset: %d checks pass"):format(checks))
