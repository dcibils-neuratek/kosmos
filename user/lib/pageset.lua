-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Paragraphs set into lines, and lines onto pages (`docs/write.md`, W2).
--
--   local pageset = use("/Kosmos/Libraries/pageset.lua")
--   local set = pageset.set(doc, measure)
--   for _, page in ipairs(set.pages) do ... page.lines ... end
--
-- **One setting, for the screen and the PDF alike.** What comes out is each
-- page's lines and, on each line, pieces of text with their look and where
-- they stand - in points, from the page's top left corner, a baseline for
-- each line. The window draws that and the PDF writer writes it, so the two
-- cannot disagree about where a line broke: there is one place a line
-- breaks, and it is here.
--
-- **The setting is handed its measure**, which is what lets the whole of it
-- run on the Mac (`tools/test_pageset.lua`):
--
--   measure.width(look, text)   the text's advance in points
--   measure.line(look)          ascent, descent and gap in points
--
-- Inside the machine that is `faces.lua` over the faces' own advance widths;
-- on the Mac it is a measure a test can do sums with.
--
-- What it does: lines broken at spaces, a word longer than a line broken
-- where it must be, a line break inside a paragraph, tabs to every 12.7 mm,
-- the four alignments - justified by widening the spaces, as a PDF's `Tw`
-- does, so the same lines come out of both - the three indents, line
-- spacing, the space before and after a paragraph and none before at the
-- top of a page, **no paragraph leaving one line alone at the foot of a page
-- or the head of the next**, a paragraph kept with the next when its style
-- says so, and page numbers. What it does not do yet, and says so in
-- `docs/write.md`: hyphenation, kerning, ligatures, lists and drop caps.

local pageset = {}

local richtext = use("/Kosmos/Libraries/richtext.lua")
local writedoc = use("/Kosmos/Libraries/writedoc.lua")

-- Tab stops every half inch, from the text's left edge.
pageset.TAB_PT = 36

-- Lines a paragraph may leave alone on a page: one is a widow or an orphan.
local LEAST = 2

--------------------------------------------------------------------------
-- Looks: each different look once, by number, so a piece names its look
-- rather than carrying seven fields.
--------------------------------------------------------------------------

local function look_key(l)
  return table.concat({ l.face, l.weight, tostring(l.italic), l.size_pt,
                        l.colour, tostring(l.underline), tostring(l.strike) },
                      "\0")
end

local function looks_table()
  local list, by_key = {}, {}

  return list, function(look)
    local key = look_key(look)
    local n = by_key[key]

    if not n then
      list[#list + 1] = look
      n = #list
      by_key[key] = n
    end

    return n
  end
end

--------------------------------------------------------------------------
-- A paragraph into tokens: words, spaces, tabs and line breaks, each with
-- its look and where its bytes start in the paragraph's text.
--------------------------------------------------------------------------

local function tokens_of(p, style, look_of)
  local out, at = {}, 1

  for _, run in ipairs(p.runs) do
    local look = look_of(richtext.look(run, style))
    local text = run.text
    local i = 1

    while i <= #text do
      local c = text:sub(i, i)
      local kind, j

      if c == " " then
        kind, j = "space", text:find("[^ ]", i) or #text + 1
      elseif c == "\t" then
        kind, j = "tab", i + 1
      elseif c == "\n" then
        kind, j = "break", i + 1
      else
        kind, j = "word", text:find("[ \t\n]", i) or #text + 1
      end

      out[#out + 1] = { kind = kind, text = text:sub(i, j - 1), look = look,
                        at = at + i - 1 }
      i = j
    end

    at = at + #text
  end

  return out
end

--------------------------------------------------------------------------
-- Breaking a paragraph into lines.
--------------------------------------------------------------------------

--
-- **The width of `text` in `look`**, asked once: a document says "the" a few
-- thousand times, and the measure is a call into C inside the machine.
--
local function widths(measure, looks)
  local cache = {}

  return function(look, text)
    local key = look .. "\0" .. text
    local w = cache[key]

    if not w then
      w = measure.width(looks[look], text)
      cache[key] = w
    end

    return w
  end
end

-- UTF-8 characters of a string, for breaking a word too long for a line.
local function chars(s)
  local out = {}
  for ch in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do out[#out + 1] = ch end
  return out
end

--
-- The lines of one paragraph, each `{ pieces, width_pt, spaces, forced,
-- first }`: its pieces left to right with `x_pt` from the line's start, how
-- wide it is, how many spaces stand between its words (for justifying), and
-- whether a line break ended it. `avail_first` and `avail` are the room for
-- the first line and the rest.
--
local function break_lines(tokens, avail_first, avail, width)
  local lines, line = {}, nil
  local pending = {}          -- spaces since the last word, not yet placed

  local function new_line(from)
    line = { pieces = {}, width_pt = 0, spaces = 0, forced = false,
             first = #lines == 0, from = from }
    lines[#lines + 1] = line
    pending = {}
  end

  local function room()
    return line.first and avail_first or avail
  end

  -- A piece of text placed at the end of the line, joined to the piece
  -- before it when that one has the same look and ends where it starts.
  local function place(tok, text, at, w, spaces)
    local last = line.pieces[#line.pieces]

    -- Where the line starts in its paragraph's bytes, for a caret.
    line.from = line.from or at

    if last and last.look == tok.look and last.at + #last.text == at
       and last.x_pt + last.width_pt == line.width_pt then
      last.text = last.text .. text
      last.width_pt = last.width_pt + w
    else
      line.pieces[#line.pieces + 1] = { text = text, look = tok.look, at = at,
                                        x_pt = line.width_pt, width_pt = w }
    end

    line.width_pt = line.width_pt + w
    line.spaces = line.spaces + (spaces or 0)
  end

  -- The spaces waiting before a word, placed now that a word follows them.
  local function flush()
    for _, s in ipairs(pending) do
      place(s, s.text, s.at, width(s.look, s.text), #s.text)
    end
    pending = {}
  end

  local function pending_width()
    local w = 0
    for _, s in ipairs(pending) do w = w + width(s.look, s.text) end
    return w
  end

  new_line()

  -- Words that are not separated by a space go together - a word in two
  -- looks is still one word - so a box is the run of word tokens between
  -- two places a line may break.
  local i = 1

  while i <= #tokens do
    local tok = tokens[i]

    if tok.kind == "space" then
      pending[#pending + 1] = tok
      i = i + 1
    elseif tok.kind == "break" then
      line.forced = true
      new_line(tok.at + 1)
      i = i + 1
    elseif tok.kind == "tab" then
      flush()

      local stop = (math.floor(line.width_pt / pageset.TAB_PT) + 1)
                   * pageset.TAB_PT

      if stop > room() and #line.pieces > 0 then
        new_line()
        stop = pageset.TAB_PT
      end

      -- An empty piece at the stop, which the next word joins: the tab
      -- itself is drawn as nothing.
      line.from = line.from or tok.at
      stop = math.min(stop, room())
      line.pieces[#line.pieces + 1] = { text = "", look = tok.look,
        at = tok.at + 1, x_pt = stop, width_pt = 0 }
      line.width_pt = stop
      i = i + 1
    else
      local j, box_w = i, 0

      while j <= #tokens and tokens[j].kind == "word" do
        box_w = box_w + width(tokens[j].look, tokens[j].text)
        j = j + 1
      end

      local gap = pending_width()

      if #line.pieces > 0 and line.width_pt + gap + box_w > room() then
        new_line()
      end

      if line.width_pt + gap + box_w <= room() or #line.pieces > 0 then
        flush()

        for k = i, j - 1 do
          local t = tokens[k]
          place(t, t.text, t.at, width(t.look, t.text))
        end
      else
        -- **Longer than a whole line**: broken between characters, as
        -- many as fit and never none.
        pending = {}

        for k = i, j - 1 do
          local t = tokens[k]
          local at = t.at

          for _, ch in ipairs(chars(t.text)) do
            local w = width(t.look, ch)

            if line.width_pt + w > room() and #line.pieces > 0 then
              new_line()
            end

            place(t, ch, at, w)
            at = at + #ch
          end
        end
      end

      i = j
    end
  end

  -- A paragraph with nothing in it is a line from its first byte.
  lines[1].from = lines[1].from or 1

  for _, l in ipairs(lines) do
    l.from = l.from or 1
  end

  return lines
end

--------------------------------------------------------------------------
-- Lines given their height and their place across the page.
--------------------------------------------------------------------------

--
-- **How tall a line is**: the tallest ascent and the deepest descent of the
-- looks on it, and the largest gap, times its paragraph's spacing. A line
-- with nothing on it - an empty paragraph - is as tall as its style's.
--
local function line_height(line, looks, measure, empty_look, spacing)
  local asc, desc, gap = 0, 0, 0
  local seen = false

  for _, pc in ipairs(line.pieces) do
    local a, d, g = measure.line(looks[pc.look])
    asc, desc, gap = math.max(asc, a), math.max(desc, d), math.max(gap, g)
    seen = true
  end

  if not seen then
    asc, desc, gap = measure.line(looks[empty_look])
  end

  line.ascent_pt = asc
  line.height_pt = (asc + desc + gap) * spacing
end

--
-- Across the page: the line's start from the indents, then moved for its
-- alignment. A justified line is widened at its spaces, all but the last
-- line and a line a break ended, and says by how much each space grew
-- (`extra_space_pt`, a PDF's `Tw`).
--
local function align(line, layout, left_pt, avail_first, avail, last)
  local room = line.first and avail_first or avail
  local start = left_pt + writedoc.pt(layout.indent_left_mm)
                + (line.first and writedoc.pt(layout.indent_first_mm) or 0)
  local slack = math.max(0, room - line.width_pt)
  local shift = 0

  line.extra_space_pt = 0

  if layout.align == "right" then
    shift = slack
  elseif layout.align == "center" then
    shift = slack / 2
  elseif layout.align == "justify" and not last and not line.forced
         and line.spaces > 0 then
    line.extra_space_pt = slack / line.spaces

    -- Each piece moves right by the spaces before it, and widens by its own.
    local before = 0

    for _, pc in ipairs(line.pieces) do
      local own = select(2, pc.text:gsub(" ", ""))
      pc.x_pt = pc.x_pt + before * line.extra_space_pt
      pc.width_pt = pc.width_pt + own * line.extra_space_pt
      before = before + own
    end

    line.width_pt = room
  end

  line.x_pt = start + shift

  for _, pc in ipairs(line.pieces) do
    pc.x_pt = line.x_pt + pc.x_pt
  end
end

--------------------------------------------------------------------------
-- Onto pages.
--------------------------------------------------------------------------

--
-- **`doc` set onto pages**, with `measure` (above). What comes back:
--
--   looks     every look a piece wears, by number: a run's look, whole
--   pages     each `{ number, width_pt, height_pt, lines, footer }`
--     lines     each `{ para, baseline_pt, ascent_pt, x_pt, width_pt,
--                       height_pt, extra_space_pt, pieces }` - the line
--                       stands from `baseline_pt - ascent_pt` down
--                       `height_pt`
--       pieces    each `{ text, look, x_pt, width_pt, at }` - `at` is where
--                 its bytes start in its paragraph's text
--     footer    the page number as a piece and its baseline, or nil
--
--
-- **What a set keeps for the next**, so a keystroke sets one paragraph
-- rather than the document (W4b): each paragraph's lines, by the paragraph
-- itself - an edit makes a new paragraph table, and those it did not touch
-- are the same tables (`richtext`'s editing) - with the looks and the widths
-- already asked. Made again whenever the column, the styles or the measure
-- are not the ones it was made for.
--
function pageset.cache()
  return {}
end

function pageset.set(doc, measure, cache)
  local page_w, page_h = writedoc.page_mm(doc)
  local m = doc.margins_mm

  page_w, page_h = writedoc.pt(page_w), writedoc.pt(page_h)

  local left = writedoc.pt(m.left)
  local column = page_w - left - writedoc.pt(m.right)
  local top, bottom = writedoc.pt(m.top), page_h - writedoc.pt(m.bottom)
  local geometry = ("%s:%s"):format(left, column)

  cache = cache or pageset.cache()

  if cache.geometry ~= geometry or cache.measure ~= measure
     or cache.styles ~= doc.styles then
    cache.geometry, cache.measure, cache.styles = geometry, measure, doc.styles
    cache.looks, cache.look_of = looks_table()
    cache.width = widths(measure, cache.looks)
    cache.paras = setmetatable({}, { __mode = "k" })
  end

  local looks, look_of, width = cache.looks, cache.look_of, cache.width

  local by_name = {}
  for _, s in ipairs(doc.styles) do by_name[s.name] = s end

  -- Every paragraph broken into lines first: the column is the same on
  -- every page, so where a line breaks does not depend on where it lands,
  -- and keeping a paragraph with the next needs to know the next.
  local paras = {}

  for n, p in ipairs(doc.body) do
    local kept = cache.paras[p]

    if kept then
      for _, line in ipairs(kept.lines) do line.para = n end
      paras[n] = kept
    else
      local style = by_name[p.style] or doc.styles[1]
      local layout = richtext.layout(p, style)
      local indent = writedoc.pt(layout.indent_left_mm)
                     + writedoc.pt(layout.indent_right_mm)
      local avail = math.max(1, column - indent)
      local avail_first = math.max(1, avail - writedoc.pt(layout.indent_first_mm))
      local lines = break_lines(tokens_of(p, style, look_of), avail_first, avail,
                                width)
      local empty = look_of(richtext.look({}, style))

      for k, line in ipairs(lines) do
        line.para = n
        line_height(line, looks, measure, empty, layout.spacing_lines)
        align(line, layout, left, avail_first, avail, k == #lines)
      end

      paras[n] = { lines = lines, layout = layout }
      cache.paras[p] = paras[n]
    end
  end

  local pages = {}
  local page, y

  local function new_page()
    page = { number = #pages + 1, width_pt = page_w, height_pt = page_h,
             lines = {} }
    pages[#pages + 1] = page
    y = top
  end

  local function put(line)
    line.baseline_pt = y + line.ascent_pt
    page.lines[#page.lines + 1] = line
    y = y + line.height_pt
  end

  -- How tall lines `from` to `to` of a paragraph are.
  local function tall(lines, from, to)
    local h = 0
    for i = from, to do h = h + lines[i].height_pt end
    return h
  end

  --
  -- **What has to follow a paragraph on its page when it is kept with the
  -- next**: the next one's space before it and its first lines - or, when
  -- that one is kept with its own next, all of it and what follows that, so
  -- a title, its subtitle and the first lines of the text move together.
  --
  local function following(n)
    local need = 0

    while paras[n + 1] do
      local after, nxt = paras[n].layout, paras[n + 1]

      need = need + after.after_pt + nxt.layout.before_pt

      if nxt.layout.keep_with_next and paras[n + 2] then
        need = need + tall(nxt.lines, 1, #nxt.lines)
        n = n + 1
      else
        return need + tall(nxt.lines, 1, math.min(LEAST, #nxt.lines))
      end
    end

    return need
  end

  new_page()

  for n, para in ipairs(paras) do
    local lines, layout = para.lines, para.layout
    local k, first = 1, true

    while k <= #lines do
      local at_top = #page.lines == 0

      -- Space before a paragraph, but never at the head of a page.
      if first and not at_top then y = y + layout.before_pt end

      -- How many of the lines left fit here.
      local left, fit, h = #lines - k + 1, 0, 0

      while fit < left and y + h + lines[k + fit].height_pt <= bottom do
        h = h + lines[k + fit].height_pt
        fit = fit + 1
      end

      if fit < left then
        -- **Never one line alone**: at least two left for the next page,
        -- and at least two here or none - unless this is the head of a
        -- page already, where moving would gain nothing.
        if left - fit < LEAST then fit = left - LEAST end
        if fit < LEAST and not at_top then fit = 0 end
        if at_top then fit = math.max(1, fit) end
      elseif first and layout.keep_with_next and not at_top
             and y + h + following(n) > bottom then
        -- **Kept with the next**: a heading that would end this page goes
        -- to the next with what it heads.
        fit = 0
      end

      if fit == 0 then
        new_page()
      else
        for i = k, k + fit - 1 do put(lines[i]) end

        k = k + fit
        first = false

        if k <= #lines then new_page() end
      end
    end

    y = y + layout.after_pt
  end

  -- Page numbers, centred in the footer, in Caption - what a caption and a
  -- page number both are: small text beside the page's matter.
  if doc.footer.on and doc.footer.page_numbers then
    local style = by_name.Caption or doc.styles[1]
    local look = look_of(richtext.look({}, style))
    local baseline = page_h - writedoc.pt(doc.footer.from_bottom_mm)

    for _, pg in ipairs(pages) do
      local text = tostring(pg.number)
      local w = width(look, text)

      pg.footer = { baseline_pt = baseline,
                    piece = { text = text, look = look,
                              x_pt = (page_w - w) / 2, width_pt = w } }
    end
  end

  return { looks = looks, pages = pages }
end

--------------------------------------------------------------------------
-- **A caret on a set page** (W4b): where a place stands, the place under a
-- point, and the lines above and below - for Write's window, a slide's
-- text box and a cell alike. A place is `richtext`'s: `{ para, at }`.
--------------------------------------------------------------------------

-- Every line of a set in order, each with the number of its page.
local function all_lines(set)
  local out = {}

  for n, page in ipairs(set.pages) do
    for _, line in ipairs(page.lines) do
      out[#out + 1] = { page = n, line = line }
    end
  end

  return out
end

-- How far into `piece` the byte `at` is, in points: its prefix's advance,
-- and a justified line's widened spaces in it.
local function offset_in(set, measure, piece, at, extra)
  local prefix = piece.text:sub(1, at - piece.at)

  if prefix == "" then return 0 end

  local spaces = select(2, prefix:gsub(" ", ""))

  return measure.width(set.looks[piece.look], prefix) + spaces * extra
end

-- Where a line's text ends, in its paragraph's bytes.
local function line_end(line)
  local last = line.pieces[#line.pieces]
  return last and (last.at + #last.text) or line.from
end

--
-- **Where `place` stands**: its page, its x and its line's baseline,
-- ascent and height, in points - the caret drawn from it - and the line
-- itself. The line is the last of its paragraph that starts at or before
-- the place, so a caret at a wrap stands at the start of the next line.
--
function pageset.locate(set, measure, place)
  local found

  for _, entry in ipairs(all_lines(set)) do
    if entry.line.para == place.para and entry.line.from <= place.at then
      found = entry
    elseif found and entry.line.para ~= place.para then
      break
    end
  end

  if not found then return nil end

  local line = found.line
  local x = line.x_pt

  for _, piece in ipairs(line.pieces) do
    if place.at >= piece.at and place.at <= piece.at + #piece.text then
      x = piece.x_pt + offset_in(set, measure, piece, place.at,
                                 line.extra_space_pt)
      break
    elseif place.at > piece.at + #piece.text then
      x = piece.x_pt + piece.width_pt
    end
  end

  return { page = found.page, line = line, x_pt = x,
           baseline_pt = line.baseline_pt, ascent_pt = line.ascent_pt,
           height_pt = line.height_pt }
end

--
-- **The place nearest `x_pt` on `line`**: before the character whose
-- middle is past it, after the last one, the line's start before the
-- first.
--
function pageset.place_on(set, measure, line, x_pt)
  local place = { para = line.para, at = line.from }

  for _, piece in ipairs(line.pieces) do
    if x_pt < piece.x_pt then return place end

    local look = set.looks[piece.look]
    local pen, at = piece.x_pt, piece.at

    for ch in piece.text:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
      local w = measure.width(look, ch)
                + (ch == " " and line.extra_space_pt or 0)

      if x_pt < pen + w / 2 then return { para = line.para, at = at } end

      pen = pen + w
      at = at + #ch
    end

    place = { para = line.para, at = at }
  end

  return place
end

--
-- **The place under a point** of page `page`, in points from its top left:
-- the line whose height holds it, or the nearest, and the place on it.
--
function pageset.hit(set, measure, page, x_pt, y_pt)
  local lines = set.pages[page] and set.pages[page].lines or {}
  local best, gap = nil, math.huge

  for _, line in ipairs(lines) do
    local top = line.baseline_pt - line.ascent_pt
    local d = 0

    if y_pt < top then d = top - y_pt
    elseif y_pt > top + line.height_pt then d = y_pt - (top + line.height_pt) end

    if d < gap then best, gap = line, d end
  end

  if not best then return nil end

  return pageset.place_on(set, measure, best, x_pt)
end

--
-- **The line above or below**, at the same x: Up and Down. `x_pt` is the
-- column a caret keeps as it passes shorter lines; nil means where it is.
-- The place itself at the document's first or last line.
--
function pageset.vertical(set, measure, place, step, x_pt)
  local here = pageset.locate(set, measure, place)

  if not here then return place end

  local lines = all_lines(set)

  for i, entry in ipairs(lines) do
    if entry.line == here.line then
      local other = lines[i + step]

      if not other then return place, x_pt or here.x_pt end

      return pageset.place_on(set, measure, other.line, x_pt or here.x_pt),
             x_pt or here.x_pt
    end
  end

  return place
end

--
-- **A selection's rectangles**, line by line, in points: `{ page, x_pt,
-- y_pt, w_pt, h_pt }` each, from place `a` to place `b` in either order -
-- the first line from where the selection starts, the last to where it
-- ends, every line between whole.
--
function pageset.selection(set, measure, a, b)
  if b.para < a.para or (b.para == a.para and b.at < a.at) then a, b = b, a end

  local from = pageset.locate(set, measure, a)
  local to = pageset.locate(set, measure, b)
  local out, inside = {}, false

  if not from or not to then return out end

  for _, entry in ipairs(all_lines(set)) do
    local line = entry.line

    if line == from.line then inside = true end

    if inside then
      local left = line == from.line and from.x_pt or line.x_pt
      local right = line == to.line and to.x_pt or (line.x_pt + line.width_pt)

      -- A line wholly selected and empty still shows that it is.
      if right <= left and line ~= to.line then right = left + 4 end

      if right > left then
        out[#out + 1] = { page = entry.page, x_pt = left,
                          y_pt = line.baseline_pt - line.ascent_pt,
                          w_pt = right - left, h_pt = line.height_pt }
      end
    end

    if line == to.line then break end
  end

  return out
end

-- **Home and End**: the start and the end of the line a place is on.
function pageset.line_ends(set, measure, place)
  local here = pageset.locate(set, measure, place)

  if not here then return place, place end

  return { para = place.para, at = here.line.from },
         { para = place.para, at = line_end(here.line) }
end

return pageset
