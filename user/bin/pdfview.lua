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
local panel    = use("/lib/panel.lua")
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

--
-- The bar's buttons, declared before anything draws them. Their actions are
-- filled in further down, once the functions they call exist.
--
local BUTTONS = {
  { x =   6, w = 54, text = "Open" },
  { x =  66, w = 54, text = "Prev" },
  { x = 126, w = 54, text = "Next" },
}

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

  --
  -- The bar: three buttons this file draws itself, then whatever it has to
  -- say in the space left over. A direct window owns every pixel, so a
  -- widget would have nothing to draw into - the buttons are rectangles
  -- with known positions and a click is a comparison against them.
  --
  s:fill(0, view_h, W, BAR, CHROME)

  local right = 6

  for _, b in ipairs(BUTTONS) do
    --
    -- Drawn with fills, because a *surface* has `fill`, `blit` and `text`
    -- and nothing else. `frame` is a method of the drawing context the
    -- widget kit hands out, and calling it here raised - which, in a
    -- direct window, means nothing is written and the compositor shows an
    -- undrawn buffer. A black window and no error at all.
    --
    local bx, by, bw, bh = b.x, view_h + 3, b.w, BAR - 6

    s:fill(bx, by, bw, bh, 0xff2c3440)
    s:fill(bx, by, bw, 1, 0xff46525f)
    s:fill(bx, by + bh - 1, bw, 1, 0xff46525f)
    s:fill(bx, by, 1, bh, 0xff46525f)
    s:fill(bx + bw - 1, by, 1, bh, 0xff46525f)
    s:text(bx + 8, view_h + 4, b.text, LABEL)
    right = b.x + b.w
  end

  s:text(right + 12, view_h + 4, said, LABEL)

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
  local ok, drawn, _, missing = pcall(pdfpage.render, doc, page, paper,
                                      scale, INK)
  render_ms = (sys.ticks() - started) // 62500

  if not ok then
    said = ("page %d: %s"):format(n, tostring(drawn))
    glyph_count = 0
  else
    glyph_count = drawn or 0
    said = ("page %d of %d - %d glyphs in %d ms - %.0f%%")
           :format(n, #doc.pages, glyph_count, render_ms, zoom * 100)

    -- Said out loud rather than left to be inferred from a blank page.
    if (missing or 0) > 0 then
      said = said .. (" - %d face%s would not load"):format(
               missing, missing == 1 and "" or "s")
    end
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

--------------------------------------------------------------------------
-- Events.
--
-- A direct window draws its own pixels, so `window:paint` has nothing to
-- send and returns early - but the event loop is still the kit's, and it
-- routes keys and clicks through the view tree. So there is one view here,
-- the size of the window, and it never draws: it exists to be the thing
-- events arrive at. `dispatch` applies focus itself, which is why that
-- still works with no painting.
--
-- The first version of this called `win:poll()`, which does not exist. The
-- window rendered its first frame, the loop raised on the first pass, and
-- the process died - leaving the compositor showing the last frame it had
-- been given. It looked exactly like a working viewer that would not
-- scroll.
--------------------------------------------------------------------------

local sink = ui.view{ x = 0, y = 0, w = W, h = H }
sink.focusable = true

local function chooser()
  local start_at = path and path:match("^(.*)/") or "/home"
  local picked = panel.open{
    start = start_at,
    on_choose = function (chosen) path = chosen ; open(chosen) end,
  }

  if picked then picked:run() end

  frame()
end

function sink:key(c)
  local last = math.max(0, paper_h - (H - BAR))
  local page_step = H - BAR - 24

  if     c == -2 then top = math.min(top + 40, last)
  elseif c == -1 then top = math.max(0, top - 40)
  elseif c == -3 then top = math.min(top + page_step, last)
  elseif c == -4 then top = math.max(0, top - page_step)
  elseif c == 46 then show(current + 1)          -- '.'
  elseif c == 44 then show(current - 1)          -- ','
  elseif c == 43 or c == 61 then
    zoom = math.min(zoom * 1.25, 4.0) ; show(current)
  elseif c == 45 then
    zoom = math.max(zoom / 1.25, 0.5) ; show(current)
  elseif c == 111 then chooser() ; return true   -- 'o'
  else return false end

  frame()
  return true
end

--
-- The chrome is drawn rather than built from widgets, because a direct
-- window owns every pixel and a widget would have nothing to draw into. So
-- the buttons are rectangles this file knows the position of, and a click
-- is a comparison.
--
BUTTONS[1].on = function () chooser() end
BUTTONS[2].on = function () show(current - 1) end
BUTTONS[3].on = function () show(current + 1) end

function sink:mouse(action, x, y)
  if action ~= "press" then return true end

  if y >= H - BAR then
    for _, b in ipairs(BUTTONS) do
      if x >= b.x and x < b.x + b.w then
        b.on()
        frame()
        return true
      end
    end
  end

  return true
end

win:add(sink)

if path then open(path) end

frame()
win:run()
