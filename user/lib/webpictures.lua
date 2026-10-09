-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A page's pictures for the Web Kit: bytes made a surface, and each kept at
-- its box's size.
--
--   local webpictures = use("/Kosmos/Libraries/webpictures.lua")
--   local pics = webpictures.new(web, page_w, view_h)
--   pics:hand(doc, k, bytes)       the page's `k`th picture, from its bytes:
--                                  true when the page took it
--   pics:to_boxes(doc)             each kept at the size the layout gave it
--   pics:decode(bytes, k)          -> surface, natural width, height
--   pics.page_w, pics.view_h       what a picture is kept no larger than
--
-- **Out of the browser** (`docs/mail.md` M5, 9 October 2026): Mail draws a
-- message's HTML with the same engine and needs the same pictures - PNG and
-- JPEG by their first bytes, SVG by its tag - so this is the one copy, and
-- the browser and Mail both hand theirs over through it. Where a picture
-- comes from - the network, the cache, a message's own part - is the
-- caller's; what it is made into is here.
--
-- A picture is kept no wider than the page, which is as wide as the engine
-- will draw it - a photograph of 4000 pixels in a column of 250 does not
-- keep 48 MB - with its own size said beside it, which is what the layout
-- measures a picture the page gave no size to by. An SVG is drawn at its
-- own size first and at its box's once the layout has given it one.

local webpictures = {}

local P = {}
P.__index = P

function webpictures.new(web, page_w, view_h)
  return setmetatable({ web = web, page_w = page_w, view_h = view_h,
                        svgs = {}, raster = {} }, P)
end

-- Everything handed over let go of, for a new page.
function P:forget()
  self.svgs, self.raster = {}, {}
end

local function most(self)
  return math.max(self.page_w, self.view_h) * 2
end

-- An SVG drawn at `w` by `h`, kept no larger than a page could show.
function P:svg_surface(svg, w, h)
  local m = most(self)

  if w > m or h > m then
    local k = m / math.max(w, h)

    w, h = math.max(1, math.floor(w * k)), math.max(1, math.floor(h * k))
  end

  local made, pic = pcall(gfx.surface, { w = w, h = h })

  if not made or not pic then return nil end

  -- A picture there is no memory to draw is a picture missing, not a
  -- page stopped.
  if not pcall(svg.draw, svg, pic) then
    pic:free()
    return nil
  end

  return pic, w, h
end

--
-- A picture's bytes decoded; nil for anything that is not one. `k`, when
-- given, is which of the page's pictures it is, so an SVG can be drawn
-- again at its box's size later.
--
function P:decode(bytes, k)
  if bytes and self.web and bytes:sub(1, 1024):find("<svg", 1, true) then
    local svg = self.web.svg(bytes)

    if not svg then return nil end

    local sw, sh = svg:size()
    local w = math.min(sw, self.page_w)
    local pic, dw, dh = self:svg_surface(svg, w, math.max(1, sh * w // sw))

    if pic and k then self.svgs[k] = { svg = svg, w = dw, h = dh } end

    return pic, sw, sh
  end

  local decode = bytes and ((bytes:sub(1, 4) == "\x89PNG" and gfx.png)
                            or (bytes:sub(1, 2) == "\xff\xd8" and gfx.jpeg))
  local ok, pic = false, nil

  if decode then ok, pic = pcall(decode, bytes) end
  if not ok or not pic then return nil end

  local pw, ph = pic:size()

  if pw > self.page_w then
    local h = math.max(1, ph * self.page_w // pw)
    local made, small = pcall(gfx.surface, { w = self.page_w, h = h })

    if made and small then
      small:stretch(pic, 0, 0, pw, ph, 0, 0, self.page_w, h, nil, true)
      pic:free()
      pic = small
    end
  end

  return pic, pw, ph
end

-- The `k`th picture handed to the page; true when it took it.
function P:hand(doc, k, bytes)
  local pic, pw, ph = self:decode(bytes, k)

  if not (pic and doc:ns_picture(k, pic, pw, ph)) then return false end

  if not self.svgs[k] then
    local sw, sh = pic:size()

    self.raster[k] = { pic = pic, pw = pw, ph = ph, w = sw, h = sh }
  end

  return true
end

--
-- Each SVG drawn again at its box's size, where the layout gave it one that
-- is not the size it was drawn at; and each other picture scaled to its box
-- once. The natural size goes back unchanged, so the layout does not move.
--
function P:to_boxes(doc)
  if not next(self.svgs) and not next(self.raster) then return end

  local m = most(self)

  for k, o in ipairs(doc:ns_objects()) do
    local s, r = self.svgs[k], self.raster[k]

    if s and o.w > 0 and o.h > 0 and (o.w ~= s.w or o.h ~= s.h) then
      local pic = self:svg_surface(s.svg, o.w, o.h)

      if pic then
        local sw, sh = s.svg:size()

        doc:ns_picture(k, pic, sw, sh)
        s.w, s.h = o.w, o.h
      end
    elseif r and not o.background and o.w > 0 and o.h > 0
           and o.w <= m and o.h <= m
           and (o.w ~= r.w or o.h ~= r.h) then
      -- Scaled from the picture as decoded, not from the last scaling,
      -- so a box that changes size again loses nothing.
      local made, scaled = pcall(gfx.surface, { w = o.w, h = o.h })

      if made and scaled then
        local sw, sh = r.pic:size()

        scaled:stretch(r.pic, 0, 0, sw, sh, 0, 0, o.w, o.h, nil, true)
        doc:ns_picture(k, scaled, r.pw, r.ph)
        r.w, r.h = o.w, o.h
      end
    end
  end
end

return webpictures
