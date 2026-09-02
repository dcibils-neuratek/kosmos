-- kosmos: application
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A PDF, in a window.
--
--   wm pdfview
--   wm pdfview:/home/odyssey.pdf
--
-- **What this shows and what it does not.** The page's text, laid out by
-- this window rather than by the document: the system font, wrapped to the
-- width you give it. Not the page as its author arranged it - no Times New
-- Roman, no columns, no images, no line breaks where the typesetter put
-- them.
--
-- That is a real limitation and it is worth being plain about, because the
-- distance from here to a viewer that draws the page properly is one piece:
-- `gfx` rasterises a glyph by *codepoint*, and a PDF with a CID font hands
-- out glyph *indices* into a font embedded in the document. Reaching those
-- needs `stbtt_GetGlyphBitmap` and a font loaded from bytes rather than
-- from the built-in table, both of which are small C additions to the
-- graphics kit. When they exist this window keeps its shape and changes
-- what it draws into.
--
-- Until then this is a reader rather than a viewer, and for a book - which
-- is what it was built against - a reader is most of what you want.

local ui       = use("/lib/ui.lua")
local panel    = use("/lib/panel.lua")
local pdf      = use("/lib/pdf.lua")
local pdfpage  = use("/lib/pdfpage.lua")
local theme    = ui.theme

local W, H = 700, 520

local path = args and args:match("^%s*(%S+)")

local win, err = ui.window{ title = "PDF", w = W, h = H, x = 90, y = 50 }

if not win then
  print("pdfview: " .. tostring(err))
  return
end

local GW, GH = gfx.font.w, gfx.font.h

local doc, lines, wrapped, top, current = nil, {}, {}, 1, 1

local where  = ui.label{ x = 12, y = 10, w = W - 24, text = "no document" }
local status = ui.label{ x = 12, y = H - 30, w = W - 24, text = "" }

local view = ui.view{ x = 12, y = 56, w = W - 24, h = H - 122 }
view.focusable = true

--------------------------------------------------------------------------
-- Wrapping.
--
-- The interpreter gives back one string per line of the page, which is the
-- typesetter's idea of a line and not this window's. A book page is set to
-- about seventy characters and this window is not, so a line is broken at
-- the last space that fits and the remainder carries on.
--------------------------------------------------------------------------

local function wrap(source, columns)
  local out = {}

  if columns < 8 then columns = 8 end

  for _, line in ipairs(source) do
    if #line == 0 then
      out[#out + 1] = ""
    end

    while #line > 0 do
      if #line <= columns then
        out[#out + 1] = line
        break
      end

      -- The last space that fits. A word longer than the window is cut
      -- rather than allowed to run off the edge, which is rare in prose and
      -- common in a URL.
      local cut = nil

      for i = columns + 1, 2, -1 do
        if line:sub(i, i) == " " then cut = i break end
      end

      if not cut then cut = columns + 1 end

      out[#out + 1] = line:sub(1, cut - 1)
      line = line:sub(cut):gsub("^%s+", "")
    end
  end

  return out
end

local function relayout()
  wrapped = wrap(lines, (view.w - 16) // GW)
  top = 1
end

--------------------------------------------------------------------------

function view:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:frame(0, 0, self.w, self.h, self.focused and theme.ring or theme.line)

  local rows = (self.h - 8) // GH
  self.rows = rows

  for i = 0, rows - 1 do
    local line = wrapped[top + i]

    if not line then break end

    g:text(8, 4 + i * GH, line, theme.text, theme.window)
  end
end

local show                                   -- forward: keys change the page

function view:key(c)
  local rows = self.rows or 20
  local last = math.max(1, #wrapped - rows + 1)

  if     c == -2 then top = math.min(top + 1, last)
  elseif c == -1 then top = math.max(1, top - 1)
  elseif c == -3 then top = math.min(top + rows, last)
  elseif c == -4 then top = math.max(1, top - rows)
  elseif c == 46 then show(current + 1)      -- '.'
  elseif c == 44 then show(current - 1)      -- ','
  else return false end

  return true
end

function view:mouse(action)
  return action == "press"
end

--------------------------------------------------------------------------
-- The document.
--------------------------------------------------------------------------

-- One region, made once and reused: `pdf.lua` reads a window at a time and
-- never holds the file, which is what makes a 1.6 MB book openable on a
-- 2 MB heap at all.
local buffer, capacity

local function source_for(file, size)
  if not buffer then
    -- Four pages, not sixteen. `pdf.lua` reads in 256-byte windows and the
    -- only larger read is a page's content stream, which goes straight into
    -- `pdfpage`'s own region and never through here.
    buffer = sys.memory(4)
    capacity = 4 * 4096
  end

  if not buffer then return nil end

  return {
    size = size,

    read = function (offset, length)
      local out, done = {}, 0

      while done < length do
        local want = length - done
        if want > capacity then want = capacity end

        local got = fs.read_into(file, buffer, offset + done, want)
        if not got or got == 0 then break end

        out[#out + 1] = sys.region_read(buffer, 0, got)
        done = done + got
      end

      return table.concat(out)
    end,

    -- The path a page takes: into pages this process owns, with no copy
    -- through the heap on the way.
    read_into = function (region, offset, length)
      return fs.read_into(file, region, offset, length)
    end,
  }
end

show = function (n)
  if not doc then return end

  if n < 1 then n = 1 end
  if n > #doc.pages then n = #doc.pages end

  current = n

  collectgarbage()

  local started = sys.ticks()
  local ok, got = pcall(pdfpage.text, doc, doc:page(n))

  if not ok then
    -- Onto the serial line as well as into the window. A window can only
    -- show what a person is there to read, and the harness that checks this
    -- is not a person: an error visible in a screenshot and invisible in a
    -- log is one that automated checking will report as a pass.
    print(("pdfview: page %d: %s"):format(n, tostring(got)))
    lines = { "this page could not be read:", tostring(got) }
  else
    lines = got
    if #lines == 0 then lines = { "(this page has no text on it)" } end
  end

  relayout()

  status.text = ("page %d of %d - %d lines, %d ms   , . to turn")
                :format(n, #doc.pages, #lines,
                        (sys.ticks() - started) // 62500)
end

local function open(file)
  local attrs, why = fs.getattr(file)

  if not attrs then
    status.text = ("cannot open %s: %s"):format(file, tostring(why))
    return
  end

  local source = source_for(file, attrs.size)

  if not source then
    status.text = "no memory for a read buffer"
    return
  end

  local ok, got = pcall(pdf.open, source)

  if not ok then
    status.text = tostring(got)
    where.text  = file
    lines = {}
    relayout()
    return
  end

  doc = got
  where.text = ("%s - PDF %s, %d pages")
               :format(file, doc.version, #doc.pages)

  -- The first page with text on it. A book's first page is a cover, and
  -- opening on a blank window looks like a failure.
  local first = 1

  for i = 1, #doc.pages do
    local resources = doc:resolve(doc:page(i).Resources) or {}
    if doc:resolve(resources.Font) then first = i break end
  end

  show(first)
end

--------------------------------------------------------------------------

win:add(where)

win:add(ui.button{
  x = 12, y = 28, w = 70, h = 24, text = "Open",
  on_click = function ()
    local chooser = panel.open{
      start = path and path:match("^(.*)/") or "/home",
      on_choose = function (chosen) open(chosen) end,
    }

    if chooser then chooser:run() end
  end,
})

win:add(ui.button{
  x = 90, y = 28, w = 70, h = 24, text = "Prev",
  on_click = function () show(current - 1) end,
})

win:add(ui.button{
  x = 168, y = 28, w = 70, h = 24, text = "Next",
  on_click = function () show(current + 1) end,
})

win:add(view)
win:add(status)

if path then
  open(path)
else
  lines = {
    "PDF",
    "",
    "Press Open and choose a document, or start this with one:",
    "",
    "    wm pdfview:/home/odyssey.pdf",
    "",
    "Comma and full stop turn the page; the arrows and PageUp and",
    "PageDown move within it.",
    "",
    "What you get is the page's text, wrapped to this window in the",
    "system font - not the page as it was typeset. Drawing it properly",
    "needs a glyph rasterised by index out of the font inside the",
    "document, which the graphics kit cannot do yet.",
  }
  relayout()
end

win:run()
