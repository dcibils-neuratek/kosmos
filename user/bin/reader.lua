-- kosmos: application
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

local where  = ui.label{ x = 12, y = 10, w = W - 24, text = "" }
local status = ui.label{ x = 12, y = H - 30, w = W - 24, text = "" }

local page = ui.view{ x = 12, y = 56, w = W - 24, h = H - 122 }
page.focusable = true

local function relayout()
  local columns = (page.w - 16) // GW
  lines = markdown.wrap(blocks, columns)
  top = 1
end

function page:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)

  local rows = (self.h - 8) // GH
  self.rows = rows

  for i = 0, rows - 1 do
    local l = lines[top + i]

    if not l then break end

    local y = 4 + i * GH

    if l.kind == "rule" then
      g:fill(8, y + GH // 2, self.w - 16, 1, theme.line)
    elseif l.kind == "blank" then
      -- nothing, and the space is the point
    elseif l.kind == "code" then
      g:fill(8, y, self.w - 16, GH, theme.sunken)
      g:text(12, y, l.text, theme.good, theme.sunken)
    elseif l.kind == "heading" then
      g:text(8, y, l.text, theme.ring, theme.window)
    elseif l.kind == "quote" then
      g:text(8, y, l.text, theme.text_dim, theme.window)
    else
      g:text(8, y, l.text, theme.text, theme.window)
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
    status.text = "could not read " .. tostring(from) .. ": " .. tostring(why)
    return
  end

  if type(body) ~= "string" then
    status.text = tostring(from) .. " is not text"
    return
  end

  blocks = markdown.parse(body)
  relayout()

  where.text  = from
  status.text = ("%d blocks, %d lines"):format(#blocks, #lines)
end

win:add(where)

win:add(ui.button{
  x = 12, y = 28, w = 70, h = 24, text = "Open",
  on_click = function()
    local chooser = panel.open{
      start = path and path:match("^(.*)/") or "/home",
      on_choose = function(chosen) load(chosen) end,
    }

    if chooser then chooser:run() end
  end,
})

win:add(page)
win:add(status)

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
  where.text  = "(the built-in page)"
  status.text = ("%d blocks, %d lines"):format(#blocks, #lines)
end

win:run()
