-- kosmos: application
-- kosmos: icon File_PDF
-- kosmos: name PDF Viewer
-- kosmos: section applications
-- kosmos: opens pdf
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A PDF, as it was typeset: the whole document as one column of pages.
--
--   wm pdfview
--   wm pdfview:/Home/odyssey.pdf
--
-- Made again on 14 October 2026 (`docs/pdfview.md`, `docs/pdfview.html`),
-- from Diego's "it needs a menu, zoom, better navigation, window resizing".
-- This is step 2, the window: its menu bar, the header's page and zoom,
-- the document scrolled whole or a page at a time, zoom by fitting or by
-- steps, a window that resizes and lays its pages out again, and a cache
-- of rendered pages. The sidebar, Find and Copy, and where a document was
-- left are the steps after.
--
-- **What a PDF is belongs to the PDF Kit** (`pdf.lua`, its one door):
-- the pages, their sizes, a page drawn into a surface at a scale, its
-- pictures. This is the window: layout, scrolling, zoom and the menus.
--
-- **Scrolling costs a blit.** A page is rendered once at the zoom in force
-- into a surface of its own, and a frame copies the band of each page in
-- view out of it - nothing interpreted, no glyph drawn. The cache holds the
-- pages in view and one either side, rendered one a pass while nothing
-- else is happening, so a page scrolled to is usually ready.
--
-- **A zoom answers at once.** The pages already rendered are drawn
-- stretched to the new size until each is rendered again at it, a pass at a
-- time - so the window moves when it is asked to and sharpens after, and
-- no key waits behind a render.
--
-- **A direct window** (`gfx.md` 19.4) whose band - its title and its
-- menus' titles - the kit draws; the header under it, the pages and the
-- foot are this file's, drawn with `pixelkit` as Mail and Maps draw theirs.
-- The menus are the kit's, and so published by name (`window:controls`):
-- `menu/View/One page at a time` is pressed from outside as by a click.

local ui      = use("/Kosmos/Libraries/ui.lua")
local files   = use("/Kosmos/Libraries/files.lua")
local panel   = use("/Kosmos/Libraries/panel.lua")
local pdf     = use("/Kosmos/Libraries/pdf.lua")
local keys    = use("/Kosmos/Libraries/keys.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local pk      = use("/Kosmos/Libraries/pixelkit.lua").new(ui)

local theme = ui.theme

local screen = gfx.screen()
local sw, sh = 1280, 800

if screen then sw, sh = screen:size() end

local path = files.words(args)[1]

local TOOL_H = 46                     -- the header under the band
local FOOT_H = 26                     -- the foot
local BAR_W  = 12                     -- the scroll bar
local GAP    = 16                     -- desk between pages, and round them

-- The zoom's steps, in per cent of actual size.
local STEPS = { 50, 67, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400 }

-- **Actual size**: a point at the screen density this desktop assumes, 96
-- to the inch - a page at its printed size. 100% is this.
local ACTUAL = 96 / 72

local INK   = 0xff101418              -- what a page's text is drawn in
local PAPER = 0xfffdfdfb              -- a page, whatever the look

--------------------------------------------------------------------------
-- The state.
--------------------------------------------------------------------------

local doc = nil
local sizes = {}                      -- each page's { w, h } in points
local fit = "width"                   -- "width", "page" or nil: a step
local pct = 100                       -- the step, when `fit` is nil
local single = false                  -- one page at a time
local current = 1                     -- the page being read
local scale = 1                       -- pixels a point, from the above
local top, left = 0, 0                -- the scroll, in pixels of the layout
local said = nil                      -- a sentence for the foot, or nil
local title = "PDF Viewer"

-- Where each page is in the layout, at `scale`: `place[i] = { y, w, h }`,
-- only for the pages laid out - all of them, or the one in single mode.
local place, order = {}, {}
local total_h, total_w = 0, 0

-- Rendered pages: `cache[i] = { surface, scale, w, h }`. An entry at
-- another scale is drawn stretched until it is rendered again.
local cache = {}

-- A page that could not be given a surface at a scale: not asked for again
-- at that scale, or the loop would ask on every pass.
local refused = {}

local thumbs_in_view                  -- the sidebar's, defined with `wanted`
local draw_sidebar                    -- defined with `frame`

local W, H = math.min(1100, sw - 80), math.min(sh - 100, 900)

--
-- **The sidebar** (step 3): the pages as thumbnails, or the document's own
-- contents. Wanted or not is the person's (`sidebar`); shown is that and
-- room for it - below `NARROW` the pages need the width, and it comes back
-- when the window is wide again.
--
local SIDE_W  = 176                   -- the sidebar's width
local NARROW  = 720                   -- below this the sidebar is hidden
local THUMB_W = 96                    -- a thumbnail's width
local TABS_H  = 48                    -- the tabs at the sidebar's top
local LABEL_H = 20                    -- a thumbnail's number under it
local ROW_H   = 28                    -- a row of the contents

local sidebar = true                  -- wanted
local tab = "pages"                   -- "pages" or "contents"
local side_top = 0                    -- the sidebar's own scroll
local thumbs = {}                     -- `thumbs[i]`: a page drawn small
local thumb_place = {}                -- `thumb_place[i] = { y, w, h }`
local thumbs_h = 0
local contents = {}                   -- the outline, flat: { title, page, depth }

local function side_w() return (sidebar and W >= NARROW) and SIDE_W or 0 end
local function view_w() return math.max(40, W - BAR_W - side_w()) end
local function view_h() return math.max(1, H - TOOL_H - FOOT_H) end
local function list_h() return math.max(1, view_h() - TABS_H) end

local PER_MS = math.max(1, ((fs.read("/Devices/cpu") or {}).counter_hz or 62500000) // 1000)

--------------------------------------------------------------------------
-- The menus. Their `items` are functions, so each opens saying what is in
-- force now - the zoom's mark, a page that has no next.
--------------------------------------------------------------------------

local act = {}                        -- filled in below, before any opens

local function zoom_label()
  if fit == "width" then return "Fit width" end
  if fit == "page" then return "Fit page" end
  return pct .. "%"
end

local function shown_pct() return math.floor(scale / ACTUAL * 100 + 0.5) end

local function view_items()
  return {
    { text = "Zoom in", hint = "Ctrl +", on_choose = function() act.zoom_by(1) end },
    { text = "Zoom out", hint = "Ctrl -", on_choose = function() act.zoom_by(-1) end },
    { separator = true },
    { text = "Fit width", hint = "Ctrl 0", mark = fit == "width",
      on_choose = function() act.fit("width") end },
    { text = "Fit page", mark = fit == "page", on_choose = function() act.fit("page") end },
    { text = "Actual size", hint = "Ctrl 1", mark = (fit == nil and pct == 100),
      on_choose = function() act.step(100) end },
    { separator = true },
    { text = "The whole document, scrolled", mark = not single,
      on_choose = function() act.single(false) end },
    { text = "One page at a time", mark = single,
      on_choose = function() act.single(true) end },
    { separator = true },
    { text = "Sidebar", hint = "Ctrl B", mark = sidebar,
      on_choose = function() act.sidebar(not sidebar) end },
    { text = "Pages", mark = tab == "pages", on_choose = function() act.tab("pages") end },
    { text = "Contents", mark = tab == "contents",
      on_choose = function() act.tab("contents") end },
  }
end

local function go_items()
  local n = doc and #doc.pages or 0

  return {
    { text = "Next page", hint = ".", disabled = current >= n or nil,
      on_choose = function() act.go(current + 1) end },
    { text = "Previous page", hint = ",", disabled = current <= 1 or nil,
      on_choose = function() act.go(current - 1) end },
    { separator = true },
    { text = "First page", hint = "Home", disabled = n == 0 or nil,
      on_choose = function() act.go(1) end },
    { text = "Last page", hint = "End", disabled = n == 0 or nil,
      on_choose = function() act.go(n) end },
  }
end

local MENUS = {
  { title = "File", items = function()
      return {
        { text = "Open...", hint = "Ctrl O", on_choose = function() act.choose() end },
        { separator = true },
        { text = "Close", hint = "Ctrl W", on_choose = function() act.close() end },
      }
    end },
  -- Copy and Find are step 4 (`docs/pdfview.md`), and say so by being
  -- there and not yet able.
  { title = "Edit", items = function()
      return {
        { text = "Copy", hint = "Ctrl C", disabled = true },
        { text = "Find", hint = "Ctrl F", disabled = true },
      }
    end },
  { title = "View", items = view_items },
  { title = "Go", items = go_items },
}

local win, err = ui.window{
  title = title, w = W, h = H, centre = true, direct = true, resizable = true,
  menubar = MENUS,
}

if not win then
  print("pdfview: " .. tostring(err))
  return
end

if not win:surface() then
  print("pdfview: this window did not get a shared surface")
  return
end

W, H = win.w, win.h

--------------------------------------------------------------------------
-- The layout: where every page goes at the zoom in force.
--------------------------------------------------------------------------

local function free(i)
  local c = cache[i]

  if c then
    c.surface:free()
    cache[i] = nil
  end
end

-- The page at the top of the view and how far down it the view starts, so
-- a new layout puts the same words at the top.
local function anchor()
  for _, i in ipairs(order) do
    local p = place[i]

    if p.y + p.h > top then
      return i, math.max(0, (top - p.y) / p.h)
    end
  end

  return order[1] or 1, 0
end

local function lay_out(keep)
  if not doc then return end

  local at, into = 1, 0

  if keep then at, into = anchor() end

  local widest, tallest = 1, 1
  local list = {}

  if single then
    list[1] = current
  else
    for i = 1, #sizes do list[i] = i end
  end

  for _, i in ipairs(list) do
    widest = math.max(widest, sizes[i].w)
    tallest = math.max(tallest, sizes[i].h)
  end

  local room_w = math.max(40, view_w() - 2 * GAP)
  local room_h = math.max(40, view_h() - 2 * GAP)

  if fit == "width" then
    scale = room_w / widest
  elseif fit == "page" then
    local s = sizes[single and current or (at or 1)] or sizes[1]
    scale = math.min(room_w / s.w, room_h / s.h)
  else
    scale = ACTUAL * pct / 100
  end

  scale = math.max(0.05, scale)

  place, order = {}, list
  local y = GAP

  for _, i in ipairs(list) do
    local pw = math.max(1, math.floor(sizes[i].w * scale + 0.5))
    local ph = math.max(1, math.floor(sizes[i].h * scale + 0.5))

    place[i] = { y = y, w = pw, h = ph }
    y = y + ph + GAP
  end

  total_h = y
  total_w = math.floor(widest * scale + 0.5) + 2 * GAP

  -- The same place again, at the new scale.
  if keep and place[at] then
    top = math.floor(place[at].y + into * place[at].h)
  elseif not keep then
    top = 0
  end

  top = math.max(0, math.min(top, total_h - view_h()))
  left = math.max(0, math.min(left, total_w - view_w()))

  -- Rendered pages no longer laid out are let go; those at another scale
  -- are kept, to be drawn stretched until they are rendered again.
  for i in pairs(cache) do
    if not place[i] then free(i) end
  end

  print(("pdfview: %d of %d, %s, %d%%, %s, the view %dx%d, the sidebar %s")
        :format(current, #sizes, zoom_label(), shown_pct(),
                single and "one page at a time" or "the whole document",
                view_w(), view_h(), side_w() > 0 and tab or "hidden"))
end

-- The pages any of whose rows are in view, in order.
local function in_view()
  local out = {}
  local vh = view_h()

  for _, i in ipairs(order) do
    local p = place[i]

    if p.y < top + vh and p.y + p.h > top then out[#out + 1] = i end
  end

  return out
end

-- The page being read: the first in view whose lower part reaches past a
-- third of the way down.
local function reading()
  local mark = top + view_h() // 3

  for _, i in ipairs(order) do
    local p = place[i]

    if p.y + p.h > mark then return i end
  end

  return order[#order] or 1
end

-- The row of the contents being read: the last entry at or before the
-- page being read.
local function contents_at()
  local at = nil

  for k, e in ipairs(contents) do
    if e.page and e.page <= current then at = k end
  end

  return at
end

-- The sidebar scrolled so the page being read - its thumbnail, or its part
-- of the contents - is in it.
local function side_follow()
  local y, h

  if tab == "pages" then
    local t = thumb_place[current]
    if not t then return end
    y, h = t.y, t.h + LABEL_H
  else
    local k = contents_at()
    if not k then return end
    y, h = (k - 1) * ROW_H, ROW_H
  end

  local lh = list_h()

  if y < side_top then
    side_top = math.max(0, y - 8)
  elseif y + h > side_top + lh then
    side_top = y + h - lh + 8
  end
end

local function scrolled()
  if single or not doc then return end

  local now = reading()

  if now ~= current then
    current = now
    side_follow()
    print(("pdfview: at page %d of %d"):format(current, #sizes))
  end
end

local function scroll_to(y)
  top = math.max(0, math.min(math.floor(y), total_h - view_h()))
  scrolled()
end

local function pan_to(x)
  left = math.max(0, math.min(math.floor(x), total_w - view_w()))
end

--------------------------------------------------------------------------
-- Rendering, a page at a time.
--------------------------------------------------------------------------

-- The thumbnails any of which is in the sidebar's view, in order.
thumbs_in_view = function()
  local out = {}
  local lh = list_h()

  for i = 1, #sizes do
    local t = thumb_place[i]

    if t and t.y < side_top + lh and t.y + t.h + LABEL_H > side_top then out[#out + 1] = i end
  end

  return out
end

-- The page that most wants rendering at `scale`: one in view first, then
-- the one either side. Nil when every one of them is ready.
local function wanted()
  if not doc then return nil end

  local seen = in_view()

  local function wants(i)
    local c = cache[i]
    return (not c or c.scale ~= scale) and refused[i] ~= scale
  end

  for _, i in ipairs(seen) do
    if wants(i) then return i end
  end

  local first, last = seen[1], seen[#seen]

  for _, i in ipairs({ last and last + 1, first and first - 1 }) do
    if i and place[i] and wants(i) then return i end
  end

  -- The pages ready, the thumbnails in the sidebar's view: after them,
  -- because a page being read matters more than its picture beside it.
  if side_w() > 0 and tab == "pages" then
    for _, i in ipairs(thumbs_in_view()) do
      if thumbs[i] == nil then return i, true end
    end
  end

  return nil
end

-- Pages further than one from the view let go: the cache is the pages in
-- view and one either side, so a long book never holds more than a few.
local function trim()
  local seen = in_view()

  if #seen == 0 then return end

  local lo, hi = seen[1] - 1, seen[#seen] + 1

  for i in pairs(cache) do
    if i < lo or i > hi then free(i) end
  end

  -- Thumbnails a sidebar's height away from its view let go too: a
  -- 377-page book draws the few in sight, never all of them.
  local near = thumbs_in_view()
  local tlo, thi = (near[1] or 1) - 12, (near[#near] or 0) + 12

  for i, t in pairs(thumbs) do
    if i < tlo or i > thi or side_w() == 0 or tab ~= "pages" then
      if t then t:free() end
      thumbs[i] = nil
    end
  end
end

-- A page drawn small for the sidebar: false when it could not be, so it is
-- not asked for again.
local function render_thumb(i)
  local sz = sizes[i]
  local t = thumb_place[i]

  if not (sz and t) then return end

  local surface
  local made = pcall(function () surface = gfx.surface{ w = t.w, h = t.h } end)

  if not made or not surface then
    thumbs[i] = false
    return
  end

  surface:fill(0, 0, t.w, t.h, PAPER)

  local ok = pcall(pdf.render, doc, doc:page(i), surface, t.w / sz.w, INK)

  thumbs[i] = surface

  -- Where its middle is on the screen as it is drawn, for whoever presses
  -- it from outside - the harnesses.
  local ox, oy = win.origin_x or 0, (win.origin_y or 0) + (win.head_h or 0)

  print(("pdfview: thumbnail %d of %d at %d,%d%s"):format(i, #sizes, ox + SIDE_W // 2,
        oy + TOOL_H + TABS_H + t.y - side_top + t.h // 2, ok and "" or ", not drawn"))
end

-- Where each thumbnail goes in the sidebar, under the one before.
local function lay_out_thumbs()
  thumb_place, thumbs_h = {}, 8

  for i, sz in ipairs(sizes) do
    local th = math.max(8, math.floor(THUMB_W * sz.h / sz.w + 0.5))

    thumb_place[i] = { y = thumbs_h, w = THUMB_W, h = th }
    thumbs_h = thumbs_h + th + LABEL_H + 10
  end
end

local function render(i)
  local p = place[i]

  if not p then return end

  local surface

  local made = pcall(function () surface = gfx.surface{ w = p.w, h = p.h } end)

  if not made or not surface then
    said = ("no memory for a %dx%d page"):format(p.w, p.h)
    refused[i] = scale
    print("pdfview: " .. said)
    return
  end

  surface:fill(0, 0, p.w, p.h, PAPER)

  local started = sys.ticks()
  local ok, drawn, _, missing = pcall(pdf.render, doc, doc:page(i), surface, scale, INK)
  local ms = (sys.ticks() - started) // PER_MS

  if not ok then
    -- Drawn as blank paper, and said, rather than asked for again forever.
    said = ("page %d: %s"):format(i, tostring(drawn))
    print("pdfview: " .. said)
  else
    print(("pdfview: page %d of %d, %d glyphs, %d faces missing, %d pictures, %d ms at %d%%")
          :format(i, #sizes, drawn or 0, missing or 0, doc.pictures_drawn or 0, ms,
                  shown_pct()))

    if (missing or 0) > 0 then
      said = ("%d face%s would not load"):format(missing, missing == 1 and "" or "s")
    end
  end

  free(i)
  cache[i] = { surface = surface, scale = scale, w = p.w, h = p.h }
end

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

-- The header's controls, placed by `frame` and read by a press.
local toggle = { icon = "sidebar" }
local back = { icon = "back" }
local forward = { icon = "forward" }
local pagebox = { w = 52, h = 30 }
local zoombox = { w = 128, h = 30 }
local tabs = { items = { { text = "Pages" }, { text = "Contents" } } }

-- Where the controls are, said once a place, for whoever presses them from
-- outside - the harnesses.
local controls_said = nil

local function say_controls()
  local ox, oy = win.origin_x or 0, (win.origin_y or 0) + (win.head_h or 0)
  local key = ("%d,%d,%d"):format(ox, oy, W)

  if key == controls_said then return end
  controls_said = key

  print(("pdfview: controls back %d,%d forward %d,%d zoom %d,%d")
        :format(ox + back.x + 13, oy + back.y + 13, ox + forward.x + 13, oy + forward.y + 13,
                ox + zoombox.x + zoombox.w // 2, oy + zoombox.y + zoombox.h // 2))
end

-- The desk's colour: the look's sunken well a fifth of the way to its
-- text - a grey a white page stands out on in a light look, and a little
-- lifted from the dark in a dark one. Towards the edge's shadow it was the
-- page's own white in Plex, and a page had no edge.
local function desk_colour()
  return theme.mix(theme.sunken, theme.text, 200)
end

local function draw_page(s, i, vx, vy, vw, vh)
  local p = place[i]
  local c = cache[i]

  local x = (math.max(total_w, vw) - p.w) // 2 - left
  local y = p.y - top

  -- The part of the page inside the view, in the view's coordinates.
  local x0, y0 = math.max(x, 0), math.max(y, 0)
  local x1, y1 = math.min(x + p.w, vw), math.min(y + p.h, vh)

  if x1 <= x0 or y1 <= y0 then return end

  if c and c.scale == scale then
    s:blit(c.surface, x0 - x, y0 - y, x1 - x0, y1 - y0, vx + x0, vy + y0)
  elseif c then
    s:stretch(c.surface, 0, 0, c.w, c.h, vx + x, vy + y, p.w, p.h, 255, false,
              vx + x0, vy + y0, x1 - x0, y1 - y0)
  else
    s:fill(vx + x0, vy + y0, x1 - x0, y1 - y0, PAPER)
  end
end

-- The sidebar: its two tabs, then the thumbnails - the page being read
-- ringed - or the contents, the part being read lit.
draw_sidebar = function(s, top_y, vh)
  s:fill(0, top_y, SIDE_W - 1, vh, theme.window)
  s:fill(SIDE_W - 1, top_y, 1, vh, theme.line_soft)

  tabs.x, tabs.y, tabs.w = 12, top_y + 10, SIDE_W - 24
  tabs.chosen = tab == "pages" and 1 or 2
  pk.segments(s, tabs)

  local ly = top_y + TABS_H
  local lh = list_h()

  if tab == "pages" then
    for _, i in ipairs(thumbs_in_view()) do
      local t = thumb_place[i]
      local x = (SIDE_W - t.w) // 2
      local y = ly + t.y - side_top

      -- Clipped to the list, which starts under the tabs.
      local y0, y1 = math.max(y, ly), math.min(y + t.h, ly + lh)

      if i == current then
        s:fill_round(x - 4, math.max(y - 4, ly), t.w + 8,
                     math.min(y + t.h + 4, ly + lh) - math.max(y - 4, ly), theme.accent, 5)
      end

      if y1 > y0 then
        if thumbs[i] then
          s:blit(thumbs[i], 0, y0 - y, t.w, y1 - y0, x, y0)
        else
          s:fill(x, y0, t.w, y1 - y0, PAPER)
        end
      end

      local label = tostring(i)
      local lty = y + t.h + 4

      if lty >= ly and lty + gfx.height() <= ly + lh then
        s:text((SIDE_W - gfx.measure(label)) // 2, lty, label,
               i == current and theme.text or theme.text_dim, nil, "ui")
      end
    end
  elseif #contents == 0 then
    pk.empty(s, 0, ly, SIDE_W - 1, math.min(lh, 120), { "No contents",
                                                         "This document has none" })
  else
    local lit = contents_at()
    local first = side_top // ROW_H + 1

    for k = first, math.min(#contents, first + lh // ROW_H + 1) do
      local e = contents[k]
      local y = ly + (k - 1) * ROW_H - side_top

      if y >= ly and y + ROW_H <= ly + lh then
        if k == lit then s:fill_round(6, y + 1, SIDE_W - 13, ROW_H - 2, theme.line, 6) end

        local indent = 12 + math.min(e.depth, 6) * 12
        local words = ui.fitted(e.title ~= "" and e.title or ("Page " .. tostring(e.page)),
                                SIDE_W - indent - 12)

        s:text(indent, y + (ROW_H - gfx.height()) // 2, words,
               e.page and theme.text or theme.text_dim, nil, "ui")
      end
    end
  end
end

local function frame()
  local s = win:surface()

  if not s then return end

  local vw, vh = view_w(), view_h()
  local n = doc and #sizes or 0

  -- The header: back and forward a page, the page and its count, the zoom.
  s:fill(0, 0, W, TOOL_H - 1, theme.window)
  s:fill(0, TOOL_H - 1, W, 1, theme.line_soft)

  local cy = (TOOL_H - 1 - 30) // 2

  toggle.x, toggle.y = 12, cy + 2
  toggle.disabled = W < NARROW
  pk.iconbutton(s, toggle)

  back.x, back.y = 48, cy + 2
  back.disabled = (not doc) or current <= 1
  pk.iconbutton(s, back)

  forward.x, forward.y = 76, cy + 2
  forward.disabled = (not doc) or current >= n
  pk.iconbutton(s, forward)

  pagebox.x, pagebox.y = 112, cy
  pk.field(s, pagebox, false)

  local number = doc and tostring(current) or "-"
  local ty = cy + (30 - gfx.height()) // 2

  s:text(pagebox.x + (pagebox.w - gfx.measure(number)) // 2, ty, number, theme.text, nil, "ui")

  local count = "of " .. n
  s:text(pagebox.x + pagebox.w + 8, ty, count, theme.text_dim, nil, "ui")

  zoombox.x = pagebox.x + pagebox.w + 8 + gfx.measure(count) + 18
  zoombox.y = cy
  zoombox.text = zoom_label()
  pk.chooser(s, zoombox)

  -- The pages.
  local top_y = TOOL_H
  local dx = side_w()

  s:fill(dx, top_y, vw, vh, desk_colour())

  if doc then
    for _, i in ipairs(in_view()) do draw_page(s, i, dx, top_y, vw, vh) end
  else
    pk.empty(s, dx, top_y, vw, vh, { "No document open",
                                      "File, Open... or Ctrl O, or a PDF from Tracker" })
  end

  -- The scroll bar: its thumb as long as the view is of the whole.
  s:fill(dx + vw, top_y, BAR_W, vh, theme.window)

  if total_h > vh then
    local th = math.max(28, vh * vh // total_h)
    local ty_ = top_y + (vh - th) * top // math.max(1, total_h - vh)

    s:fill_round(dx + vw + 3, ty_ + 2, BAR_W - 6, th - 4, theme.line, 3)
  end

  if dx > 0 then draw_sidebar(s, top_y, vh) end

  -- The foot: the zoom, and where the document came from.
  local fy = H - FOOT_H

  s:fill(0, fy, W, FOOT_H, theme.window)
  s:fill(0, fy, W, 1, theme.line_soft)

  local left_words = doc and (zoom_label() == (shown_pct() .. "%") and zoom_label()
                              or (zoom_label() .. " \194\183 " .. shown_pct() .. "%"))
                     or ""

  if said then left_words = left_words .. (left_words ~= "" and "   " or "") .. said end

  local fty = fy + (FOOT_H - gfx.height()) // 2

  s:text(12, fty, left_words, theme.text_dim, nil, "ui")

  if path and doc then
    local where = ui.fitted(path, W // 2)
    s:text(W - 12 - gfx.measure(where), fty, where, theme.text_dim, nil, "ui")
  end

  win:commit{ x = 0, y = 0, w = W, h = H }
  say_controls()
end

--------------------------------------------------------------------------
-- What the menus, the keys and the header do.
--------------------------------------------------------------------------

local function open(file)
  local attrs, why = fs.getattr(file)

  if not attrs then
    said = ("cannot open %s: %s"):format(file, tostring(why))
    print("pdfview: " .. said)
    return
  end

  -- The file read a window at a time, through the PDF Kit's door; the
  -- document before it gives its window back.
  local source, oops = pdf.file(file)

  if not source then
    said = tostring(oops)
    print("pdfview: " .. said)
    return
  end

  local started = sys.ticks()
  local ok, got = pcall(pdf.open, source)

  if not ok then
    source.close()
    said = tostring(got)
    print("pdfview: " .. said)
    return
  end

  for i in pairs(cache) do free(i) end
  if doc and doc.source.close then doc.source.close() end

  doc, path, said = got, file, nil

  -- Every page's size, once: the layout is the whole document's, so the
  -- scroll bar is true from the start rather than growing as pages are met.
  sizes = {}

  for i = 1, #doc.pages do
    local page = doc:page(i)
    local box = page and doc:resolve(page.MediaBox) or nil
    local w = box and ((box[3] or 612) - (box[1] or 0)) or 612
    local h = box and ((box[4] or 792) - (box[2] or 0)) or 792

    sizes[i] = { w = math.max(1, math.abs(w)), h = math.max(1, math.abs(h)) }
  end

  -- The contents, flat in reading order, each with its depth.
  contents = {}

  local ok_o, outline = pcall(pdf.outline, doc)

  local function flatten(list)
    for _, e in ipairs(list) do
      contents[#contents + 1] = { title = e.title, page = e.page, depth = e.depth }
      flatten(e.children or {})
    end
  end

  if ok_o then flatten(outline) end

  doc:forget()

  for i, t in pairs(thumbs) do
    if t then t:free() end
    thumbs[i] = nil
  end

  side_top = 0
  lay_out_thumbs()

  title = file:match("([^/]+)$") or file
  win:retitle(title)

  current, top, left = 1, 0, 0
  print(("pdfview: %s, %d pages, %d in its contents, read in %d ms"):format(file, #sizes,
        #contents, (sys.ticks() - started) // PER_MS))
  lay_out(false)
end

function act.go(n)
  if not doc then return end

  n = math.max(1, math.min(n, #sizes))

  current = n

  if single then
    lay_out(false)
  else
    -- The page's top at the view's, as far as the end allows - so the
    -- last page is the one being read even when it cannot reach the top.
    top = math.max(0, math.min(place[n].y - GAP, total_h - view_h()))
  end

  side_follow()

  print(("pdfview: at page %d of %d"):format(current, #sizes))
end

function act.fit(how)
  fit = how
  lay_out(true)
end

function act.step(p)
  fit, pct = nil, p
  lay_out(true)
end

-- A step in or out from what is shown now, whatever chose it.
function act.zoom_by(d)
  local now = shown_pct()
  local to = nil

  if d > 0 then
    for _, p in ipairs(STEPS) do
      if p > now then to = p break end
    end
  else
    for k = #STEPS, 1, -1 do
      if STEPS[k] < now then to = STEPS[k] break end
    end
  end

  if to then act.step(to) end
end

function act.single(on)
  if single == on then return end

  single = on
  lay_out(false)

  -- Back to the whole document at the page that was being read.
  if not single and doc and place[current] then
    top = math.max(0, math.min(place[current].y - GAP, total_h - view_h()))
  end
end

function act.choose()
  local start_at = path and path:match("^(.*)/") or "/Home"
  local picked = panel.open{
    start = start_at,
    on_choose = function (chosen) open(chosen) end,
  }

  if picked then picked:run() end
end

function act.close() win:close() end

function act.sidebar(on)
  sidebar = on
  lay_out(true)
  side_follow()
end

function act.tab(which)
  tab = which
  side_top = 0

  -- Asked for, so shown: the tab is what the person wants to see.
  if not sidebar then sidebar = true lay_out(true) end

  side_follow()
  print(("pdfview: the sidebar shows its %s"):format(tab))
end

local function zoom_menu()
  local items = {}

  items[#items + 1] = { text = "Fit width", mark = fit == "width",
                        on_choose = function() act.fit("width") end }
  items[#items + 1] = { text = "Fit page", mark = fit == "page",
                        on_choose = function() act.fit("page") end }
  items[#items + 1] = { separator = true }

  for _, p in ipairs(STEPS) do
    items[#items + 1] = { text = p .. "%", mark = (fit == nil and pct == p),
                          on_choose = function() act.step(p) end }
  end

  win:open_menu((win.origin_x or 0) + zoombox.x,
                (win.origin_y or 0) + (win.head_h or 0) + zoombox.y + zoombox.h + 2, items)
end

-- Published beside the menus (`window:controls`): the header's buttons by
-- what they do, and the page as a field that reads the page being read.
win:publish_controls(function()
  local n = doc and #sizes or 0
  local out = {
    { kind = "button", label = "Previous page", disabled = current <= 1,
      press = function() act.go(current - 1) end },
    { kind = "button", label = "Next page", disabled = current >= n,
      press = function() act.go(current + 1) end },
    { kind = "field", label = "Page", read = function() return tostring(current) end,
      write = function(text)
        local k = tonumber(text)
        if k then act.go(math.floor(k)) end
      end },
    { kind = "field", label = "Zoom", read = function() return zoom_label() end },
    { kind = "button", label = "Sidebar", press = function() act.sidebar(not sidebar) end },
  }

  -- The contents, each entry a button by its title: a script goes to a
  -- chapter by its name.
  for _, e in ipairs(contents) do
    if e.page and e.title ~= "" then
      out[#out + 1] = { kind = "contents", label = e.title, press = function() act.go(e.page) end }
    end
  end

  return out
end)

local function key(c)
  local k, mods = keys.parts(c)
  local ctrl = (mods & keys.CTRL) ~= 0
  local vh = view_h()

  if ctrl and (k == 61 or k == 43) then act.zoom_by(1)
  elseif ctrl and k == 45 then act.zoom_by(-1)
  elseif ctrl and k == 48 then act.fit("width")
  elseif ctrl and k == 49 then act.step(100)
  elseif c == 2 then act.sidebar(not sidebar)             -- Ctrl B
  elseif c == 15 then act.choose()                         -- Ctrl O
  elseif c == 23 then act.close()                          -- Ctrl W
  elseif c == 43 or c == 61 then act.zoom_by(1)
  elseif c == 45 then act.zoom_by(-1)
  elseif not doc then return false
  elseif c == keys.DOWN then scroll_to(top + 40)
  elseif c == keys.UP then scroll_to(top - 40)
  elseif c == keys.PAGEDOWN or c == keys.SPACE then
    if single and top >= total_h - vh then act.go(current + 1) else scroll_to(top + vh - 40) end
  elseif c == keys.PAGEUP then
    if single and top <= 0 then act.go(current - 1) else scroll_to(top - vh + 40) end
  elseif c == keys.HOME then act.go(1)
  elseif c == keys.END then act.go(#sizes)
  elseif c == keys.RIGHT then
    if total_w > view_w() then pan_to(left + 40) else act.go(current + 1) end
  elseif c == keys.LEFT then
    if total_w > view_w() then pan_to(left - 40) else act.go(current - 1) end
  elseif c == 46 then act.go(current + 1)                  -- .
  elseif c == 44 then act.go(current - 1)                  -- ,
  else return false end

  return true
end

--------------------------------------------------------------------------
-- The pointer.
--------------------------------------------------------------------------

local dragging = nil                  -- the scroll bar's thumb, held

local function thumb()
  local vh = view_h()
  local th = math.max(28, vh * vh // math.max(1, total_h))

  return th, TOOL_H + (vh - th) * top // math.max(1, total_h - vh)
end

-- A press in the sidebar: a tab, a thumbnail, a row of the contents.
local function press_side(x, y)
  local which = pk.segment_at(tabs, x, y)

  if which then
    act.tab(which == 1 and "pages" or "contents")
    return
  end

  local ly = y - TOOL_H - TABS_H

  if ly < 0 then return end

  if tab == "pages" then
    for _, i in ipairs(thumbs_in_view()) do
      local t = thumb_place[i]
      local ty = t.y - side_top

      if ly >= ty and ly < ty + t.h + LABEL_H then
        act.go(i)
        return
      end
    end
  else
    local e = contents[(ly + side_top) // ROW_H + 1]

    if e and e.page then act.go(e.page) end
  end
end

local function press(x, y)
  if y >= TOOL_H and y < H - FOOT_H and x < side_w() then
    press_side(x, y)
    return
  end

  if y < TOOL_H then
    if pk.inside(toggle, x, y) and not toggle.disabled then
      pk.hold(toggle, function() act.sidebar(not sidebar) end)
    elseif pk.inside(back, x, y) and not back.disabled then
      pk.hold(back, function() act.go(current - 1) end)
    elseif pk.inside(forward, x, y) and not forward.disabled then
      pk.hold(forward, function() act.go(current + 1) end)
    elseif pk.inside(zoombox, x, y) then
      zoom_menu()
    end

    return
  end

  -- The scroll bar: the thumb held and dragged, the track a view at a time.
  if x >= side_w() + view_w() and y < H - FOOT_H and total_h > view_h() then
    local th, ty = thumb()

    if y >= ty and y < ty + th then
      dragging = { from = y, top = top, th = th }
    elseif y < ty then
      scroll_to(top - view_h() + 40)
    else
      scroll_to(top + view_h() - 40)
    end
  end
end

local function drag(y)
  if not dragging then return false end

  local room = math.max(1, view_h() - dragging.th)

  scroll_to(dragging.top + (y - dragging.from) * (total_h - view_h()) / room)
  return true
end

--------------------------------------------------------------------------
-- The loop: events, then a frame when something changed, then one page
-- rendered when nothing is waiting.
--------------------------------------------------------------------------

if path then open(path) end

local decode = keys.decoder()
local dirty = true

while win.running do
  if dirty then
    trim()
    frame()
    dirty = false
  end

  local work, work_thumb = wanted()
  local reply = wmproto.poll(win.handle, work and 0 or 25)

  if not reply then break end

  -- Something pressed from outside (`window:controls`), answered inside
  -- the poll: drawn as a click would be.
  if win.served_change then
    win.served_change = false
    dirty = true
  end

  local events = reply.events or {}

  for _, ev in ipairs(events) do
    if win:direct_event(ev) then
      dirty = true
    elseif ev.type == "resize" then
      W, H = ev.w, ev.h
      controls_said = nil
      lay_out(true)
      dirty = true
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "theme" then
      dirty = true
    elseif ev.type == "key" then
      local a, b = decode(ev.code)

      for _, c in ipairs({ a, b }) do
        if key(c) then dirty = true end
      end
    elseif ev.type == "wheel" then
      if doc and (ev.x or W) < side_w() then
        local whole = tab == "pages" and thumbs_h or #contents * ROW_H

        side_top = math.max(0, math.min(side_top - (ev.n or 0) * 3 * 40, whole - list_h()))
        dirty = true
      elseif doc then
        scroll_to(top - (ev.n or 0) * 3 * 40)
        dirty = true
      end
    elseif ev.type == "mouse" and not ev.menu then
      local x, y = ev.x or 0, ev.y or 0

      if ev.action == "press" and (ev.button or "left") == "left" then
        press(x, y)
        dirty = true
      elseif ev.action == "move" then
        if drag(y) then dirty = true end
      elseif ev.action == "release" then
        dragging = nil
        if pk.release(x, y) then dirty = true end
      end
    end
  end

  local held = decode(nil)
  if held and key(held) then dirty = true end

  -- Nothing waiting, and a page wanted: rendered now, and drawn.
  if #events == 0 and not dirty and work and win.running then
    if work_thumb then render_thumb(work) else render(work) end
    dirty = true
  end
end

for i in pairs(cache) do free(i) end
