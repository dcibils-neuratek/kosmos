-- kosmos: application
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A PDF, as it was typeset.
--
--   wm pdfview
--   wm pdfview:/home/odyssey.pdf
--
--   arrows / PageUp / PageDown    scroll
--   , and .                       previous and next page
--   + and -                       zoom
--
-- **Built so that scrolling costs nothing.** The page is rendered once into
-- an offscreen surface as tall as the whole page, and a frame is one blit of
-- the visible band out of it. Nothing re-runs the content interpreter,
-- nothing re-rasterises a glyph; moving a line copies the pixels that moved
-- and stops. Rendering per frame would mean two thousand glyphs and a stack
-- machine over fourteen kilobytes of operators, sixty times a second - which
-- is not something to optimise, it is the wrong shape.
--
-- **A direct window** (`gfx.md` 19.4). The ordinary path sends drawing
-- commands and the compositor owns every pixel, which is what lets a hung
-- application keep its window - the right trade for a dialog and the wrong
-- one here, where the whole surface changes and describing it costs more
-- than copying it. So this process owns the pixels, draws into the buffer
-- the compositor is not showing, and commits with damage.
--
-- Two things underneath make the render itself quick, and both are C:
--
--   * a glyph is rasterised **once per face per size** and blitted from a
--     cache after that. A page of Times New Roman is about a hundred
--     distinct glyphs against two thousand drawn.
--   * a page is **two or three crossings**, not two thousand: the glyphs go
--     into a flat array per face and each becomes one call.
--
-- Still missing: images, so a cover page is blank. `DCTDecode` is JPEG and
-- `stb_image` is the same vendor as the rasteriser already here.

local ui       = use("/lib/ui.lua")
local pdf      = use("/lib/pdf.lua")
local pdfpage  = use("/lib/pdfpage.lua")

local W, H = 760, 620
local BAR  = 22                       -- the status line along the bottom

local path = args and args:match("^%s*(%S+)")

local win, err = ui.window{
  title = "PDF", w = W, h = H, x = 70, y = 40, direct = true,
}

if not win then
  print("pdfview: " .. tostring(err))
  return
end

if not win:surface() then
  print("pdfview: this window did not get a shared surface")
  return
end

local INK   = 0xff101418            -- the text
local PAPER = 0xfff4f1ea            -- the page
local CHROME= 0xff20262e
local LABEL = 0xffb8c2cc

local doc, current = nil, 1
local paper, paper_w, paper_h = nil, 0, 0
local top, zoom = 0, 1.0
local said = "no document"
local render_ms, glyph_count = 0, 0

--------------------------------------------------------------------------
-- The file, read a window at a time and never held.
--------------------------------------------------------------------------

local buffer, capacity

local function source_for(file, size)
  if not buffer then
    -- Four pages. `pdf.lua` reads in 256-byte windows; the only larger read
    -- is a content stream, which goes straight into `pdfpage`'s own region.
    buffer = sys.memory(4)
    capacity = 4 * 4096
  end

  if not buffer then return nil end

  return {
    size = size,

    read = function (offset, length)
      local out, done = {}, 0

      while done < length do
        local want = length - done
        if want > capacity then want = capacity end

        local got = fs.read_into(file, buffer, offset + done, want)
        if not got or got == 0 then break end

        out[#out + 1] = sys.region_read(buffer, 0, got)
        done = done + got
      end

      return table.concat(out)
    end,

    read_into = function (region, offset, length)
      return fs.read_into(file, region, offset, length)
    end,
  }
end

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

local function frame(damage_all)
  local s = win:surface()

  if not s then return end

  local view_h = H - BAR

  if paper then
    -- The whole of scrolling. `top` is a pixel offset into a surface that
    -- already holds the page.
    local band = math.min(view_h, paper_h - top)

    if band > 0 then
      s:fill(0, 0, W, view_h, PAPER)
      s:blit(paper, 0, top, math.min(paper_w, W), band, 0, 0)
    else
      s:fill(0, 0, W, view_h, PAPER)
    end
  else
    s:fill(0, 0, W, view_h, CHROME)
    s:text(12, 12, "Open a document:  wm pdfview:/home/odyssey.pdf", LABEL)
    s:text(12, 32, "arrows scroll, , and . turn pages, + and - zoom", LABEL)
  end

  s:fill(0, view_h, W, BAR, CHROME)
  s:text(6, view_h + 4, said, LABEL)

  win:commit(damage_all and nil
             or { x = 0, y = 0, w = W, h = H })
end

--------------------------------------------------------------------------
-- Pages.
--------------------------------------------------------------------------

local function show(n)
  if not doc then return end

  if n < 1 then n = 1 end
  if n > #doc.pages then n = #doc.pages end

  current = n

  local page = doc:page(n)
  local box  = doc:resolve(page.MediaBox) or { 0, 0, 612, 792 }
  local pw   = (box[3] or 612) - (box[1] or 0)
  local ph   = (box[4] or 792) - (box[2] or 0)

  -- Scaled to the window's width, so a page always fits across and zoom is
  -- a multiplier on top of that.
  local scale = (W / pw) * zoom

  local want_w = math.floor(pw * scale + 0.5)
  local want_h = math.floor(ph * scale + 0.5)

  if paper and (paper_w ~= want_w or paper_h ~= want_h) then
    paper:free()
    paper = nil
  end

  if not paper then
    local made = pcall(function ()
      paper = gfx.surface{ w = want_w, h = want_h }
    end)

    if not made or not paper then
      said = ("no memory for a %dx%d page"):format(want_w, want_h)
      return
    end

    paper_w, paper_h = want_w, want_h
  end

  paper:fill(0, 0, paper_w, paper_h, PAPER)

  local started = sys.ticks()
  local ok, drawn = pcall(pdfpage.render, doc, page, paper, scale, INK)
  render_ms = (sys.ticks() - started) // 62500

  if not ok then
    said = ("page %d: %s"):format(n, tostring(drawn))
    glyph_count = 0
  else
    glyph_count = drawn or 0
    said = ("page %d of %d - %d glyphs in %d ms - %.0f%%")
           :format(n, #doc.pages, glyph_count, render_ms, zoom * 100)
  end

  top = 0
end

local function open(file)
  local attrs, why = fs.getattr(file)

  if not attrs then
    said = ("cannot open %s: %s"):format(file, tostring(why))
    return
  end

  local source = source_for(file, attrs.size)

  if not source then
    said = "no memory for a read buffer"
    return
  end

  local ok, got = pcall(pdf.open, source)

  if not ok then
    said = tostring(got)
    return
  end

  doc = got

  -- The first page with a font on it: a cover is a single image and this
  -- cannot draw images yet, so opening on one looks like a failure.
  local first = 1

  for i = 1, #doc.pages do
    local resources = doc:resolve(doc:page(i).Resources) or {}
    if doc:resolve(resources.Font) then first = i break end
  end

  show(first)
end

--------------------------------------------------------------------------

if path then open(path) end

frame(true)

--
-- The loop.
--
-- `poll` blocks until something happens, so an idle viewer costs nothing -
-- and a scroll is a blit and a commit rather than a re-render, which is the
-- whole point of the arrangement above.
--
while win.running do
  local event = win:poll()

  if event == nil then break end

  if event.type == "key" then
    local c = event.code or event.key
    local step = 40
    local page_step = H - BAR - 24
    local last = math.max(0, paper_h - (H - BAR))
    local moved = false

    if     c == -2 then top = math.min(top + step, last)       ; moved = true
    elseif c == -1 then top = math.max(0, top - step)          ; moved = true
    elseif c == -3 then top = math.min(top + page_step, last)  ; moved = true
    elseif c == -4 then top = math.max(0, top - page_step)     ; moved = true
    elseif c == 46 then show(current + 1)                      ; moved = true
    elseif c == 44 then show(current - 1)                      ; moved = true
    elseif c == 43 or c == 61 then
      zoom = math.min(zoom * 1.25, 4.0) ; show(current) ; moved = true
    elseif c == 45 then
      zoom = math.max(zoom / 1.25, 0.5) ; show(current) ; moved = true
    end

    if moved then frame(false) end
  end
end
