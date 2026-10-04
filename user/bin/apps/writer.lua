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
--   Shift-Return                           a line break, in the paragraph
--   in a table: Tab, Shift-Tab             the next cell - a new row after
--                                          the last - and the one before
--               Return                     the cell below, or out of it
--   in a text box: Return                  a line break
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
local docxwrite = use("/Kosmos/Libraries/docxwrite.lua")
local pk        = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
local hyphen    = use("/Kosmos/Libraries/hyphen.lua")

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
local NOTED  = 0xfffbeab2       -- words a comment is about (W7d)

local ZOOMS = { 50, 75, 100, 125, 150, 200, 300 }
local SIZES = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 30, 36, 48, 64, 72 }
local SPACINGS = { 1, 1.15, 1.2, 1.5, 2 }

-- The charts the Chart tool offers (W7c).
local CHART_KINDS = { { "column", "Column" }, { "bar", "Bar" }, { "line", "Line" }, { "pie", "Pie" } }

-- The shapes the Shape tool offers (W7b), in Pages' order.
local SHAPE_KINDS = {
  { "rectangle", "Rectangle" }, { "rounded", "Rounded rectangle" }, { "oval", "Oval" },
  { "triangle", "Triangle" }, { "star", "Star" }, { "arrow", "Arrow" },
}

-- What a text box may be filled with: nothing, or a pale tint.
local FILLS = {
  { false, "None" }, { "#eef3fb", "Mist" }, { "#fdf3e1", "Sand" }, { "#e8f6ee", "Mint" },
  { "#fde8e8", "Rose" }, { "#f1eafa", "Lilac" }, { "#f2f2f2", "Grey" },
}

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

--
-- **The document's pictures** (W5): each one's bytes, as they came or as
-- the `.write` file holds them, and decoded once when first shown - read
-- from the file only when a page needs them.
--
local pictures = {}

local function decode(bytes)
  local ok, surface

  if bytes:sub(1, 2) == "\xff\xd8" then
    ok, surface = pcall(gfx.jpeg, bytes)
  else
    ok, surface = pcall(gfx.png, bytes)
  end

  return ok and surface or nil
end

local function picture(name)
  local p = pictures[name]

  if p == nil and path then
    local bytes = use("/Kosmos/Libraries/zip.lua").read(path, name, 64 * 1024 * 1024)
    p = bytes and { bytes = bytes } or false
    pictures[name] = p
  end

  if not p then return nil end

  if p.surface == nil then p.surface = decode(p.bytes) or false end

  return p.surface and p or nil
end

local drawer = pagedraw.new(measure, function(name)
  local p = picture(name)
  return p and p.surface
end)
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

-- The chart whose data is shown as a table above it, for typing into
-- (W7c): its paragraph's number, or nil. The window's, not the document's.
local data_open = nil

-- The pages set again: with the document's language's hyphenation when it
-- says so (`hyphen.lua`), and the chart's data when it is shown.
local function setting()
  return pageset.set(doc, measure, cache,
                     { hyphenate = doc.hyphenation and hyphen.language(doc.language) or nil,
                       data = data_open })
end

local set = setting()
local zoom = 4                  -- 125%, an index into ZOOMS
local top = 0                   -- how far down the desk the view is
local across = 0                -- and how far across, when a page is wider

local caret = { para = 1, at = 1 }

--
-- **A chart's data is open while the caret is in it**: in one of its cells
-- it is shown, on the chart it stays as it was, anywhere else it closes.
-- True when that changed, so the pages are set again.
--
local function fit_data()
  local p = doc.body[caret.para]
  local chart = p and p.table and p.table.chart
  local want = nil

  if chart and (caret.row or data_open == caret.para) then want = caret.para end

  if want ~= data_open then
    data_open = want
    return true
  end

  return false
end
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
--
-- **Facing pages show as spreads**, as a book opens: the first page alone
-- on the right, then each even page on the left of the odd one after it.
-- Otherwise one page under another.
--
local SPREAD_GAP = 4

local function row_of(i)
  if set.facing then return i // 2 end
  return i - 1
end

local function content_width()
  local w = page_px(set.pages[1])
  return set.facing and (2 * w + SPREAD_GAP) or w
end

local function page_at(i)
  local dx, dy, dw = desk()
  local y = GAP
  local row = row_of(i)
  local k = 1

  -- The rows above this page's, each as tall as its tallest page.
  for r = 0, row - 1 do
    local tallest = 0

    while set.pages[k] and row_of(k) == r do
      local _, h = page_px(set.pages[k])
      tallest = math.max(tallest, h)
      k = k + 1
    end

    y = y + tallest + GAP
  end

  local w = page_px(set.pages[i])
  local cw = content_width()

  -- What is wider than the desk scrolls across; what is narrower is centred.
  local left = (cw + 2 * GAP > dw) and (dx + GAP - across) or (dx + (dw - cw) // 2)
  local x = left

  if set.facing then
    x = (i % 2 == 0) and left or (left + w + SPREAD_GAP)
  end

  return x, dy + y - top
end

local function desk_height()
  local _, y = page_at(#set.pages)
  local _, h = page_px(set.pages[#set.pages])
  return y + top - TOOLS_H + h + GAP
end

local drawn = {}

local function selected()
  return anchor and (anchor.para ~= caret.para or anchor.at ~= caret.at
                     or anchor.row ~= caret.row or anchor.col ~= caret.col)
end

-- Where the comments stand, worked out once for what the pages show.
local noted = { version = nil, marks = {} }

local function marks_on(i)
  local out = {}

  if noted.version ~= version then
    noted.version = version
    noted.marks = pageset.comment_marks(set, measure, doc.body, doc.comments)
  end

  for _, c in ipairs(noted.marks) do
    for _, r in ipairs(c.rects) do
      if r.page == i then
        out[#out + 1] = { page = r.page, x_pt = r.x_pt, y_pt = r.y_pt, w_pt = r.w_pt,
                          h_pt = r.h_pt, colour = NOTED }
      end
    end
  end

  if selected() then
    for _, r in ipairs(pageset.selection(set, measure, anchor, caret)) do
      if r.page == i then
        r.colour = CHOSEN
        out[#out + 1] = r
      end
    end
  end

  return #out > 0 and out or nil
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
    local p = doc.body[a.para]
    local plain = richtext.plain(a.row and p.table.rows[a.row][a.col] or p)
    place = { para = a.para, at = math.min(a.at + 1, #plain + 1), row = a.row, col = a.col }
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
  { key = "insert",   icon = "insert",   text = "Insert" },
  { key = "table",    icon = "table",    text = "Table" },
  { key = "chart",    icon = "chart",    text = "Chart" },
  { key = "textbox",  icon = "textbox",  text = "Text" },
  { key = "shape",    icon = "shape",    text = "Shape" },
  { key = "media",    icon = "pictures", text = "Media" },
  { key = "comment",  icon = "comment",  text = "Comment" },
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

local open_menu, apply_char, apply_para, apply_style, doc_edit, apply_picture
local reshape, delete_table, table_key, line_break, apply_shape, insert_shape
local remove_block, toggle_data, uncomment

local function say_where()
  local here = doc.body[caret.para]
  local kind = (panel == "text" and richtext.comment_at(doc.body, caret) and ":comment" or "")
               .. (here and (here.picture and ":picture" or here.shape and ":shape"
                         or here.table and (here.table.box and ":box"
                                            or here.table.chart and ":chart" or ":table")) or "")
  local key = panel == "text" and ("text:" .. part .. kind) or tostring(panel)

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

  local lig = { x = x0 + w0 // 2, y = y + 4, text = "Ligatures", on = doc.ligatures }
  pk.check(s, lig)
  control("ligatures", lig, function()
    doc_edit("ligatures", { ligatures = not doc.ligatures })
  end)

  y = y + 34

  local fc = { x = x0, y = y, text = "Facing pages", on = doc.facing }
  pk.check(s, fc)
  control("facing", fc, function()
    doc_edit("facing", { facing = not doc.facing })
  end)

  local hy = { x = x0 + w0 // 2, y = y, text = "Hyphenation", on = doc.hyphenation }
  pk.check(s, hy)
  control("hyphenation", hy, function()
    doc_edit("hyphenation", { hyphenation = not doc.hyphenation })
  end)

  y = y + 30

  -- The language the document is hyphenated in.
  local tags = { "en-us", "es" }
  local words = {}
  for i, t in ipairs(tags) do words[i] = writedoc.LANGUAGES[t] end

  s:text(x0, y + (30 - gfx.height()) // 2, "Language", theme.text_dim, nil, "ui")

  local lang = { x = x0 + 120, y = y, w = w0 - 120,
                 text = writedoc.LANGUAGES[doc.language] or doc.language }
  pk.chooser(s, lang)
  control("language", lang, function()
    open_menu("language", lang.x, lang.y + lang.h + 4, lang.w, words,
              index_of(tags, doc.language),
              function(i) doc_edit("language", { language = tags[i] }) end)
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

  local here = doc.body[caret.para]

  --
  -- **In words a comment is about** (W7d): the comment's words in a field
  -- to type into, and the comment taken away.
  --
  local noted_id = panel == "text" and richtext.comment_at(doc.body, caret)

  if noted_id then
    local words = ""
    for _, c in ipairs(doc.comments) do if c.id == noted_id then words = c.text end end

    pk.label(s, x0, y, "Comment")
    y = y + 18

    local field = { x = x0, y = y, w = w0, h = 30 }
    pk.field(s, field, focus == "comment")

    local shown = words
    while #shown > 0 and gfx.measure(shown) > field.w - 20 do shown = shown:sub(2) end

    if shown == "" and focus ~= "comment" then
      s:text(field.x + 10, field.y + (30 - gfx.height()) // 2, "Type a comment", theme.text_dim, nil, "ui")
    else
      s:text(field.x + 10, field.y + (30 - gfx.height()) // 2, shown, theme.text, nil, "ui")
    end

    if focus == "comment" then
      s:fill(field.x + 10 + gfx.measure(shown) + 1, field.y + 7, 2, 16, theme.ring)
    end

    control("comment_text", field, function() focus = "comment" end)

    y = y + 38

    local del = { x = x0, y = y, text = "Delete comment" }
    pk.button(s, del)
    control("comment_delete", del, function() uncomment(noted_id) end)

    y = y + 44
  end

  if panel == "text" and here and here.picture then
    local pic = here.picture

    pk.label(s, x0, y, "Picture")
    y = y + 18

    s:text(x0, y + (30 - gfx.height()) // 2, "Width", theme.text_dim, nil, "ui")

    local wd = { x = x0 + 120, y = y, w = w0 - 120, text = ("%.0f mm"):format(pic.width_mm) }
    pk.stepper(s, wd)
    control("picture_width", wd, function(cx, cy)
      local d = pk.step_at(wd, cx, cy)

      if d and d ~= 0 then apply_picture(math.max(10, pic.width_mm + 5 * d)) end
    end)

    y = y + 38
    s:text(x0, y, ("Height %.0f mm, kept in proportion"):format(pic.height_mm),
           theme.text_dim, nil, "ui")

    say_where(s)
    return
  end

  if panel == "document" then
    draw_document(s, x0, w0, y)
    say_where(s)
    return
  end

  --
  -- **On a chart** (W7c): which kind, how tall, its data shown to type
  -- into - and while it is, how many categories and series - and the
  -- chart taken out.
  --
  if here and here.table and here.table.chart then
    local t = here.table
    local ch = t.chart

    pk.label(s, x0, y, "Chart")
    y = y + 18

    local kinds = { x = x0, y = y, w = w0, items = {} , chosen = 1 }
    for i, k in ipairs(CHART_KINDS) do
      kinds.items[i] = { text = k[2] }
      if k[1] == ch.kind then kinds.chosen = i end
    end
    pk.segments(s, kinds)
    control("chart_kind", kinds, function(cx, cy)
      local i = pk.segment_at(kinds, cx, cy)
      if i then reshape({ chart = { kind = CHART_KINDS[i][1], height_mm = ch.height_mm } }) end
    end)

    y = y + kinds.h + 10

    local ht = { x = x0, y = y, w = w0, text = ("%g mm tall"):format(ch.height_mm) }
    pk.stepper(s, ht)
    control("chart_height", ht, function(cx, cy)
      local d = pk.step_at(ht, cx, cy)
      if d and d ~= 0 then
        reshape({ chart = { kind = ch.kind, height_mm = math.max(20, ch.height_mm + 10 * d) } })
      end
    end)

    y = y + 38

    local ed = { x = x0, y = y + 4, text = "Edit data", on = data_open == caret.para }
    pk.check(s, ed)
    control("chart_data", ed, function() toggle_data() end)

    y = y + 34

    if data_open == caret.para then
      -- A row each: "4 categories" is more than half the panel holds.
      local cats = { x = x0, y = y, w = w0,
                     text = ("%d categor%s"):format(#t.rows - 1, #t.rows == 2 and "y" or "ies") }
      pk.stepper(s, cats)
      control("chart_categories", cats, function(cx, cy)
        local d = pk.step_at(cats, cx, cy)
        if d and d ~= 0 then reshape({ rows = math.max(2, #t.rows + d) }) end
      end)

      y = y + 38

      local sers = { x = x0, y = y, w = w0, text = ("%d series"):format(t.columns - 1) }
      pk.stepper(s, sers)
      control("chart_series", sers, function(cx, cy)
        local d = pk.step_at(sers, cx, cy)
        if d and d ~= 0 then reshape({ columns = math.max(2, t.columns + d) }) end
      end)

      y = y + 38
    end

    local del = { x = x0, y = y + 6, text = "Delete chart" }
    pk.button(s, del)
    control("chart_delete", del, function() delete_table() end)

    say_where(s)
    return
  end

  --
  -- **On a shape** (W7b): which shape, its width and height, its colour,
  -- and the shape taken out.
  --
  if here and here.shape then
    local sh = here.shape

    pk.label(s, x0, y, "Shape")
    y = y + 18

    local kind_name = sh.kind
    for _, k in ipairs(SHAPE_KINDS) do if k[1] == sh.kind then kind_name = k[2] end end

    local kc = { x = x0, y = y, w = w0, text = kind_name }
    pk.chooser(s, kc)
    control("shape_kind", kc, function()
      local labels, chosen = {}, nil
      for i, k in ipairs(SHAPE_KINDS) do
        labels[i] = k[2]
        if k[1] == sh.kind then chosen = i end
      end
      open_menu("shape_kind", kc.x, kc.y + kc.h + 4, kc.w, labels, chosen,
                function(i) apply_shape({ kind = SHAPE_KINDS[i][1] }) end)
    end)

    y = y + kc.h + 8

    local half = (w0 - 8) // 2
    local wd = { x = x0, y = y, w = half, text = ("%g mm wide"):format(sh.width_mm) }
    pk.stepper(s, wd)
    control("shape_width", wd, function(cx, cy)
      local d = pk.step_at(wd, cx, cy)
      if d and d ~= 0 then apply_shape({ width_mm = math.max(5, sh.width_mm + 5 * d) }) end
    end)

    local ht = { x = x0 + half + 8, y = y, w = w0 - half - 8,
                 text = ("%g mm high"):format(sh.height_mm) }
    pk.stepper(s, ht)
    control("shape_height", ht, function(cx, cy)
      local d = pk.step_at(ht, cx, cy)
      if d and d ~= 0 then apply_shape({ height_mm = math.max(5, sh.height_mm + 5 * d) }) end
    end)

    y = y + 38
    pk.label(s, x0, y, "Fill")
    y = y + 18

    local sw = { x = x0, y = y, w = 40, h = 22, colour = argb(sh.fill) }
    pk.swatch(s, sw)
    s:text(x0 + 52, y + (22 - gfx.height()) // 2, colour_name(sh.fill), theme.text, nil, "ui")
    local fill_box = { x = x0, y = y, w = w0, h = 22 }
    control("shape_fill", fill_box, function()
      local labels, chosen = {}, nil
      for i, pair in ipairs(COLOURS) do
        labels[i] = pair[2]
        if pair[1] == sh.fill then chosen = i end
      end
      open_menu("shape_fill", x0, fill_box.y + 26, 160, labels, chosen,
                function(i) apply_shape({ fill = COLOURS[i][1] }) end)
    end)

    y = y + 36

    local del = { x = x0, y = y, text = "Delete shape" }
    pk.button(s, del)
    control("shape_delete", del, function()
      remove_block(caret.para, { para = math.max(1, caret.para - 1), at = 1 })
    end)

    say_where(s)
    return
  end

  --
  -- **In a table** (W5b): its rows and columns, a stepper each, whether its
  -- first row is a header, and the table taken out - above the text's
  -- part, which works on the cell's text.
  --
  --
  -- **In a text box** (W7a): its width, its fill and its border, and the
  -- box taken out.
  --
  if here and here.table and here.table.box and caret.row then
    local box = here.table.box

    pk.label(s, x0, y, "Text box")
    y = y + 18

    local half = (w0 - 8) // 2
    local wd = { x = x0, y = y, w = half, text = ("%g mm wide"):format(box.width_mm) }
    pk.stepper(s, wd)
    control("box_width", wd, function(cx, cy)
      local d = pk.step_at(wd, cx, cy)
      if d and d ~= 0 then
        reshape({ box = { width_mm = math.max(10, box.width_mm + 5 * d),
                          border = box.border, fill = box.fill } })
      end
    end)

    local fill_name = "None"
    for _, f in ipairs(FILLS) do if f[1] == box.fill then fill_name = f[2] end end

    local fc = { x = x0 + half + 8, y = y, w = w0 - half - 8, text = fill_name }
    pk.chooser(s, fc)
    control("box_fill", fc, function()
      local labels, chosen = {}, 1
      for i, f in ipairs(FILLS) do
        labels[i] = f[2]
        if f[1] == (box.fill or false) then chosen = i end
      end
      open_menu("fill", fc.x, fc.y + fc.h + 4, fc.w, labels, chosen, function(i)
        reshape({ box = { width_mm = box.width_mm, border = box.border,
                          fill = FILLS[i][1] or nil } })
      end)
    end)

    y = y + 38

    local bd = { x = x0, y = y + 4, text = "Border", on = box.border }
    pk.check(s, bd)
    control("box_border", bd, function()
      reshape({ box = { width_mm = box.width_mm, border = not box.border, fill = box.fill } })
    end)

    local del = { x = 0, y = y, text = "Delete box" }
    del.w = pk.button_width(del.text)
    del.x = x0 + w0 - del.w
    pk.button(s, del)
    control("box_delete", del, function() delete_table() end)

    y = y + 44
  elseif here and here.table and caret.row then
    local t = here.table

    pk.label(s, x0, y, "Table")
    y = y + 18

    local half = (w0 - 8) // 2
    local rows = { x = x0, y = y, w = half,
                   text = ("%d row%s"):format(#t.rows, #t.rows == 1 and "" or "s") }
    pk.stepper(s, rows)
    control("table_rows", rows, function(cx, cy)
      local d = pk.step_at(rows, cx, cy)
      if d and d ~= 0 then reshape({ rows = #t.rows + d }) end
    end)

    local cols = { x = x0 + half + 8, y = y, w = w0 - half - 8,
                   text = ("%d column%s"):format(t.columns, t.columns == 1 and "" or "s") }
    pk.stepper(s, cols)
    control("table_columns", cols, function(cx, cy)
      local d = pk.step_at(cols, cx, cy)
      if d and d ~= 0 then reshape({ columns = t.columns + d }) end
    end)

    y = y + 38

    local hr = { x = x0, y = y + 4, text = "Header row", on = t.header }
    pk.check(s, hr)
    control("table_header", hr, function() reshape({ header = not t.header }) end)

    local del = { x = 0, y = y, text = "Delete table" }
    del.w = pk.button_width(del.text)
    del.x = x0 + w0 - del.w
    pk.button(s, del)
    control("table_delete", del, function() delete_table() end)

    y = y + 44
  end

  -- The paragraph style, in a box of its own, its name large.
  local sbox = { x = x0, y = y, w = w0, h = here and here.table and 40 or 46 }
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
  local cx, cy, ch, here = caret_px()
  local dx, dy, dw = desk()

  if not cy then return end

  -- Across first, when the page - or a spread - is wider than the desk:
  -- the caret always, and **the whole of its line where that fits** - a
  -- chart or a shape the caret is on, not only its left edge.
  local most = math.max(0, content_width() + 2 * GAP - dw)
  local px = page_at(here.page)
  local right = px + math.floor((here.line.x_pt + here.line.width_pt
                                 + set.pages[here.page].shift_pt) * scale() + 0.5)
  local by = math.max(cx - (dx + dw - 24),
                      math.min(right - (dx + dw - 24), cx - (dx + 24)))

  if by > 0 then
    across = math.min(most, across + by)
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
  return { body = doc.body, caret = caret, paper = doc.paper, comments = doc.comments,
           margins_mm = doc.margins_mm, header = doc.header, footer = doc.footer,
           facing = doc.facing, hyphenation = doc.hyphenation,
           ligatures = doc.ligatures, language = doc.language }
end

-- The pages' count and size, so the log hears when they change.
local shape = nil

local function report_if_changed()
  local now = ("%d:%s:%s:%s"):format(#set.pages, set.pages[1].width_pt, set.pages[1].height_pt,
                                     tostring(set.facing))

  if now ~= shape then
    shape = now
    report()
  end
end

local function edited(body, place, kind, keep, comments)
  if kind ~= "type" or last_kind ~= "type" then
    undo[#undo + 1] = snapshot()
    if #undo > UNDO_MOST then table.remove(undo, 1) end
  end

  redo = {}
  last_kind = kind
  doc.body = body

  if comments then doc.comments = comments end
  caret, column = place, nil

  if not keep then anchor = nil end

  fit_data()
  dirty = true
  said = nil
  set = setting()
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
  if kind ~= last_kind or (kind ~= "header_text" and kind ~= "comment_text") then
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
  doc.facing, doc.hyphenation = checked.facing, checked.hyphenation
  doc.ligatures, doc.language = checked.ligatures, checked.language
  doc.comments = checked.comments
  dirty = true
  said = nil
  set = setting()
  changed()
  report_if_changed()
end

local move

local function without_selection()
  if not selected() then return doc.body, caret end
  return richtext.delete(doc.body, anchor, caret)
end

-- A paragraph after the picture or shape the caret is on, for what is
-- typed there.
local function after_picture(body, place)
  local p = body[place.para]

  if p and (p.picture or p.shape or (p.table and p.table.chart and not place.row)) then
    local out = {}
    for i, p in ipairs(body) do out[i] = p end
    table.insert(out, place.para + 1, { style = writedoc.BODY, runs = {} })
    return out, { para = place.para + 1, at = 1 }
  end

  return body, place
end

-- The picture or shape paragraph `n` taken out.
function remove_block(n, place)
  local out = {}
  for i, p in ipairs(doc.body) do out[i] = p end
  table.remove(out, n)
  edited(out, place, "delete")
end

local function type_text(text)
  local body, place = without_selection()
  body, place = after_picture(body, place)
  body, place = richtext.type(body, place, text, pending)
  edited(body, place, "type")
end

-- **A line break** (Shift-Return, and Return in a text box): a new line in
-- the same paragraph.
function line_break()
  local body, place = without_selection()
  body, place = after_picture(body, place)
  body, place = richtext.line_break(body, place, pending)
  edited(body, place, "type")
end

local function back_or_forward(forward)
  if selected() then
    local body, place = richtext.delete(doc.body, anchor, caret)
    edited(body, place, "delete")
    return
  end

  local here = doc.body[caret.para]

  if here.picture or here.shape or (here.table and here.table.chart and not caret.row) then
    remove_block(caret.para, { para = math.max(1, caret.para - (forward and 0 or 1)), at = 1 })
    return
  end

  local before = caret.para > 1 and doc.body[caret.para - 1]

  if not forward and caret.at == 1 and not caret.row and before
     and (before.picture or before.shape or (before.table and before.table.chart)) then
    remove_block(caret.para - 1, { para = caret.para - 1, at = 1 })
    return
  end

  local other = richtext.step(doc.body, caret, forward)

  if other.para == caret.para and other.at == caret.at and other.row == caret.row
     and other.col == caret.col then
    return
  end

  -- **A table's edges hold**: Backspace at a cell's start and Delete at
  -- its end do nothing, as in Pages; into a table from beside it, the
  -- caret goes in and nothing is taken.
  if caret.row and not (other.row and richtext.same_cell(caret, other)) then return end

  if other.row and not caret.row then
    move(other)
    return
  end

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

-- A shape's kind, size or colour.
function apply_shape(fields)
  local here = doc.body[caret.para]
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local raw = {}
  for k, v in pairs(here) do raw[k] = v end

  local sh = {}
  for k, v in pairs(here.shape) do sh[k] = v end
  for k, v in pairs(fields) do sh[k] = v end

  raw.shape = sh
  body[caret.para] = richtext.paragraph(raw, by_name, here.style)

  local now = body[caret.para].shape
  print(("writer: shape %s %g by %g mm, %s"):format(now.kind, now.width_mm, now.height_mm, now.fill))
  edited(body, caret, "format", true)
end

-- A picture's width, its height kept in proportion.
function apply_picture(width_mm)
  local here = doc.body[caret.para]
  local pic = here.picture
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local raw = {}
  for k, v in pairs(here) do raw[k] = v end
  raw.picture = { name = pic.name, width_mm = width_mm,
                  height_mm = pic.height_mm * width_mm / pic.width_mm }
  body[caret.para] = richtext.paragraph(raw, by_name, here.style)
  edited(body, caret, "format", true)
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
  doc.comments = entry.comments
  doc.paper, doc.margins_mm = entry.paper, entry.margins_mm
  doc.header, doc.footer = entry.header, entry.footer
  doc.facing, doc.hyphenation = entry.facing, entry.hyphenation
  doc.ligatures, doc.language = entry.ligatures, entry.language
  last_kind = nil
  dirty = true
  fit_data()
  set = setting()
  follow()
  changed()
  report_if_changed()
end

--
-- **A selection stays in its cell**, as a text field's does; one begun
-- outside a table that reaches into it takes the table whole, its end past
-- the table on the side it went.
--
local function clamp(place)
  if not anchor then return place end

  if anchor.row then
    if richtext.same_cell(anchor, place) then return place end

    local cell = doc.body[anchor.para].table.rows[anchor.row][anchor.col]

    return { para = anchor.para, row = anchor.row, col = anchor.col,
             at = richtext.before(place, anchor) and 1 or #richtext.plain(cell) + 1 }
  end

  if place.row then
    local n = place.para

    if richtext.before(anchor, place) and doc.body[n + 1] then
      return richtext.enter(doc.body, n + 1, true)
    elseif n > 1 then
      return richtext.enter(doc.body, n - 1, false)
    end
  end

  return place
end

function move(place, extend, keep_column)
  local had = selected()

  if extend then
    anchor = anchor or caret
    place = clamp(place)
  else
    anchor = nil
  end

  caret = place
  last_kind = nil
  pending = {}

  if not keep_column then column = nil end

  if had or selected() then version = version + 1 end

  if fit_data() then
    set = setting()
    version = version + 1
  end

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

  local shown, list = {}, {}

  for _, p in ipairs(doc.body) do
    local name = p.picture and p.picture.name

    if name and not shown[name] and picture(name) then
      shown[name] = true
      list[#list + 1] = { name = name, bytes = pictures[name].bytes }
    end
  end

  local ok, why = writedoc.save(path, doc, list)

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
                              { title = (name:gsub("%.write$", "")), pictures = picture,
                                comments = pageset.comment_marks(set, measure, doc.body,
                                                                 doc.comments) })

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

-- **Word's DOCX**, beside the document (`docxwrite.lua`, W6): a document
-- somebody without Kosmos can open and go on editing.
local function export_docx()
  local to = path and path:gsub("%.write$", "") .. ".docx" or "/Home/Untitled.docx"
  local ok, notes = docxwrite.write(to, doc, { title = (name:gsub("%.write$", "")),
                                                pictures = picture })

  if ok then
    said = ("Exported %s  -  %d paragraph%s, %d KB"):format(to:match("([^/]+)$"),
      notes.paragraphs, notes.paragraphs == 1 and "" or "s", (notes.bytes + 1023) // 1024)
    print(("writer: exported %s, %d paragraphs, %d bytes"):format(to, notes.paragraphs,
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

--
-- **A picture put in** (Media, W5): read, decoded for its size, named
-- inside the document, and set after the caret's paragraph at its size at
-- 96 to the inch - no wider than the column - with a caption under it in
-- Caption, the caret in the caption, as Pages does.
--
local function insert_picture(file)
  local bytes = fs.read(file)

  if type(bytes) ~= "string" then
    said = "Could not read " .. tostring(file)
    frame()
    return
  end

  local surface = decode(bytes)

  if not surface then
    said = file:match("([^/]+)$") .. " is not a picture this reads"
    frame()
    return
  end

  local n = 1
  local ext = (file:match("%.(%w+)$") or "png"):lower()

  while pictures[("pictures/%d.%s"):format(n, ext)] ~= nil do n = n + 1 end

  local pname = ("pictures/%d.%s"):format(n, ext)
  local pw, ph = surface:size()
  local w_mm = pw * 25.4 / 96
  local page_w = writedoc.page_mm(doc)
  local column = page_w - doc.margins_mm.left - doc.margins_mm.right

  if w_mm > column then w_mm = column end

  local h_mm = w_mm * ph / pw

  pictures[pname] = { bytes = bytes, surface = surface }

  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local pic = richtext.paragraph({ style = writedoc.BODY, align = "center",
                                   picture = { name = pname, width_mm = w_mm, height_mm = h_mm } },
                                 by_name, writedoc.BODY)
  local caption = richtext.paragraph({ style = by_name.Caption and "Caption" or writedoc.BODY,
                                       align = "center", runs = {} }, by_name, writedoc.BODY)

  table.insert(body, caret.para + 1, pic)
  table.insert(body, caret.para + 2, caption)

  print(("writer: picture %s, %dx%d px, %.1f by %.1f mm"):format(pname, pw, ph, w_mm, h_mm))
  edited(body, { para = caret.para + 2, at = 1 }, "picture")
end

--
-- **A table put in** (Table, W5b): three rows of three cells, the first a
-- header, after the caret's paragraph - or the table the caret is in - the
-- caret in its first cell, and a paragraph after it to type on when there
-- was none.
--
local function insert_table()
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local style = by_name[writedoc.BODY] and writedoc.BODY or doc.styles[1].name
  local t = richtext.paragraph(richtext.new_table(3, 3, style, true), by_name, style)
  local n = caret.para + 1

  table.insert(body, n, t)

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  print(("writer: table %d by %d at paragraph %d"):format(#t.table.rows, t.table.columns, n))
  edited(body, { para = n, at = 1, row = 1, col = 1 }, "table")
end

--
-- **A text box put in** (Text, W7a): 80 mm wide, bordered, centred, after
-- the caret's paragraph, the caret in it.
--
local function insert_box()
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local style = by_name[writedoc.BODY] and writedoc.BODY or doc.styles[1].name
  local n = caret.para + 1

  table.insert(body, n, richtext.paragraph(richtext.new_box(style, 80), by_name, style))

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  print(("writer: text box at paragraph %d"):format(n))
  edited(body, { para = n, at = 1, row = 1, col = 1 }, "table")
end

--
-- **A shape put in** (Shape, W7b): 40 by 30 mm in the drawing's blue,
-- centred, after the caret's paragraph, the caret on it. The log says
-- where it is drawn, for a harness to look.
--
function insert_shape(kind)
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local style = by_name[writedoc.BODY] and writedoc.BODY or doc.styles[1].name
  local n = caret.para + 1

  table.insert(body, n, richtext.paragraph(richtext.new_shape(kind, style), by_name, style))

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  edited(body, { para = n, at = 1 }, "shape")

  local here = pageset.locate(set, measure, caret)
  local line = here.line
  local px, py = page_at(here.page)
  local sc = scale()
  local top = line.baseline_pt - line.ascent_pt + 2

  print(("writer: shape %s at paragraph %d, %dx%d px at %d,%d"):format(kind, n,
    math.floor(line.width_pt * sc + 0.5), math.floor((line.ascent_pt - 2) * sc + 0.5),
    px + math.floor((line.x_pt + set.pages[here.page].shift_pt) * sc + 0.5),
    py + math.floor(top * sc + 0.5)))
end

--
-- **A chart put in** (Chart, W7c): `kind`, over a year's seasons, after the
-- caret's paragraph, the caret on it. The log says where it is drawn.
--
local function insert_chart(kind)
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local style = by_name[writedoc.BODY] and writedoc.BODY or doc.styles[1].name
  local n = caret.para + 1

  table.insert(body, n, richtext.paragraph(richtext.new_chart(kind, style), by_name, style))

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  edited(body, { para = n, at = 1 }, "chart")

  local here = pageset.locate(set, measure, caret)
  local line = here.line
  local px, py = page_at(here.page)
  local sc = scale()

  print(("writer: chart %s at paragraph %d, %dx%d px at %d,%d"):format(kind, n,
    math.floor(line.width_pt * sc + 0.5), math.floor((line.ascent_pt - 2) * sc + 0.5),
    px + math.floor((line.x_pt + set.pages[here.page].shift_pt) * sc + 0.5),
    py + math.floor((line.baseline_pt - line.ascent_pt + 2) * sc + 0.5)))
end

-- **Edit data**: the chart's table shown above it, the caret in its first
-- number - or hidden again, the caret on the chart.
function toggle_data()
  local n = caret.para
  local t = doc.body[n].table

  if data_open == n then
    data_open = nil
    caret = { para = n, at = 1 }
  else
    data_open = n
    local r, c = math.min(2, #t.rows), math.min(2, t.columns)
    caret = { para = n, row = r, col = c, at = #richtext.plain(t.rows[r][c]) + 1 }
  end

  anchor = nil
  print(("writer: chart data %s"):format(data_open and "shown" or "hidden"))
  set = setting()
  version = version + 1
  follow()
  frame()
end

local function chart_menu(x)
  local labels = {}
  for i, k in ipairs(CHART_KINDS) do labels[i] = k[2] end
  open_menu("chart", x, TOOLS_H + 2, 160, labels, nil,
            function(i) insert_chart(CHART_KINDS[i][1]) end)
end

--
-- **A comment put in** (Comment, W7d): on the selection - or the word the
-- caret is in - a number of its own, its words typed into the panel's
-- field, which has the keyboard.
--
local function insert_comment()
  local a, b = range()

  if not selected() then
    local p = doc.body[caret.para]
    local text = richtext.plain(caret.row and p.table.rows[caret.row][caret.col] or p)
    local from, to = caret.at, caret.at

    while from > 1 and not text:sub(from - 1, from - 1):match("%s") do from = from - 1 end
    while to <= #text and not text:sub(to, to):match("%s") do to = to + 1 end

    a = { para = caret.para, at = from, row = caret.row, col = caret.col }
    b = { para = caret.para, at = to, row = caret.row, col = caret.col }
  end

  if a.para == b.para and a.at == b.at and a.row == b.row and a.col == b.col then
    said = "Select the words a comment is about"
    frame()
    return
  end

  local id = 0
  local list = {}

  for i, c in ipairs(doc.comments) do
    list[i] = c
    id = math.max(id, c.id)
  end

  -- A number no run is under: the largest kept, and any the body has.
  for _, range_ in ipairs(richtext.comment_ranges(doc.body)) do id = math.max(id, range_.id) end

  id = id + 1
  list[#list + 1] = { id = id, text = "" }

  local body = richtext.format(doc.body, a, b, { comment = id }, by_name)

  panel, focus = "text", "comment"
  edited(body, caret, "comment", true, list)
  print(("writer: comment %d on %q"):format(id, richtext.text(doc.body, a, b)))
end

-- A comment taken away: its mark off the words, its words left behind.
function uncomment(id)
  local list = {}
  for _, c in ipairs(doc.comments) do
    if c.id ~= id then list[#list + 1] = c end
  end

  focus = nil
  print(("writer: comment %d deleted"):format(id))
  edited(richtext.uncomment(doc.body, id), caret, "comment", true, list)
end

local function shape_menu(x)
  local labels = {}
  for i, k in ipairs(SHAPE_KINDS) do labels[i] = k[2] end
  open_menu("shape", x, TOOLS_H + 2, 200, labels, nil,
            function(i) insert_shape(SHAPE_KINDS[i][1]) end)
end

-- The caret's table with rows, columns or its header changed; the caret
-- kept in the table.
function reshape(fields)
  local n = caret.para
  local body = richtext.reshape(doc.body, n, fields, by_name)
  local t = body[n].table
  local place = { para = n, at = 1 }

  if caret.row then
    local r, c = math.min(caret.row, #t.rows), math.min(caret.col, t.columns)
    place = { para = n, row = r, col = c,
              at = math.min(caret.at, #richtext.plain(t.rows[r][c]) + 1) }
  end

  if t.chart then
    print(("writer: chart %s %g mm, %d categories, %d series"):format(t.chart.kind,
      t.chart.height_mm, #t.rows - 1, t.columns - 1))
  elseif t.box then
    print(("writer: text box %g mm, %s, %s"):format(t.box.width_mm,
      t.box.border and "bordered" or "no border", t.box.fill or "no fill"))
  else
    print(("writer: table %d by %d"):format(#t.rows, t.columns))
  end

  edited(body, place, "table")
end

-- The caret's table taken out, the caret where it stood.
function delete_table()
  local n = caret.para
  local t = doc.body[n].table
  local what = t.chart and "chart" or t.box and "text box" or "table"
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  table.remove(body, n)

  if #body == 0 then body[1] = { style = writedoc.BODY, runs = {} } end

  local place = body[n] and richtext.enter(body, n, true) or richtext.enter(body, n - 1, false)

  print(("writer: %s deleted"):format(what))
  edited(body, place, "table")
end

--
-- **Tab, Shift-Tab and Return in a cell**: the next cell - a new row after
-- the last cell, as Pages and Word make one - the cell before, and the
-- cell below, out of the table under its last row.
--
function table_key(step)
  local n = caret.para
  local t = doc.body[n].table

  -- **A text box is text**: Return breaks its line and Tab is a tab.
  if t.box then
    if step == "down" then
      line_break()
    elseif step == 1 then
      type_text("\t")
    end

    return
  end

  if step == "down" then
    if caret.row < #t.rows then
      local below = t.rows[caret.row + 1][caret.col]
      move({ para = n, row = caret.row + 1, col = caret.col,
             at = #richtext.plain(below) + 1 })
    elseif doc.body[n + 1] then
      move(richtext.enter(doc.body, n + 1, true))
    end

    return
  end

  local other = richtext.next_cell(doc.body, caret, step)

  if other then
    move(other)
  elseif step > 0 then
    local body = richtext.reshape(doc.body, n, { rows = #t.rows + 1 }, by_name)
    edited(body, { para = n, at = 1, row = #t.rows + 1, col = 1 }, "table")
  end
end

local choose_picture

-- **Media**: the system's Open panel at Pictures, a PNG or a JPEG chosen.
function choose_picture()
  local start = fs.getattr("/Home/Pictures") and "/Home/Pictures" or "/Home"
  local picked = use("/Kosmos/Libraries/panel.lua").open{
    start = start, title = "Choose a picture",
    filter = function(n) return n:lower():match("%.png$") or n:lower():match("%.jpe?g$") end,
    on_choose = function(chosen) insert_picture(chosen) end,
  }

  if picked then picked:run() end

  frame()
end

local TOOL_ACTS

TOOL_ACTS = {
  table = function() insert_table() end,
  textbox = function() insert_box() end,
  shape = function(t) shape_menu(t.x) end,
  chart = function(t) chart_menu(t.x) end,
  comment = function() insert_comment() end,
  --
  -- **Insert**: what goes into the text, in a list - a page break, a table,
  -- a picture, a text box, a shape and a chart; a comment is about text
  -- already there, and is the Comment tool's.
  --
  insert = function(t)
    local items = { "Page Break", "Table", "Picture...", "Text Box", "Shape...", "Chart..." }
    open_menu("insert", t.x, TOOLS_H + 2, 180, items, nil, function(i)
      if i == 1 then
        TOOL_ACTS.addpage()
      elseif i == 2 then
        insert_table()
      elseif i == 3 then
        choose_picture()
      elseif i == 4 then
        insert_box()
      elseif i == 5 then
        shape_menu(t.x)
      else
        chart_menu(t.x)
      end
    end)
  end,
  media = function() choose_picture() end,
  zoom = function(t)
    local labels = {}
    for i, z in ipairs(ZOOMS) do labels[i] = z .. "%" end
    open_menu("zoom", t.x, TOOLS_H + 2, 120, labels, zoom,
              function(i) zoom_to(i) end)
  end,
  export = function(t)
    local items = { "Save as .write", "Export PDF", "Export Word (.docx)" }
    open_menu("export", math.min(t.x, W - 220), TOOLS_H + 2, 200, items, nil,
              function(i)
                if i == 1 then save() elseif i == 2 then export() else export_docx() end
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

    if richtext.block(p) then
      local body = {}
      for j, q in ipairs(doc.body) do body[j] = q end
      table.insert(body, caret.para + 1, { style = writedoc.BODY, runs = {}, page_break_before = true })
      edited(body, { para = caret.para + 1, at = 1 }, "page")
      return
    end

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

-- **Tab is the page's**: a tab in the text, the next cell in a table. A
-- window keeps Tab for moving between its controls unless a view says it
-- takes it (`ui.lua`), and Control-Tab still does that here.
sink.takes_tab = true

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

  -- **The comment's words**, typed into its field.
  if focus == "comment" then
    local id = richtext.comment_at(doc.body, caret)
    local list, words = {}, nil

    for i, c in ipairs(doc.comments) do
      list[i] = { id = c.id, text = c.text }
      if c.id == id then words = list[i] end
    end

    if not words or c == 13 or c == 10 or c == 27 or c == 9 then
      focus = nil
      last_kind = nil

      if words then print(("writer: comment %d says %q"):format(id, words.text)) end

      frame()
      return true
    end

    if c == 8 or c == 127 then
      local cut = #words.text

      while cut > 1 and words.text:byte(cut) >= 0x80 and words.text:byte(cut) < 0xC0 do
        cut = cut - 1
      end

      words.text = words.text:sub(1, cut - 1)
    elseif c >= 32 and c < 127 then
      words.text = words.text .. string.char(c)
    else
      return true
    end

    if utf8.len(words.text) then doc_edit("comment_text", { comments = list }) end

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
      move(k == keys.HOME and richtext.enter(doc.body, 1, true)
           or richtext.enter(doc.body, #doc.body, false), shift)
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
  elseif k == 9 and caret.row and (mods == 0 or mods == keys.SHIFT) then
    table_key(mods == keys.SHIFT and -1 or 1)
  elseif (k == 13 or k == 10) and mods == keys.SHIFT then
    line_break()
  elseif mods ~= 0 then
    return false
  elseif c == 8 or c == 127 then
    back_or_forward(false)
  elseif caret.row and (c == 13 or c == 10) then
    table_key("down")
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
    anchor = richtext.enter(doc.body, 1, true)
    caret = richtext.enter(doc.body, #doc.body, false)
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
    anchor = richtext.enter(doc.body, 1, true)
    caret = richtext.enter(doc.body, #doc.body, false)
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
      caret = clamp(place)
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

shape = ("%d:%s:%s:%s"):format(#set.pages, set.pages[1].width_pt, set.pages[1].height_pt,
                               tostring(set.facing))

win:run()
