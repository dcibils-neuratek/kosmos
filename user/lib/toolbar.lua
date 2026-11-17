-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A document window's row of tools, drawn into a surface: a group from the
-- left, a group against the right edge, and a note between them
-- (`docs/office.md` O1, `docs/present.md`, *What moves out of Kosmos
-- Write*).
--
--   local ui = use("/Kosmos/Libraries/ui.lua")
--   local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
--   use("/Kosmos/Libraries/toolbar.lua").into(ui, pk)
--
--   local TOOLS = {
--     { key = "view", icon = "sidebar", text = "View" },
--     { gap = 14 },                     -- room between two groups
--     ...
--     { right = true },                 -- what follows stands at the right edge
--     { key = "format", icon = "format", text = "Format" },
--   }
--
--   pk.toolbar(s, x, y, w, h, TOOLS, {
--     word = function(t) return t.text end,   -- words that change, a zoom's
--     on = function(t) return false end,      -- a tool shown as open
--     note = "Letter - 2 pages",              -- between the groups, cut to fit
--   })
--   local t = pk.tool_at(TOOLS, x)            -- the tool a press in the row is on
--
-- Each tool is `pk.tool`'s, and is given `x`, `y` and `w` where it was
-- drawn. A tool with `later` set is shown greyed and is never pressed:
-- a step not built yet.
--
-- **Why it is `pk.toolbar` and not a library of its own**: it is a control
-- like the others `pixelkit` draws, and the door every window that draws
-- its own pixels already has. It is written here and put into `pk` because
-- `pixelkit.lua` was outside what O1's change touched; folding it into
-- that file is a move of these lines and nothing else, and no caller would
-- change.
--
-- It was Kosmos Write's, inside `writer.lua`, until O1 (17 November 2026).

local toolbar = {}

function toolbar.into(ui, pk)
  local theme = ui.theme

  function pk.toolbar(s, x0, y0, w0, h0, tools, opts)
    opts = opts or {}

    local function word(t) return opts.word and opts.word(t) or t.text end
    local function on(t) return opts.on and opts.on(t) or false end

    s:fill(x0, y0, w0, h0, theme.window)
    s:fill(x0, y0 + h0 - 1, w0, 1, theme.line_soft)

    -- Left to right until the gap that pushes the rest to the right edge.
    local x, right = x0 + 12, nil

    for i, t in ipairs(tools) do
      if t.right then right = i break end

      if t.gap then
        x = x + t.gap
      else
        local b = { x = x, y = y0 + 8, icon = t.icon, text = word(t),
                    disabled = t.later ~= nil }
        b.on = on(t)
        pk.tool(s, b)
        t.x, t.y, t.w = b.x, b.y, b.w
        x = x + t.w + 2
      end
    end

    local rx = x0 + w0 - 12

    for i = #tools, (right or #tools) + 1, -1 do
      local t = tools[i]
      local b = { x = 0, y = y0 + 8, icon = t.icon, text = word(t),
                  disabled = t.later ~= nil, on = on(t) }

      b.w = pk.tool_width(word(t))
      rx = rx - b.w
      b.x = rx
      pk.tool(s, b)
      t.x, t.y, t.w = b.x, b.y, b.w
      rx = rx - 2
    end

    if not opts.note then return end

    -- The note, in the room between the two groups.
    local from, to = x0, x0 + w0

    for _, t in ipairs(tools) do
      if t.right then break end
      if t.x then from = t.x + t.w end
    end

    for i = #tools, 1, -1 do
      if tools[i].right then break end
      to = tools[i].x or to
    end

    local room = to - from - 32
    local note = ui.fitted(opts.note, room, "ui")

    if room > 40 then
      s:text(from + 16, y0 + (h0 - gfx.height()) // 2, note, theme.text_dim, nil, "ui")
    end
  end

  -- The tool a press at `x` is on, in a row the press is already known to
  -- be in.
  function pk.tool_at(tools, x)
    for _, t in ipairs(tools) do
      if t.key and t.x and not t.later and x >= t.x and x < t.x + t.w then
        return t
      end
    end
  end

  return pk
end

return toolbar
