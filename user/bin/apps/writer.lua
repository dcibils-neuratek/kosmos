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
-- `pdf`'s. The caret, typing and undo are `textedit`'s, the pages on the
-- desk and their thumbnails `pagedesk`'s, the panel's parts `inspector`'s,
-- the row of tools `pk.toolbar`'s and the charts' kinds `chart`'s - moved
-- out of this file in O1 (`docs/office.md`), so that Mail, Present and
-- Sheets stand on them too. What is here is what each key, click and
-- control means for a document.
--
-- **Each edit is a new body**, the paragraphs it did not touch the same
-- tables (`richtext`): so an undo is the body before, kept, and setting the
-- pages again sets only the paragraphs that changed (`pageset.cache`).
--
-- **A direct window** (`gfx.md` 19.4): a page is drawn once into a surface
-- of its own and blitted after that; an edit or a selection draws again the
-- pages on the screen, and the caret is drawn on the window over them.

local ui        = use("/Kosmos/Libraries/ui.lua")
local files     = use("/Kosmos/Libraries/files.lua")
local keys      = use("/Kosmos/Libraries/keys.lua")
local richtext  = use("/Kosmos/Libraries/richtext.lua")
local writedoc  = use("/Kosmos/Libraries/writedoc.lua")
local pageset   = use("/Kosmos/Libraries/pageset.lua")
local faces     = use("/Kosmos/Libraries/faces.lua")
local pagedraw  = use("/Kosmos/Libraries/pagedraw.lua")
local pdf       = use("/Kosmos/Libraries/pdf.lua")
local docxwrite = use("/Kosmos/Libraries/docxwrite.lua")
local pk        = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
local hyphen    = use("/Kosmos/Libraries/hyphen.lua")
local textedit  = use("/Kosmos/Libraries/textedit.lua")
local pagedesk  = use("/Kosmos/Libraries/pagedesk.lua")
local inspector = use("/Kosmos/Libraries/inspector.lua")
local chart     = use("/Kosmos/Libraries/chart.lua")

use("/Kosmos/Libraries/toolbar.lua").into(ui, pk)

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

-- The charts the Chart tool offers (W7c) and the shapes the Shape tool
-- does (W7b): the kits' lists, the ones the panel shows.
local CHART_KINDS = chart.KINDS
local SHAPE_KINDS = inspector.SHAPE_KINDS

local path = files.words(args)[1]
local name = path and path:match("([^/]+)$") or "Untitled"

local win, err = ui.window{
  title = name .. " - Kosmos Write", w = W, h = H, x = 40, y = 30,
  direct = true, header = true, resizable = true,
}

if not win then
  print("writer: " .. tostring(err))
  return
end

--
-- **The title band, where the look has no title bars** (`roadmap.md` 6zj,
-- and the drawing's): Write offers a header, so in Plex and Plex Night the
-- window manager puts no bar above it and Write draws the kit's header
-- across its top - its name, and the document's - with the three at its
-- right end and a press on it moving the window. Where the look keeps the
-- bars, the window manager's is the title and there is no band. It opened
-- without one until 4 October 2026, so it wore the old bar in every look.
--
local function head_h() return win.headed and ui.layout.head or 0 end

-- Where the tools' row ends and the desk, the panel and the thumbnails
-- begin.
local function work_top() return head_h() + TOOLS_H end

-- The three, at the band's right end: said again only when where changes.
local lights_said = nil

local function place_lights()
  local l = win.headed and win.lights or nil

  if not l then
    lights_said = nil
    return
  end

  local x = W - ui.layout.lights_in - l.w
  local y = (ui.layout.head - 1 - l.h) // 2

  if lights_said == x * 65536 + y then return end

  lights_said = x * 65536 + y
  fs.send("/Running/wm", { type = "lights", window = win.handle, x = x, y = y })
end

--------------------------------------------------------------------------
-- The document, set.
--------------------------------------------------------------------------

local catalogue = faces.catalogue(gfx.typefaces())
local measure = faces.measure(catalogue, gfx.typeface)

--
-- **The document's pictures** (W5): each one's bytes, as they came or as
-- the `.write` file holds them - read from the file when it opens, never
-- inside a paint - and decoded once, when a page first shows it.
--
-- **Every picture the file holds is here from the start**, shown or not:
-- the 0.11 review found a picture put in given the name of one on a page
-- not yet viewed, which Save then lost.
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
  local held
  doc, held = writedoc.open(path)

  -- The whole path, as it arrived: what a path with a space in it was cut
  -- at is exactly what this line would show (`testing.md` 18.414).
  print(("writer: opened %s, %s"):format(path, doc and "read" or tostring(held)))

  if not doc then
    said = tostring(held)
    doc = writedoc.new()
  else
    local zip = use("/Kosmos/Libraries/zip.lua")

    for _, name in ipairs(held or {}) do
      local bytes = zip.read(path, name, 64 * 1024 * 1024)
      pictures[name] = bytes and { bytes = bytes } or false
    end
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

-- The editor (`textedit.lua`): the caret, the selection, typing, undo.
-- Made below, once what it calls after an edit is.
local ed

--
-- **A chart's data is open while the caret is in it**: in one of its cells
-- it is shown, on the chart it stays as it was, anywhere else it closes.
-- True when that changed, so the pages are set again.
--
local function fit_data()
  local caret = ed.caret
  local p = doc.body[caret.para]
  local chart_ = p and p.table and p.table.chart
  local want = nil

  if chart_ and (caret.row or data_open == caret.para) then want = caret.para end

  if want ~= data_open then
    data_open = want
    return true
  end

  return false
end

local version = 1               -- what the pages on screen were drawn from

-- Which panel is open at the right - "text", "document" or nil - and which
-- of the Text panel's three parts.
local panel, part = "text", "style"

-- View's thumbnails shown, and a field with the keyboard - the header's
-- words - when one has it.
local thumbs = false
local focus = nil

-- A frame drawn: made below, and called by what is made before it.
local frame

--------------------------------------------------------------------------
-- The desk, and the pages on it.
--------------------------------------------------------------------------

local function desk_area()
  local left = thumbs and THUMBS_W or 0
  local right = panel and (W - PANEL_W) or W
  local y = work_top()
  return left, y, right - left, H - y
end

-- Where the comments stand, worked out once for what the pages show.
local noted = { version = nil, marks = {} }

-- What is marked under the text on page `i`: the words comments are
-- about, and the selection.
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

  if ed:selected() then
    for _, r in ipairs(pageset.selection(set, measure, ed.anchor, ed.caret)) do
      if r.page == i then
        r.colour = CHOSEN
        out[#out + 1] = r
      end
    end
  end

  return #out > 0 and out or nil
end

--
-- **The desk** (`pagedesk.lua`): where each page stands at the zoom,
-- scrolled, each drawn once into a surface of its own by `pagedraw` - with
-- the comments' and the selection's marks under the text - and View's
-- thumbnails, drawn small without them.
--
local desk = pagedesk.new{
  zooms = ZOOMS, zoom = 4,      -- 125%
  gap = GAP, thumb_w = THUMB_W, theme = theme,
  area = desk_area,
  pages = function() return set.pages, set.facing end,
  version = function() return version end,
  draw = function(i, surface, sc, small)
    if small then
      drawer:page(set, set.pages[i], surface, sc, 0, 0, PAPER)
    else
      drawer:page(set, set.pages[i], surface, sc, 0, 0, PAPER, marks_on(i))
    end
  end,
}

local function scale() return desk:scale() end
local function page_at(i) return desk:page_at(i) end
local function page_px(page) return desk:page_px(page) end

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

local function draw_tools(s, note)
  local y0 = head_h()

  if y0 > 0 then
    pk.header(s, 0, 0, W, "Kosmos Write", name,
              W - ui.layout.lights_in - (win.lights and win.lights.w or 0) - 12)
  end

  pk.toolbar(s, 0, y0, W, TOOLS_H, TOOLS, {
    -- The words a tool shows that change: the zoom's percentage.
    word = function(t)
      if t.key == "zoom" then return desk:percent() .. "%" end
      return t.text
    end,
    on = function(t)
      return (t.key == "view" and thumbs) or (t.key == "format" and panel == "text")
             or (t.key == "document" and panel == "document")
    end,
    note = note,
  })
end

--------------------------------------------------------------------------
-- The panel at the right.
--------------------------------------------------------------------------

-- The panel's controls, where each is, and the list a chooser opens
-- (`inspector.lua`); its log lines are Write's.
local ins = inspector.new(ui, pk, {
  name = "writer", catalogue = catalogue,
  top = work_top, bottom = function() return H end,
  redraw = function() frame() end,
})

local index_of = inspector.index_of

local doc_edit, apply_picture, reshape, delete_table, apply_shape, insert_shape
local toggle_data, uncomment

-- Once for each panel and part, where each control is - for a harness to
-- press it by name, as the window manager says where a tab is.
local function say_where()
  local caret = ed.caret
  local here = doc.body[caret.para]
  local kind = (panel == "text" and richtext.comment_at(doc.body, caret) and ":comment" or "")
               .. (here and (here.picture and ":picture" or here.shape and ":shape"
                         or here.table and (here.table.box and ":box"
                                            or here.table.chart and ":chart" or ":table")) or "")
  local key = panel == "text" and ("text:" .. part .. kind) or tostring(panel)

  ins:say_where(key)
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
  ins:control("paper", pc, function()
    local labels, chosen = {}, nil
    for i, pp in ipairs(PAPERS) do
      labels[i] = pp.label
      if pp.name == paper.name then chosen = i end
    end
    ins:open_menu("paper", pc.x, pc.y + pc.h + 4, pc.w, labels, chosen, function(i)
      local name_ = PAPERS[i].name
      doc_edit("paper", { paper = { name = name_, landscape = paper.landscape } })
    end)
  end)

  y = y + pc.h + 8

  local turn = { x = x0, y = y, w = w0,
                 items = { { text = "Upright" }, { text = "On its side" } },
                 chosen = paper.landscape and 2 or 1 }
  pk.segments(s, turn)
  ins:control("turn", turn, function(cx, cy)
    local i = pk.segment_at(turn, cx, cy)
    if i then
      doc_edit("paper", { paper = { name = paper.name, width_mm = paper.width_mm,
                                    height_mm = paper.height_mm, landscape = i == 2 } })
    end
  end)

  y = y + turn.h + 16

  local hd = { x = x0, y = y, text = "Header", on = doc.header.on }
  pk.check(s, hd)
  ins:control("header", hd, function()
    doc_edit("header", { header = { on = not doc.header.on,
                                    from_top_mm = doc.header.from_top_mm,
                                    text = doc.header.text } })
  end)

  local ft = { x = x0 + w0 // 2, y = y, text = "Footer", on = doc.footer.on }
  pk.check(s, ft)
  ins:control("footer", ft, function()
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
    ins:control(key, st, function(cx, cy)
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

  -- Its end, where the caret is, with `...` before it when the start is cut.
  local words = ui.fitted(doc.header.text, field.w - 20, "ui", true)

  s:text(field.x + 10, field.y + (30 - gfx.height()) // 2, words, theme.text, nil, "ui")

  if focus == "header" then
    s:fill(field.x + 10 + gfx.measure(words) + 1, field.y + 7, 2, 16, theme.ring)
  end

  ins:control("header_text", field, function() focus = "header" end)

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
  ins:control("page_numbers", pn, function()
    doc_edit("footer", { footer = { on = doc.footer.on, from_bottom_mm = doc.footer.from_bottom_mm,
                                    page_numbers = not doc.footer.page_numbers } })
  end)

  local lig = { x = x0 + w0 // 2, y = y + 4, text = "Ligatures", on = doc.ligatures }
  pk.check(s, lig)
  ins:control("ligatures", lig, function()
    doc_edit("ligatures", { ligatures = not doc.ligatures })
  end)

  y = y + 34

  local fc = { x = x0, y = y, text = "Facing pages", on = doc.facing }
  pk.check(s, fc)
  ins:control("facing", fc, function()
    doc_edit("facing", { facing = not doc.facing })
  end)

  local hy = { x = x0 + w0 // 2, y = y, text = "Hyphenation", on = doc.hyphenation }
  pk.check(s, hy)
  ins:control("hyphenation", hy, function()
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
  ins:control("language", lang, function()
    ins:open_menu("language", lang.x, lang.y + lang.h + 4, lang.w, words,
              index_of(tags, doc.language),
              function(i) doc_edit("language", { language = tags[i] }) end)
  end)
end

local function draw_panel(s)
  if not panel then return end

  local px = W - PANEL_W
  local x0, w0 = px + 14, PANEL_W - 28
  local look, layout, style = ed:look()
  local caret = ed.caret
  local y

  local wt = work_top()

  s:fill(px, wt, PANEL_W, H - wt, theme.window)
  s:fill(px, wt, 1, H - wt, theme.line_soft)

  -- Text and Document, the panel's two faces.
  local tabs = { x = x0, y = wt + 12, w = w0, accent = true,
                 items = { { text = "Text" }, { text = "Document" } },
                 chosen = panel == "text" and 1 or 2 }
  pk.segments(s, tabs)
  ins:control("tabs", tabs, function(cx, cy)
    local i = pk.segment_at(tabs, cx, cy)
    if i == 1 then panel = "text" elseif i == 2 then panel = "document" end
  end)

  y = tabs.y + tabs.h + 16

  local here = doc.body[caret.para]

  -- **In words a comment is about** (W7d).
  local noted_id = panel == "text" and richtext.comment_at(doc.body, caret)

  if noted_id then
    local words = ""
    for _, c in ipairs(doc.comments) do if c.id == noted_id then words = c.text end end

    y = ins:comment(s, x0, w0, y, words, focus == "comment", {
      focus = function() focus = "comment" end,
      delete = function() uncomment(noted_id) end,
    })
  end

  if panel == "text" and here and here.picture then
    ins:picture(s, x0, w0, y, here.picture, { width = function(mm) apply_picture(mm) end })
    say_where(s)
    return
  end

  if panel == "document" then
    draw_document(s, x0, w0, y)
    say_where(s)
    return
  end

  -- What a table, a text box and a chart change through.
  local tabled = {
    reshape = function(fields) reshape(fields) end,
    delete = function() delete_table() end,
    data = function() toggle_data() end,
  }

  if here and here.table and here.table.chart then
    ins:chart(s, x0, w0, y, here.table, data_open == caret.para, tabled)
    say_where(s)
    return
  end

  if here and here.shape then
    ins:shape(s, x0, w0, y, here.shape, {
      shape = function(fields) apply_shape(fields) end,
      delete = function()
        ed:remove(ed.caret.para, { para = math.max(1, ed.caret.para - 1), at = 1 })
      end,
    })
    say_where(s)
    return
  end

  -- A text box, or a table, above the text's part, which works on the
  -- cell's text.
  if here and here.table and here.table.box and caret.row then
    y = ins:box(s, x0, w0, y, here.table.box, tabled)
  elseif here and here.table and caret.row then
    y = ins:table(s, x0, w0, y, here.table, tabled)
  end

  ins:text(s, x0, w0, y, look, layout, style, doc.styles, here and here.table, part, {
    style = function(style_name) ed:restyle(style_name) end,
    part = function(p) part = p end,
    char = function(fields) ed:format(fields) end,
    para = function(fields) ed:arrange(fields) end,
  })

  say_where(s)
end

--------------------------------------------------------------------------
-- A frame.
--------------------------------------------------------------------------

local function caret_page()
  local here = pageset.locate(set, measure, ed.caret)
  return here and here.page or 1
end

local function caret_px()
  local here = pageset.locate(set, measure, ed.caret)

  if not here then return nil end

  local px, py = page_at(here.page)
  local x = px + math.floor(here.x_pt * scale() + 0.5)
  local y = py + math.floor((here.baseline_pt - here.ascent_pt) * scale())
  local h = math.max(8, math.floor(here.height_pt * scale() + 0.5))

  return x, y, h, here
end

-- The look the last frame was drawn in, to know when it changed.
local drawn_in = nil

function frame()
  local s = win:surface()

  if not s then return end

  local dx, dy, dw = desk_area()

  desk:draw(s, DESK, SHADOW)

  local cx, cy, ch = caret_px()

  if cx and cy + ch > dy and cy < H and cx >= dx and cx < dx + dw then
    local y0 = math.max(dy, cy)
    s:fill(cx, y0, 2, math.min(cy + ch, H) - y0, CARET)
  end

  ins:begin()

  -- **View's thumbnails**: each page drawn small, the caret's ringed, its
  -- number under it. A press on one goes to that page.
  if thumbs then
    local wt = work_top()
    desk:thumbs(s, 0, wt, THUMBS_W, H - wt, caret_page())
  end

  -- What was last said, in the tools' row between the two groups.
  draw_tools(s, said or ("%s%s  -  %d page%s"):format(name, ed.dirty and " (edited)" or "",
                                                       #set.pages,
                                                       #set.pages == 1 and "" or "s"))
  draw_panel(s)
  ins:draw_menu(s)

  drawn_in = { headed = win.headed, window = theme.window, text = theme.text }
  win:commit()
  place_lights()
end

--
-- **A look changed** - its colours, or whether windows have title bars -
-- which the kit has already taken in, and which Write has to draw again
-- for: it draws everything else the moment it happens, so the kit's call
-- after each event would be a second frame for nothing.
--
win.on_paint = function()
  local d = drawn_in

  if not d or d.headed ~= win.headed or d.window ~= theme.window
     or d.text ~= theme.text then
    frame()
  end
end

local function report()
  local x, y = page_at(1)
  local w, h = page_px(set.pages[1])

  print(("writer: %s, %d page%s at %d%%, page 1 at %d,%d %dx%d"):format(
    name, #set.pages, #set.pages == 1 and "" or "s", desk:percent(), x, y, w, h))
end

--------------------------------------------------------------------------
-- The view.
--------------------------------------------------------------------------

-- The desk scrolled to show the caret: the whole of its line where that
-- fits - a chart or a shape the caret is on, not only its left edge.
local function follow()
  local cx, cy, ch, here = caret_px()

  if not cy then return end

  local px = page_at(here.page)
  local right = px + math.floor((here.line.x_pt + here.line.width_pt
                                 + set.pages[here.page].shift_pt) * scale() + 0.5)

  desk:follow(cx, cy, ch, right)
end

local function zoom_to(z)
  if not desk:zoom_to(z) then return end

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

-- What an undo puts back besides the body and the caret: the document's
-- settings, each a table no edit changes in place.
local function settings()
  return { paper = doc.paper, comments = doc.comments,
           margins_mm = doc.margins_mm, header = doc.header, footer = doc.footer,
           facing = doc.facing, hyphenation = doc.hyphenation,
           ligatures = doc.ligatures, language = doc.language }
end

local function put_back(entry)
  doc.comments = entry.comments
  doc.paper, doc.margins_mm = entry.paper, entry.margins_mm
  doc.header, doc.footer = entry.header, entry.footer
  doc.facing, doc.hyphenation = entry.facing, entry.hyphenation
  doc.ligatures, doc.language = entry.ligatures, entry.language
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

--
-- **The text, edited** (`textedit.lua`): after an edit the pages are set
-- again, the desk follows the caret and a frame is drawn; after a move the
-- pages are drawn again only when the selection changed, or a chart's
-- data opened or closed.
--
ed = textedit.new{
  doc = doc, by_name = by_name, measure = measure,
  set = function() return set end,
  body_style = writedoc.BODY,
  keep = settings, put_back = put_back,
  on = {
    edited = function()
      fit_data()
      said = nil
      set = setting()
      follow()
      changed()
      report_if_changed()
    end,
    undone = function()
      fit_data()
      set = setting()
      follow()
      changed()
      report_if_changed()
    end,
    moved = function(redraw)
      if redraw then version = version + 1 end

      if fit_data() then
        set = setting()
        version = version + 1
      end

      follow()
      frame()
    end,
    looked = function() frame() end,
    all = function() changed() end,
  },
}

--
-- **A change to the document's settings** - the paper, the margins, a
-- header or a footer - each a new table in place of the old, checked as a
-- file's would be, and undone as an edit is. A run of one kind - the
-- header's words typed - is one step to undo.
--
function doc_edit(kind, fields)
  ed:step(kind, kind == "header_text" or kind == "comment_text")

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
  ed.dirty = true
  said = nil
  set = setting()
  changed()
  report_if_changed()
end

-- A shape's kind, size or colour.
function apply_shape(fields)
  local here = doc.body[ed.caret.para]
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local raw = {}
  for k, v in pairs(here) do raw[k] = v end

  local sh = {}
  for k, v in pairs(here.shape) do sh[k] = v end
  for k, v in pairs(fields) do sh[k] = v end

  raw.shape = sh
  body[ed.caret.para] = richtext.paragraph(raw, by_name, here.style)

  local now = body[ed.caret.para].shape
  print(("writer: shape %s %g by %g mm, %s"):format(now.kind, now.width_mm, now.height_mm, now.fill))
  ed:edited(body, ed.caret, "format", true)
end

-- A picture's width, its height kept in proportion.
function apply_picture(width_mm)
  local here = doc.body[ed.caret.para]
  local pic = here.picture
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local raw = {}
  for k, v in pairs(here) do raw[k] = v end
  raw.picture = { name = pic.name, width_mm = width_mm,
                  height_mm = pic.height_mm * width_mm / pic.width_mm }
  body[ed.caret.para] = richtext.paragraph(raw, by_name, here.style)
  ed:edited(body, ed.caret, "format", true)
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

    -- Its bytes as they came, whether or not this build can draw them.
    local held = name and pictures[name]

    if held and not shown[name] then
      shown[name] = true
      list[#list + 1] = { name = name, bytes = held.bytes }
    end
  end

  local ok, why = writedoc.save(path, doc, list)

  if ok then
    ed.dirty = false
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
-- What goes in.
--------------------------------------------------------------------------

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

  -- A name no picture in the document has - loaded, or held unread.
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

  table.insert(body, ed.caret.para + 1, pic)
  table.insert(body, ed.caret.para + 2, caption)

  print(("writer: picture %s, %dx%d px, %.1f by %.1f mm"):format(pname, pw, ph, w_mm, h_mm))
  ed:edited(body, { para = ed.caret.para + 2, at = 1 }, "picture")
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
  local n = ed.caret.para + 1

  table.insert(body, n, t)

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  print(("writer: table %d by %d at paragraph %d"):format(#t.table.rows, t.table.columns, n))
  ed:edited(body, { para = n, at = 1, row = 1, col = 1 }, "table")
end

--
-- **A text box put in** (Text, W7a): 80 mm wide, bordered, centred, after
-- the caret's paragraph, the caret in it.
--
local function insert_box()
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  local style = by_name[writedoc.BODY] and writedoc.BODY or doc.styles[1].name
  local n = ed.caret.para + 1

  table.insert(body, n, richtext.paragraph(richtext.new_box(style, 80), by_name, style))

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  print(("writer: text box at paragraph %d"):format(n))
  ed:edited(body, { para = n, at = 1, row = 1, col = 1 }, "table")
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
  local n = ed.caret.para + 1

  table.insert(body, n, richtext.paragraph(richtext.new_shape(kind, style), by_name, style))

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  ed:edited(body, { para = n, at = 1 }, "shape")

  local here = pageset.locate(set, measure, ed.caret)
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
  local n = ed.caret.para + 1

  table.insert(body, n, richtext.paragraph(richtext.new_chart(kind, style), by_name, style))

  if not body[n + 1] then body[n + 1] = { style = style, runs = {} } end

  ed:edited(body, { para = n, at = 1 }, "chart")

  local here = pageset.locate(set, measure, ed.caret)
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
  local n = ed.caret.para
  local t = doc.body[n].table

  if data_open == n then
    data_open = nil
    ed.caret = { para = n, at = 1 }
  else
    data_open = n
    local r, c = math.min(2, #t.rows), math.min(2, t.columns)
    ed.caret = { para = n, row = r, col = c, at = #richtext.plain(t.rows[r][c]) + 1 }
  end

  ed.anchor = nil
  print(("writer: chart data %s"):format(data_open and "shown" or "hidden"))
  set = setting()
  version = version + 1
  follow()
  frame()
end

local function chart_menu(x)
  local labels = {}
  for i, k in ipairs(CHART_KINDS) do labels[i] = k[2] end
  ins:open_menu("chart", x, work_top() + 2, 160, labels, nil,
            function(i) insert_chart(CHART_KINDS[i][1]) end)
end

--
-- **A comment put in** (Comment, W7d): on the selection - or the word the
-- caret is in - a number of its own, its words typed into the panel's
-- field, which has the keyboard.
--
local function insert_comment()
  local a, b = ed:range()

  if not ed:selected() then
    local p = doc.body[ed.caret.para]
    local text = richtext.plain(ed.caret.row and p.table.rows[ed.caret.row][ed.caret.col] or p)
    local from, to = ed.caret.at, ed.caret.at

    while from > 1 and not text:sub(from - 1, from - 1):match("%s") do from = from - 1 end
    while to <= #text and not text:sub(to, to):match("%s") do to = to + 1 end

    a = { para = ed.caret.para, at = from, row = ed.caret.row, col = ed.caret.col }
    b = { para = ed.caret.para, at = to, row = ed.caret.row, col = ed.caret.col }
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
  ed:edited(body, ed.caret, "comment", true, list)
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
  ed:edited(richtext.uncomment(doc.body, id), ed.caret, "comment", true, list)
end

local function shape_menu(x)
  local labels = {}
  for i, k in ipairs(SHAPE_KINDS) do labels[i] = k[2] end
  ins:open_menu("shape", x, work_top() + 2, 200, labels, nil,
            function(i) insert_shape(SHAPE_KINDS[i][1]) end)
end

-- The caret's table with rows, columns or its header changed; the caret
-- kept in the table.
function reshape(fields)
  local n = ed.caret.para
  local body = richtext.reshape(doc.body, n, fields, by_name)
  local t = body[n].table
  local place = { para = n, at = 1 }

  if ed.caret.row then
    local r, c = math.min(ed.caret.row, #t.rows), math.min(ed.caret.col, t.columns)
    place = { para = n, row = r, col = c,
              at = math.min(ed.caret.at, #richtext.plain(t.rows[r][c]) + 1) }
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

  ed:edited(body, place, "table")
end

-- The caret's table taken out, the caret where it stood.
function delete_table()
  local n = ed.caret.para
  local t = doc.body[n].table
  local what = t.chart and "chart" or t.box and "text box" or "table"
  local body = {}
  for i, p in ipairs(doc.body) do body[i] = p end

  table.remove(body, n)

  if #body == 0 then body[1] = { style = writedoc.BODY, runs = {} } end

  local place = body[n] and richtext.enter(body, n, true) or richtext.enter(body, n - 1, false)

  print(("writer: %s deleted"):format(what))
  ed:edited(body, place, "table")
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
    ins:open_menu("insert", t.x, work_top() + 2, 180, items, nil, function(i)
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
    ins:open_menu("zoom", t.x, work_top() + 2, 120, labels, desk.zoom,
              function(i) zoom_to(i) end)
  end,
  export = function(t)
    local items = { "Save as .write", "Export PDF", "Export Word (.docx)" }
    ins:open_menu("export", math.min(t.x, W - 220), work_top() + 2, 200, items, nil,
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
    local p = doc.body[ed.caret.para]

    if richtext.block(p) then
      local body = {}
      for j, q in ipairs(doc.body) do body[j] = q end
      table.insert(body, ed.caret.para + 1, { style = writedoc.BODY, runs = {}, page_break_before = true })
      ed:edited(body, { para = ed.caret.para + 1, at = 1 }, "page")
      return
    end

    local at_end = { para = ed.caret.para, at = #richtext.plain(p) + 1 }
    local body, place = richtext.split(doc.body, at_end, by_name)

    body = richtext.arrange(body, place, place, { page_break_before = true }, by_name)
    ed:edited(body, place, "page")
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

function sink:wheel(n)
  desk:scroll_by(-n * 3 * 40)
  frame()
  return true
end

--
-- **A key**: a list's Escape while one is open; the comment's or the
-- header's words while a field has the keyboard; Page Up and Down, which
-- scroll; Control-S and Control-E; and anything else is the text's
-- (`textedit`).
--
function sink:key(c)
  local k = keys.parts(c)

  if ins.menu then
    if c == 27 then ins.menu = nil frame() return true end
    return true
  end

  -- **The comment's words**, typed into its field.
  if focus == "comment" then
    local id = richtext.comment_at(doc.body, ed.caret)
    local list, words = {}, nil

    for i, cm in ipairs(doc.comments) do
      list[i] = { id = cm.id, text = cm.text }
      if cm.id == id then words = list[i] end
    end

    if not words or c == 13 or c == 10 or c == 27 or c == 9 then
      focus = nil
      ed.last_kind = nil

      if words then print(("writer: comment %d says %q"):format(id, words.text)) end

      frame()
      return true
    end

    if c == 8 or c == 127 then
      -- The last character, whatever it is in bytes.
      if #words.text > 0 then
        words.text = words.text:sub(1, utf8.offset(words.text, 0, #words.text) - 1)
      end
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
      ed.last_kind = nil
      frame()
    elseif c == 8 or c == 127 then
      if #text > 0 then
        doc_edit("header_text", { header = { on = doc.header.on, from_top_mm = doc.header.from_top_mm,
                                             text = text:sub(1, utf8.offset(text, 0, #text) - 1) } })
      end
    elseif c >= 32 and c < 127 then
      doc_edit("header_text", { header = { on = doc.header.on, from_top_mm = doc.header.from_top_mm,
                                           text = text .. string.char(c) } })
    end

    return true
  end

  if k == keys.PAGEUP or k == keys.PAGEDOWN then
    local _, _, _, dh = desk_area()
    desk:scroll_by((k == keys.PAGEUP and -1 or 1) * (dh - 40))
    frame()
    return true
  elseif c == 19 then                                    -- Control-S
    save()
    return true
  elseif c == 5 then                                     -- Control-E
    export()
    return true
  end

  return ed:key(c)
end

function sink:edit(kind)
  return ed:edit(kind)
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

  if action == "press" and ins.menu then
    ins:pick(x, y)
    return true
  end

  if action == "press" and focus and not (panel and x >= W - PANEL_W) then
    focus = nil
    ed.last_kind = nil
  end

  if action == "press" and thumbs and x < THUMBS_W then
    local page = desk:thumb_at(x, y)

    if page then
      desk:show(page)
      frame()
    end

    return true
  end

  if action == "press" and y < head_h() then
    win:take_hold(x, y)
    return true
  end

  if action == "press" and y < work_top() then
    local t = pk.tool_at(TOOLS, x)

    if t then
      local act = TOOL_ACTS[t.key]
      if act then act(t) end
    end

    return true
  end

  if action == "press" and panel and x >= W - PANEL_W then
    focus = nil

    if ins:press(x, y) then frame() end

    return true
  end

  local dx, _, dw = desk_area()

  if x >= dx + dw then return true end

  local page, px, py = desk:point(x, y)
  local place = page and pageset.hit(set, measure, page, px, py)

  if place then
    if action == "press" then
      pressed = true
      if ed:press(place) then version = version + 1 end
      frame()
    else
      ed:drag(place)
      version = version + 1
      frame()
    end
  end

  return true
end

--
-- **A new size** (Diego, 5 October 2026: "I need to be able to resizse the
-- window like we do with the browsser"): the kit has already given the
-- window buffers that size (6zz e), and everything here reads `W` and `H`
-- when it draws, so taking the size is the whole of it - the three placed
-- again, the scroll held inside the new desk, and a frame now, since the
-- kit's own repaint after a resize is one this window does not draw.
--
win.on_resize = function(_, w, h)
  W, H = w, h
  sink.w, sink.h = w, h
  lights_said = nil
  ins.menu = nil
  desk:refit()
  frame()

  -- Said with where the last tool now ends, which is laid out from `W`: a
  -- window that took the size and drew at the old one would say so.
  local last = TOOLS[#TOOLS]

  print(("writer: resized to %dx%d, the tools ending at %d"):format(
        W, H, (last.x or 0) + (last.w or 0)))
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
