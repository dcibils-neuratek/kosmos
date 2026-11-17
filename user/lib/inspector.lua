-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- An inspector: the panel at a document window's right that shows what
-- is chosen and changes it - text's font and paragraph, a picture, a
-- shape, a text box, a table, a chart, a comment - drawn with `pixelkit`'s
-- controls into a surface (`docs/office.md` O1, `docs/present.md`, *What
-- moves out of Kosmos Write*).
--
--   local ins = inspector.new(ui, pk, {
--     name = "writer",                     -- what its log lines begin with
--     catalogue = catalogue,               -- faces.catalogue, for Font and Weight
--     top = function() return y end,       -- where the window's work begins
--     bottom = function() return H end,    -- and ends: a list stays inside
--     redraw = function() frame() end,
--   })
--
--   ins:begin()                            each frame, before the controls
--   ins:control(key, box, act)             a control drawn, and what a press does
--   ins:press(x, y)                        true when a control took the press
--   ins:say_where(key)                     once for each key, where each control is
--   ins:open_menu(kind, x, y, w, items, chosen, pick)
--   ins.menu                               the list open over the window, or nil
--   ins:pick(x, y)                         a press while it is open
--   ins:draw_menu(s)
--
--   y = ins:comment(s, x, w, y, words, focused, on)   on.focus, on.delete
--   ins:picture(s, x, w, y, picture, on)              on.width(mm)
--   ins:chart(s, x, w, y, t, shown, on)               on.reshape, on.data, on.delete
--   ins:shape(s, x, w, y, shape, on)                  on.shape(fields), on.delete
--   y = ins:box(s, x, w, y, box, on)                  on.reshape, on.delete
--   y = ins:table(s, x, w, y, t, on)                  on.reshape, on.delete
--   ins:text(s, x, w, y, look, layout, style, styles, compact, part, on)
--                                       on.style(name), on.part(p), on.char, on.para
--
--   inspector.COLOURS  FILLS  SIZES  SPACINGS  SHAPE_KINDS
--   inspector.index_of(list, v)  inspector.colour_name(c)  ins:weight_names(family)
--
-- **The window says what each change means**, through `on`: the inspector
-- knows a shape has a width, not what a document does when it changes.
-- Every part takes the values it shows and draws them from the top down,
-- `x` and `w` its column, and gives back how far down it went.
--
-- **Where a control is is said once for each panel and part** - a line
-- `<name>: control <key> at x,y wxh` for each - so a harness presses it by
-- name, as the window manager says where a tab is; and a list opened says
-- `<name>: menu <kind> at ...`.
--
-- It was Kosmos Write's, inside `writer.lua`, until O1 (17 November 2026):
-- Present's Format panel is its next user, and Sheets' after it.

local faces = use("/Kosmos/Libraries/faces.lua")
local pagedraw = use("/Kosmos/Libraries/pagedraw.lua")
local chart = use("/Kosmos/Libraries/chart.lua")

local inspector = {}

inspector.SIZES = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 30, 36, 48, 64, 72 }
inspector.SPACINGS = { 1, 1.15, 1.2, 1.5, 2 }

-- The shapes the Shape tool offers (W7b), in Pages' order.
inspector.SHAPE_KINDS = {
  { "rectangle", "Rectangle" }, { "rounded", "Rounded rectangle" }, { "oval", "Oval" },
  { "triangle", "Triangle" }, { "star", "Star" }, { "arrow", "Arrow" },
}

-- What a text box may be filled with: nothing, or a pale tint.
inspector.FILLS = {
  { false, "None" }, { "#eef3fb", "Mist" }, { "#fdf3e1", "Sand" }, { "#e8f6ee", "Mint" },
  { "#fde8e8", "Rose" }, { "#f1eafa", "Lilac" }, { "#f2f2f2", "Grey" },
}

-- The text colours a person picks from: the document's inks first.
inspector.COLOURS = {
  { "#1b2330", "Ink" }, { "#000000", "Black" }, { "#5b6677", "Slate" },
  { "#8e959f", "Grey" }, { "#c0392b", "Red" }, { "#d35400", "Orange" },
  { "#b7950b", "Ochre" }, { "#27ae60", "Green" }, { "#16a085", "Teal" },
  { "#2a55c9", "Blue" }, { "#8e44ad", "Purple" }, { "#ffffff", "White" },
}

local SIZES, SPACINGS = inspector.SIZES, inspector.SPACINGS
local SHAPE_KINDS, FILLS, COLOURS = inspector.SHAPE_KINDS, inspector.FILLS, inspector.COLOURS
local CHART_KINDS = chart.KINDS

function inspector.index_of(list, v)
  for i, x in ipairs(list) do if x == v then return i end end
end

function inspector.colour_name(c)
  for _, pair in ipairs(COLOURS) do
    if pair[1] == c then return pair[2] end
  end
  return c
end

local index_of, colour_name = inspector.index_of, inspector.colour_name

-- `#rrggbb` as a surface's 0xAARRGGBB, as a page is drawn with it.
local argb = pagedraw.argb

function inspector.new(ui, pk, o)
  local theme = ui.theme
  local name = o.name or "inspector"
  local catalogue = o.catalogue

  local ins = { menu = nil }

  -- The controls drawn this frame, each `{ key, box, act }`: a press finds
  -- the one it is on. Rebuilt every frame, since what is shown changes.
  local controls = {}
  local said_where = {}

  function ins:begin() controls = {} end

  function ins:control(key, box, act)
    controls[#controls + 1] = { key = key, box = box, act = act }
  end

  function ins:press(x, y)
    for _, c in ipairs(controls) do
      if pk.inside(c.box, x, y) then
        c.act(x, y)
        return true
      end
    end

    return false
  end

  function ins:say_where(key)
    if said_where[key] then return end

    said_where[key] = true

    for _, c in ipairs(controls) do
      local b = c.box
      print(("%s: control %s at %d,%d %dx%d"):format(name, c.key, b.x, b.y, b.w, b.h or 0))
    end
  end

  function ins:open_menu(kind, x, y, w, items, chosen, pick)
    local box = { x = x, y = y, w = w, items = items, chosen = chosen }
    local h = #items * 30 + 8

    -- Up from its control when it would run off the window's foot.
    if y + h > o.bottom() - 4 then box.y = math.max(o.top(), y - h - 40) end

    box.h = h
    self.menu = { box = box, pick = pick }
    print(("%s: menu %s at %d,%d %dx%d, %d items"):format(name, kind, box.x, box.y,
                                                         box.w, h, #items))
    o.redraw()
  end

  -- A press while a list is open: the item it is on picked, or the list
  -- closed.
  function ins:pick(x, y)
    local i = pk.menu_at(self.menu.box, x, y)
    local pick = self.menu.pick

    self.menu = nil

    if i then pick(i) else o.redraw() end
  end

  function ins:draw_menu(s)
    if self.menu then pk.menu(s, self.menu.box) end
  end

  function ins:weight_names(family)
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

  --
  -- **In words a comment is about** (W7d): the comment's words in a field
  -- to type into, and the comment taken away.
  --
  function ins:comment(s, x0, w0, y, words, focused, on)
    pk.label(s, x0, y, "Comment")
    y = y + 18

    local field = { x = x0, y = y, w = w0, h = 30 }
    pk.field(s, field, focused)

    local shown = ui.fitted(words, field.w - 20, "ui", true)

    if shown == "" and not focused then
      s:text(field.x + 10, field.y + (30 - gfx.height()) // 2, "Type a comment", theme.text_dim, nil, "ui")
    else
      s:text(field.x + 10, field.y + (30 - gfx.height()) // 2, shown, theme.text, nil, "ui")
    end

    if focused then
      s:fill(field.x + 10 + gfx.measure(shown) + 1, field.y + 7, 2, 16, theme.ring)
    end

    self:control("comment_text", field, function() on.focus() end)

    y = y + 38

    local del = { x = x0, y = y, text = "Delete comment" }
    pk.button(s, del)
    self:control("comment_delete", del, function() on.delete() end)

    return y + 44
  end

  -- **On a picture**: its width, its height kept in proportion.
  function ins:picture(s, x0, w0, y, pic, on)
    pk.label(s, x0, y, "Picture")
    y = y + 18

    s:text(x0, y + (30 - gfx.height()) // 2, "Width", theme.text_dim, nil, "ui")

    local wd = { x = x0 + 120, y = y, w = w0 - 120, text = ("%.0f mm"):format(pic.width_mm) }
    pk.stepper(s, wd)
    self:control("picture_width", wd, function(cx, cy)
      local d = pk.step_at(wd, cx, cy)

      if d and d ~= 0 then on.width(math.max(10, pic.width_mm + 5 * d)) end
    end)

    y = y + 38
    s:text(x0, y, ("Height %.0f mm, kept in proportion"):format(pic.height_mm),
           theme.text_dim, nil, "ui")
  end

  --
  -- **On a chart** (W7c): which kind, how tall, its data shown to type
  -- into - and while it is, how many categories and series - and the
  -- chart taken out.
  --
  function ins:chart(s, x0, w0, y, t, shown, on)
    local ch = t.chart

    pk.label(s, x0, y, "Chart")
    y = y + 18

    local kinds = { x = x0, y = y, w = w0, items = {} , chosen = 1 }
    for i, k in ipairs(CHART_KINDS) do
      kinds.items[i] = { text = k[2] }
      if k[1] == ch.kind then kinds.chosen = i end
    end
    pk.segments(s, kinds)
    self:control("chart_kind", kinds, function(cx, cy)
      local i = pk.segment_at(kinds, cx, cy)
      if i then on.reshape({ chart = { kind = CHART_KINDS[i][1], height_mm = ch.height_mm } }) end
    end)

    y = y + kinds.h + 10

    local ht = { x = x0, y = y, w = w0, text = ("%g mm tall"):format(ch.height_mm) }
    pk.stepper(s, ht)
    self:control("chart_height", ht, function(cx, cy)
      local d = pk.step_at(ht, cx, cy)
      if d and d ~= 0 then
        on.reshape({ chart = { kind = ch.kind, height_mm = math.max(20, ch.height_mm + 10 * d) } })
      end
    end)

    y = y + 38

    local ed = { x = x0, y = y + 4, text = "Edit data", on = shown }
    pk.check(s, ed)
    self:control("chart_data", ed, function() on.data() end)

    y = y + 34

    if shown then
      -- A row each: "4 categories" is more than half the panel holds.
      local cats = { x = x0, y = y, w = w0,
                     text = ("%d categor%s"):format(#t.rows - 1, #t.rows == 2 and "y" or "ies") }
      pk.stepper(s, cats)
      self:control("chart_categories", cats, function(cx, cy)
        local d = pk.step_at(cats, cx, cy)
        if d and d ~= 0 then on.reshape({ rows = math.max(2, #t.rows + d) }) end
      end)

      y = y + 38

      local sers = { x = x0, y = y, w = w0, text = ("%d series"):format(t.columns - 1) }
      pk.stepper(s, sers)
      self:control("chart_series", sers, function(cx, cy)
        local d = pk.step_at(sers, cx, cy)
        if d and d ~= 0 then on.reshape({ columns = math.max(2, t.columns + d) }) end
      end)

      y = y + 38
    end

    local del = { x = x0, y = y + 6, text = "Delete chart" }
    pk.button(s, del)
    self:control("chart_delete", del, function() on.delete() end)
  end

  --
  -- **On a shape** (W7b): which shape, its width and height, its colour,
  -- and the shape taken out.
  --
  function ins:shape(s, x0, w0, y, sh, on)
    pk.label(s, x0, y, "Shape")
    y = y + 18

    local kind_name = sh.kind
    for _, k in ipairs(SHAPE_KINDS) do if k[1] == sh.kind then kind_name = k[2] end end

    local kc = { x = x0, y = y, w = w0, text = kind_name }
    pk.chooser(s, kc)
    self:control("shape_kind", kc, function()
      local labels, chosen = {}, nil
      for i, k in ipairs(SHAPE_KINDS) do
        labels[i] = k[2]
        if k[1] == sh.kind then chosen = i end
      end
      self:open_menu("shape_kind", kc.x, kc.y + kc.h + 4, kc.w, labels, chosen,
                     function(i) on.shape({ kind = SHAPE_KINDS[i][1] }) end)
    end)

    y = y + kc.h + 8

    local half = (w0 - 8) // 2
    local wd = { x = x0, y = y, w = half, text = ("%g mm wide"):format(sh.width_mm) }
    pk.stepper(s, wd)
    self:control("shape_width", wd, function(cx, cy)
      local d = pk.step_at(wd, cx, cy)
      if d and d ~= 0 then on.shape({ width_mm = math.max(5, sh.width_mm + 5 * d) }) end
    end)

    local ht = { x = x0 + half + 8, y = y, w = w0 - half - 8,
                 text = ("%g mm high"):format(sh.height_mm) }
    pk.stepper(s, ht)
    self:control("shape_height", ht, function(cx, cy)
      local d = pk.step_at(ht, cx, cy)
      if d and d ~= 0 then on.shape({ height_mm = math.max(5, sh.height_mm + 5 * d) }) end
    end)

    y = y + 38
    pk.label(s, x0, y, "Fill")
    y = y + 18

    local sw = { x = x0, y = y, w = 40, h = 22, colour = argb(sh.fill) }
    pk.swatch(s, sw)
    s:text(x0 + 52, y + (22 - gfx.height()) // 2, colour_name(sh.fill), theme.text, nil, "ui")
    local fill_box = { x = x0, y = y, w = w0, h = 22 }
    self:control("shape_fill", fill_box, function()
      local labels, chosen = {}, nil
      for i, pair in ipairs(COLOURS) do
        labels[i] = pair[2]
        if pair[1] == sh.fill then chosen = i end
      end
      self:open_menu("shape_fill", x0, fill_box.y + 26, 160, labels, chosen,
                     function(i) on.shape({ fill = COLOURS[i][1] }) end)
    end)

    y = y + 36

    local del = { x = x0, y = y, text = "Delete shape" }
    pk.button(s, del)
    self:control("shape_delete", del, function() on.delete() end)
  end

  --
  -- **In a text box** (W7a): its width, its fill and its border, and the
  -- box taken out - above the text's part, which works on the box's text.
  --
  function ins:box(s, x0, w0, y, box, on)
    pk.label(s, x0, y, "Text box")
    y = y + 18

    local half = (w0 - 8) // 2
    local wd = { x = x0, y = y, w = half, text = ("%g mm wide"):format(box.width_mm) }
    pk.stepper(s, wd)
    self:control("box_width", wd, function(cx, cy)
      local d = pk.step_at(wd, cx, cy)
      if d and d ~= 0 then
        on.reshape({ box = { width_mm = math.max(10, box.width_mm + 5 * d),
                             border = box.border, fill = box.fill } })
      end
    end)

    local fill_name = "None"
    for _, f in ipairs(FILLS) do if f[1] == box.fill then fill_name = f[2] end end

    local fc = { x = x0 + half + 8, y = y, w = w0 - half - 8, text = fill_name }
    pk.chooser(s, fc)
    self:control("box_fill", fc, function()
      local labels, chosen = {}, 1
      for i, f in ipairs(FILLS) do
        labels[i] = f[2]
        if f[1] == (box.fill or false) then chosen = i end
      end
      self:open_menu("fill", fc.x, fc.y + fc.h + 4, fc.w, labels, chosen, function(i)
        on.reshape({ box = { width_mm = box.width_mm, border = box.border,
                             fill = FILLS[i][1] or nil } })
      end)
    end)

    y = y + 38

    local bd = { x = x0, y = y + 4, text = "Border", on = box.border }
    pk.check(s, bd)
    self:control("box_border", bd, function()
      on.reshape({ box = { width_mm = box.width_mm, border = not box.border, fill = box.fill } })
    end)

    local del = { x = 0, y = y, text = "Delete box" }
    del.w = pk.button_width(del.text)
    del.x = x0 + w0 - del.w
    pk.button(s, del)
    self:control("box_delete", del, function() on.delete() end)

    return y + 44
  end

  --
  -- **In a table** (W5b): its rows and columns, a stepper each, whether its
  -- first row is a header, and the table taken out - above the text's
  -- part, which works on the cell's text.
  --
  function ins:table(s, x0, w0, y, t, on)
    pk.label(s, x0, y, "Table")
    y = y + 18

    local half = (w0 - 8) // 2
    local rows = { x = x0, y = y, w = half,
                   text = ("%d row%s"):format(#t.rows, #t.rows == 1 and "" or "s") }
    pk.stepper(s, rows)
    self:control("table_rows", rows, function(cx, cy)
      local d = pk.step_at(rows, cx, cy)
      if d and d ~= 0 then on.reshape({ rows = #t.rows + d }) end
    end)

    local cols = { x = x0 + half + 8, y = y, w = w0 - half - 8,
                   text = ("%d column%s"):format(t.columns, t.columns == 1 and "" or "s") }
    pk.stepper(s, cols)
    self:control("table_columns", cols, function(cx, cy)
      local d = pk.step_at(cols, cx, cy)
      if d and d ~= 0 then on.reshape({ columns = t.columns + d }) end
    end)

    y = y + 38

    local hr = { x = x0, y = y + 4, text = "Header row", on = t.header }
    pk.check(s, hr)
    self:control("table_header", hr, function() on.reshape({ header = not t.header }) end)

    local del = { x = 0, y = y, text = "Delete table" }
    del.w = pk.button_width(del.text)
    del.x = x0 + w0 - del.w
    pk.button(s, del)
    self:control("table_delete", del, function() on.delete() end)

    return y + 44
  end

  --
  -- **The text's part**: the paragraph style in a box of its own, its name
  -- large - `compact` in a table's cell - then Style (the font, its weight
  -- and size, Bold Italic Underline Strike, the colour), Layout (alignment,
  -- line spacing, lists, the space round a paragraph and its indents) and
  -- More (a drop cap, kept with the next).
  --
  function ins:text(s, x0, w0, y, look, layout, style, styles, compact, part, on)
    local sbox = { x = x0, y = y, w = w0, h = compact and 40 or 46 }
    s:fill_round(sbox.x, sbox.y, sbox.w, sbox.h, theme.raised, 12)
    s:text(sbox.x + 12, sbox.y + (sbox.h - gfx.height("title")) // 2, style,
           theme.text, nil, "title")
    pk.icon(s, "descending", sbox.x + sbox.w - 26, sbox.y + 15, theme.text_dim)
    self:control("style", sbox, function()
      local names = {}
      for i, st in ipairs(styles) do names[i] = st.name end
      self:open_menu("style", sbox.x, sbox.y + sbox.h + 4, sbox.w, names,
                     index_of(names, style), function(i) on.style(names[i]) end)
    end)

    y = sbox.y + sbox.h + 14

    local parts = { x = x0, y = y, w = w0,
                    items = { { text = "Style" }, { text = "Layout" }, { text = "More" } },
                    chosen = part == "style" and 1 or part == "layout" and 2 or 3 }
    pk.segments(s, parts)
    self:control("parts", parts, function(cx, cy)
      local i = pk.segment_at(parts, cx, cy)
      on.part(({ "style", "layout", "more" })[i] or part)
    end)

    y = parts.y + parts.h + 18

    if part == "style" then
      pk.label(s, x0, y, "Font")
      y = y + 18

      local fam = { x = x0, y = y, w = w0, text = look.face }
      pk.chooser(s, fam)
      self:control("face", fam, function()
        self:open_menu("face", fam.x, fam.y + fam.h + 4, fam.w, catalogue.names,
                       index_of(catalogue.names, look.face),
                       function(i) on.char({ face = catalogue.names[i] }) end)
      end)

      y = y + fam.h + 8

      local weights = self:weight_names(look.face)
      local wt = { x = x0, y = y, w = math.floor(w0 * 0.55), text = look.weight }
      pk.chooser(s, wt)
      self:control("weight", wt, function()
        self:open_menu("weight", wt.x, wt.y + wt.h + 4, wt.w, weights,
                       index_of(weights, look.weight),
                       function(i) on.char({ weight = weights[i] }) end)
      end)

      local size = { x = wt.x + wt.w + 8, y = y, w = w0 - wt.w - 8,
                     text = ("%g pt"):format(look.size_pt) }
      pk.stepper(s, size)
      self:control("size", size, function(cx, cy)
        local step = pk.step_at(size, cx, cy)

        if step == 0 then
          local labels = {}
          for i, v in ipairs(SIZES) do labels[i] = v .. " pt" end
          self:open_menu("size", size.x, size.y + size.h + 4, size.w, labels,
                         index_of(SIZES, look.size_pt),
                         function(i) on.char({ size_pt = SIZES[i] }) end)
        elseif step then
          on.char({ size_pt = math.max(1, look.size_pt + step) })
        end
      end)

      y = y + size.h + 10

      local bold = (faces.WEIGHT[look.weight] or 400) >= 600
      local biu = { x = x0, y = y, w = w0,
                    items = { { icon = "bold" }, { icon = "italic" },
                              { icon = "underline" }, { icon = "strike" } },
                    chosen = { bold, look.italic, look.underline, look.strike } }
      pk.segments(s, biu)
      self:control("marks", biu, function(cx, cy)
        local i = pk.segment_at(biu, cx, cy)

        if i == 1 then on.char({ weight = bold and "Regular" or "Bold" })
        elseif i == 2 then on.char({ italic = not look.italic })
        elseif i == 3 then on.char({ underline = not look.underline })
        elseif i == 4 then on.char({ strike = not look.strike }) end
      end)

      y = y + biu.h + 18
      pk.label(s, x0, y, "Text colour")
      y = y + 18

      local sw = { x = x0, y = y, w = 40, h = 22, colour = argb(look.colour) }
      pk.swatch(s, sw)
      s:text(x0 + 52, y + (22 - gfx.height()) // 2, colour_name(look.colour),
             theme.text, nil, "ui")
      local colour_box = { x = x0, y = y, w = w0, h = 22 }
      self:control("colour", colour_box, function()
        local labels, chosen = {}, nil
        for i, pair in ipairs(COLOURS) do
          labels[i] = pair[2]
          if pair[1] == look.colour then chosen = i end
        end
        self:open_menu("colour", x0, colour_box.y + 26, 160, labels, chosen,
                       function(i) on.char({ colour = COLOURS[i][1] }) end)
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
      self:control("align", al, function(cx, cy)
        local i = pk.segment_at(al, cx, cy)
        if i then on.para({ align = aligns[i] }) end
      end)

      y = y + al.h + 18
      pk.label(s, x0, y, "Line spacing")
      y = y + 18

      local sp = { x = x0, y = y, w = w0,
                   text = ("%g line%s"):format(layout.spacing_lines,
                                               layout.spacing_lines == 1 and "" or "s") }
      pk.chooser(s, sp)
      self:control("spacing", sp, function()
        local labels = {}
        for i, v in ipairs(SPACINGS) do labels[i] = ("%g"):format(v) .. (v == 1 and " line" or " lines") end
        self:open_menu("spacing", sp.x, sp.y + sp.h + 4, sp.w, labels,
                       index_of(SPACINGS, layout.spacing_lines),
                       function(i) on.para({ spacing_lines = SPACINGS[i] }) end)
      end)

      y = y + sp.h + 18
      pk.label(s, x0, y, "Bullets & lists")
      y = y + 18

      local LISTS = { "none", "bullet", "number" }
      local LIST_WORDS = { "None", "Bullets", "Numbers" }
      local li = { x = x0, y = y, w = w0,
                   text = LIST_WORDS[index_of(LISTS, layout.list) or 1] }
      pk.chooser(s, li)
      self:control("list", li, function()
        self:open_menu("list", li.x, li.y + li.h + 4, li.w, LIST_WORDS,
                       index_of(LISTS, layout.list),
                       function(i) on.para({ list = LISTS[i] }) end)
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
        self:control(r[1], st, function(cx, cy)
          local step = pk.step_at(st, cx, cy)
          if step and step ~= 0 then
            on.para({ [r[4]] = math.max(r[4] == "indent_first_mm" and -200 or 0,
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
      self:control("dropcap", dc, function()
        on.para({ drop_cap_lines = capped and 0 or 3 })
      end)

      local deep = { x = x0 + 120, y = y, w = w0 - 120,
                     text = ("%d lines"):format(capped and layout.drop_cap_lines or 3) }
      pk.stepper(s, deep)
      self:control("caplines", deep, function(cx, cy)
        local d = pk.step_at(deep, cx, cy)
        if d and d ~= 0 then
          on.para({ drop_cap_lines = math.max(2, math.min(10,
            (capped and layout.drop_cap_lines or 3) + d)) })
        end
      end)

      y = y + deep.h + 16

      local kwn = { x = x0, y = y, text = "Keep with the next paragraph",
                    on = layout.keep_with_next }
      pk.check(s, kwn)
      self:control("keep", kwn, function()
        on.para({ keep_with_next = not layout.keep_with_next })
      end)
    end
  end

  return ins
end

return inspector
