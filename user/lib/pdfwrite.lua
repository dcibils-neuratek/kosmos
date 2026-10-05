-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Pages out as a PDF (`docs/write.md`, W3).
--
--   local pdfwrite = use("/Kosmos/Libraries/pdfwrite.lua")
--   local set = pageset.set(doc, measure)
--   local ok, notes = pdfwrite.write("/Home/Letter.pdf", set, measure,
--                                    { title = "Letter" })
--
-- **The PDF Kit writing, for the first time.** Diego, 4 October 2026: "pdf
-- export is key". It writes what `pageset` set and nothing else: each
-- piece where the setting placed it, in the face `faces` picked for it, so
-- the PDF is the page on the screen rather than a second opinion of it.
--
-- **The faces travel with it, and any character they have.** Each face a
-- document uses is embedded as a TrueType program (`FontFile2`) under
-- a Type 0 font whose text is glyph numbers - `Identity-H`, two bytes a
-- glyph - so a PDF shows every character its face can draw rather than the
-- two hundred of a single-byte encoding. Its widths are the ones the
-- setting used, the font's own advances, so a reader that sets the same
-- glyphs gets the same line; and a `ToUnicode` map says which character
-- each glyph is, so the text can be searched and copied. A character the
-- face has no glyph for is shown as its missing glyph, counted, and the
-- count comes back for the window to say (`notes.missing`).
--
-- The first version wrote single-byte WinAnsi fonts, and Kosmos's own
-- reader would not draw them: it reads Type 0 fonts, which is what PDFs of
-- embedded TrueType are now. It was the writer that was out of date.
--
-- **Structure here, bytes in C**, as `zip.lua` has it: the objects, the
-- dictionaries and each page's operators are Lua's - a few dozen bytes a
-- line and a decision about each - and the loops are C's: text to glyph
-- numbers in `face:glyphs`, and deflating in the Compression Kit, over
-- regions - a font program from the image's own copy straight into the
-- PDF, a page's operators from a region, the whole written from the region
-- it was made in. A `FlateDecode` stream is zlib's - two bytes, the deflate,
-- an Adler-32 - where a zip's is the deflate alone.
--
-- **Only the glyphs a document shows** (W3b): each face is a subset -
-- `face:subset`, the font with the other outlines emptied and the tables a
-- PDF reader never reads left out - named, as a PDF names a subset, with
-- six letters and a `+` before its name. Whole, a face was about 93 KB
-- deflated, and three pages in seven faces were 652 KB of font.
--
-- **Pictures** (W5) are a JPEG as it came or anything else as a PNG's data
-- (`pdfwrite.image`). What it does not do yet, said: kerning.

local pdfwrite = {}

local kit = use("/Kosmos/Kits/compress")
local regions = use("/Kosmos/Libraries/regions.lua")

--------------------------------------------------------------------------
-- The PDF's own syntax.
--------------------------------------------------------------------------

-- A number as a PDF writes one: up to three decimals, and no more digits
-- than it needs. -0 is 0.
local function num(n)
  if n ~= n then n = 0 end

  local s = ("%.3f"):format(n):gsub("0+$", ""):gsub("%.$", "")

  if s == "-0" then s = "0" end

  return s
end

pdfwrite.num = num

-- A code point as UTF-16, big-endian, in hex: what a ToUnicode map and a
-- text string both say.
local function utf16(u)
  if u >= 0x10000 then
    u = u - 0x10000
    return ("%04X%04X"):format(0xD800 + (u >> 10), 0xDC00 + (u & 0x3FF))
  end

  return ("%04X"):format(u)
end

-- A text string - the document's title - as UTF-16 with its mark: any
-- character, in a form every reader takes.
local function text_string(s)
  local out = { "<FEFF" }

  for ch in s:gmatch(utf8.charpattern) do
    local ok, u = pcall(utf8.codepoint, ch)
    if ok then out[#out + 1] = utf16(u) end
  end

  return table.concat(out) .. ">"
end

-- A PostScript name as a PDF name may hold it.
local function ps_name(s, n)
  s = type(s) == "string" and s:gsub("[^%w%-_]", "") or ""
  if s == "" then s = "KosmosFace" .. n end
  return s
end

-- `#rrggbb` as three fractions.
local function rgb(colour)
  local r, g, b = colour:match("^#(%x%x)(%x%x)(%x%x)$")

  if not r then return "0 0 0" end

  return num(tonumber(r, 16) / 255) .. " " .. num(tonumber(g, 16) / 255) .. " "
         .. num(tonumber(b, 16) / 255)
end

-- Regions: `regions.lua`'s, shared with `zip.lua` - this file began with a
-- copy of that one's, the second copy the premise in `CLAUDE.md` names.
local region, free = regions.make, regions.free

--------------------------------------------------------------------------
-- The fonts.
--------------------------------------------------------------------------

--
-- One face as the PDF will carry it: its name, its descriptor's numbers in
-- thousandths of the size, the positions of its underline and strike-out
-- in its own units, and - filled as the pages are written - the glyphs
-- shown and the character each one is.
--
local function font_of(entry, n)
  local units = entry.units
  local scale = 1000 / units
  local d = entry.face:descriptor()
  local italic = (d.italic_angle or 0) ~= 0

  return {
    n = n, entry = entry, name = ps_name(d.postscript, n),
    used = {},
    flags = 32 + (d.fixed and 1 or 0) + (italic and 64 or 0),
    bbox = ("%s %s %s %s"):format(num((d.xmin or 0) * scale),
      num((d.ymin or entry.descent) * scale), num((d.xmax or units) * scale),
      num((d.ymax or entry.ascent) * scale)),
    italic_angle = num(d.italic_angle or 0),
    ascent = num(entry.ascent * scale),
    descent = num(entry.descent * scale),
    cap_height = num((d.cap_height or entry.ascent * 0.7) * scale),
    under_at = d.underline_position or -units // 10,
    under_th = d.underline_thickness or units // 20,
    strike_at = d.strike_position or units * 3 // 10,
    strike_th = d.strike_size or units // 20,
  }
end

--
-- **A subset's tag**: six capitals from the glyphs it holds, so the same
-- glyphs of the same face are the same name and different ones are not -
-- which is what lets a reader tell two subsets of one face apart.
--
local function tag_of(glyphs, name)
  local h = 2166136261

  for i = 1, #name do h = ((h ~ name:byte(i)) * 16777619) & 0xffffffff end
  for _, g in ipairs(glyphs) do h = ((h ~ g) * 16777619) & 0xffffffff end

  local out = {}

  for i = 1, 6 do
    out[i] = string.char(65 + h % 26)
    h = h // 26
  end

  return table.concat(out)
end

--
-- `/W`: each glyph the pages show and its width, in thousandths of the
-- size, runs of neighbouring glyphs written as one.
--
local function widths_of(font)
  local glyphs = { 0 }

  for g in pairs(font.used) do glyphs[#glyphs + 1] = g end
  table.sort(glyphs)

  local out, i = {}, 1
  local scale = 1000 / font.entry.units

  while i <= #glyphs do
    local first, run = glyphs[i], {}

    repeat
      run[#run + 1] = num(font.entry.face:glyph_advance(glyphs[i]) * scale)
      i = i + 1
    until i > #glyphs or glyphs[i] ~= glyphs[i - 1] + 1

    out[#out + 1] = ("%d [%s]"):format(first, table.concat(run, " "))
  end

  return table.concat(out, " ")
end

--
-- `ToUnicode`: the character each glyph shown is, a hundred to a block as
-- the format allows.
--
local function to_unicode(font)
  local glyphs = {}

  for g in pairs(font.used) do glyphs[#glyphs + 1] = g end
  table.sort(glyphs)

  local out = {
    "/CIDInit /ProcSet findresource begin",
    "12 dict begin",
    "begincmap",
    "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def",
    "/CMapName /Adobe-Identity-UCS def",
    "/CMapType 2 def",
    "1 begincodespacerange",
    "<0000> <FFFF>",
    "endcodespacerange",
  }

  for at = 1, #glyphs, 100 do
    local last = math.min(#glyphs, at + 99)

    out[#out + 1] = ("%d beginbfchar"):format(last - at + 1)

    for i = at, last do
      local is = font.used[glyphs[i]]

      -- A ligature's glyph is the letters it stands for (`face:glyphs`).
      if type(is) == "string" then
        local parts = {}
        for _, u in utf8.codes(is) do parts[#parts + 1] = utf16(u) end
        is = table.concat(parts)
      else
        is = utf16(is)
      end

      out[#out + 1] = ("<%04X> <%s>"):format(glyphs[i], is)
    end

    out[#out + 1] = "endbfchar"
  end

  out[#out + 1] = "endcmap"
  out[#out + 1] = "CMapName currentdict /CMap defineresource pop"
  out[#out + 1] = "end"
  out[#out + 1] = "end"

  return table.concat(out, "\n") .. "\n"
end

--------------------------------------------------------------------------
-- A page's operators.
--------------------------------------------------------------------------

--
-- What draws one page: each piece shown at its baseline - a PDF's y runs
-- up from the foot of the page, the setting's down from its head - and a
-- rule under or through the text where its look says.
--
-- **A justified line's spaces are widened with `TJ`**: after each space, a
-- step right of the extra, in thousandths of the size and negative, as the
-- operator has it. `Tw`, which says the same in one number, applies only to
-- a single-byte space, and these are two-byte glyphs.
--
local function operators(set, page, font_for, notes, image_for)
  local out = {}
  local height = page.height_pt
  local shift = 0               -- a left-hand page's, on facing pages

  local function show(piece, look, baseline, extra)
    if piece.picture then
      -- **A picture**: its image drawn into its place - a PDF draws an
      -- image into the unit square, so the matrix is its size and where.
      local image = image_for(piece.picture)

      if image then
        out[#out + 1] = ("q %s 0 0 %s %s %s cm /Im%d Do Q"):format(
          num(piece.width_pt), num(piece.height_pt), num(piece.x_pt + shift),
          num(height - baseline), image.n)
      end

      return
    end

    if piece.text == "" then return end

    local font = font_for(look)
    local face = font.entry.face
    local x, y = piece.x_pt + shift, height - baseline - (piece.drop_pt or 0)
    local colour = rgb(look.colour)
    local shown

    if extra ~= 0 and piece.text:find(" ", 1, true) then
      local parts, start = {}, 1
      local step = num(-extra * 1000 / look.size_pt)
      local text = piece.text

      for i = 1, #text do
        if text:byte(i) == 32 then
          local hex, missing = face:glyphs(text:sub(start, i), font.used, set.ligatures)
          notes.missing = notes.missing + missing
          parts[#parts + 1] = "<" .. hex .. "> " .. step
          start = i + 1
        end
      end

      if start <= #text then
        local hex, missing = face:glyphs(text:sub(start), font.used, set.ligatures)
        notes.missing = notes.missing + missing
        parts[#parts + 1] = "<" .. hex .. ">"
      end

      shown = "[" .. table.concat(parts, " ") .. "] TJ"
    else
      local hex, missing = face:glyphs(piece.text, font.used, set.ligatures)
      notes.missing = notes.missing + missing
      shown = "<" .. hex .. "> Tj"
    end

    out[#out + 1] = ("BT /F%d %s Tf %s rg %s %s Td %s ET"):format(
      font.n, num(look.size_pt), colour, num(x), num(y), shown)

    local per = look.size_pt / font.entry.units

    if look.underline then
      out[#out + 1] = ("%s rg %s %s %s %s re f"):format(colour, num(x),
        num(y + font.under_at * per - font.under_th * per / 2),
        num(piece.width_pt), num(font.under_th * per))
    end

    if look.strike then
      out[#out + 1] = ("%s rg %s %s %s %s re f"):format(colour, num(x),
        num(y + font.strike_at * per - font.strike_th * per / 2),
        num(piece.width_pt), num(font.strike_th * per))
    end
  end

  --
  -- **What a line draws under its text** (`pageset`'s `art`): a table's
  -- tint as a filled rectangle and its rules as stroked lines, each in its
  -- own graphics state so its colour and width go no further.
  --
  -- A point of the setting's - x across, y down from a line's top - as
  -- the PDF's.
  local function at(xp, yp, top)
    return num(xp + shift) .. " " .. num(height - (top + yp))
  end

  -- Bezier's constant for a quarter of a circle.
  local K = 0.5523

  local function art(a, top)
    if a.kind == "rect" and a.radius_pt then
      -- Rounded: four sides and four quarter circles, as curves.
      local x0, y0, x1, y1 = a.x_pt, a.y_pt, a.x_pt + a.w_pt, a.y_pt + a.h_pt
      local r = a.radius_pt
      local k = K * r

      out[#out + 1] = table.concat({ "q", rgb(a.fill), "rg",
        at(x0 + r, y0, top), "m", at(x1 - r, y0, top), "l",
        at(x1 - r + k, y0, top), at(x1, y0 + r - k, top), at(x1, y0 + r, top), "c",
        at(x1, y1 - r, top), "l",
        at(x1, y1 - r + k, top), at(x1 - r + k, y1, top), at(x1 - r, y1, top), "c",
        at(x0 + r, y1, top), "l",
        at(x0 + r - k, y1, top), at(x0, y1 - r + k, top), at(x0, y1 - r, top), "c",
        at(x0, y0 + r, top), "l",
        at(x0, y0 + r - k, top), at(x0 + r - k, y0, top), at(x0 + r, y0, top), "c",
        "h f Q" }, " ")
    elseif a.kind == "rect" then
      out[#out + 1] = ("q %s rg %s %s %s %s re f Q"):format(rgb(a.fill),
        num(a.x_pt + shift), num(height - (top + a.y_pt + a.h_pt)), num(a.w_pt), num(a.h_pt))
    elseif a.kind == "ellipse" then
      -- Four curves, a quarter each.
      local cx, cy = a.x_pt + a.w_pt / 2, a.y_pt + a.h_pt / 2
      local rx, ry = a.w_pt / 2, a.h_pt / 2

      out[#out + 1] = table.concat({ "q", rgb(a.fill), "rg",
        at(cx + rx, cy, top), "m",
        at(cx + rx, cy + K * ry, top), at(cx + K * rx, cy + ry, top), at(cx, cy + ry, top), "c",
        at(cx - K * rx, cy + ry, top), at(cx - rx, cy + K * ry, top), at(cx - rx, cy, top), "c",
        at(cx - rx, cy - K * ry, top), at(cx - K * rx, cy - ry, top), at(cx, cy - ry, top), "c",
        at(cx + K * rx, cy - ry, top), at(cx + rx, cy - K * ry, top), at(cx + rx, cy, top), "c",
        "h f Q" }, " ")
    elseif a.kind == "poly" then
      local parts = { "q", rgb(a.fill), "rg" }

      for i = 1, #a.points, 2 do
        parts[#parts + 1] = at(a.points[i], a.points[i + 1], top)
        parts[#parts + 1] = i == 1 and "m" or "l"
      end

      parts[#parts + 1] = "h f Q"
      out[#out + 1] = table.concat(parts, " ")
    elseif a.kind == "rule" then
      out[#out + 1] = ("q %s RG %s w %s %s m %s %s l S Q"):format(rgb(a.colour),
        num(a.width_pt), num(a.x_pt + shift), num(height - (top + a.y_pt)),
        num(a.x2_pt + shift), num(height - (top + a.y2_pt)))
    end
  end

  local function draw_line(line)
    for _, a in ipairs(line.art or {}) do
      art(a, line.baseline_pt - line.ascent_pt)
    end

    if line.marker then
      show(line.marker, set.looks[line.marker.look], line.baseline_pt, 0)
    end

    for _, piece in ipairs(line.pieces) do
      show(piece, set.looks[piece.look], line.baseline_pt,
           piece.cap and 0 or line.extra_space_pt)
    end

    -- A table row's cells, each a paragraph's lines, and a chart's words.
    for _, cell in ipairs(line.cells or {}) do
      for _, l in ipairs(cell.lines) do draw_line(l) end
    end

    for _, l in ipairs(line.labels or {}) do draw_line(l) end
  end

  shift = page.shift_pt or 0

  for _, line in ipairs(page.lines) do draw_line(line) end

  shift = 0

  if page.header then
    show(page.header.piece, set.looks[page.header.piece.look],
         page.header.baseline_pt, 0)
  end

  if page.footer then
    show(page.footer.piece, set.looks[page.footer.piece.look],
         page.footer.baseline_pt, 0)
  end

  return table.concat(out, "\n") .. "\n"
end

--------------------------------------------------------------------------
-- Pictures (W5).
--------------------------------------------------------------------------

local function be16(s, i) return s:byte(i) * 256 + s:byte(i + 1) end

--
-- A JPEG's size and how many components it has, from its frame header:
-- the markers walked from the start until a start of frame. Nil when it is
-- not a JPEG this reads.
--
local function jpeg_size(b)
  if b:sub(1, 2) ~= "\xff\xd8" then return nil end

  local i = 3

  while i + 9 <= #b do
    if b:byte(i) ~= 0xFF then return nil end

    local marker = b:byte(i + 1)

    -- A start of frame - not DHT, JPG or DAC, which share the range.
    if marker >= 0xC0 and marker <= 0xCF and marker ~= 0xC4 and marker ~= 0xC8
       and marker ~= 0xCC then
      return be16(b, i + 7), be16(b, i + 5), b:byte(i + 9)
    end

    if marker == 0x01 or (marker >= 0xD0 and marker <= 0xD9) then
      i = i + 2
    else
      i = i + 2 + be16(b, i + 2)
    end
  end
end

-- A PNG's image data: its IDAT chunks, one zlib stream.
local function png_idat(png)
  local i, parts = 9, {}

  while i + 8 <= #png do
    local len = string.unpack(">I4", png, i)
    local kind = png:sub(i + 4, i + 7)

    if kind == "IDAT" then parts[#parts + 1] = png:sub(i + 8, i + 7 + len) end
    if kind == "IEND" then break end

    i = i + 12 + len
  end

  return table.concat(parts)
end

--
-- **A picture as a PDF's image**: `{ dict, data }` - the dictionary's
-- entries and the stream - from `{ bytes, surface }`. A JPEG as it came; a
-- PNG or anything else drawn over white - a transparent picture on paper -
-- and written as a PNG's data with the predictor that reads it.
--
function pdfwrite.image(got)
  if type(got.bytes) == "string" then
    local w, h, comps = jpeg_size(got.bytes)

    if w and (comps == 1 or comps == 3) then
      return { data = got.bytes,
               dict = ("/Type /XObject /Subtype /Image /Width %d /Height %d "
                       .. "/ColorSpace /%s /BitsPerComponent 8 /Filter /DCTDecode")
                      :format(w, h, comps == 1 and "DeviceGray" or "DeviceRGB") }
    end
  end

  local src = got.surface

  if not src then return nil end

  local w, h = src:size()
  local ok, white = pcall(gfx.surface, { w = w, h = h })

  if not ok or not white then return nil end

  white:fill(0, 0, w, h, 0xffffffff)
  white:stretch(src, 0, 0, w, h, 0, 0, w, h, 255, false)

  local png = gfx.encode_png(white)
  white:free()

  return { data = png_idat(png),
           dict = ("/Type /XObject /Subtype /Image /Width %d /Height %d "
                   .. "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode "
                   .. "/DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 "
                   .. "/Columns %d >>"):format(w, h, w) }
end

--------------------------------------------------------------------------
-- The file.
--------------------------------------------------------------------------

--
-- **`set` - what `pageset` set - as a PDF at `path`**, with the faces of
-- `measure` (a `faces.measure`). `info.title` names the document. True and
-- `notes` - `{ pages, fonts, bytes, missing }` - or nil and why; nothing is
-- written unless all of it is.
--
function pdfwrite.write(path, set, measure, info)
  info = info or {}

  local notes = { pages = #set.pages, fonts = 0, bytes = 0, missing = 0 }

  -- The faces the pages use, numbered in the order they are first met.
  local fonts, by_entry = {}, {}

  local function font_for(look)
    local entry = measure.face_of(look)
    local font = by_entry[entry]

    if not font then
      font = font_of(entry, #fonts + 1)
      fonts[#fonts + 1] = font
      by_entry[entry] = font
    end

    return font
  end

  --
  -- **The pictures the pages show** (W5), each an image object once
  -- however often it is shown: a JPEG as it is - a PDF reads one itself,
  -- `DCTDecode` - and anything else drawn over white and written as a PNG's
  -- data, which a PDF reads with its PNG predictor. `info.pictures(name)`
  -- gives `{ bytes, surface }`: the file as it came and it decoded.
  --
  local images, by_picture = {}, {}

  local function image_for(name)
    local image = by_picture[name]

    if image ~= nil then return image or nil end

    local got = info.pictures and info.pictures(name)

    if not got then
      by_picture[name] = false
      notes.missing_pictures = (notes.missing_pictures or 0) + 1
      return nil
    end

    image = pdfwrite.image(got)

    if not image then
      by_picture[name] = false
      return nil
    end

    image.n = #images + 1
    images[#images + 1] = image
    by_picture[name] = image
    return image
  end

  -- Every page's operators first, so the fonts and their glyphs are known
  -- before the first object is written and the room needed before it is
  -- asked for.
  local contents, biggest_content = {}, 1

  for i, page in ipairs(set.pages) do
    contents[i] = operators(set, page, font_for, notes, image_for)
    biggest_content = math.max(biggest_content, #contents[i])
  end

  notes.fonts = #fonts

  --
  -- **Comments as the PDF's own notes** (W7d): a highlight over the words
  -- each is about, on each page they stand on, its words its `/Contents` -
  -- which every reader shows as a comment, and prints only when asked.
  -- `info.comments` is `pageset.comment_marks`'.
  --
  local annots, on_page = {}, {}

  for _, c in ipairs(info.comments or {}) do
    local by_page, pages = {}, {}

    for _, r in ipairs(c.rects) do
      if not by_page[r.page] then
        by_page[r.page] = {}
        pages[#pages + 1] = r.page
      end

      table.insert(by_page[r.page], r)
    end

    table.sort(pages)

    for _, pg in ipairs(pages) do
      local h = set.pages[pg].height_pt
      local quads, x0, y0, x1, y1 = {}, math.huge, math.huge, -math.huge, -math.huge

      for _, r in ipairs(by_page[pg]) do
        local left, right = r.x_pt, r.x_pt + r.w_pt
        local high, low = h - r.y_pt, h - (r.y_pt + r.h_pt)

        quads[#quads + 1] = table.concat({ num(left), num(high), num(right), num(high),
                                           num(left), num(low), num(right), num(low) }, " ")
        x0, y0 = math.min(x0, left), math.min(y0, low)
        x1, y1 = math.max(x1, right), math.max(y1, high)
      end

      annots[#annots + 1] = ("<< /Type /Annot /Subtype /Highlight /Rect [%s %s %s %s] "
        .. "/QuadPoints [%s] /Contents %s /T %s /C [1 0.86 0.35] >>"):format(
        num(x0), num(y0), num(x1), num(y1), table.concat(quads, " "),
        text_string(c.text), text_string("Kosmos Write"))
      on_page[pg] = on_page[pg] or {}
      table.insert(on_page[pg], #annots)
    end
  end

  notes.comments = #annots

  local room, biggest = 64 * 1024, biggest_content

  for _, c in ipairs(contents) do room = room + #c + 512 end
  for _, im in ipairs(images) do room = room + #im.data + 1024 end
  for _, a in ipairs(annots) do room = room + #a + 64 end

  local biggest_program = 1

  for _, f in ipairs(fonts) do
    local _, length = f.entry.face:program()

    f.glyphs = {}
    for g in pairs(f.used) do f.glyphs[#f.glyphs + 1] = g end
    table.sort(f.glyphs)

    f.name = tag_of(f.glyphs, f.name) .. "+" .. f.name
    f.widths = widths_of(f)
    f.cmap = to_unicode(f)
    room = room + length + #f.widths + #f.cmap + 4096
    biggest = math.max(biggest, length + 4096, #f.cmap)
    biggest_program = math.max(biggest_program, length + 4096)
    biggest_content = math.max(biggest_content, #f.cmap)
  end

  local out, plain, squeezed, program, why

  out, why = region(room)
  if not out then return nil, why end

  plain, why = region(biggest_content)
  if not plain then free(out) return nil, why end

  squeezed, why = region(biggest + 1024)
  if not squeezed then free(out, plain) return nil, why end

  -- Where a face's subset is made before it is deflated: no larger than
  -- the face, a directory's worth aside.
  program, why = region(biggest_program)
  if not program then free(out, plain, squeezed) return nil, why end

  local at, offsets = 0, {}

  local function put(s)
    sys.region_write(out.cap, at, s)
    at = at + #s
  end

  local function object(n, body)
    offsets[n] = at
    put(("%d 0 obj\n%s\nendobj\n"):format(n, body))
  end

  --
  -- A stream object from `n` bytes at `src`: deflated as zlib when that is
  -- smaller, as it is when not. `extra` goes in its dictionary.
  --
  local function stream(number, src, n, extra)
    local size = kit.deflate_into(src, n, squeezed.at, squeezed.size)

    offsets[number] = at

    if size and size + 6 < n then
      local sum = kit.adler32(src, n)

      put(("%d 0 obj\n<< /Length %d /Filter /FlateDecode%s >>\nstream\n")
          :format(number, size + 6, extra or ""))
      put("\x78\x9c")
      kit.copy_into(squeezed.at, out.at + at, size)
      at = at + size
      put(string.pack(">I4", sum))
    else
      put(("%d 0 obj\n<< /Length %d%s >>\nstream\n"):format(number, n, extra or ""))
      kit.copy_into(src, out.at + at, n)
      at = at + n
    end

    put("\nendstream\nendobj\n")
  end

  -- A stream from a Lua string: into the plain region, then as above.
  local function text_stream(number, text, extra)
    sys.region_write(plain.cap, 0, text)
    stream(number, plain.at, #text, extra)
  end

  -- Numbers: the catalogue, the page tree, the information; five for each
  -- face - the font, its glyphs' font, its descriptor, its program, its
  -- ToUnicode - one for each picture, two for each page, and one for each
  -- comment's note on a page.
  local CATALOG, PAGES, INFO = 1, 2, 3
  local first_font = 4
  local first_image = first_font + 5 * #fonts
  local first_page = first_image + #images
  local first_note = first_page + 2 * #set.pages
  local count = first_note + #annots - 1

  put("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")

  object(CATALOG, ("<< /Type /Catalog /Pages %d 0 R >>"):format(PAGES))

  local kids = {}

  for i = 1, #set.pages do
    kids[i] = ("%d 0 R"):format(first_page + 2 * (i - 1))
  end

  object(PAGES, ("<< /Type /Pages /Kids [%s] /Count %d >>"):format(
    table.concat(kids, " "), #set.pages))

  object(INFO, ("<< /Title %s /Creator %s /Producer %s >>"):format(
    text_string(info.title or "Untitled"), text_string("Kosmos Write"),
    text_string("Kosmos " .. tostring((sys.build() or {}).version or ""))))

  local resources = {}

  for _, f in ipairs(fonts) do
    local n = first_font + 5 * (f.n - 1)

    resources[#resources + 1] = ("/F%d %d 0 R"):format(f.n, n)

    object(n, ("<< /Type /Font /Subtype /Type0 /BaseFont /%s "
      .. "/Encoding /Identity-H /DescendantFonts [%d 0 R] /ToUnicode %d 0 R >>")
      :format(f.name, n + 1, n + 4))

    object(n + 1, ("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s "
      .. "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
      .. "/Supplement 0 >> /FontDescriptor %d 0 R /DW 1000 /W [%s] "
      .. "/CIDToGIDMap /Identity >>"):format(f.name, n + 2, f.widths))

    object(n + 2, ("<< /Type /FontDescriptor /FontName /%s /Flags %d "
      .. "/FontBBox [%s] /ItalicAngle %s /Ascent %s /Descent %s /CapHeight %s "
      .. "/StemV 80 /FontFile2 %d 0 R >>"):format(f.name, f.flags, f.bbox,
      f.italic_angle, f.ascent, f.descent, f.cap_height, n + 3))

    local size = f.entry.face:subset(f.glyphs, program.at, program.size)
    stream(n + 3, program.at, size, (" /Length1 %d"):format(size))

    text_stream(n + 4, f.cmap)
  end

  -- The pictures, each an image object as it was made, after the faces.
  local xobjects = {}

  for _, im in ipairs(images) do
    local n = first_image + im.n - 1

    xobjects[#xobjects + 1] = ("/Im%d %d 0 R"):format(im.n, n)
    offsets[n] = at
    put(("%d 0 obj\n<< %s /Length %d >>\nstream\n"):format(n, im.dict, #im.data))
    put(im.data)
    put("\nendstream\nendobj\n")
  end

  local font_resources = "<< /Font << " .. table.concat(resources, " ") .. " >>"
                         .. (#xobjects > 0 and (" /XObject << " .. table.concat(xobjects, " ")
                                                .. " >>") or "")
                         .. " >>"

  for i, page in ipairs(set.pages) do
    local n = first_page + 2 * (i - 1)

    local refs = {}
    for _, k in ipairs(on_page[i] or {}) do refs[#refs + 1] = ("%d 0 R"):format(first_note + k - 1) end

    object(n, ("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s] "
      .. "/Resources %s /Contents %d 0 R%s >>"):format(PAGES, num(page.width_pt),
      num(page.height_pt), font_resources, n + 1,
      #refs > 0 and (" /Annots [" .. table.concat(refs, " ") .. "]") or ""))

    text_stream(n + 1, contents[i])
    contents[i] = nil
  end

  for k, a in ipairs(annots) do object(first_note + k - 1, a) end

  -- The cross-reference table: twenty bytes an object, the offset of each.
  local xref = at
  local rows = { ("xref\n0 %d\n0000000000 65535 f \n"):format(count + 1) }

  for n = 1, count do
    rows[#rows + 1] = ("%010d 00000 n \n"):format(offsets[n])
  end

  put(table.concat(rows))
  put(("trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n")
      :format(count + 1, CATALOG, INFO, xref))

  local ok
  ok, why = regions.write_file(path, out, at)

  free(out, plain, squeezed, program)

  if not ok then return nil, path .. ": " .. tostring(why) end

  notes.bytes = at

  return true, notes
end

return pdfwrite
