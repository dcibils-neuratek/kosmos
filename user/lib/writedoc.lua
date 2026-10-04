-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A Kosmos Write document, as data (`docs/write.md`, W1).
--
--   local writedoc = use("/Kosmos/Libraries/writedoc.lua")
--   local doc = writedoc.new()
--   local ok, why = writedoc.save("/Home/Letter.write", doc)
--   doc, why = writedoc.open("/Home/Letter.write")
--
-- What Write's document is that styled text is not: the paper and how it
-- is turned, the margins, a header and a footer and how far each is from
-- the edge, page numbers, and the Document panel's three switches - facing
-- pages, hyphenation, ligatures. The text itself is `richtext`'s, and the
-- file is `docfile`'s; this is the page around them.
--
-- **A document from a file is checked before anything else sees it**
-- (`writedoc.check`): the declared fields, each to its range, rebuilt into
-- a fresh table, and the text through `richtext`. A file that is not a
-- document - another format, a version newer than this reader, no body -
-- is refused with the reason in words.
--
-- Measures of the page are in millimetres, because that is what a person
-- typed in the Document panel; the conversion to points, which a PDF
-- speaks, is `writedoc.pt` and nowhere else.

local writedoc = {}

local richtext = use("/Kosmos/Libraries/richtext.lua")

writedoc.FORMAT = "kosmos-write"
writedoc.VERSION = 1

-- Upright, in millimetres. A4 first: Diego's first answer, 4 October.
writedoc.PAPERS = {
  { name = "A4", width_mm = 210, height_mm = 297 },
  { name = "Letter", width_mm = 215.9, height_mm = 279.4 },
}

local PAPER = {}

for _, p in ipairs(writedoc.PAPERS) do PAPER[p.name] = p end

-- Millimetres to points, a PDF's unit: 72 to the inch, 25.4 mm to it.
function writedoc.pt(mm)
  return mm * 72 / 25.4
end

--
-- **The styles a new document has**, in the order the Format panel lists
-- them (`docs/write.html`). Headings in IBM Plex Sans and the text in its
-- serif, both shipped in every image with their bold and italic.
--
writedoc.STYLES = {
  { name = "Title", face = "IBM Plex Sans", weight = "Bold", size_pt = 30,
    colour = "#1b2330", after_pt = 12, next = "Body" },
  { name = "Subtitle", face = "IBM Plex Sans", weight = "Regular",
    size_pt = 18, colour = "#5b6677", after_pt = 12, next = "Body" },
  { name = "Heading 1", face = "IBM Plex Sans", weight = "SemiBold",
    size_pt = 18, colour = "#1b2330", before_pt = 18, after_pt = 6,
    next = "Body" },
  { name = "Heading 2", face = "IBM Plex Sans", weight = "SemiBold",
    size_pt = 14, colour = "#1b2330", before_pt = 12, after_pt = 4,
    next = "Body" },
  { name = "Body", face = "IBM Plex Serif", weight = "Regular", size_pt = 11,
    colour = "#1b2330", spacing_lines = 1.2, after_pt = 8, next = "Body" },
  { name = "Caption", face = "IBM Plex Sans", weight = "Regular",
    size_pt = 9, italic = true, colour = "#5b6677", after_pt = 8,
    next = "Body" },
  { name = "Quote", face = "IBM Plex Serif", weight = "Regular",
    size_pt = 12, italic = true, colour = "#1b2330", spacing_lines = 1.2,
    before_pt = 6, after_pt = 12, indent_left_mm = 10, indent_right_mm = 10,
    next = "Body" },
}

-- The style a paragraph wears when it names none this document has.
writedoc.BODY = "Body"

--------------------------------------------------------------------------
-- A new one.
--------------------------------------------------------------------------

--
-- A new document, as the Document panel draws one: A4 upright, 25 mm all
-- round, a header 9 mm from the top and a footer 6 mm from the bottom, page
-- numbers, ligatures on, and one empty paragraph to type into.
--
function writedoc.new()
  local styles = richtext.styles(writedoc.STYLES, writedoc.STYLES)

  return {
    format = writedoc.FORMAT, version = writedoc.VERSION,
    paper = { name = "A4", width_mm = 210, height_mm = 297, landscape = false },
    margins_mm = { top = 25, bottom = 25, left = 25, right = 25 },
    header = { on = true, from_top_mm = 9 },
    footer = { on = true, from_bottom_mm = 6, page_numbers = true },
    facing = false, hyphenation = false, ligatures = true,
    styles = styles,
    body = { { style = writedoc.BODY, runs = {} } },
  }
end

--------------------------------------------------------------------------
-- Checked.
--------------------------------------------------------------------------

local function flag(v, default)
  if type(v) == "boolean" then return v end
  return default
end

-- A measure in [lo, hi]; the default when there is none, held to the
-- range as well - 25 mm is no margin for a page 50 mm tall.
local function measure(v, lo, hi, default)
  if type(v) ~= "number" or v ~= v or v == math.huge or v == -math.huge then
    v = default
  end
  return math.max(lo, math.min(hi, v))
end

--
-- The paper: a name this reader knows is that paper, whatever sizes came
-- with it; any other is `Custom`, with its own, from 50 mm to a metre.
--
local function paper_of(t)
  t = type(t) == "table" and t or {}

  local known = PAPER[t.name]
  local out = { landscape = flag(t.landscape, false) }

  if known then
    out.name, out.width_mm, out.height_mm = known.name, known.width_mm,
                                            known.height_mm
  else
    out.name = "Custom"
    out.width_mm = measure(t.width_mm, 50, 1000, 210)
    out.height_mm = measure(t.height_mm, 50, 1000, 297)
  end

  return out
end

-- The page as it lies, turned or not: its width and height in millimetres.
function writedoc.page_mm(doc)
  local p = doc.paper

  if p.landscape then return p.height_mm, p.width_mm end

  return p.width_mm, p.height_mm
end

--
-- **A document, checked**: a new table holding the declared fields and
-- nothing else, or nil and why it is not a document. Checking a checked
-- document gives the same document back.
--
function writedoc.check(t)
  if type(t) ~= "table" then return nil, "it is not a document" end

  if t.format ~= writedoc.FORMAT then
    return nil, ("it is not a Kosmos Write document (it says %s)")
                :format(type(t.format) == "string" and t.format or "nothing")
  end

  if math.type(t.version) ~= "integer" or t.version < 1 then
    return nil, "it does not say which version of the format it is"
  end

  if t.version > writedoc.VERSION then
    return nil, ("it was made by a newer Kosmos Write (format %d; this one "
                 .. "reads %d)"):format(t.version, writedoc.VERSION)
  end

  if type(t.body) ~= "table" then
    return nil, "it has no body"
  end

  local doc = { format = writedoc.FORMAT, version = writedoc.VERSION,
                paper = paper_of(t.paper) }
  local w, h = writedoc.page_mm(doc)

  -- Margins: no more than leaves 20 mm of the page between them.
  local m = type(t.margins_mm) == "table" and t.margins_mm or {}

  doc.margins_mm = {
    top = measure(m.top, 0, (h - 20) / 2, 25),
    bottom = measure(m.bottom, 0, (h - 20) / 2, 25),
    left = measure(m.left, 0, (w - 20) / 2, 25),
    right = measure(m.right, 0, (w - 20) / 2, 25),
  }

  local head = type(t.header) == "table" and t.header or {}
  local foot = type(t.footer) == "table" and t.footer or {}

  doc.header = { on = flag(head.on, true),
                 from_top_mm = measure(head.from_top_mm, 0, 100, 9) }
  doc.footer = { on = flag(foot.on, true),
                 from_bottom_mm = measure(foot.from_bottom_mm, 0, 100, 6),
                 page_numbers = flag(foot.page_numbers, true) }

  doc.facing = flag(t.facing, false)
  doc.hyphenation = flag(t.hyphenation, false)
  doc.ligatures = flag(t.ligatures, true)

  local by_name
  doc.styles, by_name = richtext.styles(t.styles, writedoc.STYLES)

  local fallback = by_name[writedoc.BODY] and writedoc.BODY
                   or doc.styles[1].name

  doc.body = {}

  -- A paragraph is a table; anything else in a body is not one, and is
  -- left behind rather than made into an empty one.
  for _, raw in ipairs(t.body) do
    if type(raw) == "table" then
      doc.body[#doc.body + 1] = richtext.paragraph(raw, by_name, fallback)
    end
  end

  -- A page with nothing on it still has a place to type.
  if #doc.body == 0 then
    doc.body[1] = { style = fallback, runs = {} }
  end

  return doc
end

-- A style of `doc` by its name.
function writedoc.style(doc, name)
  for _, s in ipairs(doc.styles) do
    if s.name == name then return s end
  end
end

--------------------------------------------------------------------------
-- Its file.
--------------------------------------------------------------------------

--
-- `doc` into a `.write` file at `path`; true, or nil and why. Checked on
-- the way out too, so what is written is what would be read.
--
function writedoc.save(path, doc, pictures)
  local clean, why = writedoc.check(doc)

  if not clean then return nil, why end

  return use("/Kosmos/Libraries/docfile.lua").save(path, clean, pictures)
end

--
-- The document in the `.write` file at `path`, checked, and the pictures
-- it holds by name; or nil and why, with the file's name in the why.
--
function writedoc.open(path)
  local raw, pictures = use("/Kosmos/Libraries/docfile.lua").open(path)

  if not raw then return nil, pictures end

  local doc, why = writedoc.check(raw)

  if not doc then return nil, path .. ": " .. why end

  return doc, pictures
end

return writedoc
