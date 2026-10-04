-- kosmos: application
-- kosmos: icon File_Text
-- kosmos: name Kosmos Write
-- kosmos: section applications
-- kosmos: opens write
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Kosmos Write: a document's pages, as they print, typed into
-- (`docs/write.html`, `docs/write.md` W4).
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
--   Control-S                              save
--   Control-E                              Export PDF, beside the document
--   the bar's - and +                      zoom
--
-- **The window orchestrates; the kits do the work** (`CLAUDE.md`'s premise):
-- what an edit does to the text is `richtext`'s, where a line breaks and a
-- caret stands is `pageset`'s, the pixels are `pagedraw`'s, the file is
-- `writedoc`'s and the PDF is `pdf`'s. What is here is what each key and
-- each click means - which is the part that is Kosmos Write's own.
--
-- **Each edit is a new body**, the paragraphs it did not touch the same
-- tables (`richtext`): so an undo is the body before, kept, and setting the
-- pages again sets only the paragraph that changed (`pageset.cache`).
--
-- **A direct window** (`gfx.md` 19.4), as the PDF viewer is: a page is drawn
-- once into a surface of its own and blitted after that; an edit or a
-- selection draws again the pages on the screen, and the caret is drawn on
-- the window over them, so moving it draws no page at all.
--
-- Not yet: the Format and Document panels (W4c, W4d), a word at a time
-- with Control and the arrows, and a document's own name in the title bar.

local ui        = use("/Kosmos/Libraries/ui.lua")
local keys      = use("/Kosmos/Libraries/keys.lua")
local wmproto   = use("/Kosmos/Libraries/wmproto.lua")
local richtext  = use("/Kosmos/Libraries/richtext.lua")
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
local CARET  = 0xff2a55c9       -- the drawing's accent, on paper
local CHOSEN = 0xffc9d8f6       -- a selection, under the text

-- The zooms a person steps through, as a percentage.
local ZOOMS = { 50, 75, 100, 125, 150, 200, 300 }

-- How many edits are kept to undo.
local UNDO_MOST = 200

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
local cache = pageset.cache()
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

local by_name = {}
for _, s in ipairs(doc.styles) do by_name[s.name] = s end

local set = pageset.set(doc, measure, cache)
local zoom = 4                  -- 125%, an index into ZOOMS
local top = 0                   -- how far down the desk the view is

-- The caret, where a selection was started from (nil: none), and the
-- column Up and Down keep.
local caret = { para = 1, at = 1 }
local anchor, column = nil, nil
local dirty = false

local undo, redo = {}, {}
local last_kind = nil           -- consecutive typing is one undo

-- What the pages were drawn from: a page drawn at an older one is drawn
-- again.
local version = 1

local function scale() return ZOOMS[zoom] / 100 end

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

--------------------------------------------------------------------------
-- Pages, drawn.
--------------------------------------------------------------------------

local drawn = {}                -- page index -> { surface, zoom, version }

local function selected()
  return anchor and (anchor.para ~= caret.para or anchor.at ~= caret.at)
end

-- The selection's rectangles on page `i`, as `pagedraw` marks.
local function marks_on(i)
  if not selected() then return nil end

  local out = {}

  for _, r in ipairs(pageset.selection(set, measure, anchor, caret)) do
    if r.page == i then
      r.colour = CHOSEN
      out[#out + 1] = r
    end
  end

  return out
end

local function page_surface(i)
  local d = drawn[i]

  if d and d.zoom == zoom and d.version == version then return d.surface end

  local w, h = page_px(set.pages[i])

  if d and (d.w ~= w or d.h ~= h) then
    d.surface:free()
    d = nil
  end

  if not d then
    local ok, surface = pcall(gfx.surface, { w = w, h = h })

    if not ok or not surface then
      drawn[i] = nil
      return nil
    end

    d = { surface = surface, w = w, h = h }
    drawn[i] = d
  end

  drawer:page(set, set.pages[i], d.surface, scale(), 0, 0, PAPER, marks_on(i))
  d.zoom, d.version = zoom, version

  return d.surface
end

local function forget_far(first, last)
  for i, d in pairs(drawn) do
    if i < first - 1 or i > last + 1 or not set.pages[i] then
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
  { key = "save",   text = "Save",       w = 64, right = true },
}

do
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

local function draw_bar(s)
  s:fill(0, 0, W, BAR, CHROME)
  s:fill(0, BAR - 1, W, 1, EDGE)

  for _, b in ipairs(BUTTONS) do
    local text = b.key == "zoom" and (ZOOMS[zoom] .. "%") or b.text
    local y, h = 8, BAR - 16

    if not b.label then
      s:fill(b.x, y, b.w, h, RAISED)
    end

    s:text(b.x + (b.w - gfx.measure(text)) // 2, y + (h - 16) // 2, text,
           b.label and MUTED or INK)
  end

  -- What the document is, and what was last said about it.
  local note = said or ("%s%s  -  %d page%s"):format(name, dirty and " (edited)" or "",
                                                      #set.pages,
                                                      #set.pages == 1 and "" or "s")
  s:text(BUTTONS[3].x + BUTTONS[3].w + 18, 14, note, MUTED)
end

--------------------------------------------------------------------------
-- A frame.
--------------------------------------------------------------------------

-- Where the caret is in the window, or nil when it is off the view.
local function caret_px()
  local here = pageset.locate(set, measure, caret)

  if not here then return nil end

  local px, py = page_at(here.page)
  local x = px + math.floor(here.x_pt * scale() + 0.5)
  local y = BAR + py - top + math.floor((here.baseline_pt - here.ascent_pt) * scale())
  local h = math.max(8, math.floor(here.height_pt * scale() + 0.5))

  return x, y, h, here
end

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

  -- The caret, on the window over the page: moving it draws no page.
  local cx, cy, ch = caret_px()

  if cx and cy + ch > BAR and cy < H then
    local y0 = math.max(BAR, cy)
    s:fill(cx, y0, 2, math.min(cy + ch, H) - y0, CARET)
  end

  draw_bar(s)
  win:commit()
end

-- Said when the view changes, for a harness to find the page by and a
-- person reading the log to know what is shown.
local function report()
  local x, y = page_at(1)
  local w, h = page_px(set.pages[1])

  print(("writer: %s, %d page%s at %d%%, page 1 at %d,%d %dx%d"):format(
    name, #set.pages, #set.pages == 1 and "" or "s", ZOOMS[zoom], x,
    BAR + y - top, w, h))
end

--------------------------------------------------------------------------
-- The view.
--------------------------------------------------------------------------

local function scroll_to(y)
  top = math.max(0, math.min(y, desk_height() - (H - BAR)))
end

-- The view moved, if it must, so the caret is in it.
local function follow()
  local _, cy, ch = caret_px()

  if not cy then return end

  if cy < BAR + 8 then
    scroll_to(top - (BAR + 8 - cy))
  elseif cy + ch > H - 8 then
    scroll_to(top + (cy + ch - (H - 8)))
  end
end

local function zoom_to(z)
  z = math.max(1, math.min(#ZOOMS, z))

  if z == zoom then return end

  -- The same place on the page stays under the middle of the view.
  local middle = (top + (H - BAR) / 2) / scale()

  zoom = z
  scroll_to(math.floor(middle * scale() - (H - BAR) / 2))
  frame()
  report()
end

--------------------------------------------------------------------------
-- Editing.
--------------------------------------------------------------------------

-- The pages set again after an edit or a selection, and drawn again.
local function changed()
  version = version + 1
  frame()
end

--
-- **An edit**: the body before it kept for undo - one entry for a run of
-- typing, one for anything else - the new one set, the caret moved.
--
local function edited(body, place, kind)
  if kind ~= "type" or last_kind ~= "type" then
    undo[#undo + 1] = { body = doc.body, caret = caret }
    if #undo > UNDO_MOST then table.remove(undo, 1) end
  end

  redo = {}
  last_kind = kind
  doc.body = body
  caret, anchor, column = place, nil, nil
  dirty = true
  said = nil
  set = pageset.set(doc, measure, cache)
  follow()
  changed()
end

-- The selection taken out first, when there is one: what typing over a
-- selection and Backspace on one both begin with.
local function without_selection()
  if not selected() then return doc.body, caret end

  return richtext.delete(doc.body, anchor, caret)
end

local function type_text(text)
  local body, place = without_selection()
  body, place = richtext.type(body, place, text)
  edited(body, place, "type")
end

local function back_or_forward(forward)
  if selected() then
    local body, place = richtext.delete(doc.body, anchor, caret)
    edited(body, place, "delete")
    return
  end

  local other = richtext.step(doc.body, caret, forward)

  if other.para == caret.para and other.at == caret.at then return end

  local body, place = richtext.delete(doc.body, caret, other)
  edited(body, place, "delete")
end

local function swap(from, to)
  local entry = table.remove(from)

  if not entry then return end

  to[#to + 1] = { body = doc.body, caret = caret }
  doc.body, caret, anchor, column = entry.body, entry.caret, nil, nil
  last_kind = nil
  dirty = true
  set = pageset.set(doc, measure, cache)
  follow()
  changed()
end

-- The caret moved, a selection kept or begun with Shift: the pages drawn
-- again only when a selection was or is on them.
local function move(place, extend, keep_column)
  local had = selected()

  if extend then anchor = anchor or caret else anchor = nil end

  caret = place
  last_kind = nil

  if not keep_column then column = nil end

  if had or selected() then version = version + 1 end

  follow()
  frame()
end

local function save()
  if not path then
    -- A new document's first save: Untitled, then Untitled 2, and on.
    local n = 1

    repeat
      path = ("/Home/Untitled%s.write"):format(n == 1 and "" or (" " .. n))
      n = n + 1
    until not fs.getattr(path)

    name = path:match("([^/]+)$")
  end

  local ok, why = writedoc.save(path, doc)

  if ok then
    dirty = false
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

--------------------------------------------------------------------------
-- What a person does.
--------------------------------------------------------------------------

local sink = ui.view{ x = 0, y = 0, w = W, h = H }
sink.focusable = true

-- The bytes of a character not yet whole: a key that types beyond ASCII
-- arrives as its UTF-8, a byte at a time.
local pending = ""

function sink:wheel(n)
  scroll_to(top - n * 3 * 40)
  frame()
  return true
end

function sink:key(c)
  local k, mods = keys.parts(c)
  local shift = (mods & keys.SHIFT) ~= 0
  local ctrl = (mods & keys.CTRL) ~= 0

  if k == keys.LEFT or k == keys.RIGHT then
    move(richtext.step(doc.body, caret, k == keys.RIGHT), shift)
  elseif k == keys.UP or k == keys.DOWN then
    local place
    place, column = pageset.vertical(set, measure, caret, k == keys.UP and -1 or 1,
                                     column)
    move(place, shift, true)
  elseif k == keys.HOME or k == keys.END then
    if ctrl then
      local last = #doc.body
      move(k == keys.HOME and { para = 1, at = 1 }
           or { para = last, at = #richtext.plain(doc.body[last]) + 1 }, shift)
    else
      local home, finish = pageset.line_ends(set, measure, caret)
      move(k == keys.HOME and home or finish, shift)
    end
  elseif k == keys.PAGEUP or k == keys.PAGEDOWN then
    scroll_to(top + (k == keys.PAGEUP and -1 or 1) * (H - BAR - 40))
    frame()
  elseif k == keys.DELETE and mods == 0 then
    back_or_forward(true)
  elseif mods ~= 0 then
    return false
  elseif c == 8 or c == 127 then
    back_or_forward(false)
  elseif c == 13 or c == 10 then
    local body, place = without_selection()
    body, place = richtext.split(body, place, by_name)
    edited(body, place, "return")
  elseif c == 9 then
    type_text("\t")
  elseif c == 1 then                                     -- Control-A
    anchor = { para = 1, at = 1 }
    local last = #doc.body
    caret = { para = last, at = #richtext.plain(doc.body[last]) + 1 }
    changed()
  elseif c == 19 then save()                             -- Control-S
  elseif c == 5 then export()                            -- Control-E
  elseif c == 26 then swap(undo, redo)                   -- Control-Z
  elseif c == 25 then swap(redo, undo)                   -- Control-Y
  elseif c >= 32 and c < 127 then
    pending = ""
    type_text(string.char(c))
  elseif c >= 128 and c < 256 then
    pending = pending .. string.char(c)

    if utf8.len(pending) then
      local text = pending
      pending = ""
      type_text(text)
    elseif #pending >= 4 then
      pending = ""
    end
  else
    return false
  end

  return true
end

--
-- **The clipboard**, the system's, from the window manager by way of
-- `window:dispatch_edit` as every editor here has it: the selection's text
-- out, text in where the caret is - over a selection, as typing is.
--
function sink:edit(kind)
  if kind == "selectall" then
    anchor = { para = 1, at = 1 }
    local last = #doc.body
    caret = { para = last, at = #richtext.plain(doc.body[last]) + 1 }
    changed()
    return true
  end

  if kind == "copy" or kind == "cut" then
    if not selected() then return false end
    if not wmproto.copy(richtext.text(doc.body, anchor, caret)) then return false end

    if kind == "cut" then
      local body, place = richtext.delete(doc.body, anchor, caret)
      edited(body, place, "cut")
    end

    return true
  end

  if kind == "paste" then
    local text = wmproto.paste()

    if not text or text == "" then return false end

    local body, place = without_selection()
    body, place = richtext.type(body, place, text)
    edited(body, place, "paste")
    return true
  end

  return false
end

-- The page and the point on it, in points, under a point of the window.
local function page_point(x, y)
  for i = 1, #set.pages do
    local px, py = page_at(i)
    local w, h = page_px(set.pages[i])
    local sy = BAR + py - top

    if y >= sy - GAP // 2 and y < sy + h + GAP // 2 then
      return i, (x - px) / scale(), (y - sy) / scale()
    end
  end
end

local pressed = false

function sink:mouse(action, x, y)
  if action == "release" then
    pressed = false
    return true
  end

  if action == "press" and y < BAR then
    for _, b in ipairs(BUTTONS) do
      if not b.label and x >= b.x and x < b.x + b.w then
        if b.key == "out" then zoom_to(zoom - 1)
        elseif b.key == "in" then zoom_to(zoom + 1)
        elseif b.key == "export" then export()
        elseif b.key == "save" then save() end

        return true
      end
    end

    return true
  end

  if action == "press" or (action == "move" and pressed) then
    local page, px, py = page_point(x, y)
    local place = page and pageset.hit(set, measure, page, px, py)

    if place then
      if action == "press" then
        pressed = true
        if selected() then version = version + 1 end
        anchor, caret, column = place, place, nil
        last_kind = nil
        frame()
      else
        caret = place
        version = version + 1
        frame()
      end
    end
  end

  return true
end

win:add(sink)
frame()
report()
win:run()
