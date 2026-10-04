-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A document out as Word's DOCX (`docs/write.md` W6).
--
--   local docxwrite = use("/Kosmos/Libraries/docxwrite.lua")
--   local ok, notes = docxwrite.write("/Home/Letter.docx", doc)
--
-- Diego, 3 October 2026: "Kosmos Write should be able to export as DOCX as
-- well as PDF" - so a document goes to somebody without Kosmos and stays a
-- document they can edit, not a picture of one.
--
-- **Office Open XML, as little of it as says everything Write has**: a zip
-- (`zip.lua`) of XML parts - the document's paragraphs and runs, its
-- styles, its lists' numbering, a header and a footer, its settings - each
-- named in `[Content_Types].xml` and joined by relationships. Word, Pages,
-- LibreOffice and macOS's own text system all read it.
--
-- **What maps, and how**: a table is `w:tbl`, its cells' text paragraphs
-- of their own; a paragraph is `w:p` in its style, with what it
-- changes from it - alignment, spacing, indents, a list, a page break
-- before, kept with the next; a run is `w:r` with its face, size, bold (a
-- weight of SemiBold or more), italic, underline, strike-through and
-- colour. Points are twentieths in Word's measures and halves in its sizes;
-- millimetres become twentieths of a point the same way. The paper and its
-- margins are the section's, mirrored for facing pages, hyphenated when the
-- document is, in its language.
--
-- **What does not, said**: a drop cap is written as its paragraph's first
-- letter, not raised - Word's frames for one are a layout of their own; and
-- the faces travel by name, as a DOCX's do, rather than embedded.
--
-- **A kit**: Write's first; Present's and Sheets' Office formats are the
-- same parts and the same zip, and borrow this file's XML helpers.

local docxwrite = {}

local richtext = use("/Kosmos/Libraries/richtext.lua")
local writedoc = use("/Kosmos/Libraries/writedoc.lua")

--------------------------------------------------------------------------
-- XML.
--------------------------------------------------------------------------

local ESCAPE = { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;" }

-- Text as XML may hold it: the five that mean something escaped, and the
-- bytes below a space XML 1.0 does not allow dropped.
function docxwrite.escape(s)
  return (tostring(s):gsub("[%z\1-\8\11\12\14-\31]", ""):gsub('[&<>"]', ESCAPE))
end

local esc = docxwrite.escape

local HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
local W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
          .. 'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
          .. 'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"'

-- Millimetres and points in Word's twentieths of a point.
local function twips_mm(mm) return math.floor(writedoc.pt(mm) * 20 + 0.5) end
local function twips_pt(pt) return math.floor(pt * 20 + 0.5) end

-- A style's name as Word's id for it: its letters and digits.
local function style_id(name) return (name:gsub("[^%w]", "")) end

--------------------------------------------------------------------------
-- What a run and a paragraph say.
--------------------------------------------------------------------------

local JC = { left = "left", center = "center", right = "right", justify = "both" }
local LANG = { ["en-us"] = "en-US", es = "es-ES" }

-- A look's character properties, all of them - a style's - or only those
-- in `only` - a run's own over its style.
local function rpr(look, only)
  local out = {}
  local function has(k) return not only or only[k] ~= nil end

  if has("face") and look.face then
    out[#out + 1] = ('<w:rFonts w:ascii="%s" w:hAnsi="%s" w:cs="%s"/>')
                    :format(esc(look.face), esc(look.face), esc(look.face))
  end

  if has("weight") and look.weight then
    local bold = look.weight == "SemiBold" or look.weight == "Bold"
    out[#out + 1] = bold and "<w:b/>" or '<w:b w:val="0"/>'
  end

  if has("italic") and look.italic ~= nil then
    out[#out + 1] = look.italic and "<w:i/>" or '<w:i w:val="0"/>'
  end

  if has("strike") and look.strike ~= nil then
    out[#out + 1] = look.strike and "<w:strike/>" or '<w:strike w:val="0"/>'
  end

  if has("colour") and look.colour then
    out[#out + 1] = ('<w:color w:val="%s"/>'):format(look.colour:sub(2):upper())
  end

  if has("size_pt") and look.size_pt then
    out[#out + 1] = ('<w:sz w:val="%d"/>'):format(math.floor(look.size_pt * 2 + 0.5))
  end

  if has("underline") and look.underline ~= nil then
    out[#out + 1] = ('<w:u w:val="%s"/>'):format(look.underline and "single" or "none")
  end

  if #out == 0 then return "" end

  return "<w:rPr>" .. table.concat(out) .. "</w:rPr>"
end

-- A layout's paragraph properties, all or `only` those, after its style's
-- name when one is given.
local function ppr(layout, style, only)
  local out = {}
  local function has(k) return not only or only[k] ~= nil end

  if style then out[#out + 1] = ('<w:pStyle w:val="%s"/>'):format(style_id(style)) end
  if has("keep_with_next") and layout.keep_with_next then out[#out + 1] = "<w:keepNext/>" end
  if has("page_break_before") and layout.page_break_before then
    out[#out + 1] = "<w:pageBreakBefore/>"
  end

  if has("list") and layout.list and layout.list ~= "none" then
    out[#out + 1] = ('<w:numPr><w:ilvl w:val="0"/><w:numId w:val="%d"/></w:numPr>')
                    :format(layout.list == "number" and 2 or 1)
  end

  if has("before_pt") or has("after_pt") or has("spacing_lines") then
    out[#out + 1] = ('<w:spacing w:before="%d" w:after="%d" w:line="%d" w:lineRule="auto"/>')
                    :format(twips_pt(layout.before_pt or 0), twips_pt(layout.after_pt or 0),
                            math.floor((layout.spacing_lines or 1) * 240 + 0.5))
  end

  if has("indent_left_mm") or has("indent_right_mm") or has("indent_first_mm") then
    local first = layout.indent_first_mm or 0
    local hang = first < 0 and ('w:hanging="%d"'):format(twips_mm(-first))
                 or ('w:firstLine="%d"'):format(twips_mm(first))
    out[#out + 1] = ('<w:ind w:left="%d" w:right="%d" %s/>')
                    :format(twips_mm(layout.indent_left_mm or 0),
                            twips_mm(layout.indent_right_mm or 0), hang)
  end

  if has("align") and layout.align then
    out[#out + 1] = ('<w:jc w:val="%s"/>'):format(JC[layout.align] or "left")
  end

  if #out == 0 then return "" end

  return "<w:pPr>" .. table.concat(out) .. "</w:pPr>"
end

-- A run's text: tabs and line breaks as Word's own, the rest preserved.
local function run_text(text)
  local out = {}

  for piece, sep in (text .. "\0"):gmatch("([^\t\n%z]*)([\t\n%z])") do
    if piece ~= "" then
      out[#out + 1] = ('<w:t xml:space="preserve">%s</w:t>'):format(esc(piece))
    end

    if sep == "\t" then out[#out + 1] = "<w:tab/>"
    elseif sep == "\n" then out[#out + 1] = "<w:br/>" end
  end

  return table.concat(out)
end

--------------------------------------------------------------------------
-- The parts.
--------------------------------------------------------------------------

--
-- **A picture as Word draws one inline**: its bytes in `word/media`, found
-- by a relationship, at its size in EMUs - 36,000 to the millimetre.
--
local function drawing(pic, media)
  local cx = math.floor(pic.width_mm * 36000 + 0.5)
  local cy = math.floor(pic.height_mm * 36000 + 0.5)

  return ('<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0">'
    .. '<wp:extent cx="%d" cy="%d"/><wp:docPr id="%d" name="Picture %d"/>'
    .. '<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
    .. '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
    .. '<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture">'
    .. '<pic:nvPicPr><pic:cNvPr id="%d" name="%s"/><pic:cNvPicPr/></pic:nvPicPr>'
    .. '<pic:blipFill><a:blip r:embed="%s"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
    .. '<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="%d" cy="%d"/></a:xfrm>'
    .. '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>'
    .. '</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>'):format(
    cx, cy, media.n, media.n, media.n, esc(media.file), media.rid, cx, cy)
end

--
-- **A shape as Word draws one inline** (W7b): its own preset geometry,
-- filled, without an outline - in the markup-compatibility wrapper Word
-- itself writes a shape in.
--
local PRESET = { rectangle = "rect", rounded = "roundRect", oval = "ellipse",
                 triangle = "triangle", star = "star5", arrow = "rightArrow" }

local function shape_xml(shape, id)
  local cx = math.floor(shape.width_mm * 36000 + 0.5)
  local cy = math.floor(shape.height_mm * 36000 + 0.5)

  return ('<w:r><mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" '
    .. 'xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape">'
    .. '<mc:Choice Requires="wps"><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0">'
    .. '<wp:extent cx="%d" cy="%d"/><wp:docPr id="%d" name="Shape %d"/>'
    .. '<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
    .. '<a:graphicData uri="http://schemas.microsoft.com/office/word/2010/wordprocessingShape">'
    .. '<wps:wsp><wps:cNvSpPr/><wps:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="%d" cy="%d"/></a:xfrm>'
    .. '<a:prstGeom prst="%s"><a:avLst/></a:prstGeom>'
    .. '<a:solidFill><a:srgbClr val="%s"/></a:solidFill><a:ln><a:noFill/></a:ln></wps:spPr>'
    .. '<wps:bodyPr/></wps:wsp></a:graphicData></a:graphic></wp:inline></w:drawing>'
    .. '</mc:Choice><mc:Fallback/></mc:AlternateContent></w:r>'):format(
    cx, cy, id, id, cx, cy, PRESET[shape.kind] or "rect", shape.fill:sub(2):upper())
end

--
-- **Where each comment begins and ends** (W7d), as the runs themselves:
-- Word's comment is a range opened before its first run and closed after
-- its last, wherever they are - in one paragraph, across several, in a
-- table's cell. Set by `document_xml` for the paragraphs it writes.
--
local comment_edges = nil

local function edges_of(doc)
  local first, last = {}, {}

  richtext.each_text(doc.body, function(p)
    for _, r in ipairs(p.runs or {}) do
      if r.comment then
        if not first[r.comment] then first[r.comment] = r end
        last[r.comment] = r
      end
    end
  end)

  local starts, ends = {}, {}
  for id, r in pairs(first) do starts[r] = id end
  for id, r in pairs(last) do ends[r] = id end

  return { starts = starts, ends = ends, ids = first }
end

--
-- **One paragraph as `w:p`**: its style, what it changes from it, a
-- picture, its runs. `cell` when it is a table's cell, which has none of
-- the space round a paragraph - as Write sets one - and, in a header row,
-- bold text.
--
local function paragraph_xml(p, style, media, cell, shape_id)
  local own = {}
  for _, k in ipairs(richtext.PARA_KEYS) do
    if p[k] ~= nil then own[k] = true end
  end

  local layout = richtext.layout(p, style)

  if cell then
    layout.before_pt, layout.after_pt = 0, 0
    own.before_pt, own.after_pt = true, true
    own.list, own.drop_cap_lines = nil, nil
  end

  -- A list's indents are Word's numbering's: the paragraph says only that
  -- it is in one.
  local parts = { "<w:p>", ppr(layout, p.style, own) }

  if p.picture and media[p.picture.name] then
    parts[#parts + 1] = drawing(p.picture, media[p.picture.name])
  end

  if p.shape then parts[#parts + 1] = shape_xml(p.shape, shape_id) end

  for _, r in ipairs(p.runs) do
    local raw = r
    local only = {}
    for _, k in ipairs(richtext.CHAR_KEYS) do
      if r[k] ~= nil then only[k] = true end
    end

    if cell == "header" and r.weight == nil then
      r = setmetatable({ weight = "Bold" }, { __index = r })
      only.weight = true
    end

    local opens = comment_edges and comment_edges.starts[raw]
    local closes = comment_edges and comment_edges.ends[raw]

    if opens then parts[#parts + 1] = ('<w:commentRangeStart w:id="%d"/>'):format(opens) end

    parts[#parts + 1] = "<w:r>" .. rpr(r, only) .. run_text(r.text) .. "</w:r>"

    if closes then
      parts[#parts + 1] = ('<w:commentRangeEnd w:id="%d"/><w:r><w:commentReference w:id="%d"/></w:r>')
                          :format(closes, closes)
    end
  end

  parts[#parts + 1] = "</w:p>"

  return table.concat(parts)
end

--
-- **A table as `w:tbl`** (W5b): fixed columns sharing the room between its
-- paragraph's indents, Write's rules round every cell, the room inside a
-- cell, and a header row tinted, bold and repeated on each page - Word's
-- `w:tblHeader`, which is the same rule.
--
local function table_xml(p, style, doc, by_name, media)
  local t = p.table
  local layout = richtext.layout(p, style)
  local page_w = writedoc.page_mm(doc)
  local room = page_w - doc.margins_mm.left - doc.margins_mm.right
               - layout.indent_left_mm - layout.indent_right_mm
  local box = t.box
  local pad = twips_pt(box and 8 or 4)

  -- **A text box** (W7a) is Word's table of one cell too: as wide as the
  -- box, placed as its paragraph aligns, shaded with its fill and ruled
  -- when it has a border.
  if box then room = math.min(room, box.width_mm) end

  local col = math.floor(twips_mm(room) / t.columns)
  local rule = box and '<w:%s w:val="single" w:sz="6" w:space="0" w:color="5B6677"/>'
               or '<w:%s w:val="single" w:sz="4" w:space="0" w:color="A3ABB6"/>'
  local edges = {}

  for _, e in ipairs({ "top", "left", "bottom", "right", "insideH", "insideV" }) do
    edges[#edges + 1] = (box and not box.border) and ('<w:%s w:val="nil"/>'):format(e)
                        or rule:format(e)
  end

  local JC_TABLE = { center = "center", right = "right" }
  local placed = box and JC_TABLE[layout.align]

  local out = { "<w:tbl><w:tblPr>",
    ('<w:tblW w:w="%d" w:type="dxa"/>'):format(col * t.columns),
    placed and ('<w:jc w:val="%s"/>'):format(placed) or "",
    placed and "" or ('<w:tblInd w:w="%d" w:type="dxa"/>'):format(twips_mm(layout.indent_left_mm)),
    "<w:tblBorders>", table.concat(edges), "</w:tblBorders>",
    '<w:tblLayout w:type="fixed"/>',
    ('<w:tblCellMar><w:top w:w="%d" w:type="dxa"/><w:left w:w="%d" w:type="dxa"/>'
     .. '<w:bottom w:w="%d" w:type="dxa"/><w:right w:w="%d" w:type="dxa"/></w:tblCellMar>')
      :format(pad, pad, pad, pad),
    "</w:tblPr><w:tblGrid>" }

  for _ = 1, t.columns do out[#out + 1] = ('<w:gridCol w:w="%d"/>'):format(col) end

  out[#out + 1] = "</w:tblGrid>"

  for r, row in ipairs(t.rows) do
    local header = t.header and r == 1

    out[#out + 1] = header and "<w:tr><w:trPr><w:tblHeader/></w:trPr>" or "<w:tr>"

    for _, cell in ipairs(row) do
      local fill = header and "E9EDF2" or box and box.fill and box.fill:sub(2):upper()

      out[#out + 1] = ('<w:tc><w:tcPr><w:tcW w:w="%d" w:type="dxa"/>%s</w:tcPr>'):format(col,
        fill and ('<w:shd w:val="clear" w:color="auto" w:fill="%s"/>'):format(fill) or "")
      out[#out + 1] = paragraph_xml(cell, by_name[cell.style] or style, media,
                                    header and "header" or "cell")
      out[#out + 1] = "</w:tc>"
    end

    out[#out + 1] = "</w:tr>"
  end

  out[#out + 1] = "</w:tbl>"

  return table.concat(out)
end

--
-- **A chart as Word's own** (W7c): a chart part of its own with the numbers
-- written into it - `c:strLit` and `c:numLit`, no workbook beside it - so
-- Word draws it, and it says what it shows without Kosmos.
--
local CHART_COLOURS = { "2A55C9", "D35400", "27AE60", "8E44AD", "C0392B", "16A085" }

local function chart_part(t)
  local data = richtext.chart_data(t)
  local kind = t.chart.kind
  local out = { HEAD, '<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" ',
    'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" ',
    'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">',
    '<c:chart><c:autoTitleDeleted val="1"/><c:plotArea><c:layout/>' }

  if kind == "pie" then
    out[#out + 1] = '<c:pieChart><c:varyColors val="1"/>'
  elseif kind == "line" then
    out[#out + 1] = '<c:lineChart><c:grouping val="standard"/><c:varyColors val="0"/>'
  else
    out[#out + 1] = ('<c:barChart><c:barDir val="%s"/><c:grouping val="clustered"/>'
                     .. '<c:varyColors val="0"/>'):format(kind == "bar" and "bar" or "col")
  end

  local function points(list, as_number)
    local p = { ('<c:ptCount val="%d"/>'):format(#list) }

    for i, v in ipairs(list) do
      p[#p + 1] = ('<c:pt idx="%d"><c:v>%s</c:v></c:pt>'):format(i - 1,
        as_number and ("%.15g"):format(v) or esc(v))
    end

    return table.concat(p)
  end

  for s, name in ipairs(data.series) do
    if kind == "pie" and s > 1 then break end

    local fill = ('<a:solidFill><a:srgbClr val="%s"/></a:solidFill>'):format(
      CHART_COLOURS[(s - 1) % #CHART_COLOURS + 1])

    out[#out + 1] = ('<c:ser><c:idx val="%d"/><c:order val="%d"/><c:tx><c:v>%s</c:v></c:tx>')
                    :format(s - 1, s - 1, esc(name))

    if kind == "line" then
      out[#out + 1] = '<c:spPr><a:ln w="19050">' .. fill .. '</a:ln></c:spPr>'
    elseif kind ~= "pie" then
      out[#out + 1] = '<c:spPr>' .. fill .. '</c:spPr>'
    end

    out[#out + 1] = '<c:cat><c:strLit>' .. points(data.categories) .. '</c:strLit></c:cat>'
    out[#out + 1] = '<c:val><c:numLit>' .. points(data.values[s], true) .. '</c:numLit></c:val>'
    out[#out + 1] = '</c:ser>'
  end

  if kind == "pie" then
    out[#out + 1] = '<c:firstSliceAng val="0"/></c:pieChart>'
  else
    if kind == "line" then
      out[#out + 1] = '<c:marker val="1"/><c:axId val="111"/><c:axId val="222"/></c:lineChart>'
    else
      out[#out + 1] = '<c:gapWidth val="43"/><c:axId val="111"/><c:axId val="222"/></c:barChart>'
    end

    local cat_at, val_at = kind == "bar" and "l" or "b", kind == "bar" and "b" or "l"

    out[#out + 1] = ('<c:catAx><c:axId val="111"/><c:scaling><c:orientation val="%s"/></c:scaling>'
      .. '<c:delete val="0"/><c:axPos val="%s"/><c:numFmt formatCode="General" sourceLinked="0"/>'
      .. '<c:tickLblPos val="nextTo"/><c:crossAx val="222"/><c:crosses val="autoZero"/></c:catAx>')
      :format(kind == "bar" and "maxMin" or "minMax", cat_at)
    out[#out + 1] = ('<c:valAx><c:axId val="222"/><c:scaling><c:orientation val="minMax"/></c:scaling>'
      .. '<c:delete val="0"/><c:axPos val="%s"/><c:majorGridlines/>'
      .. '<c:numFmt formatCode="General" sourceLinked="0"/><c:tickLblPos val="nextTo"/>'
      .. '<c:crossAx val="111"/><c:crosses val="autoZero"/><c:crossBetween val="between"/></c:valAx>')
      :format(val_at)
  end

  out[#out + 1] = '</c:plotArea><c:legend><c:legendPos val="t"/><c:overlay val="0"/></c:legend>'
                  .. '<c:plotVisOnly val="1"/></c:chart></c:chartSpace>'

  return table.concat(out)
end

-- A chart drawn inline, its part found by a relationship, as wide as the
-- column and as tall as it says.
local function chart_drawing(k, width_mm, height_mm)
  local cx = math.floor(width_mm * 36000 + 0.5)
  local cy = math.floor(height_mm * 36000 + 0.5)

  return ('<w:p><w:pPr><w:jc w:val="center"/></w:pPr><w:r><w:drawing>'
    .. '<wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="%d" cy="%d"/>'
    .. '<wp:docPr id="%d" name="Chart %d"/>'
    .. '<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
    .. '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">'
    .. '<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="rIdChart%d"/>'
    .. '</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>'):format(
    cx, cy, 2000 + k, k, k)
end

-- The document's charts in order: what `chartN.xml` each is.
local function charts_of(doc)
  local out = {}

  for _, p in ipairs(doc.body) do
    if p.table and p.table.chart then out[#out + 1] = p end
  end

  return out
end

docxwrite.chart_part = chart_part

local function document_xml(doc, media)
  media = media or {}
  comment_edges = edges_of(doc)

  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local body = {}
  local charts = 0
  local page_w = writedoc.page_mm(doc)
  local column_mm = page_w - doc.margins_mm.left - doc.margins_mm.right

  for i, p in ipairs(doc.body) do
    local style = by_name[p.style] or doc.styles[1]

    if p.table and p.table.chart then
      charts = charts + 1
      body[#body + 1] = chart_drawing(charts, column_mm, p.table.chart.height_mm)
    elseif p.table then
      body[#body + 1] = table_xml(p, style, doc, by_name, media)

      -- Word joins two tables that touch, and ends a body on a paragraph:
      -- an empty one after a table that has no paragraph after it.
      local after = doc.body[i + 1]
      if not after or (after.table and not after.table.chart) then body[#body + 1] = "<w:p/>" end
    else
      -- A drawing's id is the document's to keep unique: a shape's above
      -- every picture's.
      body[#body + 1] = paragraph_xml(p, style, media, nil, 1000 + i)
    end
  end

  local w_mm, h_mm = writedoc.page_mm(doc)
  local m = doc.margins_mm
  local refs = {}

  if doc.header.on and doc.header.text ~= "" then
    refs[#refs + 1] = '<w:headerReference w:type="default" r:id="rIdHeader"/>'
  end

  if doc.footer.on and doc.footer.page_numbers then
    refs[#refs + 1] = '<w:footerReference w:type="default" r:id="rIdFooter"/>'
  end

  local sect = ('<w:sectPr>%s<w:pgSz w:w="%d" w:h="%d"%s/>'
    .. '<w:pgMar w:top="%d" w:right="%d" w:bottom="%d" w:left="%d" '
    .. 'w:header="%d" w:footer="%d" w:gutter="0"/></w:sectPr>'):format(
    table.concat(refs), twips_mm(w_mm), twips_mm(h_mm),
    doc.paper.landscape and ' w:orient="landscape"' or "",
    twips_mm(m.top), twips_mm(m.right), twips_mm(m.bottom), twips_mm(m.left),
    twips_mm(doc.header.from_top_mm), twips_mm(doc.footer.from_bottom_mm))

  comment_edges = nil

  return HEAD .. "<w:document " .. W .. "><w:body>" .. table.concat(body)
         .. sect .. "</w:body></w:document>"
end

--
-- **The comments' words** (W7d), each in a paragraph of its own: those the
-- body refers to, and no other.
--
local function comments_xml(doc)
  local used = edges_of(doc).ids
  local out = { HEAD, "<w:comments ", W, ">" }

  for _, c in ipairs(doc.comments or {}) do
    if used[c.id] then
      out[#out + 1] = ('<w:comment w:id="%d" w:author="Kosmos Write" w:initials="KW">'
        .. '<w:p><w:r>%s</w:r></w:p></w:comment>'):format(c.id, run_text(c.text))
    end
  end

  out[#out + 1] = "</w:comments>"

  return table.concat(out)
end

local function styles_xml(doc)
  local out = { HEAD, "<w:styles ", W, ">",
    ('<w:docDefaults><w:rPrDefault><w:rPr><w:lang w:val="%s"/></w:rPr></w:rPrDefault>'
     .. '</w:docDefaults>'):format(LANG[doc.language] or "en-US") }

  for _, st in ipairs(doc.styles) do
    -- Body is Word's default paragraph style, as it is a new paragraph's here.
    out[#out + 1] = ('<w:style w:type="paragraph" w:styleId="%s"%s><w:name w:val="%s"/>'
                     .. '<w:qFormat/>%s%s</w:style>'):format(
      style_id(st.name), st.name == writedoc.BODY and ' w:default="1"' or "",
      esc(st.name), ppr(st, nil), rpr(st))
  end

  out[#out + 1] = "</w:styles>"
  return table.concat(out)
end

-- A bullet list, number 1, and a numbered one, number 2: hanging 6 mm as
-- the page has them.
local function numbering_xml()
  local hang = twips_mm(6)

  local function abstract(id, fmt, text)
    return ('<w:abstractNum w:abstractNumId="%d"><w:lvl w:ilvl="0"><w:start w:val="1"/>'
            .. '<w:numFmt w:val="%s"/><w:lvlText w:val="%s"/><w:lvlJc w:val="left"/>'
            .. '<w:pPr><w:ind w:left="%d" w:hanging="%d"/></w:pPr></w:lvl></w:abstractNum>')
           :format(id, fmt, text, hang, hang)
  end

  return HEAD .. "<w:numbering " .. W .. ">" .. abstract(0, "bullet", "\u{2022}")
         .. abstract(1, "decimal", "%1.")
         .. '<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>'
         .. '<w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num></w:numbering>'
end

local function settings_xml(doc)
  return HEAD .. "<w:settings " .. W .. ">"
         .. (doc.facing and "<w:mirrorMargins/>" or "")
         .. (doc.hyphenation and "<w:autoHyphenation/>" or "")
         .. "</w:settings>"
end

local function header_xml(doc)
  return HEAD .. "<w:hdr " .. W .. '><w:p><w:pPr><w:pStyle w:val="Caption"/><w:jc w:val="center"/>'
         .. "</w:pPr><w:r>" .. run_text(doc.header.text) .. "</w:r></w:p></w:hdr>"
end

local function footer_xml()
  return HEAD .. "<w:ftr " .. W .. '><w:p><w:pPr><w:pStyle w:val="Caption"/><w:jc w:val="center"/>'
         .. '</w:pPr><w:fldSimple w:instr="PAGE"><w:r><w:t>1</w:t></w:r></w:fldSimple>'
         .. "</w:p></w:ftr>"
end

--
-- **`doc` as a DOCX at `path`**: true and `{ paragraphs, bytes }`, or nil
-- and why; nothing is written unless all of it is (`zip.write`).
--
function docxwrite.write(path, doc, info)
  info = info or {}

  -- The pictures the document shows, each once, as their own files.
  local media, files = {}, {}

  for _, p in ipairs(doc.body) do
    local name = p.picture and p.picture.name
    local got = name and not media[name] and info.pictures and info.pictures(name)

    if got and type(got.bytes) == "string" then
      local ext = name:match("%.(%w+)$") or "png"
      local n = #files + 1
      local file = ("image%d.%s"):format(n, ext:lower())

      media[name] = { n = n, file = file, rid = "rIdImage" .. n }
      files[#files + 1] = { name = "word/media/" .. file, text = got.bytes,
                            rid = "rIdImage" .. n, file = file }
    end
  end

  local charts = charts_of(doc)
  local commented = next(edges_of(doc).ids) ~= nil
  local CT = "application/vnd.openxmlformats-officedocument.wordprocessingml"
  local header = doc.header.on and doc.header.text ~= ""
  local footer = doc.footer.on and doc.footer.page_numbers

  local types = { HEAD, '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">',
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>',
    '<Default Extension="xml" ContentType="application/xml"/>',
    '<Default Extension="png" ContentType="image/png"/>',
    '<Default Extension="jpg" ContentType="image/jpeg"/>',
    '<Default Extension="jpeg" ContentType="image/jpeg"/>',
    '<Override PartName="/word/document.xml" ContentType="', CT, '.document.main+xml"/>',
    '<Override PartName="/word/styles.xml" ContentType="', CT, '.styles+xml"/>',
    '<Override PartName="/word/numbering.xml" ContentType="', CT, '.numbering+xml"/>',
    '<Override PartName="/word/settings.xml" ContentType="', CT, '.settings+xml"/>',
    '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>' }

  if header then
    types[#types + 1] = '<Override PartName="/word/header1.xml" ContentType="' .. CT .. '.header+xml"/>'
  end

  if footer then
    types[#types + 1] = '<Override PartName="/word/footer1.xml" ContentType="' .. CT .. '.footer+xml"/>'
  end

  for k = 1, #charts do
    types[#types + 1] = ('<Override PartName="/word/charts/chart%d.xml" ContentType="'
      .. 'application/vnd.openxmlformats-officedocument.drawingml.chart+xml"/>'):format(k)
  end

  if commented then
    types[#types + 1] = '<Override PartName="/word/comments.xml" ContentType="' .. CT .. '.comments+xml"/>'
  end

  types[#types + 1] = "</Types>"

  local REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
  local doc_rels = { HEAD, '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">',
    '<Relationship Id="rIdStyles" Type="', REL, 'styles" Target="styles.xml"/>',
    '<Relationship Id="rIdNumbering" Type="', REL, 'numbering" Target="numbering.xml"/>',
    '<Relationship Id="rIdSettings" Type="', REL, 'settings" Target="settings.xml"/>' }

  if header then
    doc_rels[#doc_rels + 1] = '<Relationship Id="rIdHeader" Type="' .. REL .. 'header" Target="header1.xml"/>'
  end

  if footer then
    doc_rels[#doc_rels + 1] = '<Relationship Id="rIdFooter" Type="' .. REL .. 'footer" Target="footer1.xml"/>'
  end

  for _, f in ipairs(files) do
    doc_rels[#doc_rels + 1] = ('<Relationship Id="%s" Type="%simage" Target="media/%s"/>')
                              :format(f.rid, REL, f.file)
  end

  for k = 1, #charts do
    doc_rels[#doc_rels + 1] = ('<Relationship Id="rIdChart%d" Type="%schart" Target="charts/chart%d.xml"/>')
                              :format(k, REL, k)
  end

  if commented then
    doc_rels[#doc_rels + 1] = '<Relationship Id="rIdComments" Type="' .. REL
                              .. 'comments" Target="comments.xml"/>'
  end

  doc_rels[#doc_rels + 1] = "</Relationships>"

  local entries = {
    { name = "[Content_Types].xml", text = table.concat(types) },
    { name = "_rels/.rels", text = HEAD
      .. '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
      .. '<Relationship Id="rIdDocument" Type="' .. REL .. 'officeDocument" Target="word/document.xml"/>'
      .. '<Relationship Id="rIdCore" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>'
      .. "</Relationships>" },
    { name = "docProps/core.xml", text = HEAD
      .. '<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" '
      .. 'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>' .. esc(info.title or "")
      .. "</dc:title><dc:creator>Kosmos Write</dc:creator></cp:coreProperties>" },
    { name = "word/_rels/document.xml.rels", text = table.concat(doc_rels) },
    { name = "word/document.xml", text = document_xml(doc, media) },
    { name = "word/styles.xml", text = styles_xml(doc) },
    { name = "word/numbering.xml", text = numbering_xml() },
    { name = "word/settings.xml", text = settings_xml(doc) },
  }

  if header then entries[#entries + 1] = { name = "word/header1.xml", text = header_xml(doc) } end

  for _, f in ipairs(files) do entries[#entries + 1] = { name = f.name, text = f.text } end

  for k, p in ipairs(charts) do
    entries[#entries + 1] = { name = ("word/charts/chart%d.xml"):format(k), text = chart_part(p.table) }
  end

  if commented then entries[#entries + 1] = { name = "word/comments.xml", text = comments_xml(doc) } end
  if footer then entries[#entries + 1] = { name = "word/footer1.xml", text = footer_xml() } end

  -- The zip when it is written, so the parts can be read on the Mac
  -- without the Compression Kit.
  local ok, why = use("/Kosmos/Libraries/zip.lua").write{ entries = entries, to = path }

  if not ok then return nil, why end

  local attrs = fs.getattr(path)

  return true, { paragraphs = #doc.body, bytes = attrs and attrs.size or 0 }
end

-- The parts, without the zip: what the Mac's test reads (`test_docx.lua`).
docxwrite.parts = { document = document_xml, styles = styles_xml, comments = comments_xml,
                    numbering = numbering_xml, settings = settings_xml }

return docxwrite
