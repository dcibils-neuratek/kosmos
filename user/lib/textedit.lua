-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Rich text edited: the caret, the selection, typing, deleting, formatting,
-- and undo - over a `richtext` body set by `pageset` (`docs/office.md` O1,
-- `docs/present.md`, *What moves out of Kosmos Write*).
--
--   local textedit = use("/Kosmos/Libraries/textedit.lua")
--   local ed = textedit.new{
--     doc = doc,                     -- holds .body and .comments; edits replace them
--     by_name = by_name,             -- the document's styles, by name
--     measure = measure,             -- faces.measure, for a line's ends and Up and Down
--     set = function() return set end,     -- the body as it is set now
--     body_style = writedoc.BODY,    -- a paragraph made after a picture
--     keep = function() return { ... } end,   -- what else an undo puts back
--     put_back = function(entry) ... end,     -- and how
--     on = { edited =, undone =, moved =, looked =, all = },
--   }
--
--   ed.caret, ed.anchor              where the caret is, and the selection's other end
--   ed.dirty                         an edit since the last save
--   ed:key(c)                        a key: true when it was the text's
--   ed:edit(kind)                    "selectall", "copy", "cut", "paste"
--   ed:type(text)  ed:line_break()  ed:delete(forward)
--   ed:format(fields)  ed:arrange(fields)  ed:restyle(name)
--   ed:undo()  ed:redo()
--   ed:move(place, extend, keep_column)
--   ed:press(place)  ed:drag(place)  a pointer's press, and its drag
--   ed:selected()  ed:range()  ed:look()
--   ed:edited(body, place, kind, keep, comments)   a new body, for undo
--   ed:step(kind, joins)             a step to undo, for an edit the window makes
--   ed:remove(n, place)              a block paragraph taken out
--   ed:cell_key(step)                Tab, Shift-Tab and Return in a table
--
-- **The window keeps what is not text**: what a page is, how far it is
-- scrolled, the pictures, the file. What happens after each change is the
-- window's, told through `on`:
--
--   on.edited()        the body changed: set it again, follow the caret, draw
--   on.undone()        the same, for an undo or a redo
--   on.moved(redraw)   the caret moved; `redraw` when the selection changed
--   on.looked()        a look chosen for what is typed next, nothing selected
--   on.all()           everything selected
--
-- **Each edit is a new body**, the paragraphs it did not touch the same
-- tables (`richtext`): an undo is the body before, kept with whatever
-- `keep` adds, so a run of typing is one step and anything else is one.
--
-- It was Kosmos Write's, inside `writer.lua`, until O1 (17 November 2026):
-- Mail's composer, Present's text boxes and Sheets' cells are its next
-- users, and none of them should type a second copy of it.

local richtext = use("/Kosmos/Libraries/richtext.lua")
local pageset  = use("/Kosmos/Libraries/pageset.lua")
local faces    = use("/Kosmos/Libraries/faces.lua")
local keys     = use("/Kosmos/Libraries/keys.lua")
local wmproto  = use("/Kosmos/Libraries/wmproto.lua")

local textedit = {}

local UNDO_MOST = 200

function textedit.new(o)
  local doc = o.doc
  local by_name = o.by_name
  local measure = o.measure
  local on = o.on or {}
  local most = o.most or UNDO_MOST

  local function nothing() end

  local function hook(name) return on[name] or nothing end

  local ed = {
    caret = { para = 1, at = 1 },
    anchor = nil,
    column = nil,                -- where Up and Down aim, across
    pending = {},                -- a look chosen with nothing selected, for typing
    dirty = false,
    last_kind = nil,
    undo_steps = {}, redo_steps = {},   -- what undo and redo put back, newest last
  }

  local utf8_pending = ""

  function ed:selected()
    local a, c = self.anchor, self.caret
    return a and (a.para ~= c.para or a.at ~= c.at or a.row ~= c.row or a.col ~= c.col)
  end

  -- The range a format applies to: the selection, or the caret's paragraph
  -- for a paragraph's fields.
  function ed:range()
    if self:selected() then return self.anchor, self.caret end
    return self.caret, self.caret
  end

  -- The look a panel shows: the selection's first character's, or what
  -- typing at the caret would be.
  function ed:look()
    local place = self.caret

    if self:selected() then
      local a = richtext.before(self.anchor, self.caret) and self.anchor or self.caret
      local p = doc.body[a.para]
      local plain = richtext.plain(a.row and p.table.rows[a.row][a.col] or p)
      place = { para = a.para, at = math.min(a.at + 1, #plain + 1), row = a.row, col = a.col }
    end

    local look, layout, style = richtext.look_at(doc.body, place, by_name)

    if not self:selected() then
      for k, v in pairs(self.pending) do look[k] = v end
    end

    return look, layout, style
  end

  -- What an undo puts back: the body and the caret, and what `keep` adds -
  -- each a table no edit changes in place.
  function ed:snapshot()
    local entry = o.keep and o.keep() or {}
    entry.body, entry.caret = doc.body, self.caret
    return entry
  end

  --
  -- **A step to undo**: the state before it kept - unless `joins` and this
  -- is the same kind as the step before, so a run of typing, or of a
  -- field's words, is one step.
  --
  function ed:step(kind, joins)
    if not joins or kind ~= self.last_kind then
      self.undo_steps[#self.undo_steps + 1] = self:snapshot()
      if #self.undo_steps > most then table.remove(self.undo_steps, 1) end
    end

    self.redo_steps = {}
    self.last_kind = kind
  end

  --
  -- **An edit**: the body before it kept for undo - one entry for a run of
  -- typing, one for anything else - the new one in its place, the caret
  -- moved. A format keeps the selection (`keep`), so a word made bold can
  -- be made italic next.
  --
  function ed:edited(body, place, kind, keep, comments)
    self:step(kind, kind == "type")
    doc.body = body

    if comments then doc.comments = comments end
    self.caret, self.column = place, nil

    if not keep then self.anchor = nil end

    self.dirty = true
    hook("edited")()
  end

  local function swap(self, from, to)
    local entry = table.remove(from)

    if not entry then return end

    to[#to + 1] = self:snapshot()
    doc.body, self.caret, self.anchor, self.column = entry.body, entry.caret, nil, nil

    if o.put_back then o.put_back(entry) end

    self.last_kind = nil
    self.dirty = true
    hook("undone")()
  end

  function ed:undo() swap(self, self.undo_steps, self.redo_steps) end
  function ed:redo() swap(self, self.redo_steps, self.undo_steps) end

  --
  -- **A selection stays in its cell**, as a text field's does; one begun
  -- outside a table that reaches into it takes the table whole, its end past
  -- the table on the side it went.
  --
  function ed:clamp(place)
    local anchor = self.anchor

    if not anchor then return place end

    if anchor.row then
      if richtext.same_cell(anchor, place) then return place end

      local cell = doc.body[anchor.para].table.rows[anchor.row][anchor.col]

      return { para = anchor.para, row = anchor.row, col = anchor.col,
               at = richtext.before(place, anchor) and 1 or #richtext.plain(cell) + 1 }
    end

    if place.row then
      local n = place.para

      if richtext.before(anchor, place) and doc.body[n + 1] then
        return richtext.enter(doc.body, n + 1, true)
      elseif n > 1 then
        return richtext.enter(doc.body, n - 1, false)
      end
    end

    return place
  end

  function ed:move(place, extend, keep_column)
    local had = self:selected()

    if extend then
      self.anchor = self.anchor or self.caret
      place = self:clamp(place)
    else
      self.anchor = nil
    end

    self.caret = place
    self.last_kind = nil
    self.pending = {}

    if not keep_column then self.column = nil end

    hook("moved")((had or self:selected()) and true or false)
  end

  -- A pointer pressed on `place`: the caret there, nothing selected. True
  -- when a selection was there before, which the pages showed.
  function ed:press(place)
    local had = self:selected()

    self.anchor, self.caret, self.column = place, place, nil
    self.last_kind = nil
    self.pending = {}

    return had and true or false
  end

  -- And dragged to `place`: the selection's far end, kept in its cell.
  function ed:drag(place)
    self.caret = self:clamp(place)
  end

  local function without_selection(self)
    if not self:selected() then return doc.body, self.caret end
    return richtext.delete(doc.body, self.anchor, self.caret)
  end

  -- A paragraph after the picture, shape or chart the caret is on, for
  -- what is typed there.
  local function after_block(body, place)
    local p = body[place.para]

    if p and (p.picture or p.shape or (p.table and p.table.chart and not place.row)) then
      local out = {}
      for i, q in ipairs(body) do out[i] = q end
      table.insert(out, place.para + 1, { style = o.body_style, runs = {} })
      return out, { para = place.para + 1, at = 1 }
    end

    return body, place
  end

  -- The block paragraph `n` - a picture, a shape, a chart - taken out.
  function ed:remove(n, place)
    local out = {}
    for i, p in ipairs(doc.body) do out[i] = p end
    table.remove(out, n)
    self:edited(out, place, "delete")
  end

  function ed:type(text)
    local body, place = without_selection(self)
    body, place = after_block(body, place)
    body, place = richtext.type(body, place, text, self.pending)
    self:edited(body, place, "type")
  end

  -- **A line break** (Shift-Return, and Return in a text box): a new line in
  -- the same paragraph.
  function ed:line_break()
    local body, place = without_selection(self)
    body, place = after_block(body, place)
    body, place = richtext.line_break(body, place, self.pending)
    self:edited(body, place, "type")
  end

  -- Backspace (`forward` false) and Delete.
  function ed:delete(forward)
    local caret = self.caret

    if self:selected() then
      local body, place = richtext.delete(doc.body, self.anchor, caret)
      self:edited(body, place, "delete")
      return
    end

    local here = doc.body[caret.para]

    if here.picture or here.shape or (here.table and here.table.chart and not caret.row) then
      self:remove(caret.para, { para = math.max(1, caret.para - (forward and 0 or 1)), at = 1 })
      return
    end

    local before = caret.para > 1 and doc.body[caret.para - 1]

    if not forward and caret.at == 1 and not caret.row and before
       and (before.picture or before.shape or (before.table and before.table.chart)) then
      self:remove(caret.para - 1, { para = caret.para - 1, at = 1 })
      return
    end

    local other = richtext.step(doc.body, caret, forward)

    if other.para == caret.para and other.at == caret.at and other.row == caret.row
       and other.col == caret.col then
      return
    end

    -- **A table's edges hold**: Backspace at a cell's start and Delete at
    -- its end do nothing, as in Pages; into a table from beside it, the
    -- caret goes in and nothing is taken.
    if caret.row and not (other.row and richtext.same_cell(caret, other)) then return end

    if other.row and not caret.row then
      self:move(other)
      return
    end

    local body, place = richtext.delete(doc.body, caret, other)
    self:edited(body, place, "delete")
  end

  -- Character fields over the selection, or for what is typed next.
  function ed:format(fields)
    if self:selected() then
      local a, b = self:range()
      self:edited(richtext.format(doc.body, a, b, fields, by_name), self.caret, "format", true)
    else
      for k, v in pairs(fields) do self.pending[k] = v end
      hook("looked")()
    end
  end

  -- Paragraph fields over the paragraphs the selection or the caret is in.
  function ed:arrange(fields)
    local a, b = self:range()
    self:edited(richtext.arrange(doc.body, a, b, fields, by_name), self.caret, "format", true)
  end

  function ed:restyle(style_name)
    local a, b = self:range()
    self:edited(richtext.restyle(doc.body, a, b, style_name, by_name), self.caret, "format", true)
  end

  function ed:all()
    self.anchor = richtext.enter(doc.body, 1, true)
    self.caret = richtext.enter(doc.body, #doc.body, false)
    hook("all")()
  end

  --
  -- **Tab, Shift-Tab and Return in a cell**: the next cell - a new row after
  -- the last cell, as Pages and Word make one - the cell before, and the
  -- cell below, out of the table under its last row.
  --
  function ed:cell_key(step)
    local caret = self.caret
    local n = caret.para
    local t = doc.body[n].table

    -- **A text box is text**: Return breaks its line and Tab is a tab.
    if t.box then
      if step == "down" then
        self:line_break()
      elseif step == 1 then
        self:type("\t")
      end

      return
    end

    if step == "down" then
      if caret.row < #t.rows then
        local below = t.rows[caret.row + 1][caret.col]
        self:move({ para = n, row = caret.row + 1, col = caret.col,
                    at = #richtext.plain(below) + 1 })
      elseif doc.body[n + 1] then
        self:move(richtext.enter(doc.body, n + 1, true))
      end

      return
    end

    local other = richtext.next_cell(doc.body, caret, step)

    if other then
      self:move(other)
    elseif step > 0 then
      local body = richtext.reshape(doc.body, n, { rows = #t.rows + 1 }, by_name)
      self:edited(body, { para = n, at = 1, row = #t.rows + 1, col = 1 }, "table")
    end
  end

  --
  -- **A key, if it is the text's**: the caret's keys, with Shift a
  -- selection; Backspace, Delete, Return and Tab; Control-A, B, U, Z and Y;
  -- and what is typed, a character of several bytes held until it is
  -- whole. False for a key the text does not take, which the window may.
  --
  function ed:key(c)
    local k, mods = keys.parts(c)
    local shift = (mods & keys.SHIFT) ~= 0
    local ctrl = (mods & keys.CTRL) ~= 0
    local caret = self.caret

    if k == keys.LEFT or k == keys.RIGHT then
      self:move(richtext.step(doc.body, caret, k == keys.RIGHT), shift)
    elseif k == keys.UP or k == keys.DOWN then
      local place
      place, self.column = pageset.vertical(o.set(), measure, caret, k == keys.UP and -1 or 1,
                                            self.column)
      self:move(place, shift, true)
    elseif k == keys.HOME or k == keys.END then
      if ctrl then
        self:move(k == keys.HOME and richtext.enter(doc.body, 1, true)
                  or richtext.enter(doc.body, #doc.body, false), shift)
      else
        local home, finish = pageset.line_ends(o.set(), measure, caret)
        self:move(k == keys.HOME and home or finish, shift)
      end
    elseif k == keys.DELETE and mods == 0 then
      self:delete(true)
    elseif k == 9 and caret.row and (mods == 0 or mods == keys.SHIFT) then
      self:cell_key(mods == keys.SHIFT and -1 or 1)
    elseif (k == 13 or k == 10) and mods == keys.SHIFT then
      self:line_break()
    elseif mods ~= 0 then
      return false
    elseif c == 8 or c == 127 then
      self:delete(false)
    elseif caret.row and (c == 13 or c == 10) then
      self:cell_key("down")
    elseif c == 13 or c == 10 then
      local p = doc.body[caret.para]

      -- **Return on an empty list item ends the list**, as Pages does: the
      -- way out of a list is the key that went on with it.
      if not self:selected() and p.list and p.list ~= "none" and richtext.plain(p) == "" then
        self:edited(richtext.arrange(doc.body, caret, caret, { list = "none" }, by_name),
                    caret, "return")
      else
        local body, place = without_selection(self)
        body, place = richtext.split(body, place, by_name)
        self:edited(body, place, "return")
      end
    elseif c == 9 then
      self:type("\t")
    elseif c == 1 then                                     -- Control-A
      self:all()
    elseif c == 2 then                                     -- Control-B
      local look = self:look()
      self:format({ weight = (faces.WEIGHT[look.weight] or 400) >= 600 and "Regular"
                             or "Bold" })
    elseif c == 21 then                                    -- Control-U
      self:format({ underline = not self:look().underline })
    elseif c == 26 then self:undo()                        -- Control-Z
    elseif c == 25 then self:redo()                        -- Control-Y
    elseif c >= 32 and c < 127 then
      utf8_pending = ""
      self:type(string.char(c))
    elseif c >= 128 and c < 256 then
      utf8_pending = utf8_pending .. string.char(c)

      if utf8.len(utf8_pending) then
        local text = utf8_pending
        utf8_pending = ""
        self:type(text)
      elseif #utf8_pending >= 4 then
        utf8_pending = ""
      end
    else
      return false
    end

    return true
  end

  -- **The system's edit keys**: everything, and copy, cut and paste through
  -- the window manager's clipboard.
  function ed:edit(kind)
    if kind == "selectall" then
      self:all()
      return true
    end

    if kind == "copy" or kind == "cut" then
      if not self:selected() then return false end
      if not wmproto.copy(richtext.text(doc.body, self.anchor, self.caret)) then return false end

      if kind == "cut" then
        local body, place = richtext.delete(doc.body, self.anchor, self.caret)
        self:edited(body, place, "cut")
      end

      return true
    end

    if kind == "paste" then
      local text = wmproto.paste()

      if not text or text == "" then return false end

      local body, place = without_selection(self)
      body, place = richtext.type(body, place, text)
      self:edited(body, place, "paste")
      return true
    end

    return false
  end

  return ed
end

return textedit
