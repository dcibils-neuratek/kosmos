-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A document on a page: words in a proportional face, wrapped to a column,
-- with a caret between characters (`roadmap.md` 6zs, `docs/texteditor.html`).
--
--   local docview = use("/Kosmos/Libraries/docview.lua")
--   local page = docview.new(ui, { x = 0, y = 46, w = 760, h = 500,
--                                  text = body,
--                                  faces = function(kind) return face end,
--                                  style = md.line,          -- or nil
--                                  on_change = function(page) end })
--
-- **Not `ui.editor`**, which is monospace by construction: it numbers lines,
-- puts a block caret *on* a cell and turns a click into a column by
-- dividing, all of which is arithmetic on a cell every glyph shares. A
-- letter is in a proportional face and wraps, so here everything is
-- *measured* - where a row breaks, where the caret stands, which character a
-- click lands on - with the faces it is drawn in, asked for on every draw.
--
-- **Over the same `textbuf`** the IDE's editor edits, so undo, the
-- selection and every change being a `replace` are the ones that are
-- tested already. What is this file's is the page: rows, the caret's place
-- on them, Up and Down by what is seen rather than by line, and the steps a
-- caret takes being whole characters - a byte of `é` is not a place.
--
-- **Styled, when it is given a `style`** (step 2): a function that says what
-- a line is - `mdstyle.line`, for Markdown - and the page draws the same
-- bytes that way. A heading in the heading face and larger, with space
-- above it; bold, italic and code in their faces; a list's bullet, a
-- checkbox that ticks when clicked, a quotation's bar, a code block's
-- ground. **The marks stay** - `#`, `**`, `- [ ]` - faint, and the ones that
-- start a line hang in the margin, so the words line up down the page and
-- what is on the disk is what is on the screen (agreed on 28 September).
-- With no `style` every line is plain, in the body face.
--
-- `faces(kind)` gives the face for "body", "bold", "italic", "bolditalic",
-- "code", "h1", "h2" and "h3", asked on every draw: a sized face is given
-- back when the desktop's faces change.
--
-- **Pixels, not rows, are what it scrolls by**, since a heading's row is
-- taller than a paragraph's.
--
-- `docview.wrap`, `next_char` and `prev_char` are pure, and
-- `tools/test_docview.lua` holds them to a measure on the Mac.

local textbuf = use("/Kosmos/Libraries/textbuf.lua")
local keys    = use("/Kosmos/Libraries/keys.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")

local docview = {}

--------------------------------------------------------------------------
-- Characters, which are UTF-8: a place is a byte that starts one.
--------------------------------------------------------------------------

local function starts(s, i)
  local b = s:byte(i)

  return b == nil or b < 0x80 or b >= 0xC0
end

-- The place after the character at `i`.
function docview.next_char(s, i)
  i = i + 1

  while i <= #s and not starts(s, i) do i = i + 1 end

  return i
end

-- The place of the character before `i`.
function docview.prev_char(s, i)
  i = i - 1

  while i > 1 and not starts(s, i) do i = i - 1 end

  return math.max(1, i)
end

--
-- **A line as rows no wider than `width`**: a list of `{ from, to }`, the
-- bytes of each, `to` the last, starting at byte `from` of the line (1 when
-- not given - a styled line's hanging mark is not wrapped with its words).
-- `measure(a, b)` is how wide bytes `a` to `b` are. A row breaks after the
-- spaces that follow a word, and they stay on it, as every editor keeps
-- them; a word wider than the row alone is cut where it stops fitting, at a
-- character and never inside one. Nothing to wrap is one empty row.
--
function docview.wrap(s, width, measure, from)
  from = from or 1

  if from > #s or not width then return { { from, #s } } end

  local rows = {}
  local start, at, x = from, from, 0

  while at <= #s do
    local _, token_end = s:find("^[^ ]* *", at)
    local word_end = at + #(s:sub(at, token_end):match("^[^ ]*")) - 1
    local ww = (word_end >= at) and measure(at, word_end) or 0

    if x > 0 and x + ww > width then
      rows[#rows + 1] = { start, at - 1 }
      start, x = at, 0
    end

    if x == 0 and ww > width then
      -- As much of it as fits, and at least one character.
      local e = docview.next_char(s, at) - 1

      while true do
        local further = docview.next_char(s, e + 1) - 1

        if further > word_end or measure(at, further) > width then break end

        e = further
      end

      rows[#rows + 1] = { start, e }
      start, at = e + 1, e + 1
    else
      x = x + measure(at, token_end)
      at = token_end + 1
    end
  end

  if start <= #s or #rows == 0 then rows[#rows + 1] = { start, #s } end

  return rows
end

--------------------------------------------------------------------------
-- The view.
--------------------------------------------------------------------------

local LEADING = 55      -- a row's height past its face's, in hundredths
local PAD_Y   = 22      -- above the first row and below the last
local PAD_X   = 26      -- the least room either side of the column
local MARGIN  = 10      -- from a hanging mark to what it marks
local BOX     = 14      -- a checklist's box

-- A sentence of the letters prose is made of, measured to know how wide
-- "seventy characters" is in a face whose letters are not one width.
local SAMPLE = "the quick brown fox jumps over a lazy dog, "

-- A line with no style: every byte the body's.
local PLAIN = { kind = "para", hang = 0, level = 0 }

-- What each kind of line is drawn in, and the space above a heading, in
-- hundredths of the body's height.
local BASE = { h1 = "h1", h2 = "h2", h3 = "h3", code = "code", fence = "code",
               quote = "italic" }
local ABOVE = { h1 = 40, h2 = 30, h3 = 20 }   -- a blank line is usually above too
local LISTS = { item = true, check = true, number = true }

function docview.new(ui, spec)
  local theme = ui.theme
  local v = ui.view(spec)
  local buf = textbuf.new(spec.text)

  v.buf = buf
  v.lines = buf.lines
  v.focusable = true
  v.wrap = (spec.wrap ~= false)
  v.column = spec.column or 70
  v.faces = spec.faces
  v.style = spec.style
  v.continue = spec.continue
  v.on_change = spec.on_change
  v.ground = spec.ground      -- the page's colour; the sunken well when not said
  v.scroll = 0                -- pixels scrolled from the top
  v.hscroll = 0               -- and from the left, when lines do not wrap
  v.version = 0
  v.dirty = false

  local face_of = {}          -- kind -> the face this pass draws it in
  local GH, RH = 16, 24       -- the body's height, and a body row's

  local rows, first, total = {}, {}, 0
  local laid, laid_version = nil, -1
  local cache, cached, cache_for = {}, 0, nil
  local want = nil            -- the x Up and Down aim for, past the indent

  local KINDS = { "body", "bold", "italic", "bolditalic", "code", "h1", "h2", "h3" }

  local function metrics()
    for _, k in ipairs(KINDS) do
      face_of[k] = (v.faces and v.faces(k)) or "ui"
    end

    GH = gfx.height(face_of.body)
    RH = GH + (GH * LEADING) // 100
  end

  -- The face a span of a line is drawn in: the line's own when it is a
  -- heading or code, the span's weight and slant otherwise.
  local function span_face(entry, what)
    local kind = entry.info.kind

    if BASE[kind] and kind ~= "quote" then return face_of[BASE[kind]] end
    if what == "code" then return face_of.code end
    if what == "bold" or what == "italic" or what == "bolditalic" then
      return face_of[what]
    end

    return face_of[entry.base]
  end

  -- How wide bytes `a` to `b` of a line are, span by span.
  local function width_of(line, entry, a, b)
    if b < a then return 0 end

    local spans = entry.info.spans

    if not spans then return gfx.measure(line:sub(a, b), face_of[entry.base]) end

    local w = 0

    for _, sp in ipairs(spans) do
      local sa, sb = math.max(a, sp[1]), math.min(b, sp[2])

      if sa <= sb then w = w + gfx.measure(line:sub(sa, sb), span_face(entry, sp[3])) end
    end

    return w
  end

  -- Where the column is and how wide.
  local function geometry(self)
    local room = self.w - 2 * PAD_X - (ui.SCROLL_W + 2)

    if not self.wrap then return PAD_X, math.max(1, room) end

    local each = gfx.measure(SAMPLE, face_of.body) / #SAMPLE
    local width = math.max(1, math.min(room, math.floor(each * self.column)))

    return PAD_X + (room - width) // 2, width
  end

  -- A line read, measured and broken into rows: what it is, its face, how
  -- far in its words start, and the space above it.
  local function entry_for(self, line, state, width)
    local info, after = PLAIN, false

    if self.style then info, after = self.style(line, state) end

    local kind = info.kind
    local base = BASE[kind] or "body"
    local bullet = GH + GH // 2
    local indent = 0

    if LISTS[kind] then
      indent = bullet * ((info.level or 0) + 1)
    elseif kind == "quote" then
      indent = GH
    elseif kind == "code" or kind == "fence" then
      indent = GH // 2 + 4
    end

    local gh = gfx.height(face_of[base])
    local entry = { info = info, after = after, base = base, indent = indent,
                    gh = gh, rh = gh + (gh * LEADING) // 100,
                    gap = ((ABOVE[kind] or 0) * GH) // 100, bullet = bullet }

    local room = self.wrap and math.max(1, width - indent - ((kind == "code") and GH // 2 or 0))
                 or nil

    entry.pieces = docview.wrap(line, room, function(a, b)
      return width_of(line, entry, a, b)
    end, (info.hang or 0) + 1)

    return entry
  end

  local function layout(self)
    local _, width = geometry(self)
    local key = tostring(face_of.body) .. ":" .. tostring(face_of.h1) .. ":"
                .. width .. ":" .. tostring(self.wrap) .. ":" .. tostring(self.style)

    if laid == key and laid_version == self.version then return end

    if cache_for ~= key or cached > 4 * #buf.lines + 256 then
      cache, cached, cache_for = {}, 0, key
    end

    rows, first = {}, {}

    local y, state = PAD_Y, false

    for n, line in ipairs(buf.lines) do
      local slot = state and (line .. "\1") or line
      local entry = cache[slot]

      if not entry then
        entry = entry_for(self, line, state, width)
        cache[slot] = entry
        cached = cached + 1
      end

      state = entry.after
      first[n] = #rows + 1

      for k, p in ipairs(entry.pieces) do
        local gap = (k == 1) and entry.gap or 0
        local h = entry.rh + gap

        rows[#rows + 1] = { i = #rows + 1, n = n, from = p[1], to = p[2], y = y,
                            h = h, gap = gap, first = (k == 1), entry = entry }
        y = y + h
      end
    end

    first[#buf.lines + 1] = #rows + 1
    total = y + PAD_Y
    laid, laid_version = key, self.version
  end

  local function hang_of(r) return r.entry.info.hang or 0 end

  -- The row a place is on: the last of its line's rows that starts at or
  -- before it, so a place at a break is at the start of the row below; a
  -- place in a line's mark is on its first row.
  local function row_of(n, x)
    local i = first[n] or 1

    while i + 1 < (first[n + 1] or #rows + 1) and rows[i + 1].from <= x do
      i = i + 1
    end

    return i
  end

  local function last_of_line(i)
    return rows[i + 1] == nil or rows[i + 1].n ~= rows[i].n
  end

  -- Where a line's hanging mark ends, in the page's pixels: before its
  -- bullet for a list, before its words for anything else.
  local function mark_right(x0, r)
    local e = r.entry

    if LISTS[e.info.kind] then return x0 + e.indent - e.bullet - MARGIN // 2 end

    return x0 - MARGIN
  end

  -- The mark as it is drawn: without the spaces around it.
  local function mark_span(line, r)
    local hang = hang_of(r)
    local a = (line:find("%S") or 1)
    local b = hang

    while b >= a and line:sub(b, b):match("%s") do b = b - 1 end

    return a, b
  end

  -- How far from the column's edge a place is drawn: in its row's words,
  -- or in the mark hanging before them.
  local function x_of(x0, r, x)
    local line = buf.lines[r.n]

    if r.first and x <= hang_of(r) and r.entry.info.kind ~= "number" then
      local a, b = mark_span(line, r)
      local left = mark_right(x0, r) - width_of(line, r.entry, a, b)

      return left + width_of(line, r.entry, a, math.min(b, x - 1)) - v.hscroll
    end

    if r.first and x <= hang_of(r) then
      -- A number is drawn where a bullet would be, ending at its words.
      local a, b = mark_span(line, r)
      local left = x0 + r.entry.indent - MARGIN // 2 - width_of(line, r.entry, a, b)

      return left + width_of(line, r.entry, a, math.min(b, x - 1)) - v.hscroll
    end

    return x0 + r.entry.indent + width_of(line, r.entry, r.from, x - 1) - v.hscroll
  end

  -- The place in row `r` nearest `px` pixels past its indent: past a
  -- character's middle is after it. The end of a row that wraps is before
  -- its last character, which is the space it broke at.
  local function place_in_row(r, px)
    local line = buf.lines[r.n]
    local at, w = r.from, 0

    while at <= r.to do
      local nxt = docview.next_char(line, at)
      local cw = width_of(line, r.entry, at, nxt - 1)

      if w + cw / 2 > px then break end

      w, at = w + cw, nxt
    end

    if at > r.to and r.to >= r.from and not last_of_line(r.i) then
      at = math.max(r.from, docview.prev_char(line, r.to + 1))
    end

    return at
  end

  --
  -- What the buffer did, told to the view and to whoever is watching: a
  -- version that rises with every change, and whether it is saved.
  --
  local function after_edit(self)
    self.lines = buf.lines

    if buf.changed_from ~= math.huge then
      self.version = self.version + 1
      buf.changed_from = math.huge
      self.dirty = buf.dirty
      self.words_at = nil

      if self.on_change then self.on_change(self) end
    else
      self.dirty = buf.dirty
    end
  end

  -- The caret back into view - when it moved, not on every paint, or a wheel
  -- that scrolled the page would be pulled back by the next one.
  local function follow(self)
    if self.followed then return end

    self.followed = true

    local r = rows[row_of(buf.cy, buf.cx)]

    if not r then return end

    if r.y < self.scroll + PAD_Y // 2 then
      self.scroll = math.max(0, r.y - PAD_Y)
    elseif r.y + r.h > self.scroll + self.h then
      self.scroll = r.y + r.h - self.h + PAD_Y // 2
    end

    if not self.wrap then
      local _, width = geometry(self)
      local cx = r.entry.indent + width_of(buf.lines[r.n], r.entry, r.from, buf.cx - 1)

      if cx < self.hscroll then self.hscroll = math.max(0, cx - width // 4) end
      if cx > self.hscroll + width then self.hscroll = cx - width + width // 4 end
    else
      self.hscroll = 0
    end
  end

  local function clamp_scroll(self)
    local most = math.max(0, total - self.h)

    if self.scroll > most then self.scroll = most end
    if self.scroll < 0 then self.scroll = 0 end
  end

  --------------------------------------------------------------------------
  -- The text, as the window sees it.
  --------------------------------------------------------------------------

  function v:content() return buf:content() end

  function v:set(body)
    buf:set(body)
    self.lines = buf.lines
    self.scroll, self.hscroll = 0, 0
    self.version = self.version + 1
    self.dirty = false
    self.words_at = nil
    self.followed = nil
    want = nil
  end

  function v:saved()
    buf:saved()
    self.dirty = false
  end

  function v:selected() return buf:selected() end

  function v:insert(text)
    buf:insert(text)
    self.followed = nil
    after_edit(self)
    return true
  end

  -- The selection, or the caret, between `before` and `after`: a word made
  -- bold is `**word**`, and nothing selected leaves the caret between the
  -- two, ready for the word.
  function v:surround(before, after)
    local y1, x1, y2, x2 = buf:selection()

    if y1 then
      local inside = buf:between(y1, x1, y2, x2)

      buf:replace(y1, x1, y2, x2, before .. inside .. after)
    else
      local y, x = buf.cy, buf.cx

      buf:replace(y, x, y, x, before .. after)
      buf.cy, buf.cx = y, x + #before
    end

    self.followed = nil
    after_edit(self)
  end

  -- The line the caret is on begins with `lead` in place of whatever mark
  -- it had - a heading, a bullet, a quotation - as one step to undo.
  function v:set_lead(lead)
    local y = buf.cy
    local line = buf.lines[y]
    local hang = 0

    if self.style then hang = (self.style(line, false).hang or 0) end

    if line:sub(1, hang) == lead then lead = "" end   -- asked again: taken off

    local x = buf.cx

    buf:replace(y, 1, y, hang + 1, lead)
    buf.cy, buf.cx = y, math.max(1 + #lead, x - hang + #lead)
    self.followed = nil
    after_edit(self)
  end

  -- A checklist item's box, ticked or opened, on line `n`; false when the
  -- line has none.
  function v:toggle_check(n)
    n = n or buf.cy

    if not self.style then return false end

    local info = self.style(buf.lines[n], false)

    if info.kind ~= "check" or not info.box then return false end

    local keep_y, keep_x = buf.cy, buf.cx

    buf:replace(n, info.box, n, info.box + 1, info.checked and " " or "x")
    buf.cy, buf.cx = keep_y, keep_x
    after_edit(self)
    return true
  end

  -- Words, as a person counts them: runs of what is not a space.
  function v:words()
    if self.words_at ~= self.version then
      local n = 0

      for _, line in ipairs(buf.lines) do
        for _ in line:gmatch("%S+") do n = n + 1 end
      end

      self.word_count, self.words_at = n, self.version
    end

    return self.word_count
  end

  -- Where the caret is on the page, in the view's own pixels, and its row's
  -- height - for the right click's menu, and for a harness.
  function v:caret_at()
    metrics()
    layout(self)

    local r = rows[row_of(buf.cy, buf.cx)]
    local x0 = geometry(self)

    if not r then return x0, PAD_Y, RH end

    return x_of(x0, r, buf.cx), r.y + r.gap - self.scroll, r.h - r.gap
  end

  --------------------------------------------------------------------------
  -- Finding: every match, and one of them current.
  --------------------------------------------------------------------------

  --
  -- **Case does not matter**, which is what a person looking for a word in a
  -- letter means. The matches are found again after every change, so they
  -- never point at text that has moved.
  --
  local function search(self)
    local found = {}
    local needle = self.needle

    if needle and needle ~= "" then
      local low = needle:lower()

      for n, line in ipairs(buf.lines) do
        local text, at = line:lower(), 1

        while true do
          local s, e = text:find(low, at, true)

          if not s then break end

          found[#found + 1] = { n = n, from = s, to = e + 1 }
          at = e + 1
        end
      end
    end

    self.found, self.found_version = found, self.version
    return found
  end

  local function matches(self)
    if self.found_version ~= self.version then search(self) end

    return self.found or {}
  end

  -- Look for `needle`, and choose the first match at or after the caret.
  function v:find(needle)
    self.needle = needle
    self.current = nil

    local found = search(self)
    local sy, sx = buf:selection()
    local at_y, at_x = sy or buf.cy, sx or buf.cx

    for i, m in ipairs(found) do
      if m.n > at_y or (m.n == at_y and m.from >= at_x) then
        self:choose_match(i)
        return #found
      end
    end

    if #found > 0 then self:choose_match(1) end

    return #found
  end

  -- The match that is current, selected and in view.
  function v:choose_match(i)
    local found = matches(self)
    local m = found[i]

    if not m then self.current = nil return false end

    self.current = i
    buf.anchor = { m.n, m.from }
    buf.cy, buf.cx = m.n, m.to
    buf.open = nil
    want = nil
    self.followed = nil
    return true
  end

  -- The next or the one before, going round.
  function v:step_match(by)
    local found = matches(self)

    if #found == 0 then return false end

    local i = ((self.current or 0) - 1 + by) % #found + 1

    if not self.current and by < 0 then i = #found end

    return self:choose_match(i)
  end

  function v:match_count() return #matches(self) end

  -- The current match, replaced, and the next one chosen.
  function v:replace_match(with)
    local found = matches(self)
    local m = self.current and found[self.current]

    if not m then return false end

    buf:replace(m.n, m.from, m.n, m.to, with or "")
    after_edit(self)

    local now = matches(self)

    for i, after in ipairs(now) do
      if after.n > m.n or (after.n == m.n and after.from >= m.from + #(with or "")) then
        self:choose_match(i)
        return true
      end
    end

    if #now > 0 then self:choose_match(1) else self.current = nil end

    return true
  end

  -- Every match, as one step to undo; how many there were.
  function v:replace_all(with)
    local found = matches(self)

    if #found == 0 then return 0 end

    buf:group(function()
      for i = #found, 1, -1 do
        local m = found[i]

        buf:replace(m.n, m.from, m.n, m.to, with or "")
      end
    end)

    self.current = nil
    after_edit(self)
    return #found
  end

  --------------------------------------------------------------------------
  -- Drawing.
  --------------------------------------------------------------------------

  local HIT, HIT_NOW = 0xffffe58a, 0xffffc933

  -- The colours of this pass, from the look.
  local C = {}

  local function colours()
    C.text = theme.text
    C.dim = theme.text_dim
    C.faint = theme.mix(theme.sunken, theme.text_dim, 450)
    C.link = theme.accent
    C.selection = theme.mix(theme.sunken, theme.accent, 260)
    C.hit = theme.mix(theme.sunken, HIT, 800)
    C.hit_now = theme.mix(theme.sunken, HIT_NOW, 850)
    C.code_ground = theme.mix(theme.sunken, theme.text_dim, 70)
    C.rule = theme.mix(theme.sunken, theme.text_dim, 250)
    C.done = theme.mix(theme.sunken, theme.text_dim, 750)
  end

  local SPAN_COLOUR = { mark = "faint", link = "link", url = "faint" }

  -- A run of a row's bytes from x, in its span's face and colour, the part
  -- left of the column's edge left out a character at a time when lines do
  -- not wrap and the page has scrolled sideways. Returns where it ended.
  local function draw_run(g, line, entry, from, to, x, left, y_base, face, colour)
    if to < from then return x end

    while x < left and from <= to do
      local nxt = docview.next_char(line, from)

      x = x + gfx.measure(line:sub(from, nxt - 1), face)
      from = nxt
    end

    if from > to then return x end

    local text = line:sub(from, to)
    local w = gfx.measure(text, face)

    -- Faces of one size are not of one height; their feet are put level.
    g:text(x, y_base - gfx.height(face), text, colour, nil, face)
    return x + w
  end

  -- A row's words, span by span.
  local function draw_words(g, r, x0, y_base, done)
    local line = buf.lines[r.n]
    local e = r.entry
    local x = x0 + e.indent - v.hscroll
    local left = x0
    local spans = e.info.spans or { { r.from, r.to, nil } }
    local start_x = x

    for _, sp in ipairs(spans) do
      local a, b = math.max(r.from, sp[1]), math.min(r.to, sp[2])

      if a <= b then
        local face = span_face(e, sp[3])
        local colour = C[SPAN_COLOUR[sp[3]] or "text"]

        if done and sp[3] ~= "mark" then colour = C.done end
        if e.info.kind == "fence" then colour = C.faint end

        if sp[3] == "code" and e.info.kind ~= "code" then
          local w = width_of(line, e, a, b)
          local fh = gfx.height(face)

          g:fill_round(x - 2, y_base - fh - 1, w + 4, fh + 2, C.code_ground, 4)
        end

        x = draw_run(g, line, e, a, b, x, left, y_base, face, colour)
      end
    end

    -- A ticked item is struck through, words and all.
    if done and x > start_x then
      g:fill(start_x, y_base - GH // 2 + 1, x - start_x, 1, C.done)
    end
  end

  -- A band behind bytes `a` to `b - 1` of row `r`, clipped to the column.
  local function band(g, r, a, b, x0, width, y, h, colour, eol)
    local x1 = x_of(x0, r, a)
    local x2 = x_of(x0, r, b) + (eol and GH // 3 or 0)

    if x1 < x0 - PAD_X then x1 = x0 - PAD_X end
    if x2 > x0 + width + GH then x2 = x0 + width + GH end
    if x2 > x1 then g:fill(x1, y, x2 - x1, h, colour) end
  end

  -- What a styled line has that is not its words: a code block's ground, a
  -- quotation's bar, a rule, a bullet, a box, and its mark in the margin.
  local function draw_furniture(g, r, x0, width, y, y_base)
    local e = r.entry
    local info = e.info
    local kind = info.kind
    local line = buf.lines[r.n]

    if kind == "code" or kind == "fence" then
      g:fill(x0, y, width, r.h, C.code_ground)
    elseif kind == "quote" then
      g:fill(x0 + 2, y + r.gap, 3, r.h - r.gap, C.rule)
    elseif kind == "rule" then
      g:fill(x0, y + r.h // 2, width, 1, C.rule)
    end

    if not r.first then return end

    local body_h = gfx.height(face_of.body)
    local hang = hang_of(r)

    if kind == "item" then
      local dot = "\u{2022}"
      local w = gfx.measure(dot, face_of.body)

      g:text(x0 + e.indent - e.bullet + (e.bullet - w) // 2 - MARGIN // 2,
             y_base - body_h, dot, C.dim, nil, face_of.body)
    elseif kind == "check" then
      local bx = x0 + e.indent - e.bullet + (e.bullet - BOX) // 2 - MARGIN // 2
      local by = y_base - (body_h + BOX) // 2 - 1

      if info.checked then
        g:fill_round(bx, by, BOX, BOX, C.link, 3)
        g:line_icon(bx - 1, by - 1, "check", theme.sunken)
      else
        g:fill_round(bx, by, BOX, BOX, C.dim, 3)
        g:fill_round(bx + 1, by + 1, BOX - 2, BOX - 2, theme.sunken, 2)
      end
    end

    -- The mark, faint - or a number, which is what it says, dim.
    if hang > 0 then
      local a, b = mark_span(line, r)

      if b >= a then
        local text = line:sub(a, b)
        local w = gfx.measure(text, face_of.body)
        local right = (kind == "number") and (x0 + e.indent - MARGIN // 2)
                      or mark_right(x0, r)

        g:text(right - w - v.hscroll, y_base - body_h, text,
               (kind == "number") and C.dim or C.faint, nil, face_of.body)
      end
    end
  end

  function v:draw(g)
    metrics()
    colours()
    layout(self)
    follow(self)
    clamp_scroll(self)

    local x0, width = geometry(self)
    local sy1, sx1, sy2, sx2 = buf:selection()
    local found = self.needle and matches(self) or {}

    g:fill(0, 0, self.w, self.h, self.ground or theme.sunken)

    -- The first row in view, by halving.
    local lo, hi = 1, #rows

    while lo < hi do
      local mid = (lo + hi + 1) // 2

      if rows[mid].y <= self.scroll then lo = mid else hi = mid - 1 end
    end

    -- Matches by line, so each row looks only at its own.
    local by_line = {}

    for i, m in ipairs(found) do
      local list = by_line[m.n] or {}

      list[#list + 1] = { m = m, now = (i == self.current) }
      by_line[m.n] = list
    end

    for i = lo, #rows do
      local r = rows[i]
      local y = r.y - self.scroll

      if y >= self.h then break end

      if y + r.h > 0 then
        local line = buf.lines[r.n]
        local row_end = r.to + 1
        local text_y = y + r.gap
        local text_h = r.h - r.gap
        local fh = r.entry.gh
        local y_base = text_y + (text_h - fh) // 2 + fh
        local from = r.first and (hang_of(r) > 0 and 1 or r.from) or r.from

        draw_furniture(g, r, x0, width, y, y_base)

        if sy1 and r.n >= sy1 and r.n <= sy2 then
          local a = (r.n == sy1) and math.max(sx1, from) or from
          local b = (r.n == sy2) and math.min(sx2, row_end) or row_end
          local eol = (r.n < sy2) and last_of_line(i)

          if b > a or eol then
            band(g, r, a, math.max(a, b), x0, width, text_y, text_h, C.selection, eol)
          end
        end

        -- The matches over the selection, since the current one *is* the
        -- selection and says so in the stronger of the two yellows.
        for _, f in ipairs(by_line[r.n] or {}) do
          local a, b = math.max(f.m.from, from), math.min(f.m.to, row_end)

          if b > a then
            band(g, r, a, b, x0, width, text_y, text_h, f.now and C.hit_now or C.hit)
          end
        end

        draw_words(g, r, x0, y_base, r.entry.info.checked)
      end
    end

    -- The caret: a line between two characters, in the accent.
    if self.focused and not sy1 then
      local r = rows[row_of(buf.cy, buf.cx)]

      if r then
        local cx = x_of(x0, r, buf.cx)
        local fh = (r.first and buf.cx <= hang_of(r)) and GH or r.entry.gh
        local text_y = r.y - self.scroll + r.gap
        local cy = text_y + (r.h - r.gap - r.entry.gh) // 2 + (r.entry.gh - fh)

        if cy + fh > 0 and cy < self.h then
          g:fill(cx - 1, cy, 2, fh, theme.accent)
        end
      end
    end

    self.bar = ui.scrollbar(g, self.w, self.h, total, self.h, self.scroll + 1)
  end

  --------------------------------------------------------------------------
  -- Keys.
  --------------------------------------------------------------------------

  local SHIFT, CTRL = keys.SHIFT, keys.CTRL

  -- Moving the caret by hand, which `textbuf` does for its own moves: with
  -- Shift the selection grows from where it began, without it there is none.
  local function move_to(n, x, extend)
    if extend then
      buf.anchor = buf.anchor or { buf.cy, buf.cx }
    else
      buf.anchor = nil
    end

    buf.open = nil
    buf.cy, buf.cx = buf:clamp(n, x)
  end

  -- How far past its row's indent a place is, which is what Up and Down
  -- keep: a bullet's words line up with a paragraph's under them.
  local function offset_in_row(r, x)
    if r.first and x <= hang_of(r) then return 0 end

    return width_of(buf.lines[r.n], r.entry, r.from, x - 1)
  end

  -- Up or down by `by` rows as they are seen, keeping to the same x.
  local function vertical(self, by, extend)
    local i = row_of(buf.cy, buf.cx)

    want = want or offset_in_row(rows[i], buf.cx)

    local target = rows[i + by]

    if not target then
      if by < 0 then move_to(1, 1, extend)
      else move_to(#buf.lines, #buf.lines[#buf.lines] + 1, extend) end
      want = nil
      return
    end

    local keep = want

    move_to(target.n, place_in_row(target, keep), extend)
    want = keep
  end

  -- A step left or right that lands on a character, never inside one.
  local function settle(dir)
    local line = buf.lines[buf.cy]

    while buf.cx > 1 and buf.cx <= #line and not starts(line, buf.cx) do
      buf.cx = buf.cx + dir
    end
  end

  local CTRL_ENTER = keys.with(13, CTRL)

  function v:key(c)
    metrics()
    layout(self)
    self.followed = nil

    local k, mods = keys.parts(c)
    local shift = (mods & SHIFT) ~= 0
    local ctrl = (mods & CTRL) ~= 0

    -- Control and Return ticks a checklist item, or opens it again.
    if c == CTRL_ENTER then
      self:toggle_check()
      return true
    end

    if (mods & ~(SHIFT | CTRL)) ~= 0 then return false end

    if k == keys.UP or k == keys.DOWN then
      if ctrl then
        self.scroll = self.scroll + ((k == keys.UP) and -RH or RH)
        self.followed = true
      else
        vertical(self, (k == keys.UP) and -1 or 1, shift)
      end
      return true
    end

    if k == keys.LEFT or k == keys.RIGHT then
      local left = (k == keys.LEFT)

      if ctrl then
        if left then buf:word_left(shift) else buf:word_right(shift) end
      elseif left then
        buf:left(shift)
      else
        buf:right(shift)
      end

      settle(left and -1 or 1)
      want = nil
      return true
    end

    if k == keys.HOME or k == keys.END then
      if ctrl then
        if k == keys.HOME then buf:text_start(shift) else buf:text_end(shift) end
      else
        local i = row_of(buf.cy, buf.cx)
        local r = rows[i]

        if k == keys.HOME then
          move_to(r.n, r.from, shift)
        elseif last_of_line(i) then
          move_to(r.n, r.to + 1, shift)
        else
          move_to(r.n, math.max(r.from, docview.prev_char(buf.lines[r.n], r.to + 1)), shift)
        end
      end

      want = nil
      return true
    end

    if k == keys.PAGEUP or k == keys.PAGEDOWN then
      local page = math.max(1, (self.h - RH) // RH)

      -- The page and the caret by the same rows, so the caret stays where it
      -- was on the screen; `follow` fixes the ends, where the page cannot.
      vertical(self, (k == keys.PAGEUP) and -page or page, shift)
      self.scroll = self.scroll + ((k == keys.PAGEUP) and -1 or 1) * page * RH
      clamp_scroll(self)
      return true
    end

    local done = true

    if c == 26 then                                           -- ^Z
      buf:undo()
    elseif c == 25 then                                       -- ^Y
      buf:redo()
    elseif c == 10 or c == 13 then                            -- Enter
      --
      -- **A list goes on**: Return after an item starts the next - the same
      -- bullet, the next number, an open box - and Return on an empty one
      -- takes its mark away, which is the way out of a list.
      --
      local line = buf.lines[buf.cy]
      local lead, ends = nil, false

      if self.continue and not buf:selection() then lead, ends = self.continue(line) end

      if lead and ends then
        buf:replace(buf.cy, 1, buf.cy, #line + 1, "")
      elseif lead then
        buf:insert("\n" .. lead)
      else
        buf:newline(false)
      end
    elseif c == 8 or c == 127 then                            -- Backspace
      if not buf:delete_selected() then
        if buf.cx > 1 then
          local line = buf.lines[buf.cy]

          buf:replace(buf.cy, docview.prev_char(line, buf.cx), buf.cy, buf.cx,
                      "", "erase")
        else
          buf:backspace()
        end
      end
    elseif k == keys.DELETE and mods == 0 then
      if not buf:delete_selected() then
        local line = buf.lines[buf.cy]

        if buf.cx <= #line then
          buf:replace(buf.cy, buf.cx, buf.cy, docview.next_char(line, buf.cx),
                      "", "erase")
        else
          buf:delete_forward()
        end
      end
    elseif c >= 32 and c < 127 then
      buf:insert(string.char(c), "type")
    else
      done = false
    end

    if done then want = nil end

    after_edit(self)
    return done
  end

  --------------------------------------------------------------------------
  -- The clipboard, from the window manager's prefix by way of
  -- `window:dispatch_edit`, as `ui.editor` has it.
  --------------------------------------------------------------------------

  function v:edit(kind)
    if kind == "selectall" then
      buf:select_all()
      return true
    end

    if kind == "copy" or kind == "cut" then
      local text = buf:selected()

      if not text then return false end

      if not wmproto.copy(text) then return false end

      if kind == "cut" then
        buf:delete_selected()
        self.followed = nil
        after_edit(self)
      end

      return true
    end

    if kind == "paste" then
      local text = wmproto.paste()

      if not text or text == "" then return false end

      return self:insert(text)
    end

    return false
  end

  --------------------------------------------------------------------------
  -- The pointer.
  --------------------------------------------------------------------------

  -- The row under a point of the view, by halving.
  local function row_at(self, y)
    local py = y + self.scroll
    local lo, hi = 1, #rows

    while lo < hi do
      local mid = (lo + hi + 1) // 2

      if rows[mid].y <= py then lo = mid else hi = mid - 1 end
    end

    return rows[lo], py
  end

  -- The place under a point of the view: in a row's words, or in the mark
  -- hanging before them.
  local function place_at(self, x, y)
    local x0 = geometry(self)
    local r, py = row_at(self, y)

    if not r then return 1, 1 end
    if py < r.y then return r.n, r.from end

    local line = buf.lines[r.n]
    local hang = hang_of(r)

    if r.first and hang > 0 and x < x0 + r.entry.indent - v.hscroll then
      -- In the margin: the nearest place in the mark, or its end.
      local best, far = hang + 1, math.huge

      for at = 1, hang + 1 do
        local d = math.abs(x_of(x0, r, at) - x)

        if d < far then best, far = at, d end
      end

      return r.n, best
    end

    return r.n, place_in_row(r, x - x0 - r.entry.indent + self.hscroll)
  end

  -- Whether a point is on a checklist item's box.
  local function on_box(self, x, y)
    local x0 = geometry(self)
    local r = row_at(self, y)

    if not (r and r.first and r.entry.info.kind == "check") then return nil end

    local e = r.entry
    local bx = x0 + e.indent - e.bullet + (e.bullet - BOX) // 2 - MARGIN // 2

    if x >= bx - 3 and x <= bx + BOX + 3 then return r.n end

    return nil
  end

  function v:mouse(action, x, y)
    metrics()
    layout(self)

    local to = ui.scrollbar_mouse(self, action, x, y, self.w, self.h, total,
                                  self.h, self.scroll + 1)

    if to then
      self.scroll = to - 1
      self.followed = true
      return true
    end

    -- A box is ticked by clicking it, which writes the `x` into the file:
    -- held on the press and ticked on the release over the same box
    -- (`ui.click`; Diego, 9 October: a click is a press and a release, "in
    -- all kosmos"). A press on a box that cannot tick places the caret, as
    -- it always did.
    local function box_at(px, py)
      if px < 0 or py < 0 or px >= self.w or py >= self.h then return nil end

      local n = on_box(self, px, py)

      if not (n and self.style) then return nil end

      local info = self.style(buf.lines[n], false)

      if info.kind ~= "check" or not info.box then return nil end

      return n, function() self:toggle_check(n) end
    end

    if self.click_key ~= nil or (action == "press" and box_at(x, y)) then
      return ui.click(self, action, x, y, box_at)
    end

    if action == "press" or action == "move" then
      local n, at = place_at(self, x, y)

      if action == "press" then
        buf:place(n, at, false)
        buf.anchor = { buf.cy, buf.cx }
      else
        buf.cy, buf.cx = buf:clamp(n, at)
        buf.open = nil
      end

      want = nil
      self.followed = nil
    end

    return true
  end

  function v:wheel(n)
    metrics()
    self.scroll = self.scroll - n * ui.WHEEL_ROWS * RH
    clamp_scroll(self)
    self.followed = true
    return true
  end

  -- A different style, or none: plain text, or Markdown.
  function v:restyle(style, continue)
    self.style, self.continue = style, continue
    self.version = self.version + 1
    self.words_at = nil
  end

  return v
end

return docview
