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
local function widths(measure, looks, ligatures)
  local cache = {}

  return function(look, text)
    local key = look .. "\0" .. text
    local w = cache[key]

    if not w then
      w = measure.width(looks[look], text, ligatures)
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
-- whether a line break ended it. `room_of(k)` is the room for line `k` - a
-- first line's indent, a drop cap's lines beside it.
--
local function break_lines(tokens, room_of, width, hyphenate)
  local lines, line = {}, nil
  local pending = {}          -- spaces since the last word, not yet placed

  local function new_line(from)
    line = { pieces = {}, width_pt = 0, spaces = 0, forced = false,
             first = #lines == 0, from = from }
    lines[#lines + 1] = line
    pending = {}
  end

  local function room()
    return room_of(#lines)
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

  --
  -- **A word broken with a hyphen** (the Document panel's switch): at the
  -- last of its breaks where what comes before, and the hyphen, still fit
  -- - its punctuation kept out of the lookup and on its own side. Nil when
  -- no break fits.
  --
  local function hyphen_split(tok, gap)
    local lead, core, trail = tok.text:match("^([^%a\128-\255]*)([%a\128-\255]+)(.-)$")

    if not core or core == "" or trail:match("[%a\128-\255]") then return nil end

    local breaks = hyphenate(core)
    local dash = width(tok.look, "-")

    for k = #breaks, 1, -1 do
      local head = tok.text:sub(1, #lead + breaks[k])
      local head_w = width(tok.look, head)

      if line.width_pt + gap + head_w + dash <= room() then
        return { head = head, head_w = head_w, dash_w = dash,
                 tail = tok.text:sub(#head + 1) }
      end
    end
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
      local split = nil

      if #line.pieces > 0 and line.width_pt + gap + box_w > room() then
        split = hyphenate and j == i + 1 and hyphen_split(tok, gap)
        if not split then new_line() end
      end

      if split then
        -- The head and its hyphen on this line, the tail a word of its own
        -- for the next: the hyphen is drawn, never text (`soft`).
        flush()
        place(tok, split.head, tok.at, split.head_w)
        line.pieces[#line.pieces + 1] = { text = "-", look = tok.look,
          at = tok.at + #split.head, x_pt = line.width_pt,
          width_pt = split.dash_w, soft = true }
        line.width_pt = line.width_pt + split.dash_w
        new_line()
        tokens[i] = { kind = "word", text = split.tail, look = tok.look,
                      at = tok.at + #split.head }
      elseif line.width_pt + gap + box_w <= room() or #line.pieces > 0 then
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

      -- A word split by a hyphen is placed again, its tail, next time round.
      if not split then i = j end
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
    if not pc.cap then
      local a, d, g = measure.line(looks[pc.look])
      asc, desc, gap = math.max(asc, a), math.max(desc, d), math.max(gap, g)
      seen = true
    end
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
local function align(line, layout, start, room, last)
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
-- Lists and drop caps.
--------------------------------------------------------------------------

-- How far a list's lines stand in from its marker.
pageset.LIST_MM = 6

-- The room between a drop cap and the lines beside it.
pageset.CAP_GAP_MM = 1.5

--
-- **A drop cap's piece**: the paragraph's first character, taken out of its
-- first word token, in that character's look at a size whose capital
-- height - about seven tenths of an em - reaches from the first line's
-- capitals down to the last line's baseline. Nil when the paragraph does
-- not begin with a word.
--
function pageset.drop_cap(tokens, layout, measure, looks, look_of, width)
  local tok = tokens[1]

  if not tok or tok.kind ~= "word" then return nil end

  local ch = tok.text:match("^[%z\1-\127\194-\244][\128-\191]*")
  local base = looks[tok.look]
  local a, d, g = measure.line(base)
  local pitch = (a + d + g) * layout.spacing_lines
  local size = base.size_pt + (layout.drop_cap_lines - 1) * pitch / 0.7

  local look = {}
  for k, v in pairs(base) do look[k] = v end
  look.size_pt = math.floor(size * 10 + 0.5) / 10

  local cap_look = look_of(look)

  tok.text = tok.text:sub(#ch + 1)
  tok.at = tok.at + #ch

  if tok.text == "" then table.remove(tokens, 1) end

  return { text = ch, look = cap_look, at = 1, x_pt = 0, cap = true,
           width_pt = width(cap_look, ch) }
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
--     header    the header's words as a piece and its baseline, or nil
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

--
-- `opts.hyphenate`, when the document says to hyphenate, is a function from
-- a word to where it may break (`hyphen.lua`): handed in, so the setting
-- stays a sum a test can do on the Mac.
--
function pageset.set(doc, measure, cache, opts)
  local page_w, page_h = writedoc.page_mm(doc)
  local m = doc.margins_mm

  page_w, page_h = writedoc.pt(page_w), writedoc.pt(page_h)

  local left = writedoc.pt(m.left)
  local column = page_w - left - writedoc.pt(m.right)
  local top, bottom = writedoc.pt(m.top), page_h - writedoc.pt(m.bottom)
  local hyphenate = doc.hyphenation and opts and opts.hyphenate or nil
  local geometry = ("%s:%s:%s:%s:%s"):format(left, column, tostring(doc.ligatures),
                                          tostring(hyphenate ~= nil), doc.language)

  cache = cache or pageset.cache()

  if cache.geometry ~= geometry or cache.measure ~= measure
     or cache.styles ~= doc.styles then
    cache.geometry, cache.measure, cache.styles = geometry, measure, doc.styles
    cache.looks, cache.look_of = looks_table()
    cache.width = widths(measure, cache.looks, doc.ligatures)
    cache.paras = setmetatable({}, { __mode = "k" })
  end

  local looks, look_of, width = cache.looks, cache.look_of, cache.width

  local by_name = {}
  for _, s in ipairs(doc.styles) do by_name[s.name] = s end

  -- Every paragraph broken into lines first: the column is the same on
  -- every page, so where a line breaks does not depend on where it lands,
  -- and keeping a paragraph with the next needs to know the next.
  local paras = {}
  local numbered = 0

  for n, p in ipairs(doc.body) do
    local kept = cache.paras[p]

    if kept then
      for _, line in ipairs(kept.lines) do line.para = n end
      paras[n] = kept
    elseif p.picture then
      -- **A picture** (W5): one line as tall as the picture, scaled down
      -- to the column when it is wider, placed as its paragraph aligns.
      local style = by_name[p.style] or doc.styles[1]
      local layout = richtext.layout(p, style)
      local inner = left + writedoc.pt(layout.indent_left_mm)
      local room = math.max(1, column - writedoc.pt(layout.indent_left_mm)
                                   - writedoc.pt(layout.indent_right_mm))
      local w = writedoc.pt(p.picture.width_mm)
      local h = writedoc.pt(p.picture.height_mm)

      if w > room then w, h = room, h * room / w end

      local line = { pieces = { { picture = p.picture.name, text = "", at = 1,
                                  look = look_of(richtext.look({}, style)),
                                  x_pt = 0, width_pt = w, height_pt = h } },
                     width_pt = w, spaces = 0, forced = false, first = true,
                     from = 1, para = n, ascent_pt = h, height_pt = h + 4 }

      align(line, layout, inner, room, true)

      paras[n] = { lines = { line }, layout = layout, style = style, inner = inner }
      cache.paras[p] = paras[n]
    else
      local style = by_name[p.style] or doc.styles[1]
      local layout = richtext.layout(p, style)
      local tokens = tokens_of(p, style, look_of)
      local empty = look_of(richtext.look({}, style))
      local inner = left + writedoc.pt(layout.indent_left_mm)
      local column_room = math.max(1, column - writedoc.pt(layout.indent_left_mm)
                                          - writedoc.pt(layout.indent_right_mm))

      -- **A list hangs**: every line in from its marker, the first line's
      -- indent not applied.
      local listed = layout.list ~= "none"
      local hang = listed and writedoc.pt(pageset.LIST_MM) or 0
      local first_in = listed and 0 or writedoc.pt(layout.indent_first_mm)

      -- **A drop cap**: the first character taken out of the text and set
      -- as tall as `drop_cap_lines` lines, those lines beside it.
      local cap = layout.drop_cap_lines >= 2 and pageset.drop_cap(tokens, layout,
                    measure, looks, look_of, width)
      local cap_lines = cap and layout.drop_cap_lines or 0
      local cap_room = cap and (cap.width_pt + writedoc.pt(pageset.CAP_GAP_MM)) or 0

      local function start_of(k)
        return inner + hang + (k == 1 and first_in or 0)
               + (k <= cap_lines and cap_room or 0)
      end

      local function room_of(k)
        return math.max(1, column_room - hang - (k == 1 and first_in or 0)
                           - (k <= cap_lines and cap_room or 0))
      end

      local lines = break_lines(tokens, room_of, width, hyphenate)

      for k, line in ipairs(lines) do
        line.para = n
        line_height(line, looks, measure, empty, layout.spacing_lines)
        align(line, layout, start_of(k), room_of(k), k == #lines)
      end

      if cap then
        local first = lines[1]

        cap.x_pt = inner + hang
        cap.drop_pt = 0

        for k = 2, math.min(cap_lines, #lines) do
          cap.drop_pt = cap.drop_pt + lines[k].height_pt
        end

        -- With fewer lines than the cap is tall, the lines it would have
        -- stood beside are counted as the first one's height.
        for _ = #lines + 1, cap_lines do
          cap.drop_pt = cap.drop_pt + first.height_pt
        end

        table.insert(first.pieces, 1, cap)
        first.from = 1
      end

      paras[n] = { lines = lines, layout = layout, style = style, inner = inner,
                   listed = listed }
      cache.paras[p] = paras[n]
    end

    -- **A list's marker**, set again on every pass: a number depends on the
    -- paragraphs before it, which the cache cannot know. It is drawn, never
    -- typed into - the caret and a copy do not meet it.
    local para = paras[n]
    local first = para.lines[1]

    if para.listed then
      if para.layout.list == "number" then
        numbered = numbered + 1
      else
        numbered = 0
      end

      local look = look_of(richtext.look({}, para.style))
      local text = para.layout.list == "number" and (numbered .. ".") or "\u{2022}"

      first.marker = { text = text, look = look, x_pt = para.inner,
                       width_pt = width(look, text) }
    else
      numbered = 0
      first.marker = nil
    end
  end

  local pages = {}
  local page, y

  -- **Facing pages**: a left-hand page - an even one - has its margins the
  -- other way round, the inside one at its right, so its lines stand over
  -- by the difference. Lines are set once for every page; the shift is the
  -- page's, and what draws or finds a place on it adds it.
  local mirror = doc.facing and (writedoc.pt(m.right) - writedoc.pt(m.left)) or 0

  local function new_page()
    local number = #pages + 1

    page = { number = number, width_pt = page_w, height_pt = page_h,
             lines = {}, shift_pt = number % 2 == 0 and mirror or 0 }
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

    -- **A page break before it** (Add Page): this paragraph begins a
    -- page, wherever the one before it ended.
    if layout.page_break_before and #page.lines > 0 then
      new_page()
    end

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

  -- The header's words, centred, `from_top_mm` from the page's top to
  -- their top, in Caption as the page numbers are.
  if doc.header.on and doc.header.text ~= "" then
    local style = by_name.Caption or doc.styles[1]
    local look = look_of(richtext.look({}, style))
    local ascent = measure.line(looks[look])
    local baseline = writedoc.pt(doc.header.from_top_mm) + ascent
    local w = width(look, doc.header.text)

    for _, pg in ipairs(pages) do
      pg.header = { baseline_pt = baseline,
                    piece = { text = doc.header.text, look = look,
                              x_pt = (page_w - w) / 2, width_pt = w } }
    end
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

  return { looks = looks, pages = pages, ligatures = doc.ligatures,
           facing = doc.facing }
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

  return measure.width(set.looks[piece.look], prefix, set.ligatures) + spaces * extra
end

-- Where a line's text ends, in its paragraph's bytes: a hyphen it ends
-- with is drawn, not text.
local function line_end(line)
  for k = #line.pieces, 1, -1 do
    local p = line.pieces[k]
    if not p.soft then return p.at + #p.text end
  end
  return line.from
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
    if piece.soft then
      -- A hyphen is not a place.
    elseif place.at >= piece.at and place.at <= piece.at + #piece.text then
      x = piece.x_pt + offset_in(set, measure, piece, place.at,
                                 line.extra_space_pt)
      break
    elseif place.at > piece.at + #piece.text then
      x = piece.x_pt + piece.width_pt
    end
  end

  return { page = found.page, line = line, x_pt = x + set.pages[found.page].shift_pt,
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
    if piece.soft then return place end
    if x_pt < piece.x_pt then return place end

    local look = set.looks[piece.look]
    local pen, at = piece.x_pt, piece.at

    for ch in piece.text:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
      local w = measure.width(look, ch, set.ligatures)
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

  return pageset.place_on(set, measure, best, x_pt - set.pages[page].shift_pt)
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

      return pageset.place_on(set, measure, other.line,
                              (x_pt or here.x_pt) - set.pages[other.page].shift_pt),
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
      local shift = set.pages[entry.page].shift_pt
      local left = line == from.line and from.x_pt or (line.x_pt + shift)
      local right = line == to.line and to.x_pt or (line.x_pt + line.width_pt + shift)

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
