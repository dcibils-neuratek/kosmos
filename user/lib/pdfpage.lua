-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A page's content stream, turned into positioned text.
--
-- `pdf.lua` finds the objects; this walks the little stack machine inside
-- one of them and says what ends up where. It draws nothing: what comes out
-- is a list of runs, each with a position, a size and the glyphs, and a
-- caller turns those into pixels or into text depending on what it is for.
--
-- **Why the interpreter is Lua.** It is a loop, but it is a loop over
-- *decisions* - push an operand, match an operator, multiply two matrices -
-- and `design.md` 6 puts those here. The loops over *bytes* it depends on
-- are already C and were already written: `sys.inflate` for the stream and
-- the rasteriser for the glyphs. If a page turns out to be slow the honest
-- next step is to measure which of the three it is in, not to assume.
--
-- **What a PDF calls text is not characters.** With `Identity-H`, which is
-- what this document and most modern producers use, a string in the content
-- stream is a sequence of two-byte *glyph numbers* in the embedded font.
-- Drawing them needs no more than that. Knowing what they *say* needs the
-- font's `/ToUnicode` table, which is a separate object and is parsed here
-- too - so `runs` is for drawing and `text` is for reading, and neither is
-- built on a guess about the other.

local pdfpage = {}

local pdf      = use("/lib/pdf.lua")
local compress = use("/kits/compress")
local pdfkit   = use("/kits/pdf")

--------------------------------------------------------------------------
-- Matrices.
--
-- PDF's are 3x3 with a fixed last column, so six numbers carry them and
-- multiplication is written out rather than looped. `a b c d e f` is the
-- order the operators use, which is the order to keep them in.
--------------------------------------------------------------------------

local function identity()
  return { a = 1, b = 0, c = 0, d = 1, e = 0, f = 0 }
end

-- Never mutated; only ever copied out of.
local IDENTITY = identity()

--
-- How many tokens one crossing into the scanner brings back.
--
-- `gfx.md` 19.11 puts a Lua/C crossing at about two thousand pixels of
-- work, so a call per token would trade a slow loop for a slow boundary.
-- Batching answers that, and the number is bounded from the other side:
-- each batch is two Lua tables of this many entries plus the strings in
-- them, live at once, on a heap with a hard ceiling.
--
-- Measured in a window, which is the tight case - a windowed process starts
-- around 640 KB because the UI kit is loaded, against 330 KB for a console
-- one. Before the page list was slimmed, 512 worked and 1024 did not. After
-- it, 2048 works. 256 is chosen well inside that: fifteen crossings instead
-- of four on a page of this document, which is microseconds, in exchange
-- for a margin that does not depend on what else the program is doing.
--
local BATCH = 256

local function multiply(m, n)
  return {
    a = m.a * n.a + m.b * n.c,
    b = m.a * n.b + m.b * n.d,
    c = m.c * n.a + m.d * n.c,
    d = m.c * n.b + m.d * n.d,
    e = m.e * n.a + m.f * n.c + n.e,
    f = m.e * n.b + m.f * n.d + n.f,
  }
end

--
-- The same, into a table that already exists.
--
-- A page of this document is 663 shows and each one multiplied matrices
-- three times, every product a fresh six-field table - about three thousand
-- of them per page, plus one per operator for the operand stack. None of it
-- lives longer than the statement that made it, and all of it has to be
-- collected: against a heap with a hard ceiling at 2 MB the collector could
-- not keep pace, and a page that worked in a console program ran out of
-- memory in a window, where the UI kit had already taken its share.
--
-- Every product is computed into locals before anything is stored, so `out`
-- may be `m` or `n` and a matrix can be multiplied into itself.
--
local function multiply_into(m, n, out)
  local a = m.a * n.a + m.b * n.c
  local b = m.a * n.b + m.b * n.d
  local c = m.c * n.a + m.d * n.c
  local d = m.c * n.b + m.d * n.d
  local e = m.e * n.a + m.f * n.c + n.e
  local f = m.e * n.b + m.f * n.d + n.f

  out.a, out.b, out.c, out.d, out.e, out.f = a, b, c, d, e, f
  return out
end

local function copy_into(from, to)
  to.a, to.b, to.c, to.d, to.e, to.f =
    from.a, from.b, from.c, from.d, from.e, from.f
  return to
end

--------------------------------------------------------------------------
-- /ToUnicode, in the two forms that actually appear.
--
-- The CMap is PostScript and a general reader of it would be an
-- interpreter for a second language. It does not have to be: what matters
-- is between `beginbfchar` and `endbfchar`, where a code maps to a string,
-- and between `beginbfrange` and `endbfrange`, where a run of codes maps to
-- a run of characters. Everything else in the object is boilerplate that
-- says which conventions are in force.
--------------------------------------------------------------------------

local function utf8_of(code)
  -- A surrogate pair is two UTF-16 values and appears for anything past the
  -- basic plane. Rare in a book; wrong-looking rather than fatal if unhandled.
  if code >= 0xD800 and code <= 0xDFFF then return "?" end
  return utf8.char(code)
end

local function hex_string(text)
  -- <0041> and <00410042> both appear: the second is a ligature or an
  -- accented form written as more than one character.
  local out = {}
  for pair in text:gmatch("%x%x%x%x") do
    out[#out + 1] = utf8_of(tonumber(pair, 16))
  end
  return table.concat(out)
end

function pdfpage.tounicode(bytes)
  -- Kept as the two shapes the CMap states them in, rather than expanded
  -- into one entry per code. A `bfrange` may legally span thousands of
  -- codes, and writing each one into a Lua table turned a 500-byte object
  -- into megabytes - which on a 2 MB heap is the difference between a page
  -- and `not enough memory`. Found exactly that way.
  local map = { single = {}, ranges = {} }

  for body in bytes:gmatch("beginbfchar(.-)endbfchar") do
    for code, value in body:gmatch("<(%x+)>%s*<(%x+)>") do
      map.single[tonumber(code, 16)] = hex_string(value)
    end
  end

  for body in bytes:gmatch("beginbfrange(.-)endbfrange") do
    for first, last, value in body:gmatch("<(%x+)>%s*<(%x+)>%s*<(%x+)>") do
      map.ranges[#map.ranges + 1] = {
        from = tonumber(first, 16),
        to   = tonumber(last, 16),
        base = tonumber(value:sub(1, 4), 16),
      }
    end
  end

  return map
end

-- What one code says. A handful of ranges is a short scan and beats the
-- table that holding them expanded would cost.
function pdfpage.char(font, code)
  local map = font.unicode
  if not map then return "" end

  local hit = map.single[code]
  if hit then return hit end

  for _, range in ipairs(map.ranges) do
    if code >= range.from and code <= range.to then
      return utf8_of(range.base + (code - range.from))
    end
  end

  return ""
end

--------------------------------------------------------------------------
-- Widths, so that more than one glyph in a string lands in the right place.
--
-- A CID font's widths are `/W`, in two shapes at once: `c [w w w]` gives a
-- width per code from `c`, and `c1 c2 w` gives one width for everything
-- between. `/DW` is the default for anything named by neither, and is 1000
-- when it is absent - a thousandth of the font size being PDF's unit here.
--------------------------------------------------------------------------

local function widths_of(doc, descendant)
  local widths = { single = {}, ranges = {} }
  local default = doc:resolve(descendant.DW) or 1000
  local w = doc:resolve(descendant.W)

  if type(w) ~= "table" then return widths, default end

  local i = 1
  while i <= #w do
    local first = doc:resolve(w[i])
    local next_ = doc:resolve(w[i + 1])

    if type(next_) == "table" then
      for k, value in ipairs(next_) do
        widths.single[first + k - 1] = doc:resolve(value)
      end
      i = i + 2
    elseif next_ ~= nil then
      local width = doc:resolve(w[i + 2])
      if type(first) == "number" and type(next_) == "number"
         and type(width) == "number" then
        -- A range, held as one. Expanding it is what ran the heap out.
        widths.ranges[#widths.ranges + 1] =
          { from = first, to = next_, width = width }
      end
      i = i + 3
    else
      break
    end
  end

  return widths, default
end

-- How far the pen moves for one code, in thousandths of the font size.
function pdfpage.width(font, code)
  local hit = font.widths.single[code]
  if hit then return hit end

  for _, range in ipairs(font.widths.ranges) do
    if code >= range.from and code <= range.to then return range.width end
  end

  return font.default_width
end

--
-- The font program itself, inflated into a region and handed to the
-- rasteriser.
--
-- Not a Lua string: `stb_truetype` reads from the buffer for the life of
-- the font rather than taking a copy, and a font program is a couple of
-- hundred kilobytes against a 2 MB heap. So it goes into pages this process
-- owns, exactly as a content stream does, and what Lua holds is an address.
--
-- One region per font, kept for as long as the document is open. Freeing it
-- while a `docfont` still points into it is a use-after-free, which is why
-- nothing here frees it and why closing a document is the only thing that
-- should.
--
--
-- The font program, loaded once per face and shared by every size.
--
-- Two mistakes are baked out of this, both found the same evening.
--
-- The first version allocated per face *per size* and never released, so
-- the second page ran the machine out of memory. A font program does not
-- depend on the size it is drawn at - only the rasterised glyphs do - so it
-- is inflated once and every `docfont` over it points at the same bytes.
--
-- The second asked for a fixed 512 KB, twice, for a font of 28 KB. A region
-- is *contiguous* physical pages, so a large round number is a much
-- stronger request than the same number of free pages, and the second face
-- on the page simply could not get one. The sizes come from the stream now:
-- the compressed length is known from the object, and `inflated_size` asks
-- the decompressor how big the answer will be without producing it.
--
local function pages_for(bytes)
  return (bytes + 4095) // 4096
end

local function program_of(doc, font)
  if font.program ~= nil then
    return font.program or nil
  end

  font.program = false          -- so a failure is not retried every page

  local offset, length, filter = doc:stream_range(font.file)

  if not offset then return nil end

  local raw = sys.memory(pages_for(length))

  if not raw then
    return nil
  end

  local raw_at = sys.memory_map(raw)
  local done = 0

  while done < length do
    local got = doc.source.read_into(raw, offset + done, length - done)
    if not got or got == 0 then break end
    done = done + got
  end

  if done ~= length then return nil end

  if filter[1] ~= "FlateDecode" then
    font.program = { at = raw_at, size = length,
                     cap = pages_for(length) * 4096 }
    return font.program
  end

  -- Room over the top, because a subset font has no `cmap` and a minimal
  -- one is synthesised on the end of it; see `ensure_cmap` in docfont.c.
  local sized, want = pcall(compress.inflated_size, raw_at, length)

  if not sized then return nil end

  want = want + 32

  local out = sys.memory(pages_for(want))

  if not out then return nil end

  local out_at = sys.memory_map(out)
  local cap    = pages_for(want) * 4096

  font.program = { at = out_at,
                   size = compress.inflate_into(raw_at, length, out_at, cap),
                   cap = cap }

  return font.program
end

-- Everything about one font that drawing or reading a run needs.
function pdfpage.font(doc, dict)
  local font = { subtype = dict.Subtype, base = dict.BaseFont, two_byte = false }

  local descendants = doc:resolve(dict.DescendantFonts)
  local descendant  = descendants and doc:resolve(descendants[1])

  if descendant then
    -- Identity-H is two bytes a glyph, and is what a CID font here uses.
    font.two_byte = doc:resolve(dict.Encoding) == "Identity-H"
    font.widths, font.default_width = widths_of(doc, descendant)

    local descriptor = doc:resolve(descendant.FontDescriptor)
    font.file = descriptor and descriptor.FontFile2
  else
    font.widths, font.default_width = { single = {}, ranges = {} }, 500
  end

  local unicode = doc:resolve(dict.ToUnicode)
  local offset, length = doc:stream_range(unicode)

  if offset then
    local ok, bytes = pcall(doc.source.read, offset, length)
    if ok and bytes then
      local plain = compress.inflate(bytes)
      font.unicode = pdfpage.tounicode(plain)
    end
  end

  font.unicode = font.unicode or { single = {}, ranges = {} }
  return font
end

--
-- The same font, with something that can draw it.
--
-- Separate from `pdfpage.font` because reading a page and drawing one want
-- different things: extracting text needs the `/ToUnicode` table and no
-- rasteriser at all, and rendering needs the opposite. A reader that paid
-- for a font program it never draws would be paying half a megabyte per
-- face to produce a string.
--
-- One rasteriser per face *per size*: a glyph cache is keyed by glyph, and
-- the same glyph at ten and at twenty pixels is two different bitmaps.
function pdfpage.rasteriser(doc, font, px)
  if font.file == nil then return nil end

  font.handles = font.handles or {}

  if font.handles[px] == nil then
    local program = program_of(doc, font)

    if program then
      local ok, handle = pcall(gfx.docfont, program.at, program.size,
                               program.cap, px)

      font.handles[px] = ok and handle or false
    else
      font.handles[px] = false
    end
  end

  return font.handles[px] or nil
end

-- Two regions, made once and reused for every page: the compressed stream
-- lands in the first and is inflated into the second, and the scanner reads
-- the second where it lies. Nothing about the page reaches the Lua heap
-- except the text that comes out - which is the same rule `gfx` follows for
-- pixels, and for the same reason.
-- Sized against the document rather than guessed.
--
-- The largest compressed content stream in a 254-page book is 5,518 bytes
-- and the largest inflated one is about 45 KB, so 16 pages in and 32 out
-- carry any page of it with room over. The first version asked for 16 and
-- 64, which was 320 KB of contiguous pages for no reason - and a program
-- that also has a window and a UI kit could not get them. It failed as
-- `not enough memory` from inside `sys.memory`, three layers from anything
-- that looked like a cause, and worked perfectly in a console program that
-- had the machine to itself.
--
-- `pmm_alloc_contiguous` is the constraint, not the total: a run of 64 free
-- pages is a stronger thing to ask for than 64 free pages.
local RAW_PAGES, OUT_PAGES = 16, 32

local buffers

local function regions()
  if not buffers then
    local raw, why1 = sys.memory(RAW_PAGES)
    local out, why2 = sys.memory(OUT_PAGES)

    if not raw or not out then
      -- Named, because "could not allocate" is the same sentence whether
      -- the machine is out of memory or this process is out of capability
      -- slots, and those are entirely different problems. Telling them
      -- apart took an evening once.
      local i = sys.info() or {}

      print(("pdfpage: page buffers: %s / %s (regions %s/%s, pages free %s)")
            :format(tostring(why1), tostring(why2),
                    tostring(i.regions_used), tostring(i.regions_total),
                    tostring(i.pages_free)))
      return nil
    end

    buffers = {
      raw = raw, out = out,
      raw_at = sys.memory_map(raw), out_at = sys.memory_map(out),
      raw_max = RAW_PAGES * 4096, out_max = OUT_PAGES * 4096,
    }
  end

  return buffers
end

--------------------------------------------------------------------------
-- A page, as pixels.
--------------------------------------------------------------------------

--
-- The fonts a page uses, loaded before anything large is allocated.
--
-- A region is *contiguous* physical pages, and a font program wants a
-- handful of them. A rendered page wants seven hundred. Asking for the big
-- one first leaves the address space with no run long enough for the small
-- ones, and the second face on the page fails to load with plenty of free
-- memory - which reads as a font problem and is an ordering problem.
--
-- So the caller does this, then allocates its surface, then renders.
-- Returns the fonts so the render need not build them twice.
--
function pdfpage.prepare(doc, page)
  local resources = doc:resolve(page.Resources) or {}
  local dicts     = doc:resolve(resources.Font) or {}
  local fonts     = {}

  for name, dict in pairs(dicts) do
    local font = pdfpage.font(doc, doc:resolve(dict))

    fonts[name] = font
    program_of(doc, font)
  end

  return fonts
end

--
-- Draws one page into `surface` at `scale`, and returns what it cost.
--
-- **Glyphs are accumulated and drawn per font, not per glyph.** A page is
-- around two thousand of them; a call across the Lua/C boundary for each
-- would cost more than the drawing, since `gfx.md` 19.11 puts a crossing at
-- about two thousand pixels of work. So the walk fills a flat array of
-- glyph, x, y for each face and size in use, and each of those becomes one
-- call - two or three per page rather than two thousand.
--
-- Flat arrays rather than tables of three, for the reason the interpreter
-- learned the hard way: a table per glyph is thousands of objects for the
-- collector to walk, on a heap that has 2 MB in it.
--
-- PDF's y axis points up from the bottom of the page and a surface's points
-- down from the top, so the flip happens here, once per glyph, in the same
-- arithmetic that applies the scale.
--
function pdfpage.render(doc, page, surface, scale, colour, fonts)
  fonts = fonts or pdfpage.prepare(doc, page)

  local box    = doc:resolve(page.MediaBox) or { 0, 0, 612, 792 }
  local height = box[4] or 792

  local offset, length, filter = doc:stream_range(page.dict.Contents)
  if not offset then return 0, 0 end

  local b = regions()
  if not b then error("pdfpage: no memory for the page buffers") end
  if length > b.raw_max then
    error(("pdfpage: a %d byte stream needs a bigger buffer"):format(length))
  end

  local done = 0

  while done < length do
    local got = doc.source.read_into(b.raw, offset + done, length - done)
    if not got or got == 0 then break end
    done = done + got
  end

  if done ~= length then
    error(("pdfpage: read %d bytes of %d"):format(done, length))
  end

  local size = length

  if filter[1] == "FlateDecode" then
    size = compress.inflate_into(b.raw_at, length, b.out_at, b.out_max)
  end

  local at = filter[1] == "FlateDecode" and b.out_at or b.raw_at

  -- One bucket per face and size actually used on this page.
  local buckets, order = {}, {}

  pdfpage.walk(at, size, fonts, function (x, y, px, font, bytes)
    local at_px = math.floor(px * scale + 0.5)

    if at_px < 4 then at_px = 4 end
    if at_px > 200 then at_px = 200 end

    local key = tostring(font) .. ":" .. at_px
    local bucket = buckets[key]

    if not bucket then
      bucket = { font = font, px = at_px, runs = {} }
      buckets[key] = bucket
      order[#order + 1] = bucket
    end

    local runs = bucket.runs
    local n    = #runs
    local step = font.two_byte and 2 or 1
    local pen  = x

    for i = 1, #bytes, step do
      local code = step == 2
                   and (bytes:byte(i) * 256 + (bytes:byte(i + 1) or 0))
                   or  bytes:byte(i)

      n = n + 1 ; runs[n] = code
      n = n + 1 ; runs[n] = math.floor(pen * scale + 0.5)
      n = n + 1 ; runs[n] = math.floor((height - y) * scale + 0.5)

      pen = pen + (pdfpage.width(font, code) / 1000) * px
    end
  end)

  local drawn, faces = 0, 0

  for _, bucket in ipairs(order) do
    local handle = pdfpage.rasteriser(doc, bucket.font, bucket.px)

    if handle then
      drawn = drawn + (handle:draw(surface, colour, bucket.runs) or 0)
      faces = faces + 1
    end
  end

  doc:forget()

  return drawn, faces
end

--------------------------------------------------------------------------
-- The stack machine.
--
-- Operands accumulate and an operator consumes them. Only the operators a
-- page of text actually uses are implemented, and an unknown one clears the
-- stack rather than stopping: a content stream is allowed to contain things
-- this does not draw, and refusing the page because of one would be the
-- wrong trade for a reader.
--------------------------------------------------------------------------

-- Walks the stream and calls `emit(x, y, size, font, bytes)` for every
-- string shown. Nothing is kept.
--
-- It returns runs to nobody on purpose. This producer emits a whole
-- BT/Tf/Tm/Tj/ET around *each glyph*, so a page of text is about two
-- thousand shows - and a Lua table per show, plus one per glyph inside it,
-- plus one per line part, was the 2 MB heap gone before the page finished.
-- A callback costs nothing and the caller keeps only what it actually
-- wants, which for reading is one string per line.
function pdfpage.walk(at, length, fonts, emit)
  local NUMBER      = pdfkit.NUMBER
  local NAME        = pdfkit.NAME
  local STRING      = pdfkit.STRING
  local OPERATOR    = pdfkit.OPERATOR
  local ARRAY_OPEN  = pdfkit.ARRAY_OPEN
  local ARRAY_CLOSE = pdfkit.ARRAY_CLOSE

  local stack, array = {}, nil

  local ctm, stored = identity(), {}
  local tm, tlm     = identity(), identity()

  -- Scratch, reused for the life of the walk. Nothing here outlives the
  -- statement that fills it.
  local text_m = identity()
  local trm    = identity()
  local step_m = identity()

  local font, size, leading = nil, 0, 0
  local char_space, word_space, horizontal = 0, 0, 1

  local function number(i)
    local v = stack[#stack + i]
    return type(v) == "number" and v or 0
  end

  -- Where the next glyph lands, and how far the pen then moves.
  local function show(text)
    if not font or #text == 0 then return end

    text_m.a, text_m.d = size * horizontal, size
    text_m.b, text_m.c, text_m.e, text_m.f = 0, 0, 0, 0

    multiply_into(tm, ctm, step_m)
    multiply_into(text_m, step_m, trm)

    emit(trm.e, trm.f,
         math.sqrt(trm.a * trm.a + trm.b * trm.b), font, text)

    -- The pen advances in text space, before the matrices are applied.
    -- Codes are decoded as they are stepped over rather than collected: a
    -- table per string is the allocation this whole function exists to
    -- avoid.
    local advance, step = 0, font.two_byte and 2 or 1

    for i = 1, #text, step do
      local code = step == 2
                   and (text:byte(i) * 256 + (text:byte(i + 1) or 0))
                   or  text:byte(i)

      advance = advance + (pdfpage.width(font, code) / 1000) * size
                + char_space

      if code == 32 and not font.two_byte then
        advance = advance + word_space
      end
    end

    step_m.a, step_m.d = 1, 1
    step_m.b, step_m.c, step_m.f = 0, 0, 0
    step_m.e = advance * horizontal

    multiply_into(step_m, tm, tm)
  end

  local function next_line(tx, ty)
    step_m.a, step_m.d = 1, 1
    step_m.b, step_m.c = 0, 0
    step_m.e, step_m.f = tx, ty

    multiply_into(step_m, tlm, tlm)
    copy_into(tlm, tm)
  end

  local offset = 0

  while true do
    local kinds, values, next_at = pdfkit.scan(at, length, offset, BATCH)

    if #kinds == 0 then break end
    offset = next_at

    for i = 1, #kinds do
      local kind, value = kinds[i], values[i]

      if kind == ARRAY_OPEN then
        array = {}

      elseif kind == ARRAY_CLOSE then
        stack[#stack + 1] = array or {}
        array = nil

      elseif array and (kind == NUMBER or kind == STRING) then
        array[#array + 1] = value

      elseif kind == NUMBER or kind == STRING or kind == NAME then
        stack[#stack + 1] = value

      elseif kind == OPERATOR then
        local op = value

        if op == "cm" then
          step_m.a, step_m.b, step_m.c = number(-5), number(-4), number(-3)
          step_m.d, step_m.e, step_m.f = number(-2), number(-1), number(0)

          multiply_into(step_m, ctm, ctm)

        -- `q` has to copy, now that `ctm` is written in place. There are a
        -- handful of these on a page against a matrix product per glyph, so
        -- this is the allocation worth keeping.
        elseif op == "q"  then stored[#stored + 1] = copy_into(ctm, {})
        elseif op == "Q"  then
          if #stored > 0 then
            copy_into(stored[#stored], ctm)
            stored[#stored] = nil
          end

        elseif op == "BT" then
          copy_into(IDENTITY, tm)
          copy_into(IDENTITY, tlm)
        elseif op == "ET" then font = nil

        elseif op == "Tf" then
          font = fonts[stack[#stack - 1]]
          size = number(0)

        elseif op == "Tm" then
          tlm.a, tlm.b, tlm.c = number(-5), number(-4), number(-3)
          tlm.d, tlm.e, tlm.f = number(-2), number(-1), number(0)
          copy_into(tlm, tm)

        elseif op == "Td"  then next_line(number(-1), number(0))
        elseif op == "TD"  then
          leading = -number(0)
          next_line(number(-1), number(0))
        elseif op == "T*"  then next_line(0, -leading)
        elseif op == "TL"  then leading = number(0)
        elseif op == "Tc"  then char_space = number(0)
        elseif op == "Tw"  then word_space = number(0)
        elseif op == "Tz"  then horizontal = number(0) / 100

        elseif op == "Tj"  then
          if type(stack[#stack]) == "string" then show(stack[#stack]) end

        elseif op == "'" then
          next_line(0, -leading)
          if type(stack[#stack]) == "string" then show(stack[#stack]) end

        elseif op == "TJ" then
          local parts = stack[#stack]
          if type(parts) == "table" then
            for _, part in ipairs(parts) do
              if type(part) == "string" then
                show(part)
              elseif type(part) == "number" then
                -- Kerning, in thousandths, and it moves the pen backwards.
                step_m.a, step_m.d = 1, 1
                step_m.b, step_m.c, step_m.f = 0, 0, 0
                step_m.e = -part / 1000 * size * horizontal

                multiply_into(step_m, tm, tm)
              end
            end
          end
        end

        stack = {}
      end
    end
  end
end

-- Every code in a shown string, as text.
function pdfpage.decode(font, bytes)
  local out, step = {}, font.two_byte and 2 or 1

  for i = 1, #bytes, step do
    local code = step == 2
                 and (bytes:byte(i) * 256 + (bytes:byte(i + 1) or 0))
                 or  bytes:byte(i)
    out[#out + 1] = pdfpage.char(font, code)
  end

  return table.concat(out)
end

--------------------------------------------------------------------------
-- One page, as text.
--------------------------------------------------------------------------

-- Runs come out in the order the producer emitted them, which is not
-- reading order and is not lines. Grouping by the y they landed on is what
-- turns a page back into something to read - and the tolerance matters:
-- exactly equal fails on a line whose baseline shifts by a rounding, and
-- too loose merges a line with the one under it.
--
-- Lines come out in the order the producer emitted them, which is not
-- reading order. Grouping by the y a show landed on is what turns a page
-- back into something to read, and the tolerance matters: exactly equal
-- fails on a baseline that shifts by a rounding, too loose merges a line
-- with the one beneath it.
--
-- Text is appended in emission order within a line rather than sorted by x.
-- That is an assumption about the producer and it is stated here so that a
-- page which comes out scrambled points straight at it.
--
local TOLERANCE = 2

function pdfpage.text(doc, page)
  local resources = doc:resolve(page.Resources) or {}
  local dicts     = doc:resolve(resources.Font) or {}
  local fonts     = {}

  for name, dict in pairs(dicts) do
    fonts[name] = pdfpage.font(doc, doc:resolve(dict))
  end

  local offset, length, filter = doc:stream_range(page.dict.Contents)
  if not offset then return {} end

  local b = regions()
  if not b then
    error("pdfpage: no memory for the page buffers")
  end

  if length > b.raw_max then
    error(("pdfpage: a %d byte stream needs a bigger buffer"):format(length))
  end

  -- Straight from the filesystem into the region: the bytes are never a
  -- Lua string on the way in. A source that cannot do that - the host's,
  -- which has a file and no regions - says so rather than being worked
  -- around, because the fallback would be the copy this exists to remove.
  if type(doc.source.read_into) ~= "function" then
    error("pdfpage: this source cannot read into a region")
  end

  local done = 0
  while done < length do
    local got = doc.source.read_into(b.raw, offset + done, length - done)
    if not got or got == 0 then break end
    done = done + got
  end

  if done ~= length then
    error(("pdfpage: read %d bytes of %d at %d"):format(done, length, offset))
  end

  local size = length

  if filter[1] == "FlateDecode" then
    size = compress.inflate_into(b.raw_at, length, b.out_at, b.out_max)
  end

  local at = filter[1] == "FlateDecode" and b.out_at or b.raw_at

  local lines = {}

  pdfpage.walk(at, size, fonts, function (_, y, _, font, bytes)
    local line

    for _, candidate in ipairs(lines) do
      if math.abs(candidate.y - y) <= TOLERANCE then line = candidate break end
    end

    if not line then
      line = { y = y, parts = {} }
      lines[#lines + 1] = line
    end

    line.parts[#line.parts + 1] = pdfpage.decode(font, bytes)
  end)

  -- Anything this page needed and the next one will not: the fonts, its
  -- own dictionary, the objects behind them. A reader that turns pages
  -- otherwise accumulates every page it has seen.
  doc:forget()

  -- The y axis points up in PDF space, so the larger y is the higher line.
  table.sort(lines, function (p, q) return p.y > q.y end)

  local out = {}
  for _, line in ipairs(lines) do
    out[#out + 1] = table.concat(line.parts)
  end

  return out
end

return pdfpage
