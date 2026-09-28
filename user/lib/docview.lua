-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A document on a page: words in a proportional face, wrapped to a column,
-- with a caret between characters (`roadmap.md` 6zs, `docs/texteditor.html`).
--
--   local docview = use("/Kosmos/Libraries/docview.lua")
--   local page = docview.new(ui, { x = 0, y = 46, w = 760, h = 500,
--                                  text = body,
--                                  face = function() return size:face() end,
--                                  on_change = function(page) end })
--
-- **Not `ui.editor`**, which is monospace by construction: it numbers lines,
-- puts a block caret *on* a cell and turns a click into a column by
-- dividing, all of which is arithmetic on a cell every glyph shares. A
-- letter is in a proportional face and wraps, so here everything is
-- *measured* - where a row breaks, where the caret stands, which character a
-- click lands on - with the face it is drawn in, asked for on every draw.
--
-- **Over the same `textbuf`** the IDE's editor edits, so undo, the
-- selection and every change being a `replace` are the ones that are
-- tested already. What is this file's is the page: rows, the caret's place
-- on them, Up and Down by what is seen rather than by line, and the steps a
-- caret takes being whole characters - a byte of `é` is not a place.
--
-- **Pixels, not rows, are what it scrolls by**, so a row may one day be
-- taller than another (Markdown's headings, step 2) without the scrolling
-- changing.
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
-- **A line as rows no wider than `width`**, measured by `measure`: a list of
-- `{ from, to }`, the bytes of each, `to` the last. A row breaks after the
-- spaces that follow a word, and they stay on it, as every editor keeps
-- them; a word wider than the row alone is cut where it stops fitting, at
-- a character and never inside one. An empty line is one empty row.
--
function docview.wrap(s, width, measure)
  if s == "" or not width then return { { 1, #s } } end

  local rows = {}
  local from, at, x = 1, 1, 0

  while at <= #s do
    local _, token_end = s:find("^[^ ]* *", at)
    local word = s:sub(at, token_end):match("^[^ ]*")
    local ww = measure(word)

    if x > 0 and x + ww > width then
      rows[#rows + 1] = { from, at - 1 }
      from, x = at, 0
    end

    if x == 0 and ww > width then
      -- As much of it as fits, and at least one character.
      local word_end = at + #word - 1
      local e = docview.next_char(s, at) - 1

      while true do
        local further = docview.next_char(s, e + 1) - 1

        if further > word_end or measure(s:sub(at, further)) > width then
          break
        end

        e = further
      end

      rows[#rows + 1] = { from, e }
      from, at = e + 1, e + 1
    else
      x = x + measure(s:sub(at, token_end))
      at = token_end + 1
    end
  end

  if from <= #s or #rows == 0 then rows[#rows + 1] = { from, #s } end

  return rows
end

--------------------------------------------------------------------------
-- The view.
--------------------------------------------------------------------------

local LEADING = 55      -- a row's height past its face's, in hundredths
local PAD_Y   = 22      -- above the first row and below the last
local PAD_X   = 26      -- the least room either side of the column

-- A sentence of the letters prose is made of, measured to know how wide
-- "seventy characters" is in a face whose letters are not one width.
local SAMPLE = "the quick brown fox jumps over a lazy dog, "

function docview.new(ui, spec)
  local theme = ui.theme
  local v = ui.view(spec)
  local buf = textbuf.new(spec.text)

  v.buf = buf
  v.lines = buf.lines
  v.focusable = true
  v.wrap = (spec.wrap ~= false)
  v.column = spec.column or 70
  v.face = spec.face
  v.on_change = spec.on_change
  v.scroll = 0                -- pixels scrolled from the top
  v.hscroll = 0               -- and from the left, when lines do not wrap
  v.version = 0
  v.dirty = false

  local F = "ui"              -- the face this pass measures and draws in
  local GH, RH = 16, 24       -- its height, and a row's

  local rows, first, total = {}, {}, 0
  local laid, laid_version = nil, -1
  local cache, cached, cache_for = {}, 0, nil
  local want = nil            -- the x Up and Down aim for, in the column

  local function measure(s) return gfx.measure(s, F) end

  local function metrics()
    F = (v.face and v.face()) or "ui"
    GH = gfx.height(F)
    RH = GH + (GH * LEADING) // 100
  end

  -- Where the column is and how wide.
  local function geometry(self)
    local room = self.w - 2 * PAD_X - (ui.SCROLL_W + 2)

    if not self.wrap then return PAD_X, math.max(1, room) end

    local each = gfx.measure(SAMPLE, F) / #SAMPLE
    local width = math.max(1, math.min(room, math.floor(each * self.column)))

    return PAD_X + (room - width) // 2, width
  end

  local function layout(self)
    local _, width = geometry(self)
    local key = tostring(F) .. ":" .. width .. ":" .. tostring(self.wrap)

    if laid == key and laid_version == self.version then return end

    if cache_for ~= key or cached > 4 * #buf.lines + 256 then
      cache, cached, cache_for = {}, 0, key
    end

    rows, first = {}, {}

    local y = PAD_Y

    for n, line in ipairs(buf.lines) do
      local pieces = cache[line]

      if not pieces then
        pieces = self.wrap and docview.wrap(line, width, measure) or { { 1, #line } }
        cache[line] = pieces
        cached = cached + 1
      end

      first[n] = #rows + 1

      for _, p in ipairs(pieces) do
        rows[#rows + 1] = { i = #rows + 1, n = n, from = p[1], to = p[2], y = y }
        y = y + RH
      end
    end

    first[#buf.lines + 1] = #rows + 1
    total = y + PAD_Y
    laid, laid_version = key, self.version
  end

  -- The row a place is on: the last of its line's rows that starts at or
  -- before it, so a place at a break is at the start of the row below.
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

  -- How far into its row a place is, in pixels.
  local function x_in_row(r, x)
    return measure(buf.lines[r.n]:sub(r.from, x - 1))
  end

  -- The place in row `r` nearest `px` pixels into it: past a character's
  -- middle is after it. The end of a row that wraps is before its last
  -- character, which is the space it broke at - after it is the next row.
  local function place_in_row(r, px)
    local line = buf.lines[r.n]
    local at, w = r.from, 0

    while at <= r.to do
      local nxt = docview.next_char(line, at)
      local cw = measure(line:sub(at, nxt - 1))

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

    local i = row_of(buf.cy, buf.cx)
    local r = rows[i]

    if not r then return end

    if r.y < self.scroll + PAD_Y // 2 then
      self.scroll = math.max(0, r.y - PAD_Y)
    elseif r.y + RH > self.scroll + self.h then
      self.scroll = r.y + RH - self.h + PAD_Y // 2
    end

    if not self.wrap then
      local _, width = geometry(self)
      local cx = x_in_row(r, buf.cx)

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

  -- Where the caret is on the page, in the view's own pixels, and a row's
  -- height - for the right click's menu, and for a harness.
  function v:caret_at()
    metrics()
    layout(self)

    local r = rows[row_of(buf.cy, buf.cx)]
    local x0 = geometry(self)

    if not r then return x0, PAD_Y, RH end

    return x0 + x_in_row(r, buf.cx) - self.hscroll, r.y - self.scroll, RH
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

  -- A run of a row's bytes, starting `px` into the column; the part left of
  -- the column's edge, when lines do not wrap and the page has scrolled
  -- sideways, is left out a character at a time.
  local function draw_run(g, line, from, to, x0, px, y, colour)
    if to < from then return end

    local x = x0 + px - v.hscroll

    while x < x0 and from <= to do
      local nxt = docview.next_char(line, from)

      x = x + measure(line:sub(from, nxt - 1))
      from = nxt
    end

    if from <= to then g:text(x, y, line:sub(from, to), colour, nil, F) end
  end

  -- A band behind bytes `a` to `b - 1` of row `r`, clipped to the column.
  local function band(g, r, a, b, x0, width, y, colour, eol)
    local x1 = x0 + x_in_row(r, a) - v.hscroll
    local x2 = x0 + x_in_row(r, b) - v.hscroll + (eol and GH // 3 or 0)

    if x1 < x0 then x1 = x0 end
    if x2 > x0 + width + GH then x2 = x0 + width + GH end
    if x2 > x1 then g:fill(x1, y, x2 - x1, RH, colour) end
  end

  function v:draw(g)
    metrics()
    layout(self)
    follow(self)
    clamp_scroll(self)

    local x0, width = geometry(self)
    local text_y = (RH - GH) // 2
    local sy1, sx1, sy2, sx2 = buf:selection()
    local selection = theme.mix(theme.sunken, theme.accent, 260)
    local hit = theme.mix(theme.sunken, HIT, 800)
    local hit_now = theme.mix(theme.sunken, HIT_NOW, 850)
    local found = self.needle and matches(self) or {}

    g:fill(0, 0, self.w, self.h, theme.sunken)

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

      local line = buf.lines[r.n]
      local row_end = r.to + 1

      if y + RH > 0 then
        if sy1 and r.n >= sy1 and r.n <= sy2 then
          local a = (r.n == sy1) and math.max(sx1, r.from) or r.from
          local b = (r.n == sy2) and math.min(sx2, row_end) or row_end
          local eol = (r.n < sy2) and last_of_line(i)

          if b > a or eol then band(g, r, a, math.max(a, b), x0, width, y, selection, eol) end
        end

        -- The matches over the selection, since the current one *is* the
        -- selection and says so in the stronger of the two yellows.
        for _, f in ipairs(by_line[r.n] or {}) do
          local a, b = math.max(f.m.from, r.from), math.min(f.m.to, row_end)

          if b > a then band(g, r, a, b, x0, width, y, f.now and hit_now or hit) end
        end

        draw_run(g, line, r.from, r.to, x0, 0, y + text_y, theme.text)
      end
    end

    -- The caret: a line between two characters, in the accent.
    if self.focused and not sy1 then
      local r = rows[row_of(buf.cy, buf.cx)]

      if r then
        local cx = x0 + x_in_row(r, buf.cx) - self.hscroll
        local cy = r.y - self.scroll + text_y

        if cx >= x0 - 1 and cy + GH > 0 and cy < self.h then
          g:fill(cx - 1, cy, 2, GH, theme.accent)
        end
      end
    end

    self.bar = ui.draw_scrollbar(g, self.w, self.h, total, self.h, self.scroll + 1)
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

  -- Up or down by `by` rows as they are seen, keeping to the same x.
  local function vertical(self, by, extend)
    local i = row_of(buf.cy, buf.cx)
    local r = rows[i]

    want = want or x_in_row(r, buf.cx)

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

  function v:key(c)
    metrics()
    layout(self)
    self.followed = nil

    local k, mods = keys.parts(c)
    local shift = (mods & SHIFT) ~= 0
    local ctrl = (mods & CTRL) ~= 0

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
      buf:newline(false)
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

  -- The place under a point of the view.
  local function place_at(self, x, y)
    local x0 = geometry(self)
    local py = y + self.scroll
    local lo, hi = 1, #rows

    while lo < hi do
      local mid = (lo + hi + 1) // 2

      if rows[mid].y <= py then lo = mid else hi = mid - 1 end
    end

    local r = rows[lo]

    if not r then return 1, 1 end
    if py < r.y then return r.n, r.from end

    return r.n, place_in_row(r, x - x0 + self.hscroll)
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

  return v
end

return docview
