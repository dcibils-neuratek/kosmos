-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The kit's header and controls, drawn into a surface.
--
--   local ui = use("/lib/ui.lua")
--   local pk = use("/lib/pixelkit.lua").new(ui)
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

  return pk
end

return pixelkit
