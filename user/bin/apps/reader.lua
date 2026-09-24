-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Misc_Book
-- A markdown viewer: manuals and tutorials, inside the system they describe.
--
--   wm reader                     the guide that ships in the image
--   wm reader:/home/notes.md      any file
--
--   arrows, PageUp/PageDown       move
--   Open                          choose a file
--
-- `roadmap.md` asked for this at M7 and named the missing piece exactly: not
-- the parser, which is small and in `/lib/markdown.lua`, but "a view that
-- wraps text". That is what this is - the parser produces blocks, the
-- wrapper turns them into lines that fit, and this draws the lines with a
-- colour per kind.
--
-- Headings are not larger, because the font is one size. They are brighter
-- and they have space around them, which is the same information carried by
-- what this display can actually do. Pretending to a second font size by
-- drawing a heading twice as wide would be worse than saying it plainly.

local ui       = use("/lib/ui.lua")
local panel    = use("/lib/panel.lua")
local markdown = use("/lib/markdown.lua")
local theme    = ui.theme

local W, H = 620, 460

local path = args:match("^%s*(%S+)")

local win, err = ui.window{ title = "Reader", w = W, h = H, x = 110, y = 60 }

if not win then
  print("reader: " .. tostring(err))
  return
end

local GW, GH = gfx.font.w, gfx.font.h

local blocks, lines, top = {}, {}, 1

local L = ui.layout

--
-- **As `docs/apps.html` draws it** (`roadmap.md` 5zp): the header names the
-- document and holds Open, and the page is the rest of the window - white,
-- edge to edge, with the drawings' 26 of margin inside it rather than a
-- framed box with a label above and a status line below. What the status
-- line said - how many blocks and lines - is the header's `sub`, beside
-- the document's name.
--
local header                          -- made below, once `open_one` exists

local page = ui.view{ x = 0, y = L.head, w = W, h = H - L.head,
                      follow = { "left", "right", "top", "bottom" } }
page.focusable = true

local PAGE_IN = L.page_side

local function relayout()
  local columns = (page.w - 2 * PAGE_IN) // GW
  lines = markdown.wrap(blocks, columns)
  top = 1
end

function page:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)

  local top_pad = L.page_top - 4
  local rows = (self.h - top_pad - L.page_foot) // GH
  self.rows = rows

  for i = 0, rows - 1 do
    local l = lines[top + i]

    if not l then break end

    local y = top_pad + i * GH
    local x = PAGE_IN

    if l.kind == "rule" then
      g:fill(x, y + GH // 2, self.w - 2 * x, 1, theme.line_soft)
    elseif l.kind == "blank" then
      -- nothing, and the space is the point
    elseif l.kind == "code" then
      g:fill(x - 6, y, self.w - 2 * x + 12, GH, theme.window)
      g:text(x, y, l.text, theme.text, theme.window, "mono")
    elseif l.kind == "heading" then
      g:text(x, y, l.text, theme.text, theme.sunken, "heading")
    elseif l.kind == "quote" then
      g:text(x, y, l.text, theme.text_dim, theme.sunken)
    else
      g:text(x, y, l.text, theme.text, theme.sunken)
    end
  end
end

function page:key(c)
  local rows = self.rows or 20

  if c == -2 then top = math.min(top + 1, math.max(1, #lines - rows + 1))
  elseif c == -1 then top = math.max(1, top - 1)
  elseif c == -3 then top = math.min(top + rows, math.max(1, #lines - rows + 1))
  elseif c == -4 then top = math.max(1, top - rows)
  else return false end

  return true
end

function page:mouse(action, x, y)
  return action == "press"
end

local function load(from)
  local body, why = fs.read(from)

  if not body then
    header.sub = "could not read " .. tostring(from) .. ": " .. tostring(why)
    return
  end

  if type(body) ~= "string" then
    header.sub = tostring(from) .. " is not text"
    return
  end

  blocks = markdown.parse(body)
  relayout()

  header.title = from:match("([^/]+)$") or from
  header.sub = ("%d blocks, %d lines"):format(#blocks, #lines)
end

local function open_one()
  local chooser = panel.open{
    start = path and path:match("^(.*)/") or "/home",
    on_choose = function(chosen) load(chosen) end,
  }

  if chooser then chooser:run() end
end

header = ui.header{ x = 0, y = 0, w = W, title = "Reader",
                    sub = "the built-in page",
                    right = { ui.button{ text = "Open", on_click = open_one } } }

win:add(header)
win:add(page)

if path then
  load(path)
else
  -- Something to read on an empty machine, so that opening this with no
  -- argument shows what it is for rather than an empty window.
  blocks = markdown.parse([[
# Reader

A markdown viewer, written for the manuals this system will carry.

## What it renders

- headings, at any level
- paragraphs, wrapped to the window
- bullet and numbered lists
- block quotes
- horizontal rules

Inline `code` keeps its backticks, and a fenced block is shown whole:

```
local ui = use("/lib/ui.lua")
print("hello from Kosmos")
```

> Headings are brighter rather than larger. The font has one size, and
> pretending otherwise would be a worse lie than saying so.

---

## Reading a file

Press Open and choose one. `wm reader:/home/notes.md` opens it directly.
]])
  relayout()
  header.sub = ("the built-in page · %d blocks, %d lines")
               :format(#blocks, #lines)
end

win:run()
