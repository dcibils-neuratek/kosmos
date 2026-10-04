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
-- **What maps, and how**: a paragraph is `w:p` in its style, with what it
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

local function document_xml(doc, media)
  media = media or {}

  local by_name = {}
  for _, st in ipairs(doc.styles) do by_name[st.name] = st end

  local body = {}

  for _, p in ipairs(doc.body) do
    local style = by_name[p.style] or doc.styles[1]
    local own = {}
    for _, k in ipairs(richtext.PARA_KEYS) do
      if p[k] ~= nil then own[k] = true end
    end

    -- A list's indents are Word's numbering's: the paragraph says only that
    -- it is in one.
    local parts = { "<w:p>", ppr(richtext.layout(p, style), p.style, own) }

    if p.picture and media[p.picture.name] then
      parts[#parts + 1] = drawing(p.picture, media[p.picture.name])
    end

    for _, r in ipairs(p.runs) do
      local only = {}
      for _, k in ipairs(richtext.CHAR_KEYS) do
        if r[k] ~= nil then only[k] = true end
      end

      parts[#parts + 1] = "<w:r>" .. rpr(r, only) .. run_text(r.text) .. "</w:r>"
    end

    parts[#parts + 1] = "</w:p>"
    body[#body + 1] = table.concat(parts)
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

  return HEAD .. "<w:document " .. W .. "><w:body>" .. table.concat(body)
         .. sect .. "</w:body></w:document>"
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
  if footer then entries[#entries + 1] = { name = "word/footer1.xml", text = footer_xml() } end

  -- The zip when it is written, so the parts can be read on the Mac
  -- without the Compression Kit.
  local ok, why = use("/Kosmos/Libraries/zip.lua").write{ entries = entries, to = path }

  if not ok then return nil, why end

  local attrs = fs.getattr(path)

  return true, { paragraphs = #doc.body, bytes = attrs and attrs.size or 0 }
end

-- The parts, without the zip: what the Mac's test reads (`test_docx.lua`).
docxwrite.parts = { document = document_xml, styles = styles_xml,
                    numbering = numbering_xml, settings = settings_xml }

return docxwrite
