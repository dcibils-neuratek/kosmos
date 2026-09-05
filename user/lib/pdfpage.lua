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

  if done ~= length then
    sys.release(raw)
    return nil
  end

  if filter[1] ~= "FlateDecode" then
    font.program = { at = raw_at, size = length,
                     cap = pages_for(length) * 4096 }
    return font.program
  end

  -- Room over the top, because a subset font has no `cmap` and a minimal
  -- one is synthesised on the end of it; see `ensure_cmap` in docfont.c.
  local sized, want = pcall(compress.inflated_size, raw_at, length)

  if not sized then
    sys.release(raw)
    return nil
  end

  want = want + 32

  local out = sys.memory(pages_for(want))

  if not out then
    sys.release(raw)
    return nil
  end

  local out_at = sys.memory_map(out)
  local cap    = pages_for(want) * 4096

  font.program = { at = out_at,
                   size = compress.inflate_into(raw_at, length, out_at, cap),
                   cap = cap }

  --
  -- The compressed copy, given back.
  --
  -- `raw` is scratch: the stream lands in it, is inflated out of it, and is
  -- never wanted again - only `out` is kept. It was never released, so every
  -- font that loaded cost a capability for the life of the process, and a
  -- thread gets thirty-two.
  --
  -- Not on the path above this, where the stream was not compressed and
  -- `raw_at` *is* the program.
  --
  sys.release(raw)

  return font.program
end

--
-- Everything about one font that drawing or reading a run needs.
--
-- **Cached on the document, and that is the whole of a real bug.**
--
-- `font.program` is a region and `font.handles` are rasterisers, and the
-- comments on both say "once per face, shared by every size". That was true
-- of the *table they live on* and false of the document: `pdfpage.render`
-- built a fresh font table for every page, so every page inflated every font
-- program into a new region and made a new rasteriser for every face, and
-- nothing freed the previous page's.
--
-- Measured on a 254-page book, paging forward: page 13 rendered no glyphs
-- and lost two faces, page 23 lost three, and page 33 could not allocate the
-- paper at all. Read a page, then it stops.
--
-- `Doc:get` caches by object number, so the same indirect reference resolves
-- to the same table every time and identity is a sound key. A font
-- dictionary written inline in a page's resources - legal, and rare - gets a
-- fresh table per page and misses the cache, which is exactly what every
-- font did before this and no worse.
--
function pdfpage.font(doc, ref)
  --
  -- Keyed by the object *number*, and that is the whole of it.
  --
  -- The first attempt keyed on the resolved table and never hit once,
  -- because `pdfpage.render` ends with `doc:forget()` - which is
  -- `self.cache = {}`, on purpose, to bound what a 254-page document keeps.
  -- So every page re-parses the font dictionary into a brand new table, and
  -- a cache keyed on that table misses every time *and* grows by an entry a
  -- page. It looked like a fix and measured like nothing.
  --
  -- An object number survives `forget` because it is what the file says.
  --
  local key = pdf.isref(ref) and ref.num or nil
  local dict = doc:resolve(ref)

  if type(dict) ~= "table" then
    return { two_byte = false,
             widths = { single = {}, ranges = {} }, default_width = 500 }
  end

  doc.fonts = doc.fonts or {}

  if key and doc.fonts[key] then return doc.fonts[key] end

  local font = { subtype = dict.Subtype, base = dict.BaseFont, two_byte = false }

  if key then doc.fonts[key] = font end

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

    if not program then
      -- No program to rasterise, and there never will be: this font is not
      -- embedded, or its stream would not inflate. Remembered, because
      -- asking again on every page is asking the same question.
      font.handles[px] = false
    else
      local ok, handle = pcall(gfx.docfont, program.at, program.size,
                               program.cap, px)

      if ok and handle then
        font.handles[px] = handle
      end

      --
      -- A refused *allocation* is not remembered, and the difference
      -- matters more than it looks.
      --
      -- This used to write `false` here as well, so the two failures were
      -- one: a face that could not be rasterised because the machine was
      -- momentarily out of regions was written off for the life of the
      -- document. What that looks like is a reader that renders eight pages
      -- and then shows blank paper for ever, on a book it was reading a
      -- moment ago - and going back does not help, because the answer was
      -- cached rather than the cause fixed.
      --
      -- Left as nil it is asked again on the next page, which costs one
      -- allocation attempt and can succeed. A document whose fonts really
      -- are too big for this machine pays a failed call per page, which is
      -- the cheaper of the two mistakes by a long way.
      --
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
-- That reasoning was about contiguity, and contiguity stopped being the
-- constraint when a region became a list of pages rather than a run. The
-- sizes stay because they are the right sizes - a content stream is a few
-- kilobytes and the largest in this book inflates to about 45 KB - not
-- because a bigger one could not be allocated.
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

function pdfpage.render(doc, page, surface, scale, colour)
  local resources = doc:resolve(page.Resources) or {}
  local dicts     = doc:resolve(resources.Font) or {}
  local fonts     = {}

  for name, dict in pairs(dicts) do
    -- The *reference*, not the resolved dictionary: `pdfpage.font` keys its
    -- cache on the object number, which is the only thing about a font that
    -- survives `doc:forget()`.
    fonts[name] = pdfpage.font(doc, dict)
  end

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

  local drawn, faces, missing = 0, 0, 0

  for _, bucket in ipairs(order) do
    local handle = pdfpage.rasteriser(doc, bucket.font, bucket.px)

    if handle then
      drawn = drawn + (handle:draw(surface, colour, bucket.runs) or 0)
      faces = faces + 1
    else
      missing = missing + 1
    end
  end

  --
  -- Keep only the rasterisers this page used, and let the rest go.
  --
  -- **The font *program* is cached for the document's life and the
  -- rasterisers are not, and the difference is what each one costs when it
  -- is wrong.**
  --
  -- A program is a region, and `sys.memory` hands back a capability index
  -- out of the thirty-two a thread gets. Allocating one per face *per page*
  -- ran the process out of capability slots: on a 254-page book, page 13
  -- lost two faces and page 23 lost three. So programs are made once per
  -- face and reused, which is bounded by how many faces a document has.
  --
  -- A rasteriser is a `docfont` - a userdata with a `__gc`, holding a glyph
  -- cache on the Lua heap - and caching *those* for the document's life was
  -- a fix that made things worse. Keyed by face and pixel size, a book
  -- accumulates a new one every time the type changes size, and the heap
  -- they crowd is the heap the page surface comes out of: with them pinned,
  -- page 23 stopped being "two faces missing" and became "not enough
  -- memory". Measured, both ways round.
  --
  -- So this prunes to what the page actually drew. Consecutive pages in the
  -- same face at the same size - which is most of a book - keep their glyph
  -- caches and pay nothing; a page that changes face drops the old one and
  -- the collector takes it. Reuse the space, do not keep adding to it.
  --
  -- `false` entries stay: those are faces that will never load, remembered
  -- so they are not retried once a page, and they hold nothing.
  --
  local used = {}

  for _, bucket in ipairs(order) do
    local per = used[bucket.font]

    if not per then per = {} ; used[bucket.font] = per end

    per[bucket.px] = true
  end

  for _, font in pairs(fonts) do
    if font.handles then
      local per = used[font]

      for px, handle in pairs(font.handles) do
        if handle ~= false and not (per and per[px]) then
          font.handles[px] = nil
        end
      end
    end
  end

  doc:forget()

  --
  -- `missing` is the third return and it exists because of a bug report
  -- nobody could act on: a page that drew nothing said "0 glyphs" and that
  -- was the whole of what it said. Zero glyphs has two causes that look
  -- identical from outside - a page with no text on it, and a page whose
  -- every font failed to load - and telling them apart needed the reader to
  -- say which. A silent failure that reports a number is worse than one
  -- that reports nothing, because the number looks like an answer.
  --
  return drawn, faces, missing
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

return pdfpage
