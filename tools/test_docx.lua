-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Kosmos Write's DOCX on the host (`user/lib/docxwrite.lua`, `docs/write.md`
-- W6): the parts Word reads, from a document of every look - styles, runs,
-- alignment, lists, a page break, the section's paper and margins, and
-- text that would break the XML if it were not escaped.

tabletext = dofile("user/init/tabletext.lua")

local loaded = {}

function use(path)
  local name = path:match("^/Kosmos/Libraries/(.+)$")
  assert(name, "no such library here: " .. path)
  if not loaded[name] then loaded[name] = dofile("user/lib/" .. name) end
  return loaded[name]
end

local writedoc = use("/Kosmos/Libraries/writedoc.lua")
local docx = use("/Kosmos/Libraries/docxwrite.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then checks = checks + 1 else fails = fails + 1 print("  " .. what) end
end

local function has(text, want, what)
  check(text:find(want, 1, true) ~= nil, what .. ": no " .. want)
end

local doc = writedoc.check{ format = "kosmos-write", version = 1,
  paper = { name = "Letter", landscape = false },
  margins_mm = { top = 25, bottom = 25, left = 20, right = 30 },
  facing = true, hyphenation = true, language = "es",
  body = {
    { style = "Title", runs = { { text = "A <title> & more" } } },
    { style = "Body", align = "justify", runs = {
      { text = "plain " }, { text = "bold", weight = "Bold" }, { text = " " },
      { text = "italic", italic = true }, { text = " red", colour = "#c0392b" },
      { text = " big", size_pt = 14.5 }, { text = "\tafter a tab\nand a break" } } },
    { style = "Body", list = "number", runs = { { text = "first" } } },
    { style = "Body", list = "bullet", page_break_before = true, runs = { { text = "dot" } } },
  } }

local d = docx.parts.document(doc)

has(d, '<w:pStyle w:val="Title"/>', "the title's paragraph style")
has(d, "A &lt;title&gt; &amp; more", "the title's text escaped")
has(d, '<w:jc w:val="both"/>', "justified")
has(d, '<w:r><w:rPr><w:b/></w:rPr><w:t xml:space="preserve">bold</w:t></w:r>', "a bold run")
has(d, "<w:i/>", "an italic run")
has(d, '<w:color w:val="C0392B"/>', "a red run")
has(d, '<w:sz w:val="29"/>', "a 14.5 point run, in half points")
has(d, "<w:tab/>", "a tab")
has(d, "<w:br/>", "a line break")
has(d, '<w:numId w:val="2"/>', "a numbered item")
has(d, '<w:numId w:val="1"/>', "a bulleted item")
has(d, "<w:pageBreakBefore/>", "a page break before")
has(d, '<w:pgSz w:w="12240" w:h="15840"/>', "Letter, 8.5 by 11 inches in twentieths of a point")
has(d, 'w:left="1134"', "a 20 mm left margin")
has(d, 'w:right="1701"', "a 30 mm right margin")

-- Nothing undeclared reaches the XML: every element opened is closed.
local opened, closed = 0, 0
for tag in d:gmatch("<w:%a+[^>]*>") do
  if tag:sub(-2) ~= "/>" then opened = opened + 1 end
end
for _ in d:gmatch("</w:%a+>") do closed = closed + 1 end
check(opened == closed, ("%d elements opened and %d closed"):format(opened, closed))

local st = docx.parts.styles(doc)
has(st, '<w:style w:type="paragraph" w:styleId="Body" w:default="1">', "Body as the default style")
has(st, '<w:style w:type="paragraph" w:styleId="Heading1">', "Heading 1's id")
has(st, '<w:lang w:val="es-ES"/>', "the document's language")
has(st, '<w:rFonts w:ascii="IBM Plex Sans"', "a style's face")

local se = docx.parts.settings(doc)
has(se, "<w:mirrorMargins/>", "facing pages")
has(se, "<w:autoHyphenation/>", "hyphenation")

has(docx.parts.numbering(doc), '<w:numFmt w:val="decimal"/>', "numbers")

-- **A table** (W5b): `w:tbl` with fixed columns sharing the column, rules
-- round every cell, the header row repeated, tinted and bold, each cell a
-- paragraph without the space round one - and a paragraph after a table
-- that ends the body, as Word wants.
local function cell(text) return { style = "Body", runs = { { text = text } } } end

local tdoc = writedoc.check{ format = "kosmos-write", version = 1,
  margins_mm = { top = 25, bottom = 25, left = 25, right = 25 },
  body = {
    { style = "Body", runs = { { text = "Before" } } },
    { style = "Body", table = { columns = 2, header = true, rows = {
      { cell("Planet"), cell("Moons") }, { cell("Mars"), cell("2") } } } },
  } }
local td = docx.parts.document(tdoc)

has(td, '<w:tblGrid><w:gridCol w:w="4535"/><w:gridCol w:w="4535"/></w:tblGrid>',
    "two columns sharing A4's 160 mm column")
has(td, '<w:tblHeader/>', "the header row repeated")
has(td, 'w:fill="E9EDF2"', "the header row's tint")
has(td, '<w:insideV w:val="single"', "rules between the columns")
has(td, '<w:b/></w:rPr><w:t xml:space="preserve">Planet</w:t>', "the header's text bold")
has(td, '<w:t xml:space="preserve">Mars</w:t>', "a cell's text")
has(td, 'w:before="0" w:after="0"', "a cell without a paragraph's space round it")
check(td:find("</w:tbl><w:p/>", 1, true) ~= nil, "a table that ends the body has no paragraph after it")
check(not td:find('Mars</w:t></w:r><w:r><w:rPr><w:b/>', 1, true) and
      select(2, td:gsub("<w:b/>", "")) == 2, "a row below the header is bold")

local o2, c2 = 0, 0
for tag in td:gmatch("<w:%a+[^>]*>") do
  if tag:sub(-2) ~= "/>" then o2 = o2 + 1 end
end
for _ in td:gmatch("</w:%a+>") do c2 = c2 + 1 end
check(o2 == c2, ("in a table, %d elements opened and %d closed"):format(o2, c2))

-- **A text box** (W7a): Word's table of one cell, as wide as the box,
-- centred, shaded with its fill; and one without a border has none.
local bdoc = writedoc.check{ format = "kosmos-write", version = 1, body = {
  { style = "Body", align = "center", table = { columns = 1, rows = { { cell("Note") } },
    box = { width_mm = 60, fill = "#eef3fb" } } },
  { style = "Body", table = { columns = 1, rows = { { cell("Bare") } },
    box = { width_mm = 40, border = false } } },
  { style = "Body", runs = { { text = "after" } } } } }
local bd = docx.parts.document(bdoc)

has(bd, '<w:tblW w:w="3402" w:type="dxa"/><w:jc w:val="center"/>', "a 60 mm box, centred")
has(bd, 'w:fill="EEF3FB"', "the box's fill")
has(bd, '<w:top w:val="single" w:sz="6"', "the box's border")
has(bd, '<w:top w:val="nil"/>', "a box without a border")

-- **Shapes** (W7b): Word's own preset shapes, filled and unoutlined, each
-- drawing with an id of its own.
local sdoc = writedoc.check{ format = "kosmos-write", version = 1, body = {
  { style = "Body", shape = { kind = "star", width_mm = 40, height_mm = 30, fill = "#d35400" } },
  { style = "Body", shape = { kind = "arrow", width_mm = 50, height_mm = 20 } } } }
local sd = docx.parts.document(sdoc)

has(sd, '<a:prstGeom prst="star5">', "a star")
has(sd, '<a:prstGeom prst="rightArrow">', "an arrow")
has(sd, '<a:srgbClr val="D35400"/>', "the star's fill")
has(sd, '<wp:extent cx="1440000" cy="1080000"/>', "40 by 30 mm in EMUs")
has(sd, '<mc:Choice Requires="wps">', "the wrapper Word writes a shape in")
check(sd:find('wp:docPr id="1001"', 1, true) and sd:find('wp:docPr id="1002"', 1, true),
      "two shapes do not have drawings' ids of their own")

-- **Charts** (W7c): drawn inline from a chart part of its own, the numbers
-- written into it; and a chart is not a table in Word's body.
local cdoc = writedoc.check{ format = "kosmos-write", version = 1, body = {
  { style = "Body", runs = { { text = "before" } } },
  use("/Kosmos/Libraries/richtext.lua").new_chart("column", "Body") } }
local cd = docx.parts.document(cdoc)

has(cd, '<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="rIdChart1"/>',
    "the chart drawn from its part")
check(not cd:find("<w:tbl>", 1, true), "a chart was written as a table")

local part = docx.chart_part(cdoc.body[2].table)
has(part, '<c:barDir val="col"/>', "columns")
has(part, '<c:tx><c:v>2026</c:v></c:tx>', "the second series' name")
has(part, '<c:pt idx="1"><c:v>Summer</c:v></c:pt>', "a category")
has(part, '<c:pt idx="3"><c:v>14</c:v></c:pt>', "the last number")
check(select(2, part:gsub("<c:ser>", "")) == 2, "a chart's two series are not two")

local o3, c3 = 0, 0
for tag in part:gmatch("<c:%a+[^>]*>") do
  if tag:sub(-2) ~= "/>" then o3 = o3 + 1 end
end
for _ in part:gmatch("</c:%a+>") do c3 = c3 + 1 end
check(o3 == c3, ("in a chart, %d elements opened and %d closed"):format(o3, c3))

-- **Comments** (W7d): Word's range round the commented runs - across two
-- paragraphs here - its reference after them, and its words in a part of
-- their own; a comment nothing refers to is not written.
local mdoc = writedoc.check{ format = "kosmos-write", version = 1, body = {
  { style = "Body", runs = { { text = "plain " }, { text = "noted", comment = 5 } } },
  { style = "Body", runs = { { text = "still", comment = 5 }, { text = " after" } } } },
  comments = { { id = 5, text = "Two <paragraphs>" } } }
mdoc.comments[#mdoc.comments + 1] = { id = 6, text = "orphan" }
local md = docx.parts.document(mdoc)

has(md, '<w:commentRangeStart w:id="5"/><w:r><w:t xml:space="preserve">noted</w:t></w:r>',
    "the range opened before the first commented run")
has(md, '<w:t xml:space="preserve">still</w:t></w:r><w:commentRangeEnd w:id="5"/>'
    .. '<w:r><w:commentReference w:id="5"/></w:r>', "the range closed after the last, in the next paragraph")
check(select(2, md:gsub("commentRangeStart", "")) == 1, "a comment across two paragraphs was opened twice")

local mc = docx.parts.comments(mdoc)
has(mc, '<w:comment w:id="5" w:author="Kosmos Write" w:initials="KW">', "the comment")
has(mc, "Two &lt;paragraphs&gt;", "its words, escaped")
check(not mc:find("orphan", 1, true), "a comment nothing refers to was written")

if fails > 0 then
  print(("docx: %d of %d checks failed"):format(fails, checks + fails))
  os.exit(1)
end

print(("docx: %d checks pass"):format(checks))
