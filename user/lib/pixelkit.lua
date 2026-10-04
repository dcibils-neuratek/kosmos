-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The kit's header and controls, drawn into a surface.
--
--   local ui = use("/Kosmos/Libraries/ui.lua")
--   local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
--   pk.header(s, 0, 0, W, "PDF", "page 3 of 120")
--   pk.iconbutton(s, { x = 10, y = 10, icon = "back" })
--   pk.button(s, { x = W - 90, y = 7, text = "Open", go = true })
--
-- **For a window that draws its own pixels** (`gfx.md` 19.4) - the browser,
-- the PDF viewer, Paint, System Benchmark. Those own every pixel they show,
-- so they cannot be handed `ui.header` or a `ui.button`: a widget is a list
-- of commands the window manager draws, and a direct window sends none.
-- They drew their own chrome, each its own way, and on 24 September that
-- was four windows with bars of word buttons and bevels beside a desktop
-- whose every other window had the drawings' header (`roadmap.md` 5zs).
--
-- So this is the same header and the same controls, at the same numbers
-- (`ui.layout`, `docs/apps.html`), drawn with a surface's own primitives.
-- It keeps no state and does no input: where a control is, and whether it
-- is pressed, is the application's, because an application that draws its
-- own pixels already routes its own events.
--
-- The line icons are the kit's pictures (`tools/lineicons.py`), decoded
-- once and painted through their coverage in a look's colour - the same
-- `tint` the window manager uses for a widget's icon.

local pixelkit = {}

function pixelkit.new(ui)
  local theme = ui.theme
  local L = ui.layout
  local pk = { L = L }

  local ICON, BOX, BUTTON_H, R = 15, 26, 31, 7
  local pictures = {}

  -- A line icon's coverage, decoded the first time it is asked for.
  local function picture(name)
    local p = pictures[name]

    if p == nil then
      local bytes = sys.asset("line/" .. name .. "-15.png")
      local ok, made = false, nil

      if bytes then ok, made = pcall(gfx.png, bytes) end

      p = (ok and made) or false
      pictures[name] = p
    end

    return p or nil
  end

  -- A line icon at `x, y`, 15 across, in `colour`.
  function pk.icon(s, name, x, y, colour)
    local p = picture(name)

    if p then s:tint(p, 0, 0, ICON, ICON, x, y, colour or theme.text_dim) end
  end

  --
  -- **The header**: 46 with its rule the last pixel, on the header's
  -- colour, the subject in the title face 18 in and what the window is
  -- looking at beside it in the dim `ui` face. Returns where the words
  -- ended, for a caller that puts something after them.
  --
  -- `room` is where the controls on the right begin, so the words are cut
  -- with an ellipsis before them rather than run under them.
  --
  function pk.header(s, x, y, w, title, sub, room, title_x)
    s:fill(x, y, w, L.head - 1, theme.sunken)
    s:fill(x, y + L.head - 1, w, 1, theme.line_soft)

    local tx = x + (title_x or L.head_in)
    title = tostring(title or "")

    if title ~= "" then
      s:text(tx, y + (L.head - 1 - gfx.height("title")) // 2, title,
             theme.text, nil, "title")
      tx = tx + gfx.measure(title, "title") + 8
    end

    sub = tostring(sub or "")

    if sub ~= "" then
      local limit = (room or (x + w)) - L.head_edge - tx

      if gfx.measure(sub) > limit then
        while #sub > 0 and gfx.measure(sub .. "...") > limit do
          sub = sub:sub(1, -2)
        end

        sub = (sub ~= "") and (sub .. "...") or ""
      end

      s:text(tx, y + (L.head - 1 - gfx.height()) // 2, sub, theme.text_dim,
             nil, "ui")
      tx = tx + gfx.measure(sub)
    end

    return tx
  end

  -- Where a control sits in a header's band: centred in the 45 above the
  -- rule.
  function pk.centre(h) return (L.head - 1 - h) // 2 end

  --
  -- **An icon button**: 26 square, no border, the icon in the dim colour; a
  -- quiet rounded fill while it is held, and nothing at all while it cannot
  -- be used but the icon at a third of its strength.
  --
  -- `b` is `{ x, y, icon, pressed, disabled }`, and gets its `w` and `h`.
  --
  function pk.iconbutton(s, b)
    b.w, b.h = BOX, BOX

    if b.pressed and not b.disabled then
      s:fill_round(b.x, b.y, BOX, BOX, theme.line_soft, 6)
    end

    local colour = b.disabled and theme.mix(theme.sunken, theme.text_dim, 350)
                   or (b.pressed and theme.text or theme.text_dim)

    pk.icon(s, b.icon, b.x + (BOX - ICON) // 2, b.y + (BOX - ICON) // 2,
            colour)
  end

  -- How wide a button with these words is: the words and 13 either side.
  function pk.button_width(text)
    return gfx.measure(tostring(text or "")) + 26
  end

  --
  -- **A button**: 31 tall, a radius of 7, the words centred - on the
  -- sunken colour with a one-pixel rule, or filled with the accent when it
  -- is the verb that starts something (`go`). Held, it goes a shade darker.
  --
  -- `b` is `{ x, y, text, go, pressed, disabled }`, and gets its `w` and
  -- `h` unless it has them.
  --
  function pk.button(s, b)
    b.h = b.h or BUTTON_H
    b.w = b.w or pk.button_width(b.text)

    local label = tostring(b.text or "")
    local ty = b.y + (b.h - gfx.height()) // 2
    local tx = b.x + (b.w - gfx.measure(label)) // 2

    if b.go and not b.disabled then
      local fill = b.pressed and theme.lift(theme.accent, -24) or theme.accent

      s:fill_round(b.x, b.y, b.w, b.h, fill, R)
      s:text(tx, ty, label, theme.text_on, nil, "ui")
      return
    end

    s:fill_round(b.x, b.y, b.w, b.h,
                 b.pressed and theme.line_soft or theme.sunken, R)
    s:frame_round(b.x, b.y, b.w, b.h, theme.line_soft, R)
    s:text(tx, ty, label, b.disabled and theme.text_dim or theme.text, nil,
           "ui")
  end

  --
  -- **A field's box**: the button's well and rule, the ring when it has the
  -- keyboard. The words are the caller's, because what of them shows - a
  -- URL scrolled to its caret - is the caller's to decide.
  --
  function pk.field(s, b, focused)
    b.h = b.h or BUTTON_H

    s:fill_round(b.x, b.y, b.w, b.h, theme.sunken, R)
    s:frame_round(b.x, b.y, b.w, b.h,
                  focused and theme.ring or theme.line_soft, R)
  end

  -- Whether a point is on a control that has been drawn.
  function pk.inside(b, x, y)
    return b.x and b.w and x >= b.x and x < b.x + b.w
           and y >= b.y and y < b.y + b.h
  end

  --
  -- **The empty state**: a line in the label face and the rest dim under
  -- it, centred in `x, y, w, h` - Video's and Mixer's, for a window with
  -- nothing in it yet.
  --
  function pk.empty(s, x, y, w, h, lines)
    local step = gfx.height("text") + 6
    local ty = y + (h - #lines * step) // 2

    for i, line in ipairs(lines) do
      local face = (i == 1) and "label" or "text"
      local tw = gfx.measure(line, face)

      s:text(x + (w - tw) // 2, ty, line,
             (i == 1) and theme.text or theme.text_dim, nil, face)
      ty = ty + step
    end
  end

  --------------------------------------------------------------------------
  -- **An inspector's controls** (`docs/write.html`): what Kosmos Write's
  -- toolbar and its Format and Document panels are made of, and Present's
  -- and Sheets' after it - so here, for every window that draws its own
  -- pixels, rather than in the first application to want them. Each draws
  -- itself from a table and gives it its size; where a press lands in one
  -- is `pk.inside`, or `pk.part` for one with parts.
  --------------------------------------------------------------------------

  local SMALL                   -- the tools' words, a size under the ui's

  local function small()
    SMALL = SMALL or ui.sized("ui", math.max(9, (theme.fonts.ui and theme.fonts.ui.px or 13) - 2))
    return SMALL
  end

  local function centred(s, text, x, w, y, colour, face)
    s:text(x + (w - gfx.measure(text, face)) // 2, y, text, colour, nil, face)
  end

  --
  -- **A tool**: an icon over its word, as a document window's toolbar has
  -- them - the raised fill and the accent's icon when it is the one open.
  -- `b` is `{ x, y, icon, text, on, disabled }`, and gets `w` and `h`.
  --
  -- How wide a tool with these words is.
  function pk.tool_width(text)
    return math.max(52, gfx.measure(tostring(text or ""), small()) + 16)
  end

  function pk.tool(s, b)
    local face = small()

    b.h = b.h or 48
    b.w = b.w or pk.tool_width(b.text)

    if b.on then s:fill_round(b.x, b.y, b.w, b.h, theme.raised, 10) end

    local ink = b.disabled and theme.mix(theme.window, theme.text_dim, 400)
                or (b.on and theme.accent or theme.text)

    pk.icon(s, b.icon, b.x + (b.w - ICON) // 2, b.y + 8, ink)
    centred(s, b.text, b.x, b.w, b.y + 8 + ICON + 6,
            b.disabled and ink or (b.on and theme.text or theme.text_dim), face)
  end

  --
  -- **Segments**: two or more choices in one rounded bar, the chosen one
  -- raised - a panel's tabs, Bold Italic Underline Strike, the four
  -- alignments. `b` is `{ x, y, w, items, chosen, h, accent }`: each item
  -- `{ text }` or `{ icon }`, `chosen` an index, or a set of them
  -- (`chosen[i]` true) where more than one can be on; `accent` fills the
  -- chosen one with the accent, as a panel's tabs are.
  --
  function pk.segments(s, b)
    b.h = b.h or 28

    local n = #b.items
    local cell = (b.w - 6) / n

    s:fill_round(b.x, b.y, b.w, b.h, theme.raised, 9)

    for i, item in ipairs(b.items) do
      local cx = math.floor(b.x + 3 + (i - 1) * cell)
      local cw = math.floor(b.x + 3 + i * cell) - cx
      local on = type(b.chosen) == "table" and b.chosen[i] or b.chosen == i

      if on then
        s:fill_round(cx, b.y + 3, cw, b.h - 6,
                     b.accent and theme.accent or theme.line, 7)
      end

      local ink = on and (b.accent and theme.text_on or theme.text) or theme.text_dim

      if item.icon then
        pk.icon(s, item.icon, cx + (cw - ICON) // 2, b.y + (b.h - ICON) // 2, ink)
      else
        centred(s, item.text, cx, cw, b.y + (b.h - gfx.height()) // 2, ink, "ui")
      end
    end
  end

  -- Which segment a point is on, or nil.
  function pk.segment_at(b, x, y)
    if not pk.inside(b, x, y) then return nil end
    return math.min(#b.items, 1 + math.floor((x - b.x - 3) / ((b.w - 6) / #b.items)))
  end

  --
  -- **A chooser**: a value in a well with the arrow that says a list opens
  -- under it (`pk.menu`). `b` is `{ x, y, w, text, h, face }`.
  --
  function pk.chooser(s, b)
    b.h = b.h or 30

    s:fill_round(b.x, b.y, b.w, b.h, theme.raised, 8)

    local text = tostring(b.text or "")
    local room = b.w - 34

    while #text > 1 and gfx.measure(text, b.face) > room do text = text:sub(1, -2) end

    s:text(b.x + 10, b.y + (b.h - gfx.height(b.face)) // 2, text, theme.text, nil,
           b.face or "ui")
    pk.icon(s, "descending", b.x + b.w - ICON - 8, b.y + (b.h - ICON) // 2,
            theme.text_dim)
  end

  --
  -- **A stepper**: a number with its unit in a well, and minus and plus at
  -- its right end. `b` is `{ x, y, w, text, h }`; `pk.step_at` says which
  -- end a press took: -1, 1, or 0 for the number itself.
  --
  function pk.stepper(s, b)
    b.h = b.h or 30

    s:fill_round(b.x, b.y, b.w, b.h, theme.raised, 8)
    s:text(b.x + 10, b.y + (b.h - gfx.height()) // 2, tostring(b.text or ""),
           theme.text, nil, "ui")

    local bx = b.x + b.w - 2 * (ICON + 10)

    s:fill(bx, b.y + 6, 1, b.h - 12, theme.line_soft)
    pk.icon(s, "minus", bx + 5, b.y + (b.h - ICON) // 2, theme.text_dim)
    pk.icon(s, "plus", bx + ICON + 15, b.y + (b.h - ICON) // 2, theme.text_dim)
  end

  function pk.step_at(b, x, y)
    if not pk.inside(b, x, y) then return nil end

    local bx = b.x + b.w - 2 * (ICON + 10)

    if x < bx then return 0 end
    if x < bx + ICON + 10 then return -1 end
    return 1
  end

  --
  -- **A box to tick**, and its words. `b` is `{ x, y, text, on }`, and gets
  -- `w` and `h`.
  --
  function pk.check(s, b)
    b.h = b.h or 22
    b.w = b.w or (16 + 8 + gfx.measure(b.text or ""))

    local by = b.y + (b.h - 16) // 2

    s:fill_round(b.x, by, 16, 16, b.on and theme.accent or theme.line, 4)

    if b.on then pk.icon(s, "check", b.x + 1, by + 1, theme.text_on) end

    s:text(b.x + 24, b.y + (b.h - gfx.height()) // 2, tostring(b.text or ""),
           theme.text, nil, "ui")
  end

  --
  -- **A colour's swatch**, ringed so a white one shows on a light panel.
  -- `b` is `{ x, y, colour, w, h }`, `colour` 0xAARRGGBB.
  --
  function pk.swatch(s, b)
    b.w, b.h = b.w or 40, b.h or 20

    s:fill_round(b.x - 1, b.y - 1, b.w + 2, b.h + 2, theme.line, 6)
    s:fill_round(b.x, b.y, b.w, b.h, b.colour, 5)
  end

  -- A panel's small heading, in the dim colour.
  function pk.label(s, x, y, text)
    s:text(x, y, tostring(text), theme.text_dim, nil, small())
  end

  --
  -- **A list that opens over the window** - a chooser's, a toolbar menu's:
  -- each item a line, the chosen one ticked, the one under the pointer
  -- lifted. `b` is `{ x, y, w, items, chosen, hover }`, each item a string
  -- or `{ text, face }` - a face being a paragraph style's look - and gets
  -- its `h`; `pk.menu_at` is the item under a point.
  --
  local ROW = 30

  function pk.menu(s, b)
    b.h = #b.items * ROW + 8

    s:fill_round(b.x + 3, b.y + 4, b.w, b.h, theme.edge_dark or 0xff000000, 10)
    s:fill_round(b.x, b.y, b.w, b.h, theme.raised, 10)
    s:frame_round(b.x, b.y, b.w, b.h, theme.line, 10)

    for i, item in ipairs(b.items) do
      local text = type(item) == "table" and item.text or tostring(item)
      local face = type(item) == "table" and item.face or "ui"
      local ry = b.y + 4 + (i - 1) * ROW

      if b.hover == i then
        s:fill_round(b.x + 4, ry, b.w - 8, ROW, theme.line, 7)
      end

      if b.chosen == i then
        pk.icon(s, "check", b.x + 10, ry + (ROW - ICON) // 2, theme.accent)
      end

      s:text(b.x + 32, ry + (ROW - gfx.height(face)) // 2, text, theme.text, nil,
             face)
    end
  end

  function pk.menu_at(b, x, y)
    if not pk.inside(b, x, y) then return nil end
    local i = 1 + (y - b.y - 4) // ROW
    if i >= 1 and i <= #b.items then return i end
  end

  return pk
end

return pixelkit
