-- kosmos: application
-- kosmos: icon File_Text
-- kosmos: name Kosmos Write
-- kosmos: section applications
-- kosmos: opens write
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Kosmos Write: a document's pages, as they print (`docs/write.html`,
-- `docs/write.md` W4).
--
--   wm writer                     a new document
--   wm writer:/Home/Letter.write  that one
--
-- `writer` and not `write`, which is `print`'s sibling and a name every
-- program already holds: a program by that name could not be started by it.
--
--   + and -                      zoom
--   arrows, Page Up, Page Down   scroll
--   Control-E                    Export PDF, beside the document
--
-- **This is the first part of the window**: the pages on a dark desk, each
-- one exactly the page the PDF prints - set by `pageset` with the fonts' own
-- advances and drawn by `pagedraw` with the PDF reader's rasteriser - and
-- zoom, scrolling and Export. Typing, the Format and Document panels and
-- the page thumbnails follow, each a step of its own (`roadmap.md` W4).
--
-- **A direct window** (`gfx.md` 19.4), as the PDF viewer is: a page is drawn
-- once into a surface of its own at the zoom it is shown at, and a frame is
-- blits of the visible part of each. Scrolling draws nothing again; zoom
-- draws the pages it shows, once.

local ui        = use("/Kosmos/Libraries/ui.lua")
local writedoc  = use("/Kosmos/Libraries/writedoc.lua")
local pageset   = use("/Kosmos/Libraries/pageset.lua")
local faces     = use("/Kosmos/Libraries/faces.lua")
local pagedraw  = use("/Kosmos/Libraries/pagedraw.lua")
local pdf       = use("/Kosmos/Libraries/pdf.lua")

local W, H = 920, 700
local BAR = 44                  -- the toolbar, across the top
local GAP = 24                  -- round each page, on the desk

-- The Night look's surfaces (`docs/write.html`): the desk darker than the
-- bar, the bar's buttons raised from it.
local DESK   = 0xff121821
local CHROME = 0xff1e2636
local RAISED = 0xff2a3446
local EDGE   = 0xff343f53
local INK    = 0xffe6eaf2
local MUTED  = 0xffa3adbf
local PAPER  = 0xffffffff
local SHADOW = 0xff0a0e14

-- The zooms a person steps through, as a percentage.
local ZOOMS = { 50, 75, 100, 125, 150, 200, 300 }

local path = args and args:match("^%s*(%S+)")
local name = path and path:match("([^/]+)$") or "Untitled"

local win, err = ui.window{
  title = name .. " - Kosmos Write", w = W, h = H, x = 60, y = 40,
  direct = true,
}

if not win then
  print("writer: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The document, set.
--------------------------------------------------------------------------

local measure = faces.measure(faces.catalogue(gfx.typefaces()), gfx.typeface)
local drawer = pagedraw.new(measure)
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

local set = pageset.set(doc, measure)
local zoom = 4                  -- 125%, an index into ZOOMS
local top = 0                   -- how far down the desk the view is

local function scale() return ZOOMS[zoom] / 100 end

-- A page's size on the desk, in pixels.
local function page_px(page)
  return math.floor(page.width_pt * scale() + 0.5),
         math.floor(page.height_pt * scale() + 0.5)
end

-- Where page `i` stands on the desk, the desk's top at nought.
local function page_at(i)
  local y = GAP

  for k = 1, i - 1 do
    local _, h = page_px(set.pages[k])
    y = y + h + GAP
  end

  local w = page_px(set.pages[i])

  return math.max(GAP, (W - w) // 2), y
end

local function desk_height()
  local _, y = page_at(#set.pages)
  local _, h = page_px(set.pages[#set.pages])
  return y + h + GAP
end

--
-- **Each page drawn once at the zoom it is shown at**, and kept while it is
-- near the view; a page far from it gives its pixels back, so a long
-- document costs the pages on screen and no more.
--
local drawn = {}                -- page index -> { surface, zoom }

local function page_surface(i)
  local d = drawn[i]

  if d and d.zoom == zoom then return d.surface end

  if d then d.surface:free() end

  local w, h = page_px(set.pages[i])
  local ok, surface = pcall(gfx.surface, { w = w, h = h })

  if not ok or not surface then
    drawn[i] = nil
    return nil
  end

  drawer:page(set, set.pages[i], surface, scale(), 0, 0, PAPER)
  drawn[i] = { surface = surface, zoom = zoom }

  return surface
end

local function forget_far(first, last)
  for i, d in pairs(drawn) do
    if i < first - 1 or i > last + 1 then
      d.surface:free()
      drawn[i] = nil
    end
  end
end

--------------------------------------------------------------------------
-- The bar.
--------------------------------------------------------------------------

-- Buttons this file draws and hit-tests itself: a direct window owns every
-- pixel, so there is no widget to hand them to.
local BUTTONS = {
  { key = "out",    text = "-",          w = 32 },
  { key = "zoom",   text = "",           w = 64, label = true },
  { key = "in",     text = "+",          w = 32 },
  { key = "export", text = "Export PDF", w = 104, right = true },
}

local function layout_buttons()
  local x = 12

  for _, b in ipairs(BUTTONS) do
    if not b.right then
      b.x = x
      x = x + b.w + 6
    end
  end

  local r = W - 12

  for i = #BUTTONS, 1, -1 do
    local b = BUTTONS[i]

    if b.right then
      r = r - b.w
      b.x = r
      r = r - 6
    end
  end
end

layout_buttons()

local function draw_bar(s)
  s:fill(0, 0, W, BAR, CHROME)
  s:fill(0, BAR - 1, W, 1, EDGE)

  for _, b in ipairs(BUTTONS) do
    local text = b.key == "zoom" and (ZOOMS[zoom] .. "%") or b.text
    local y, h = 8, BAR - 16

    if not b.label then
      s:fill(b.x, y, b.w, h, RAISED)
    end

    local tw = gfx.measure(text)
    s:text(b.x + (b.w - tw) // 2, y + (h - 16) // 2, text, b.label and MUTED or INK)
  end

  -- What the document is, and what was last said about it.
  local note = said or ("%s  -  %d page%s"):format(name, #set.pages,
                                                    #set.pages == 1 and "" or "s")
  s:text(BUTTONS[3].x + BUTTONS[3].w + 18, 14, note, MUTED)
end

--------------------------------------------------------------------------
-- A frame.
--------------------------------------------------------------------------

local function frame()
  local s = win:surface()

  if not s then return end

  local view_h = H - BAR

  s:fill(0, BAR, W, view_h, DESK)

  local first, last = nil, nil

  for i = 1, #set.pages do
    local x, y = page_at(i)
    local w, h = page_px(set.pages[i])
    local sy = BAR + y - top

    if sy < H and sy + h > BAR then
      first = first or i
      last = i

      local surface = page_surface(i)

      -- A shadow down and to the right, the paper over it.
      s:fill(x + 3, sy + 3, w, h, SHADOW)

      if surface then
        local from = math.max(0, BAR - sy)
        local band = math.min(h - from, H - (sy + from))

        if band > 0 then
          s:blit(surface, 0, from, w, band, x, sy + from)
        end
      end
    end
  end

  if first then forget_far(first, last) end

  draw_bar(s)
  win:commit()
end

-- Said once a page is on the screen, for a harness to find it by and a
-- person reading the log to know what opened.
local function report()
  local x, y = page_at(1)
  local w, h = page_px(set.pages[1])

  print(("writer: %s, %d page%s at %d%%, page 1 at %d,%d %dx%d"):format(
    name, #set.pages, #set.pages == 1 and "" or "s", ZOOMS[zoom], x,
    BAR + y - top, w, h))
end

--------------------------------------------------------------------------
-- What a person does.
--------------------------------------------------------------------------

local function scroll_to(y)
  top = math.max(0, math.min(y, desk_height() - (H - BAR)))
  frame()
end

local function zoom_to(z)
  z = math.max(1, math.min(#ZOOMS, z))

  if z == zoom then return end

  -- The same place on the page stays under the middle of the view.
  local middle = (top + (H - BAR) / 2) / scale()

  zoom = z
  top = math.floor(middle * scale() - (H - BAR) / 2)
  scroll_to(top)
  report()
end

local function export()
  local to = path and path:gsub("%.write$", "") .. ".pdf" or "/Home/Untitled.pdf"
  local ok, notes = pdf.write(to, set, measure,
                                   { title = (name:gsub("%.write$", "")) })

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

local sink = ui.view{ x = 0, y = 0, w = W, h = H }
sink.focusable = true

function sink:wheel(n)
  scroll_to(top - n * 3 * 40)
  return true
end

function sink:key(c)
  local view_h = H - BAR

  if     c == -2 then scroll_to(top + 40)
  elseif c == -1 then scroll_to(top - 40)
  elseif c == -3 then scroll_to(top + view_h - 40)
  elseif c == -4 then scroll_to(top - view_h + 40)
  elseif c == 43 or c == 61 then zoom_to(zoom + 1)       -- + and =
  elseif c == 45 then zoom_to(zoom - 1)                  -- -
  elseif c == 5 then export()                            -- Control-E
  else return false end

  return true
end

function sink:mouse(action, x, y)
  if action ~= "press" or y >= BAR then return true end

  for _, b in ipairs(BUTTONS) do
    if not b.label and x >= b.x and x < b.x + b.w then
      if b.key == "out" then zoom_to(zoom - 1)
      elseif b.key == "in" then zoom_to(zoom + 1)
      elseif b.key == "export" then export() end

      return true
    end
  end

  return true
end

win:add(sink)
frame()
report()
win:run()
