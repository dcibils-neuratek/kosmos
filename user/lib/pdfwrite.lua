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
-- document uses is embedded whole as a TrueType program (`FontFile2`) under
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
-- What it does not do yet, said: subsetting (a face is embedded whole,
-- about a hundred kilobytes deflated), kerning, pictures (W5).

local pdfwrite = {}

local kit = use("/Kosmos/Kits/compress")

local PAGE = 4096

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

  for ch in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
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

--------------------------------------------------------------------------
-- Regions, as `zip.lua` keeps them.
--------------------------------------------------------------------------

local function region(bytes)
  local pages = math.max(1, (bytes + PAGE - 1) // PAGE)
  local cap = sys.memory(pages)

  if not cap then
    return nil, ("no memory for %d KB"):format(pages * PAGE // 1024)
  end

  local at = sys.memory_map(cap)

  if not at then
    sys.release(cap)
    return nil, "could not map a region"
  end

  return { cap = cap, at = at, size = pages * PAGE }
end

local function free(...)
  for _, r in ipairs({ ... }) do
    if r then sys.release(r.cap) end
  end
end

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
      out[#out + 1] = ("<%04X> <%s>"):format(glyphs[i], utf16(font.used[glyphs[i]]))
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
local function operators(set, page, font_for, notes)
  local out = {}
  local height = page.height_pt

  local function show(piece, look, baseline, extra)
    if piece.text == "" then return end

    local font = font_for(look)
    local face = font.entry.face
    local x, y = piece.x_pt, height - baseline
    local colour = rgb(look.colour)
    local shown

    if extra ~= 0 and piece.text:find(" ", 1, true) then
      local parts, start = {}, 1
      local step = num(-extra * 1000 / look.size_pt)
      local text = piece.text

      for i = 1, #text do
        if text:byte(i) == 32 then
          local hex, missing = face:glyphs(text:sub(start, i), font.used)
          notes.missing = notes.missing + missing
          parts[#parts + 1] = "<" .. hex .. "> " .. step
          start = i + 1
        end
      end

      if start <= #text then
        local hex, missing = face:glyphs(text:sub(start), font.used)
        notes.missing = notes.missing + missing
        parts[#parts + 1] = "<" .. hex .. ">"
      end

      shown = "[" .. table.concat(parts, " ") .. "] TJ"
    else
      local hex, missing = face:glyphs(piece.text, font.used)
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

  for _, line in ipairs(page.lines) do
    for _, piece in ipairs(line.pieces) do
      show(piece, set.looks[piece.look], line.baseline_pt, line.extra_space_pt)
    end
  end

  if page.footer then
    show(page.footer.piece, set.looks[page.footer.piece.look],
         page.footer.baseline_pt, 0)
  end

  return table.concat(out, "\n") .. "\n"
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

  -- Every page's operators first, so the fonts and their glyphs are known
  -- before the first object is written and the room needed before it is
  -- asked for.
  local contents, biggest_content = {}, 1

  for i, page in ipairs(set.pages) do
    contents[i] = operators(set, page, font_for, notes)
    biggest_content = math.max(biggest_content, #contents[i])
  end

  notes.fonts = #fonts

  local room, biggest = 64 * 1024, biggest_content

  for _, c in ipairs(contents) do room = room + #c + 512 end

  for _, f in ipairs(fonts) do
    local _, length = f.entry.face:program()

    f.program_length = length
    f.widths = widths_of(f)
    f.cmap = to_unicode(f)
    room = room + length + #f.widths + #f.cmap + 4096
    biggest = math.max(biggest, length, #f.cmap)
    biggest_content = math.max(biggest_content, #f.cmap)
  end

  local out, plain, squeezed, why

  out, why = region(room)
  if not out then return nil, why end

  plain, why = region(biggest_content)
  if not plain then free(out) return nil, why end

  squeezed, why = region(biggest + 1024)
  if not squeezed then free(out, plain) return nil, why end

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
  -- ToUnicode - and two for each page.
  local CATALOG, PAGES, INFO = 1, 2, 3
  local first_font = 4
  local first_page = first_font + 5 * #fonts
  local count = first_page + 2 * #set.pages - 1

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

    local src = f.entry.face:program()
    stream(n + 3, src, f.program_length,
           (" /Length1 %d"):format(f.program_length))

    text_stream(n + 4, f.cmap)
  end

  local font_resources = "<< /Font << " .. table.concat(resources, " ") .. " >> >>"

  for i, page in ipairs(set.pages) do
    local n = first_page + 2 * (i - 1)

    object(n, ("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s] "
      .. "/Resources %s /Contents %d 0 R >>"):format(PAGES, num(page.width_pt),
      num(page.height_pt), font_resources, n + 1))

    text_stream(n + 1, contents[i])
    contents[i] = nil
  end

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
  ok, why = fs.write_from(path, out.cap, at)

  if not ok then
    local written, oops = fs.write(path, sys.region_read(out.cap, 0, at))
    ok, why = written, oops or why
  end

  free(out, plain, squeezed)

  if not ok then return nil, path .. ": " .. tostring(why) end

  notes.bytes = at

  return true, notes
end

return pdfwrite
