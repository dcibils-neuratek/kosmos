-- kosmos: application
-- kosmos: icon File_Text
-- kosmos: name Kosmos Write
-- kosmos: section applications
-- kosmos: opens write
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Kosmos Write: a document's pages, as they print, typed into and
-- formatted (`docs/write.html`, `docs/write.md` W4).
--
--   wm writer                     a new document
--   wm writer:/Home/Letter.write  that one
--
-- `writer` and not `write`, which is `print`'s sibling and a name every
-- program already holds: a program by that name could not be started by it.
--
--   typing, Return, Backspace, Delete      as anywhere
--   arrows, Home, End                      the caret; with Shift, a selection
--   Control-Home, Control-End              the document's ends
--   Page Up, Page Down                     scroll
--   Control-A                              everything
--   Control-C, X, V                        copy, cut, paste (the system's)
--   Control-Z, Control-Y                   undo, redo
--   Control-B, Control-U                   bold, underline
--   Control-S                              save
--   Control-E                              Export PDF, beside the document
--
-- **As drawn** (`docs/write.html`): the tools across the top - View, Zoom
-- and Add Page; Insert, Table, Chart, Text, Shape, Media and Comment;
-- Export, Format and Document at the right - the pages on a dark desk, and
-- the panel Format and Document switch at the right. A tool a step has not
-- built yet is shown, greyed, and does nothing (`roadmap.md` W4-W7).
--
-- **The window orchestrates; the kits do the work** (`CLAUDE.md`'s premise):
-- what an edit or a format does to the text is `richtext`'s, where a line
-- breaks and a caret stands is `pageset`'s, the pixels are `pagedraw`'s,
-- the controls are `pixelkit`'s, the file is `writedoc`'s and the PDF is
-- `pdf`'s. What is here is what each key, click and control means.
--
-- **Each edit is a new body**, the paragraphs it did not touch the same
-- tables (`richtext`): so an undo is the body before, kept, and setting the
-- pages again sets only the paragraphs that changed (`pageset.cache`).
--
-- **A direct window** (`gfx.md` 19.4): a page is drawn once into a surface
-- of its own and blitted after that; an edit or a selection draws again the
-- pages on the screen, and the caret is drawn on the window over them.

local ui        = use("/Kosmos/Libraries/ui.lua")
local keys      = use("/Kosmos/Libraries/keys.lua")
local wmproto   = use("/Kosmos/Libraries/wmproto.lua")
local richtext  = use("/Kosmos/Libraries/richtext.lua")
local writedoc  = use("/Kosmos/Libraries/writedoc.lua")
local pageset   = use("/Kosmos/Libraries/pageset.lua")
local faces     = use("/Kosmos/Libraries/faces.lua")
local pagedraw  = use("/Kosmos/Libraries/pagedraw.lua")
local pdf       = use("/Kosmos/Libraries/pdf.lua")
local pk        = use("/Kosmos/Libraries/pixelkit.lua").new(ui)

local theme = ui.theme

local W, H = 1200, 800
local TOOLS_H = 64              -- the row of tools across the top
local PANEL_W = 300             -- Format's and Document's panel, at the right
local THUMBS_W = 150            -- View's page thumbnails, at the left
local THUMB_W = 92              -- a thumbnail's width
local GAP = 24                  -- round each page, on the desk

-- The desk is dark whatever the look, as the drawing has it; the chrome is
-- the look's.
local DESK   = 0xff121821
local PAPER  = 0xffffffff
local SHADOW = 0xff0a0e14
local CARET  = 0xff2a55c9       -- the drawing's accent, on paper
local CHOSEN = 0xffc9d8f6       -- a selection, under the text

local ZOOMS = { 50, 75, 100, 125, 150, 200, 300 }
local SIZES = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 30, 36, 48, 64, 72 }
local SPACINGS = { 1, 1.15, 1.2, 1.5, 2 }

-- The text colours a person picks from: the document's inks first.
local COLOURS = {
  { "#1b2330", "Ink" }, { "#000000", "Black" }, { "#5b6677", "Slate" },
  { "#8e959f", "Grey" }, { "#c0392b", "Red" }, { "#d35400", "Orange" },
  { "#b7950b", "Ochre" }, { "#27ae60", "Green" }, { "#16a085", "Teal" },
  { "#2a55c9", "Blue" }, { "#8e44ad", "Purple" }, { "#ffffff", "White" },
}

local UNDO_MOST = 200

local path = args and args:match("^%s*(%S+)")
local name = path and path:match("([^/]+)$") or "Untitled"

local win, err = ui.window{
  title = name .. " - Kosmos Write", w = W, h = H, x = 40, y = 30,
  direct = true,
}

if not win then
  print("writer: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The document, set.
--------------------------------------------------------------------------

local catalogue = faces.catalogue(gfx.typefaces())
local measure = faces.measure(catalogue, gfx.typeface)
local drawer = pagedraw.new(measure)
local cache = pageset.cache()
local doc, said

if path then
  local why
  doc, why = writedoc.open(path)

  if not doc then
    said = tostring(why)
    doc = writedoc.new()
  end
else
  doc = writedoc.new()
end

local by_name = {}
for _, s in ipairs(doc.styles) do by_name[s.name] = s end

local set = pageset.set(doc, measure, cache)
local zoom = 4                  -- 125%, an index into ZOOMS
local top = 0                   -- how far down the desk the view is
local across = 0                -- and how far across, when a page is wider

local caret = { para = 1, at = 1 }
local anchor, column = nil, nil
local dirty = false
local pending = {}              -- a look chosen with nothing selected, for typing

local undo, redo = {}, {}
local last_kind = nil

local version = 1               -- what the pages on screen were drawn from

-- Which panel is open at the right - "text", "document" or nil - and which
-- of the Text panel's three parts.
local panel, part = "text", "style"

-- A list open over the window, and what picking from it does.
local menu = nil

-- View's thumbnails shown, and a field with the keyboard - the header's
-- words - when one has it.
local thumbs = false
local focus = nil

local function scale() return ZOOMS[zoom] / 100 end

--------------------------------------------------------------------------
-- The desk, and the pages on it.
--------------------------------------------------------------------------

local function desk()
  local left = thumbs and THUMBS_W or 0
  local right = panel and (W - PANEL_W) or W
  return left, TOOLS_H, right - left, H - TOOLS_H
end

local function page_px(page)
  return math.floor(page.width_pt * scale() + 0.5),
         math.floor(page.height_pt * scale() + 0.5)
end

-- Where page `i` stands, in the window, with the desk scrolled.
local function page_at(i)
  local dx, dy, dw = desk()
  local y = GAP

  for k = 1, i - 1 do
    local _, h = page_px(set.pages[k])
    y = y + h + GAP
  end

  local w = page_px(set.pages[i])

  -- A page wider than the desk scrolls across; a narrower one is centred.
  local x = (w + 2 * GAP > dw) and (dx + GAP - across) or (dx + (dw - w) // 2)

  return x, dy + y - top
end

local function desk_height()
  local _, y = page_at(#set.pages)
  local _, h = page_px(set.pages[#set.pages])
  return y + top - TOOLS_H + h + GAP
end

local drawn = {}

local function selected()
  return anchor and (anchor.para ~= caret.para or anchor.at ~= caret.at)
end

local function marks_on(i)
  if not selected() then return nil end

  local out = {}

  for _, r in ipairs(pageset.selection(set, measure, anchor, caret)) do
    if r.page == i then
      r.colour = CHOSEN
      out[#out + 1] = r
    end
  end

  return out
end

local function page_surface(i)
  local d = drawn[i]

  if d and d.zoom == zoom and d.version == version then return d.surface end

  local w, h = page_px(set.pages[i])

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

  drawer:page(set, set.pages[i], d.surface, scale(), 0, 0, PAPER, marks_on(i))
  d.zoom, d.version = zoom, version

  return d.surface
end

local function forget_far(first, last)
  for i, d in pairs(drawn) do
    if i < first - 1 or i > last + 1 or not set.pages[i] then
      d.surface:free()
      drawn[i] = nil
    end
  end
end

--------------------------------------------------------------------------
-- The look at the caret, for the panel.
--------------------------------------------------------------------------

-- The range a format applies to: the selection, or the caret's paragraph
-- for a paragraph's fields.
local function range()
  if selected() then return anchor, caret end
  return caret, caret
end

-- The look the panel shows: the selection's first character's, or what
-- typing at the caret would be.
local function shown_look()
  local place = caret

  if selected() then
    local a = richtext.before(anchor, caret) and anchor or caret
    local plain = richtext.plain(doc.body[a.para])
    place = { para = a.para, at = math.min(a.at + 1, #plain + 1) }
  end

  local look, layout, style = richtext.look_at(doc.body, place, by_name)

  if not selected() then
    for k, v in pairs(pending) do look[k] = v end
  end

  return look, layout, style
end

--------------------------------------------------------------------------
-- The tools, across the top.
--------------------------------------------------------------------------

local TOOLS = {
  { key = "view",     icon = "sidebar",  text = "View" },
  { key = "zoom",     icon = "zoom",     text = "Zoom" },
  { key = "addpage",  icon = "new",      text = "Add Page" },
  { gap = 14 },
  { key = "insert",   icon = "insert",   text = "Insert",   later = "W5" },
  { key = "table",    icon = "table",    text = "Table",    later = "W5" },
  { key = "chart",    icon = "chart",    text = "Chart",    later = "W7" },
  { key = "textbox",  icon = "textbox",  text = "Text",     later = "W7" },
  { key = "shape",    icon = "shape",    text = "Shape",    later = "W7" },
  { key = "media",    icon = "pictures", text = "Media",    later = "W5" },
  { key = "comment",  icon = "comment",  text = "Comment",  later = "W7" },
  { right = true },
  { key = "export",   icon = "export",   text = "Export" },
  { key = "format",   icon = "format",   text = "Format" },
  { key = "document", icon = "page",     text = "Document" },
}

local function draw_tools(s)
  s:fill(0, 0, W, TOOLS_H, theme.window)
  s:fill(0, TOOLS_H - 1, W, 1, theme.line_soft)

  -- The words a tool shows that change: the zoom's percentage.
  local function word(t)
    if t.key == "zoom" then return ZOOMS[zoom] .. "%" end
    return t.text
  end

  -- Left to right until the gap that pushes the rest to the right edge.
  local x, right = 12, nil

  for i, t in ipairs(TOOLS) do
    if t.right then right = i break end

    if t.gap then
      x = x + t.gap
    else
      local b = { x = x, y = 8, icon = t.icon, text = word(t),
                  disabled = t.later ~= nil }
      b.on = t.key == "view" and thumbs
      pk.tool(s, b)
      t.x, t.y, t.w = b.x, b.y, b.w
      x = x + t.w + 2
    end
  end

  local rx = W - 12

  for i = #TOOLS, (right or #TOOLS) + 1, -1 do
    local t = TOOLS[i]
    local b = { x = 0, y = 8, icon = t.icon, text = word(t),
                disabled = t.later ~= nil,
                on = (t.key == "format" and panel == "text")
                     or (t.key == "document" and panel == "document") }

    b.w = pk.tool_width(word(t))
    rx = rx - b.w
    b.x = rx
    pk.tool(s, b)
    t.x, t.y, t.w = b.x, b.y, b.w
    rx = rx - 2
  end
end

--------------------------------------------------------------------------
-- The panel at the right.
--------------------------------------------------------------------------

-- The controls drawn this frame, each `{ box, act }`: a press finds the
-- one it is on. Rebuilt every frame, since what is shown changes.
local controls = {}

-- Once for each panel and part, where each control is - for a harness to
-- press it by name, as the window manager says where a tab is.
local said_where = {}

local function control(key, box, act)
  controls[#controls + 1] = { key = key, box = box, act = act }
end

local function weight_names(family)
  local seen, out = {}, {}

  for _, f in ipairs(catalogue.families[family] or {}) do
    for wname, wn in pairs(faces.WEIGHT) do
      if wn == f.weight and not seen[wname] then
        seen[wname] = true
        out[#out + 1] = { wname, wn }
      end
    end
  end

  table.sort(out, function(a, b) return a[2] < b[2] end)

  local names = {}
  for i, wv in ipairs(out) do names[i] = wv[1] end
  return #names > 0 and names or { "Regular" }
end

local function index_of(list, v)
  for i, x in ipairs(list) do if x == v then return i end end
end

local function colour_name(c)
  for _, pair in ipairs(COLOURS) do
    if pair[1] == c then return pair[2] end
  end
  return c
end

local function argb(c) return 0xff000000 | (tonumber((c or "#000000"):sub(2), 16) or 0) end

local open_menu, apply_char, apply_para, apply_style, doc_edit

local function say_where()
  local key = panel == "text" and ("text:" .. part) or tostring(panel)

  if not said_where[key] then
    said_where[key] = true

    for _, c in ipairs(controls) do
      local b = c.box
      print(("writer: control %s at %d,%d %dx%d"):format(c.key, b.x, b.y, b.w, b.h or 0))
    end
  end
end

local PAPERS = {
  { name = "A4", label = "A4, 210 by 297 mm" },
  { name = "Letter", label = "Letter, 8.5 by 11 in" },
}

--
-- **The Document panel** (`docs/write.html`): the paper and which way it
-- lies, a header and a footer and how far each is from the edge, the
-- header's words, the four margins, and page numbers.
--
local function draw_document(s, x0, w0, y)
  local paper = doc.paper

  pk.label(s, x0, y, "Paper")
  y = y + 18

  local label = paper.name
  for _, pp in ipairs(PAPERS) do if pp.name == paper.name then label = pp.label end end

  local pc = { x = x0, y = y, w = w0, text = label }
  pk.chooser(s, pc)
  control("paper", pc, function()
    local labels, chosen = {}, nil
    for i, pp in ipairs(PAPERS) do
      labels[i] = pp.label
      if pp.name == paper.name then chosen = i end
    end
    open_menu("paper", pc.x, pc.y + pc.h + 4, pc.w, labels, chosen, function(i)
      local name_ = PAPERS[i].name
      doc_edit("paper", { paper = { name = name_, landscape = paper.landscape } })
    end)
  end)

  y = y + pc.h + 8

  local turn = { x = x0, y = y, w = w0,
                 items = { { text = "Upright" }, { text = "On its side" } },
                 chosen = paper.landscape and 2 or 1 }
  pk.segments(s, turn)
  control("turn", turn, function(cx, cy)
    local i = pk.segment_at(turn, cx, cy)
    if i then
      doc_edit("paper", { paper = { name = paper.name, width_mm = paper.width_mm,
                                    height_mm = paper.height_mm, landscape = i == 2 } })
    end
  end)

  y = y + turn.h + 16

  local hd = { x = x0, y = y, text = "Header", on = doc.header.on }
  pk.check(s, hd)
  control("header", hd, function()
    doc_edit("header", { header = { on = not doc.header.on,
                                    from_top_mm = doc.header.from_top_mm,
                                    text = doc.header.text } })
  end)

  local ft = { x = x0 + w0 // 2, y = y, text = "Footer", on = doc.footer.on }
  pk.check(s, ft)
  control("footer", ft, function()
    doc_edit("footer", { footer = { on = not doc.footer.on,
                                    from_bottom_mm = doc.footer.from_bottom_mm,
                                    page_numbers = doc.footer.page_numbers } })
  end)

  y = y + 30

  -- A stepper with its words at the left, as the Layout part has them.
  local function row(key, words, text, step)
    s:text(x0, y + (30 - gfx.height()) // 2, words, theme.text_dim, nil, "ui")

    local st = { x = x0 + 120, y = y, w = w0 - 120, text = text }
    pk.stepper(s, st)
    control(key, st, function(cx, cy)
      local d = pk.step_at(st, cx, cy)
      if d and d ~= 0 then step(d) end
    end)

    y = y + st.h + 8
  end

  row("from_top", "From the top", ("%g mm"):format(doc.header.from_top_mm), function(d)
    doc_edit("header", { header = { on = doc.header.on, text = doc.header.text,
                                    from_top_mm = math.max(0, doc.header.from_top_mm + d) } })
  end)
  row("from_bottom", "From the bottom", ("%g mm"):format(doc.footer.from_bottom_mm), function(d)
    doc_edit("footer", { footer = { on = doc.footer.on, page_numbers = doc.footer.page_numbers,
                                    from_bottom_mm = math.max(0, doc.footer.from_bottom_mm + d) } })
  end)

  -- The header's words, typed into a field.
  s:text(x0, y + (30 - gfx.height()) // 2, "Header text", theme.text_dim, nil, "ui")

  local field = { x = x0 + 120, y = y, w = w0 - 120, h = 30 }
  pk.field(s, field, focus == "header")

  local words = doc.header.text
  local room = field.w - 20

  while #words > 0 and gfx.measure(words) > room do words = words:sub(2) end

  s:text(field.x + 10, field.y + (30 - gfx.height()) // 2, words, theme.text, nil, "ui")

  if focus == "header" then
    s:fill(field.x + 10 + gfx.measure(words) + 1, field.y + 7, 2, 16, theme.ring)
  end

  control("header_text", field, function() focus = "header" end)

  y = y + 38
  pk.label(s, x0, y, "Margins")
  y = y + 18

  local m = doc.margins_mm

  for _, side in ipairs({ { "top", "Top" }, { "bottom", "Bottom" },
                          { "left", "Left" }, { "right", "Right" } }) do
    row("margin_" .. side[1], side[2], ("%g mm"):format(m[side[1]]), function(d)
      local next_m = { top = m.top, bottom = m.bottom, left = m.left, right = m.right }
      next_m[side[1]] = math.max(0, m[side[1]] + d)
      doc_edit("margins", { margins_mm = next_m })
    end)
  end

  local pn = { x = x0, y = y + 4, text = "Page numbers", on = doc.footer.page_numbers }
  pk.check(s, pn)
  control("page_numbers", pn, function()
    doc_edit("footer", { footer = { on = doc.footer.on, from_bottom_mm = doc.footer.from_bottom_mm,
                                    page_numbers = not doc.footer.page_numbers } })
  end)
end

local function draw_panel(s)
  if not panel then return end

  local px = W - PANEL_W
  local x0, w0 = px + 14, PANEL_W - 28
  local look, layout, style = shown_look()
  local y

  s:fill(px, TOOLS_H, PANEL_W, H - TOOLS_H, theme.window)
  s:fill(px, TOOLS_H, 1, H - TOOLS_H, theme.line_soft)

  -- Text and Document, the panel's two faces.
  local tabs = { x = x0, y = TOOLS_H + 12, w = w0, accent = true,
                 items = { { text = "Text" }, { text = "Document" } },
                 chosen = panel == "text" and 1 or 2 }
  pk.segments(s, tabs)
  control("tabs", tabs, function(cx, cy)
    local i = pk.segment_at(tabs, cx, cy)
    if i == 1 then panel = "text" elseif i == 2 then panel = "document" end
  end)

  y = tabs.y + tabs.h + 16

  if panel == "document" then
    draw_document(s, x0, w0, y)
    say_where(s)
    return
  end

  -- The paragraph style, in a box of its own, its name large.
  local sbox = { x = x0, y = y, w = w0, h = 46 }
  s:fill_round(sbox.x, sbox.y, sbox.w, sbox.h, theme.raised, 12)
  s:text(sbox.x + 12, sbox.y + (sbox.h - gfx.height("title")) // 2, style,
         theme.text, nil, "title")
  pk.icon(s, "descending", sbox.x + sbox.w - 26, sbox.y + 15, theme.text_dim)
  control("style", sbox, function()
    local names = {}
    for i, st in ipairs(doc.styles) do names[i] = st.name end
    open_menu("style", sbox.x, sbox.y + sbox.h + 4, sbox.w, names,
              index_of(names, style), function(i) apply_style(names[i]) end)
  end)

  y = sbox.y + sbox.h + 14

  local parts = { x = x0, y = y, w = w0,
                  items = { { text = "Style" }, { text = "Layout" }, { text = "More" } },
                  chosen = part == "style" and 1 or part == "layout" and 2 or 3 }
  pk.segments(s, parts)
  control("parts", parts, function(cx, cy)
    local i = pk.segment_at(parts, cx, cy)
    part = ({ "style", "layout", "more" })[i] or part
  end)

  y = parts.y + parts.h + 18

  if part == "style" then
    pk.label(s, x0, y, "Font")
    y = y + 18

    local fam = { x = x0, y = y, w = w0, text = look.face }
    pk.chooser(s, fam)
    control("face", fam, function()
      open_menu("face", fam.x, fam.y + fam.h + 4, fam.w, catalogue.names,
                index_of(catalogue.names, look.face),
                function(i) apply_char({ face = catalogue.names[i] }) end)
    end)

    y = y + fam.h + 8

    local weights = weight_names(look.face)
    local wt = { x = x0, y = y, w = math.floor(w0 * 0.55), text = look.weight }
    pk.chooser(s, wt)
    control("weight", wt, function()
      open_menu("weight", wt.x, wt.y + wt.h + 4, wt.w, weights,
                index_of(weights, look.weight),
                function(i) apply_char({ weight = weights[i] }) end)
    end)

    local size = { x = wt.x + wt.w + 8, y = y, w = w0 - wt.w - 8,
                   text = ("%g pt"):format(look.size_pt) }
    pk.stepper(s, size)
    control("size", size, function(cx, cy)
      local step = pk.step_at(size, cx, cy)

      if step == 0 then
        local labels = {}
        for i, v in ipairs(SIZES) do labels[i] = v .. " pt" end
        open_menu("size", size.x, size.y + size.h + 4, size.w, labels,
                  index_of(SIZES, look.size_pt),
                  function(i) apply_char({ size_pt = SIZES[i] }) end)
      elseif step then
        apply_char({ size_pt = math.max(1, look.size_pt + step) })
      end
    end)

    y = y + size.h + 10

    local bold = (faces.WEIGHT[look.weight] or 400) >= 600
    local biu = { x = x0, y = y, w = w0,
                  items = { { icon = "bold" }, { icon = "italic" },
                            { icon = "underline" }, { icon = "strike" } },
                  chosen = { bold, look.italic, look.underline, look.strike } }
    pk.segments(s, biu)
    control("marks", biu, function(cx, cy)
      local i = pk.segment_at(biu, cx, cy)

      if i == 1 then apply_char({ weight = bold and "Regular" or "Bold" })
      elseif i == 2 then apply_char({ italic = not look.italic })
      elseif i == 3 then apply_char({ underline = not look.underline })
      elseif i == 4 then apply_char({ strike = not look.strike }) end
    end)

    y = y + biu.h + 18
    pk.label(s, x0, y, "Text colour")
    y = y + 18

    local sw = { x = x0, y = y, w = 40, h = 22, colour = argb(look.colour) }
    pk.swatch(s, sw)
    s:text(x0 + 52, y + (22 - gfx.height()) // 2, colour_name(look.colour),
           theme.text, nil, "ui")
    local colour_box = { x = x0, y = y, w = w0, h = 22 }
    control("colour", colour_box, function()
      local labels, chosen = {}, nil
      for i, pair in ipairs(COLOURS) do
        labels[i] = pair[2]
        if pair[1] == look.colour then chosen = i end
      end
      open_menu("colour", x0, y + 26, 160, labels, chosen,
                function(i) apply_char({ colour = COLOURS[i][1] }) end)
    end)
  elseif part == "layout" then
    pk.label(s, x0, y, "Alignment")
    y = y + 18

    local aligns = { "left", "center", "right", "justify" }
    local al = { x = x0, y = y, w = w0,
                 items = { { icon = "align-left" }, { icon = "align-center" },
                           { icon = "align-right" }, { icon = "align-justify" } },
                 chosen = index_of(aligns, layout.align) }
    pk.segments(s, al)
    control("align", al, function(cx, cy)
      local i = pk.segment_at(al, cx, cy)
      if i then apply_para({ align = aligns[i] }) end
    end)

    y = y + al.h + 18
    pk.label(s, x0, y, "Line spacing")
    y = y + 18

    local sp = { x = x0, y = y, w = w0,
                 text = ("%g line%s"):format(layout.spacing_lines,
                                             layout.spacing_lines == 1 and "" or "s") }
    pk.chooser(s, sp)
    control("spacing", sp, function()
      local labels = {}
      for i, v in ipairs(SPACINGS) do labels[i] = ("%g"):format(v) .. (v == 1 and " line" or " lines") end
      open_menu("spacing", sp.x, sp.y + sp.h + 4, sp.w, labels,
                index_of(SPACINGS, layout.spacing_lines),
                function(i) apply_para({ spacing_lines = SPACINGS[i] }) end)
    end)

    y = y + sp.h + 18
    pk.label(s, x0, y, "Bullets & lists")
    y = y + 18

    local LISTS = { "none", "bullet", "number" }
    local LIST_WORDS = { "None", "Bullets", "Numbers" }
    local li = { x = x0, y = y, w = w0,
                 text = LIST_WORDS[index_of(LISTS, layout.list) or 1] }
    pk.chooser(s, li)
    control("list", li, function()
      open_menu("list", li.x, li.y + li.h + 4, li.w, LIST_WORDS,
                index_of(LISTS, layout.list),
                function(i) apply_para({ list = LISTS[i] }) end)
    end)

    y = y + li.h + 18

    -- The space around a paragraph and its indents, a stepper each.
    local rows = {
      { "before", "Space before", ("%g pt"):format(layout.before_pt), "before_pt", 2 },
      { "after", "Space after", ("%g pt"):format(layout.after_pt), "after_pt", 2 },
      { "first", "First line", ("%g mm"):format(layout.indent_first_mm), "indent_first_mm", 5 },
      { "left", "Left indent", ("%g mm"):format(layout.indent_left_mm), "indent_left_mm", 5 },
      { "right", "Right indent", ("%g mm"):format(layout.indent_right_mm), "indent_right_mm", 5 },
    }

    for _, r in ipairs(rows) do
      s:text(x0, y + (30 - gfx.height()) // 2, r[2], theme.text_dim, nil, "ui")

      local st = { x = x0 + 120, y = y, w = w0 - 120, text = r[3] }
      pk.stepper(s, st)
      control(r[1], st, function(cx, cy)
        local step = pk.step_at(st, cx, cy)
        if step and step ~= 0 then
          apply_para({ [r[4]] = math.max(r[4] == "indent_first_mm" and -200 or 0,
                                         layout[r[4]] + step * r[5]) })
        end
      end)

      y = y + st.h + 8
    end
  else
    -- A drop cap, and how many lines deep it is.
    local capped = layout.drop_cap_lines >= 2
    local dc = { x = x0, y = y + 4, text = "Drop cap", on = capped }
    pk.check(s, dc)
    control("dropcap", dc, function()
      apply_para({ drop_cap_lines = capped and 0 or 3 })
    end)

    local deep = { x = x0 + 120, y = y, w = w0 - 120,
                   text = ("%d lines"):format(capped and layout.drop_cap_lines or 3) }
    pk.stepper(s, deep)
    control("caplines", deep, function(cx, cy)
      local d = pk.step_at(deep, cx, cy)
      if d and d ~= 0 then
        apply_para({ drop_cap_lines = math.max(2, math.min(10,
          (capped and layout.drop_cap_lines or 3) + d)) })
      end
    end)

    y = y + deep.h + 16

    local kwn = { x = x0, y = y, text = "Keep with the next paragraph",
                  on = layout.keep_with_next }
    pk.check(s, kwn)
    control("keep", kwn, function()
      apply_para({ keep_with_next = not layout.keep_with_next })
    end)
  end

  say_where(s)
end

--------------------------------------------------------------------------
-- A frame.
--------------------------------------------------------------------------

--
-- **View's thumbnails**: each page drawn small by the same drawer - its
-- lines grey rules at that size, as text that small looks - the caret's
-- page ringed, its number under it. A press on one goes to that page.
--
local thumb_drawn = {}
local thumb_boxes = {}

local function caret_page()
  local here = pageset.locate(set, measure, caret)
  return here and here.page or 1
end

local function draw_thumbs(s)
  thumb_boxes = {}

  if not thumbs then return end

  s:fill(0, TOOLS_H, THUMBS_W, H - TOOLS_H, theme.window)
  s:fill(THUMBS_W - 1, TOOLS_H, 1, H - TOOLS_H, theme.line_soft)

  local current = caret_page()
  local y = TOOLS_H + 14

  for i, page in ipairs(set.pages) do
    local sc = THUMB_W / page.width_pt
    local th = math.floor(page.height_pt * sc + 0.5)
    local x = (THUMBS_W - THUMB_W) // 2

    if y + th > H then break end

    local d = thumb_drawn[i]

    if not d or d.version ~= version or d.h ~= th then
      if d then d.surface:free() end

      local ok, surface = pcall(gfx.surface, { w = THUMB_W, h = th })

      if ok and surface then
        drawer:page(set, page, surface, sc, 0, 0, PAPER)
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
    s:text((THUMBS_W - gfx.measure(n)) // 2, y + th + 4, n, theme.text_dim, nil, "ui")

    thumb_boxes[#thumb_boxes + 1] = { x = x, y = y, w = THUMB_W, h = th, page = i }
    y = y + th + 30
  end

  for i, d in pairs(thumb_drawn) do
    if not set.pages[i] then d.surface:free() thumb_drawn[i] = nil end
  end
end

local function caret_px()
  local here = pageset.locate(set, measure, caret)

  if not here then return nil end

  local px, py = page_at(here.page)
  local x = px + math.floor(here.x_pt * scale() + 0.5)
  local y = py + math.floor((here.baseline_pt - here.ascent_pt) * scale())
  local h = math.max(8, math.floor(here.height_pt * scale() + 0.5))

  return x, y, h, here
end

local function frame()
  local s = win:surface()

  if not s then return end

  local dx, dy, dw, dh = desk()

  s:fill(dx, dy, dw, dh, DESK)

  local first, last = nil, nil

  for i = 1, #set.pages do
    local x, sy = page_at(i)
    local w, h = page_px(set.pages[i])

    if sy < H and sy + h > dy then
      first = first or i
      last = i

      local surface = page_surface(i)
      local cut = math.max(0, dx - x)                 -- scrolled off the left
      local vw = math.min(w - cut, dx + dw - (x + cut))

      -- A shadow down and to the right, inside the desk.
      local s0, s1 = math.max(dy, sy + 3), math.min(H, sy + 3 + h)

      if s1 > s0 then
        s:fill(math.max(dx, x + 3), s0,
               math.max(0, math.min(x + 3 + w, dx + dw) - math.max(dx, x + 3)), s1 - s0,
               SHADOW)
      end

      if surface and vw > 0 then
        local from = math.max(0, dy - sy)
        local band = math.min(h - from, H - (sy + from))

        if band > 0 then
          s:blit(surface, cut, from, vw, band, x + cut, sy + from)
        end
      end
    end
  end

  if first then forget_far(first, last) end

  local cx, cy, ch = caret_px()

  if cx and cy + ch > dy and cy < H and cx >= dx and cx < dx + dw then
    local y0 = math.max(dy, cy)
    s:fill(cx, y0, 2, math.min(cy + ch, H) - y0, CARET)
  end

  controls = {}
  draw_thumbs(s)
  draw_tools(s)
  draw_panel(s)

  -- What was last said, in the tools' row between the two groups.
  local note = said or ("%s%s  -  %d page%s"):format(name, dirty and " (edited)" or "",
                                                      #set.pages,
                                                      #set.pages == 1 and "" or "s")
  local from, to = 0, W

  for _, t in ipairs(TOOLS) do
    if t.right then break end
    if t.x then from = t.x + t.w end
  end

  for i = #TOOLS, 1, -1 do
    if TOOLS[i].right then break end
    to = TOOLS[i].x or to
  end

  local room = to - from - 32

  while #note > 1 and gfx.measure(note) > room do note = note:sub(1, -2) end

  if room > 40 then
    s:text(from + 16, (TOOLS_H - gfx.height()) // 2, note, theme.text_dim, nil, "ui")
  end

  if menu then pk.menu(s, menu.box) end

  win:commit()
end

local function report()
  local x, y = page_at(1)
  local w, h = page_px(set.pages[1])

  print(("writer: %s, %d page%s at %d%%, page 1 at %d,%d %dx%d"):format(
    name, #set.pages, #set.pages == 1 and "" or "s", ZOOMS[zoom], x, y, w, h))
end

--------------------------------------------------------------------------
-- The view.
--------------------------------------------------------------------------

local function scroll_to(y)
  local _, _, _, dh = desk()
  top = math.max(0, math.min(y, desk_height() - dh))
end

local function follow()
  local cx, cy, ch = caret_px()
  local dx, dy, dw = desk()

  if not cy then return end

  -- Across first, when the page is wider than the desk.
  local w = page_px(set.pages[1])
  local most = math.max(0, w + 2 * GAP - dw)

  if cx > dx + dw - 24 then
    across = math.min(most, across + (cx - (dx + dw - 24)))
  elseif cx < dx + 24 then
    across = math.max(0, across - (dx + 24 - cx))
  end

  across = math.min(across, most)

  if cy < dy + 8 then
    scroll_to(top - (dy + 8 - cy))
  elseif cy + ch > H - 8 then
    scroll_to(top + (cy + ch - (H - 8)))
  end
end

local function zoom_to(z)
  z = math.max(1, math.min(#ZOOMS, z))

  if z == zoom then return end

  local _, _, _, dh = desk()
  local middle = (top + dh / 2) / scale()

  zoom = z
  scroll_to(math.floor(middle * scale() - dh / 2))
  frame()
  report()
end

--------------------------------------------------------------------------
-- Editing and formatting.
--------------------------------------------------------------------------

local function changed()
  version = version + 1
  frame()
end

--
-- **An edit**: the body before it kept for undo - one entry for a run of
-- typing, one for anything else - the new one set, the caret moved. A
-- format keeps the selection, so a word made bold can be made italic next.
--
-- What an undo puts back: the body and the document's settings, each a
-- table no edit changes in place.
local function snapshot()
  return { body = doc.body, caret = caret, paper = doc.paper,
           margins_mm = doc.margins_mm, header = doc.header, footer = doc.footer }
end

-- The pages' count and size, so the log hears when they change.
local shape = nil

local function report_if_changed()
  local now = ("%d:%s:%s"):format(#set.pages, set.pages[1].width_pt, set.pages[1].height_pt)

  if now ~= shape then
    shape = now
    report()
  end
end

local function edited(body, place, kind, keep)
  if kind ~= "type" or last_kind ~= "type" then
    undo[#undo + 1] = snapshot()
    if #undo > UNDO_MOST then table.remove(undo, 1) end
  end

  redo = {}
  last_kind = kind
  doc.body = body
  caret, column = place, nil

  if not keep then anchor = nil end

  dirty = true
  said = nil
  set = pageset.set(doc, measure, cache)
  follow()
  changed()
  report_if_changed()
end

--
-- **A change to the document's settings** - the paper, the margins, a
-- header or a footer - each a new table in place of the old, checked as a
-- file's would be, and undone as an edit is. A run of one kind - the
-- header's words typed - is one step to undo.
--
function doc_edit(kind, fields)
  if kind ~= last_kind or kind ~= "header_text" then
    undo[#undo + 1] = snapshot()
    if #undo > UNDO_MOST then table.remove(undo, 1) end
  end

  redo = {}
  last_kind = kind

  local t = {}
  for k, v in pairs(doc) do t[k] = v end
  for k, v in pairs(fields) do t[k] = v end

  local checked = writedoc.check(t)

  if not checked then return end

  doc.paper, doc.margins_mm = checked.paper, checked.margins_mm
  doc.header, doc.footer = checked.header, checked.footer
  dirty = true
  said = nil
  set = pageset.set(doc, measure, cache)
  changed()
  report_if_changed()
end

local function without_selection()
  if not selected() then return doc.body, caret end
  return richtext.delete(doc.body, anchor, caret)
end

local function type_text(text)
  local body, place = without_selection()
  body, place = richtext.type(body, place, text, pending)
  edited(body, place, "type")
end

local function back_or_forward(forward)
  if selected() then
    local body, place = richtext.delete(doc.body, anchor, caret)
    edited(body, place, "delete")
    return
  end

  local other = richtext.step(doc.body, caret, forward)

  if other.para == caret.para and other.at == caret.at then return end

  local body, place = richtext.delete(doc.body, caret, other)
  edited(body, place, "delete")
end

-- Character fields over the selection, or for what is typed next.
function apply_char(fields)
  if selected() then
    local a, b = range()
    edited(richtext.format(doc.body, a, b, fields, by_name), caret, "format", true)
  else
    for k, v in pairs(fields) do pending[k] = v end
    frame()
  end
end

-- Paragraph fields over the paragraphs the selection or the caret is in.
function apply_para(fields)
  local a, b = range()
  edited(richtext.arrange(doc.body, a, b, fields, by_name), caret, "format", true)
end

function apply_style(style_name)
  local a, b = range()
  edited(richtext.restyle(doc.body, a, b, style_name, by_name), caret, "format", true)
end

local function swap(from, to)
  local entry = table.remove(from)

  if not entry then return end

  to[#to + 1] = snapshot()
  doc.body, caret, anchor, column = entry.body, entry.caret, nil, nil
  doc.paper, doc.margins_mm = entry.paper, entry.margins_mm
  doc.header, doc.footer = entry.header, entry.footer
  last_kind = nil
  dirty = true
  set = pageset.set(doc, measure, cache)
  follow()
  changed()
  report_if_changed()
end

local function move(place, extend, keep_column)
  local had = selected()

  if extend then anchor = anchor or caret else anchor = nil end

  caret = place
  last_kind = nil
  pending = {}

  if not keep_column then column = nil end

  if had or selected() then version = version + 1 end

  follow()
  frame()
end

local function save()
  if not path then
    local n = 1

    repeat
      path = ("/Home/Untitled%s.write"):format(n == 1 and "" or (" " .. n))
      n = n + 1
    until not fs.getattr(path)

    name = path:match("([^/]+)$")
  end

  local ok, why = writedoc.save(path, doc)

  if ok then
    dirty = false
    said = "Saved " .. name
    print(("writer: saved %s, %d paragraphs"):format(path, #doc.body))
  else
    said = "Not saved: " .. tostring(why)
    print("writer: not saved: " .. tostring(why))
  end

  frame()
end

local function export()
  local to = path and path:gsub("%.write$", "") .. ".pdf" or "/Home/Untitled.pdf"
  local ok, notes = pdf.write(to, set, measure,
                              { title = (name:gsub("%.write$", "")) })

  if ok then
    said = ("Exported %s  -  %d page%s, %d KB"):format(to:match("([^/]+)$"),
      notes.pages, notes.pages == 1 and "" or "s", (notes.bytes + 1023) // 1024)

    if notes.missing > 0 then
      said = said .. (", %d character%s its faces lack"):format(notes.missing,
        notes.missing == 1 and "" or "s")
    end

    print(("writer: exported %s, %d pages, %d bytes"):format(to, notes.pages,
                                                            notes.bytes))
  else
    said = "Export stopped: " .. tostring(notes)
    print("writer: export stopped: " .. tostring(notes))
  end

  frame()
end

--------------------------------------------------------------------------
-- Menus.
--------------------------------------------------------------------------

function open_menu(kind, x, y, w, items, chosen, pick)
  local box = { x = x, y = y, w = w, items = items, chosen = chosen }
  local h = #items * 30 + 8

  -- Up from its control when it would run off the window's foot.
  if y + h > H - 4 then box.y = math.max(TOOLS_H, y - h - 40) end

  box.h = h
  menu = { box = box, pick = pick }
  print(("writer: menu %s at %d,%d %dx%d, %d items"):format(kind, box.x, box.y,
                                                           box.w, h, #items))
  frame()
end

local TOOL_ACTS = {
  zoom = function(t)
    local labels = {}
    for i, z in ipairs(ZOOMS) do labels[i] = z .. "%" end
    open_menu("zoom", t.x, TOOLS_H + 2, 120, labels, zoom,
              function(i) zoom_to(i) end)
  end,
  export = function(t)
    local items = { "Save as .write", "Export PDF" }
    open_menu("export", math.min(t.x, W - 220), TOOLS_H + 2, 200, items, nil,
              function(i)
                if i == 1 then save() else export() end
              end)
  end,
  format = function()
    panel = (panel == "text") and nil or "text"
    version = version + 1
    frame()
  end,
  document = function()
    panel = (panel == "document") and nil or "document"
    version = version + 1
    frame()
  end,
  view = function()
    thumbs = not thumbs
    version = version + 1
    print(("writer: thumbnails %s"):format(thumbs and "shown" or "hidden"))
    frame()
  end,
  --
  -- **Add Page**: a paragraph after the caret's that begins a page of its
  -- own, the caret in it - Pages' blank page, made of a page break.
  --
  addpage = function()
    local p = doc.body[caret.para]
    local at_end = { para = caret.para, at = #richtext.plain(p) + 1 }
    local body, place = richtext.split(doc.body, at_end, by_name)

    body = richtext.arrange(body, place, place, { page_break_before = true }, by_name)
    edited(body, place, "page")
  end,
}

--------------------------------------------------------------------------
-- What a person does.
--------------------------------------------------------------------------

local sink = ui.view{ x = 0, y = 0, w = W, h = H }
sink.focusable = true

local utf8_pending = ""

function sink:wheel(n)
  scroll_to(top - n * 3 * 40)
  frame()
  return true
end

function sink:key(c)
  local k, mods = keys.parts(c)
  local shift = (mods & keys.SHIFT) ~= 0
  local ctrl = (mods & keys.CTRL) ~= 0

  if menu then
    if c == 27 then menu = nil frame() return true end
    return true
  end

  if focus == "header" then
    local text = doc.header.text

    if c == 13 or c == 10 or c == 27 or c == 9 then
      focus = nil
      last_kind = nil
      frame()
    elseif c == 8 or c == 127 then
      if #text > 0 then
        local cut = #text

        while cut > 1 and text:byte(cut) >= 0x80 and text:byte(cut) < 0xC0 do cut = cut - 1 end

        doc_edit("header_text", { header = { on = doc.header.on, from_top_mm = doc.header.from_top_mm,
                                             text = text:sub(1, cut - 1) } })
      end
    elseif c >= 32 and c < 127 then
      doc_edit("header_text", { header = { on = doc.header.on, from_top_mm = doc.header.from_top_mm,
                                           text = text .. string.char(c) } })
    end

    return true
  end

  if k == keys.LEFT or k == keys.RIGHT then
    move(richtext.step(doc.body, caret, k == keys.RIGHT), shift)
  elseif k == keys.UP or k == keys.DOWN then
    local place
    place, column = pageset.vertical(set, measure, caret, k == keys.UP and -1 or 1,
                                     column)
    move(place, shift, true)
  elseif k == keys.HOME or k == keys.END then
    if ctrl then
      local last = #doc.body
      move(k == keys.HOME and { para = 1, at = 1 }
           or { para = last, at = #richtext.plain(doc.body[last]) + 1 }, shift)
    else
      local home, finish = pageset.line_ends(set, measure, caret)
      move(k == keys.HOME and home or finish, shift)
    end
  elseif k == keys.PAGEUP or k == keys.PAGEDOWN then
    local _, _, _, dh = desk()
    scroll_to(top + (k == keys.PAGEUP and -1 or 1) * (dh - 40))
    frame()
  elseif k == keys.DELETE and mods == 0 then
    back_or_forward(true)
  elseif mods ~= 0 then
    return false
  elseif c == 8 or c == 127 then
    back_or_forward(false)
  elseif c == 13 or c == 10 then
    local p = doc.body[caret.para]

    -- **Return on an empty list item ends the list**, as Pages does: the
    -- way out of a list is the key that went on with it.
    if not selected() and p.list and p.list ~= "none" and richtext.plain(p) == "" then
      edited(richtext.arrange(doc.body, caret, caret, { list = "none" }, by_name),
             caret, "return")
    else
      local body, place = without_selection()
      body, place = richtext.split(body, place, by_name)
      edited(body, place, "return")
    end
  elseif c == 9 then
    type_text("\t")
  elseif c == 1 then                                     -- Control-A
    anchor = { para = 1, at = 1 }
    local last = #doc.body
    caret = { para = last, at = #richtext.plain(doc.body[last]) + 1 }
    changed()
  elseif c == 2 then                                     -- Control-B
    local look = shown_look()
    apply_char({ weight = (faces.WEIGHT[look.weight] or 400) >= 600 and "Regular"
                          or "Bold" })
  elseif c == 21 then                                    -- Control-U
    apply_char({ underline = not shown_look().underline })
  elseif c == 19 then save()                             -- Control-S
  elseif c == 5 then export()                            -- Control-E
  elseif c == 26 then swap(undo, redo)                   -- Control-Z
  elseif c == 25 then swap(redo, undo)                   -- Control-Y
  elseif c >= 32 and c < 127 then
    utf8_pending = ""
    type_text(string.char(c))
  elseif c >= 128 and c < 256 then
    utf8_pending = utf8_pending .. string.char(c)

    if utf8.len(utf8_pending) then
      local text = utf8_pending
      utf8_pending = ""
      type_text(text)
    elseif #utf8_pending >= 4 then
      utf8_pending = ""
    end
  else
    return false
  end

  return true
end

function sink:edit(kind)
  if kind == "selectall" then
    anchor = { para = 1, at = 1 }
    local last = #doc.body
    caret = { para = last, at = #richtext.plain(doc.body[last]) + 1 }
    changed()
    return true
  end

  if kind == "copy" or kind == "cut" then
    if not selected() then return false end
    if not wmproto.copy(richtext.text(doc.body, anchor, caret)) then return false end

    if kind == "cut" then
      local body, place = richtext.delete(doc.body, anchor, caret)
      edited(body, place, "cut")
    end

    return true
  end

  if kind == "paste" then
    local text = wmproto.paste()

    if not text or text == "" then return false end

    local body, place = without_selection()
    body, place = richtext.type(body, place, text)
    edited(body, place, "paste")
    return true
  end

  return false
end

local function page_point(x, y)
  for i = 1, #set.pages do
    local px, py = page_at(i)
    local w, h = page_px(set.pages[i])

    if y >= py - GAP // 2 and y < py + h + GAP // 2 then
      return i, (x - px) / scale(), (y - py) / scale()
    end
  end
end

local pressed = false

function sink:mouse(action, x, y)
  if action == "release" then
    pressed = false
    return true
  end

  if action ~= "press" and not (action == "move" and pressed) then
    return true
  end

  if action == "press" and menu then
    local i = pk.menu_at(menu.box, x, y)
    local pick = menu.pick

    menu = nil

    if i then pick(i) else frame() end

    return true
  end

  if action == "press" and focus and not (panel and x >= W - PANEL_W) then
    focus = nil
    last_kind = nil
  end

  if action == "press" and thumbs and x < THUMBS_W then
    for _, b in ipairs(thumb_boxes) do
      if pk.inside(b, x, y) then
        local _, py = page_at(b.page)
        scroll_to(top + (py - TOOLS_H) - GAP)
        frame()
        return true
      end
    end

    return true
  end

  if action == "press" and y < TOOLS_H then
    for _, t in ipairs(TOOLS) do
      if t.key and t.x and not t.later and x >= t.x and x < t.x + t.w then
        local act = TOOL_ACTS[t.key]
        if act then act(t) end
        return true
      end
    end

    return true
  end

  if action == "press" and panel and x >= W - PANEL_W then
    focus = nil

    for _, c in ipairs(controls) do
      if pk.inside(c.box, x, y) then
        c.act(x, y)
        frame()
        return true
      end
    end

    return true
  end

  local dx, _, dw = desk()

  if x >= dx + dw then return true end

  local page, px, py = page_point(x, y)
  local place = page and pageset.hit(set, measure, page, px, py)

  if place then
    if action == "press" then
      pressed = true
      if selected() then version = version + 1 end
      anchor, caret, column = place, place, nil
      last_kind = nil
      pending = {}
      frame()
    else
      caret = place
      version = version + 1
      frame()
    end
  end

  return true
end

win:add(sink)
frame()
report()

for _, t in ipairs(TOOLS) do
  if t.key and t.x then
    print(("writer: tool %s at %d,%d %dx%d"):format(t.key, t.x, t.y, t.w, 48))
  end
end

shape = ("%d:%s:%s"):format(#set.pages, set.pages[1].width_pt, set.pages[1].height_pt)

win:run()
