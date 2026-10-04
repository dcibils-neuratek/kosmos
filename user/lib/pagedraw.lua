-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A set page drawn onto a surface (`docs/write.md`, W4).
--
--   local pagedraw = use("/Kosmos/Libraries/pagedraw.lua")
--   local drawer = pagedraw.new(measure)
--   drawer:page(set, page, surface, scale, x, y)
--
-- **The page the PDF prints, on the screen.** What `pageset` placed is
-- drawn where it placed it: each glyph at the pen the font's own advances
-- move it to - `face:place`, the same sums the PDF's `/W` carries - and
-- rasterised by the PDF reader's own rasteriser (`gfx.docfont`), from the
-- image's copy of the font, sized by the em. So the screen is not a second
-- setting of the text with a screen font's rounded widths: it is the PDF's
-- page, drawn, at `scale` pixels to the point.
--
-- **A kit**: Write's window is its first user and Present's slides its
-- next.
--
-- Glyphs are drawn a face, a size and a colour at a time - one crossing
-- into C for each, not one per glyph (`gfx.md` 19.11) - and a rasteriser
-- keeps its glyphs for as long as the drawer lives, so a page drawn again
-- at the same scale rasterises nothing. A face smaller than four pixels -
-- a page shown as a thumbnail - is drawn as a grey rule where its line is,
-- which is what text that small looks like anyway.

local pagedraw = {}

local Drawer = {}
Drawer.__index = Drawer

-- `#rrggbb` as the surface's 0xAARRGGBB.
local function argb(colour)
  local n = tonumber((colour or "#000000"):sub(2), 16) or 0
  return 0xff000000 | n
end

function pagedraw.new(measure)
  return setmetatable({ measure = measure, rasters = {}, descriptors = {} },
                      Drawer)
end

--
-- The rasteriser for `entry`'s face at `px`, made once: `gfx.docfont` over
-- the image's copy of the font, which it reads and never writes.
--
function Drawer:raster(entry, px)
  local key = entry.file .. "\0" .. px
  local r = self.rasters[key]

  if r == nil then
    local at, len = entry.face:program()
    local ok, made = pcall(gfx.docfont, at, len, len, px)

    r = ok and made or false
    self.rasters[key] = r
  end

  return r or nil
end

--
-- **`page` of `set`, drawn on `surface` at `scale` pixels to the point**,
-- its top left at `x`, `y`: the paper first, then `marks` - rectangles in
-- points with a colour, a selection - then the text over them. Returns how
-- many glyphs were drawn.
--
function Drawer:page(set, page, surface, scale, x, y, paper, marks)
  local w = math.floor(page.width_pt * scale + 0.5)
  local h = math.floor(page.height_pt * scale + 0.5)
  local buckets, order = {}, {}
  local drawn = 0

  surface:fill(x, y, w, h, paper or 0xffffffff)

  for _, m in ipairs(marks or {}) do
    local mx = math.floor(x + m.x_pt * scale)
    local my = math.floor(y + m.y_pt * scale)

    surface:fill(mx, my, math.max(1, math.floor(x + (m.x_pt + m.w_pt) * scale) - mx),
                 math.max(1, math.floor(y + (m.y_pt + m.h_pt) * scale) - my),
                 m.colour)
  end

  -- A left-hand page's lines stand over on facing pages (`pageset`); a
  -- header and a page number are centred on the page, and do not.
  local shift = 0

  local function piece(pc, baseline_pt, extra_pt)
    if pc.text == "" then return end

    local look = set.looks[pc.look]
    local entry = self.measure.face_of(look)
    local px = math.floor(look.size_pt * scale + 0.5)
    local colour = argb(look.colour)
    local left = x + (pc.x_pt + shift) * scale
    -- A drop cap stands lower than its line, by the lines beside it.
    local base = y + math.floor((baseline_pt + (pc.drop_pt or 0)) * scale + 0.5)

    if px < 4 then
      -- Too small to read: the line's grey, where its words are.
      local thick = math.max(1, px // 2)
      surface:fill(math.floor(left), base - thick, math.max(1,
                   math.floor(pc.width_pt * scale)), thick, 0xffb8bcc4)
      return
    end

    local raster = self:raster(entry, px)

    if not raster then return end

    local key = entry.file .. "\0" .. px .. "\0" .. colour
    local bucket = buckets[key]

    if not bucket then
      bucket = { raster = raster, colour = colour, runs = {} }
      buckets[key] = bucket
      order[#order + 1] = bucket
    end

    entry.face:place(pc.text, left, base, look.size_pt * scale / entry.units,
                     extra_pt * scale, bucket.runs, set.ligatures)

    -- Underline and strike-out at the font's own positions, as the PDF's.
    if look.underline or look.strike then
      local d = self.descriptors[entry.file]
      local per = look.size_pt * scale / entry.units

      if not d then
        d = entry.face:descriptor()
        self.descriptors[entry.file] = d
      end

      local function rule(at, thick)
        local t = math.max(1, math.floor(thick * per + 0.5))
        surface:fill(math.floor(left), base - math.floor(at * per + 0.5) - t // 2,
                     math.floor(pc.width_pt * scale + 0.5), t, colour)
      end

      if look.underline then
        rule(d.underline_position or -entry.units // 10,
             d.underline_thickness or entry.units // 20)
      end

      if look.strike then
        rule(d.strike_position or entry.units * 3 // 10,
             d.strike_size or entry.units // 20)
      end
    end
  end

  shift = page.shift_pt or 0

  for _, line in ipairs(page.lines) do
    -- A list's marker, before the line's text.
    if line.marker then piece(line.marker, line.baseline_pt, 0) end

    for _, pc in ipairs(line.pieces) do
      piece(pc, line.baseline_pt, pc.cap and 0 or line.extra_space_pt)
    end
  end

  shift = 0

  if page.header then
    piece(page.header.piece, page.header.baseline_pt, 0)
  end

  if page.footer then
    piece(page.footer.piece, page.footer.baseline_pt, 0)
  end

  for _, b in ipairs(order) do
    drawn = drawn + (b.raster:draw(surface, b.colour, b.runs) or 0)
  end

  return drawn
end

return pagedraw
