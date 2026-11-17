-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Pages on a desk: where each stands at a zoom, scrolled down and across,
-- drawn once into a surface of its own and blitted after that, and their
-- thumbnails down the side (`docs/office.md` O1, `docs/present.md`, *What
-- moves out of Kosmos Write*).
--
--   local pagedesk = use("/Kosmos/Libraries/pagedesk.lua")
--   local desk = pagedesk.new{
--     zooms = { 50, 75, 100, 125, 150, 200, 300 }, zoom = 4,   -- an index
--     gap = 24,                          -- round each page
--     area = function() return x, y, w, h end,     -- the desk, in the window
--     pages = function() return pages, facing end, -- each { width_pt, height_pt }
--     version = function() return n end,  -- what the pages are drawn from
--     draw = function(i, surface, scale, small) end,  -- page i, into a surface
--     theme = ui.theme,                  -- the thumbnails' chrome
--     thumb_w = 92,                      -- a thumbnail's width
--   }
--
--   desk:scale()  desk:percent()       the zoom, as a scale and as words
--   desk:page_at(i)                    where page i stands: x, y
--   desk:page_px(page)                 how big a page is drawn: w, h
--   desk:content_width()  desk:height()
--   desk:draw(s, colour, shadow)       the desk and the pages on it
--   desk:point(x, y)                   the page a point is on: i, x_pt, y_pt
--   desk:scroll_to(y)  desk:scroll_by(dy)  desk:refit()
--   desk:follow(x, y, h, right)        scrolled to show a caret at x, y
--   desk:zoom_to(z)                    true when the zoom changed
--   desk:thumbs(s, x, y, w, h, current)   the thumbnails, `current` ringed
--   desk:thumb_at(x, y)                the page whose thumbnail a press is on
--   desk:show(i)                       scrolled to page i's top
--
-- **What a page is, the desk does not know.** It is a size and a function
-- that draws it into a surface, so Write's pages and Present's slides are
-- the same to it. A page is drawn again when the zoom or `version` changes,
-- and a page far from the screen gives its surface back.
--
-- **Facing pages show as spreads**, as a book opens: the first page alone
-- on the right, then each even page on the left of the odd one after it.
-- Otherwise one page under another.
--
-- It was Kosmos Write's, inside `writer.lua`, until O1 (17 November 2026).

local pagedesk = {}

local SPREAD_GAP = 4

function pagedesk.new(o)
  local zooms = o.zooms
  local GAP = o.gap or 24
  local area = o.area
  local theme = o.theme
  local THUMB_W = o.thumb_w or 92        -- a thumbnail's width

  local desk = { zoom = o.zoom or 1, top = 0, across = 0 }

  local drawn = {}              -- each page's surface, by number
  local thumb_drawn = {}
  local thumb_boxes = {}

  local function pages()
    local list, facing = o.pages()
    return list, facing
  end

  function desk:scale() return zooms[self.zoom] / 100 end
  function desk:percent() return zooms[self.zoom] end

  function desk:page_px(page)
    return math.floor(page.width_pt * self:scale() + 0.5),
           math.floor(page.height_pt * self:scale() + 0.5)
  end

  local function row_of(i, facing)
    if facing then return i // 2 end
    return i - 1
  end

  function desk:content_width()
    local list, facing = pages()
    local w = self:page_px(list[1])
    return facing and (2 * w + SPREAD_GAP) or w
  end

  -- Where page `i` stands, in the window, with the desk scrolled.
  function desk:page_at(i)
    local list, facing = pages()
    local dx, dy, dw = area()
    local y = GAP
    local row = row_of(i, facing)
    local k = 1

    -- The rows above this page's, each as tall as its tallest page.
    for r = 0, row - 1 do
      local tallest = 0

      while list[k] and row_of(k, facing) == r do
        local _, h = self:page_px(list[k])
        tallest = math.max(tallest, h)
        k = k + 1
      end

      y = y + tallest + GAP
    end

    local w = self:page_px(list[i])
    local cw = self:content_width()

    -- What is wider than the desk scrolls across; what is narrower is centred.
    local left = (cw + 2 * GAP > dw) and (dx + GAP - self.across) or (dx + (dw - cw) // 2)
    local x = left

    if facing then
      x = (i % 2 == 0) and left or (left + w + SPREAD_GAP)
    end

    return x, dy + y - self.top
  end

  -- How tall all the pages are, with the gaps round them.
  function desk:height()
    local list = pages()
    local _, dy = area()
    local _, y = self:page_at(#list)
    local _, h = self:page_px(list[#list])
    return y + self.top - dy + h + GAP
  end

  function desk:scroll_to(y)
    local _, _, _, dh = area()
    self.top = math.max(0, math.min(y, self:height() - dh))
  end

  function desk:scroll_by(dy) self:scroll_to(self.top + dy) end

  -- The scroll held inside the desk, after the desk changed size.
  function desk:refit()
    local _, _, dw = area()
    self.across = math.max(0, math.min(self.across, self:content_width() + 2 * GAP - dw))
    self:scroll_to(self.top)
  end

  local function surface_of(self, i)
    local list = pages()
    local d = drawn[i]
    local version = o.version()

    if d and d.zoom == self.zoom and d.version == version then return d.surface end

    local w, h = self:page_px(list[i])

    if d and (d.w ~= w or d.h ~= h) then
      d.surface:free()
      d = nil
    end

    if not d then
      local ok, surface = pcall(gfx.surface, { w = w, h = h })

      if not ok or not surface then
        drawn[i] = nil
        return nil
      end

      d = { surface = surface, w = w, h = h }
      drawn[i] = d
    end

    o.draw(i, d.surface, self:scale(), false)
    d.zoom, d.version = self.zoom, version

    return d.surface
  end

  local function forget_far(first, last)
    local list = pages()

    for i, d in pairs(drawn) do
      if i < first - 1 or i > last + 1 or not list[i] then
        d.surface:free()
        drawn[i] = nil
      end
    end
  end

  --
  -- **The desk drawn**, in `colour`, and each page on the screen blitted
  -- from its surface with a `shadow` down and to the right. Pages more
  -- than one from the screen give their surfaces back.
  --
  function desk:draw(s, colour, shadow)
    local list = pages()
    local dx, dy, dw, dh = area()
    local bottom = dy + dh

    s:fill(dx, dy, dw, dh, colour)

    local first, last = nil, nil

    for i = 1, #list do
      local x, sy = self:page_at(i)
      local w, h = self:page_px(list[i])

      if sy < bottom and sy + h > dy then
        first = first or i
        last = i

        local surface = surface_of(self, i)
        local cut = math.max(0, dx - x)                 -- scrolled off the left
        local vw = math.min(w - cut, dx + dw - (x + cut))

        -- A shadow down and to the right, inside the desk.
        local s0, s1 = math.max(dy, sy + 3), math.min(bottom, sy + 3 + h)

        if s1 > s0 then
          s:fill(math.max(dx, x + 3), s0,
                 math.max(0, math.min(x + 3 + w, dx + dw) - math.max(dx, x + 3)), s1 - s0,
                 shadow)
        end

        if surface and vw > 0 then
          local from = math.max(0, dy - sy)
          local band = math.min(h - from, bottom - (sy + from))

          if band > 0 then
            s:blit(surface, cut, from, vw, band, x + cut, sy + from)
          end
        end
      end
    end

    if first then forget_far(first, last) end
  end

  -- The page a point in the window is on, and where on it in points.
  function desk:point(x, y)
    local list = pages()

    for i = 1, #list do
      local px, py = self:page_at(i)
      local _, h = self:page_px(list[i])

      if y >= py - GAP // 2 and y < py + h + GAP // 2 then
        return i, (x - px) / self:scale(), (y - py) / self:scale()
      end
    end
  end

  --
  -- **Scrolled to show a caret** at `x`, `y`, `h` high, whose line ends at
  -- `right`: across first, when the page - or a spread - is wider than the
  -- desk, the caret always and **the whole of its line where that fits** -
  -- a chart or a shape the caret is on, not only its left edge - then down.
  --
  function desk:follow(cx, cy, ch, right)
    local dx, dy, dw, dh = area()
    local bottom = dy + dh
    local most = math.max(0, self:content_width() + 2 * GAP - dw)
    local by = math.max(cx - (dx + dw - 24),
                        math.min(right - (dx + dw - 24), cx - (dx + 24)))

    if by > 0 then
      self.across = math.min(most, self.across + by)
    elseif cx < dx + 24 then
      self.across = math.max(0, self.across - (dx + 24 - cx))
    end

    self.across = math.min(self.across, most)

    if cy < dy + 8 then
      self:scroll_to(self.top - (dy + 8 - cy))
    elseif cy + ch > bottom - 8 then
      self:scroll_to(self.top + (cy + ch - (bottom - 8)))
    end
  end

  -- A new zoom, the middle of the view held where it was.
  function desk:zoom_to(z)
    z = math.max(1, math.min(#zooms, z))

    if z == self.zoom then return false end

    local _, _, _, dh = area()
    local middle = (self.top + dh / 2) / self:scale()

    self.zoom = z
    self:scroll_to(math.floor(middle * self:scale() - dh / 2))
    return true
  end

  --
  -- **The thumbnails**, down a strip at `x`, `y`, `w` by `h`: each page
  -- drawn small by the same `draw` - told it is small - page `current`
  -- ringed, its number under it.
  --
  function desk:thumbs(s, x0, y0, w0, h0, current)
    local list = pages()
    local version = o.version()
    local bottom = y0 + h0

    thumb_boxes = {}

    s:fill(x0, y0, w0, h0, theme.window)
    s:fill(x0 + w0 - 1, y0, 1, h0, theme.line_soft)

    local y = y0 + 14

    for i, page in ipairs(list) do
      local sc = THUMB_W / page.width_pt
      local th = math.floor(page.height_pt * sc + 0.5)
      local x = x0 + (w0 - THUMB_W) // 2

      if y + th > bottom then break end

      local d = thumb_drawn[i]

      if not d or d.version ~= version or d.h ~= th then
        if d then d.surface:free() end

        local ok, surface = pcall(gfx.surface, { w = THUMB_W, h = th })

        if ok and surface then
          o.draw(i, surface, sc, true)
          d = { surface = surface, version = version, h = th }
          thumb_drawn[i] = d
        else
          d = nil
        end
      end

      if i == current then
        s:fill_round(x - 3, y - 3, THUMB_W + 6, th + 6, theme.accent, 5)
      end

      if d then s:blit(d.surface, 0, 0, THUMB_W, th, x, y) end

      local n = tostring(i)
      s:text(x0 + (w0 - gfx.measure(n)) // 2, y + th + 4, n, theme.text_dim, nil, "ui")

      thumb_boxes[#thumb_boxes + 1] = { x = x, y = y, w = THUMB_W, h = th, page = i }
      y = y + th + 30
    end

    for i, d in pairs(thumb_drawn) do
      if not list[i] then d.surface:free() thumb_drawn[i] = nil end
    end
  end

  function desk:thumb_at(x, y)
    for _, b in ipairs(thumb_boxes) do
      if x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h then return b.page end
    end
  end

  -- Scrolled so page `i`'s top is a gap below the desk's.
  function desk:show(i)
    local _, dy = area()
    local _, py = self:page_at(i)
    self:scroll_to(self.top + (py - dy) - GAP)
  end

  return desk
end

return pagedesk
