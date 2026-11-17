-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The find bar: a field for what is looked for, how many there are and
-- which one is shown, the arrows, a field for what replaces it, Replace and
-- All, and the switches - drawn into a surface (`docs/office.md`, *Find and
-- replace*, O2; `docs/write.html`, *The next Write*).
--
--   local ui = use("/Kosmos/Libraries/ui.lua")
--   local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
--   use("/Kosmos/Libraries/findbar.lua").into(ui, pk)
--
--   local bar = { x = 0, y = 110, w = 900,
--                 needle = "river", count = 11, current = 3,
--                 replacing = true, replacement = "",
--                 case = false, words = true, within = false,
--                 focus = "find" }         -- or "replace", or nil
--   pk.findbar(s, bar)                     -- draws it, and gives it its `h`
--   pk.findbar_at(bar, x, y)               -- "find", "previous", "next",
--                                          -- "replacement", "replace", "all",
--                                          -- "case", "words", "within", "close"
--
-- **A control, not a search.** What is looked for and where is the
-- window's: Write's body through `textedit:find`, Sheets' cells, the Text
-- Editor's `textbuf:find`, the PDF Viewer's `pdf.text`. The bar says what
-- was typed and what was pressed, and shows the count it is told - one bar,
-- several searches, and the field written once.
--
-- The arrows are the line icons `ascending` and `descending`, since a
-- triangle is a glyph not every face here has.
--
-- `pk.findbar` is written here and put into `pk`, as `pk.toolbar` is
-- (`toolbar.lua`), because `pixelkit.lua` was outside what O2 touched;
-- folding it in is a move of these lines.

local findbar = {}

findbar.H = 44

function findbar.into(ui, pk)
  local theme = ui.theme

  -- A field's words, the end of them when they are longer than it, and the
  -- caret after them when it has the keyboard - or, `chosen`, the words
  -- shown selected, as a bar opened again shows them: the next letter
  -- typed replaces them.
  local function field(s, box, words, hint, focused, chosen)
    pk.field(s, box, focused)

    local shown = ui.fitted(words or "", box.w - 20, "ui", true)
    local ty = box.y + (box.h - gfx.height()) // 2

    if shown == "" and not focused then
      s:text(box.x + 10, ty, hint, theme.text_dim, nil, "ui")
    elseif chosen and focused and shown ~= "" then
      s:fill_round(box.x + 8, box.y + 6, gfx.measure(shown) + 4, box.h - 12, theme.accent, 3)
      s:text(box.x + 10, ty, shown, theme.text_on, nil, "ui")
      return
    else
      s:text(box.x + 10, ty, shown, theme.text, nil, "ui")
    end

    if focused then
      s:fill(box.x + 10 + gfx.measure(shown) + 1, box.y + 7, 2, box.h - 14, theme.ring)
    end
  end

  function pk.findbar(s, b)
    b.h = findbar.H

    local parts = {}
    local y = b.y + (b.h - 31) // 2
    local x = b.x + 14

    b.parts = parts

    s:fill(b.x, b.y, b.w, b.h, theme.window)
    s:fill(b.x, b.y + b.h - 1, b.w, 1, theme.line_soft)

    -- A button of words, or of a line icon, the accent's when it is on.
    local function button(key, text, on, w, icon)
      local box = { x = x, y = y, text = text, go = on and true or false, w = w }
      pk.button(s, box)
      if icon then pk.icon(s, icon, box.x + (box.w - 15) // 2, box.y + 8, theme.text) end
      parts[#parts + 1] = { key = key, box = box }
      x = x + box.w + 6
      return box
    end

    local find = { x = x, y = y, w = 230, h = 31 }
    field(s, find, b.needle, "Find", b.focus == "find", b.chosen)
    parts[#parts + 1] = { key = "find", box = find }
    x = x + find.w + 8

    -- How many, and which: "3 of 11", "None", or nothing before a word.
    local count = ""
    if b.needle and b.needle ~= "" then
      count = (b.count or 0) == 0 and "None"
              or ("%d of %d"):format(b.current or 0, b.count)
    end

    s:text(x, y + (31 - gfx.height()) // 2, count, theme.text_dim, nil, "ui")
    x = x + math.max(64, gfx.measure(count) + 10)

    button("previous", "", false, 34, "ascending")
    button("next", "", false, 34, "descending")
    x = x + 8

    if b.replacing then
      local with = { x = x, y = y, w = 200, h = 31 }
      field(s, with, b.replacement, "Replace with\u{2026}", b.focus == "replace")
      parts[#parts + 1] = { key = "replacement", box = with }
      x = x + with.w + 8

      button("replace", "Replace", false)
      button("all", "All", false)
      x = x + 8
    end

    button("case", "Aa", b.case)
    button("words", "Whole words", b.words)
    button("within", "In selection", b.within)

    local close = { x = b.x + b.w - 14 - 26, y = b.y + (b.h - 26) // 2, icon = "close" }
    pk.iconbutton(s, close)
    parts[#parts + 1] = { key = "close", box = close }
  end

  -- The part of the bar a press is on, by its name, or nil.
  function pk.findbar_at(b, x, y)
    for _, p in ipairs(b.parts or {}) do
      if pk.inside(p.box, x, y) then return p.key end
    end
  end

  return pk
end

return findbar
