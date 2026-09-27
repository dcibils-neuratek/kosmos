-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Text being edited: its lines, a caret, a selection, and every change
-- undoable.
--
-- `ui.editor` was an array of lines and a cursor with each edit written out
-- where its key was handled, "which is what a text editor is until it is a
-- good one". The IDE's editor (`roadmap.md` 6n, step 1) needs undo, and undo
-- is only simple if there is one way to change the text: **every edit is a
-- `replace`** - take out the bytes between two places, put a string in - and
-- a `replace` is exactly what undo records and plays back the other way. A
-- keystroke, a paste, a cut, an indent of forty lines: all of them are one
-- or more of these, and none of them has an undo of its own to get wrong.
--
-- **A selection is two carets, and the second one is the cursor**, as it
-- always was in `ui.editor`: `anchor` where it began, `cy`/`cx` where it
-- is. Moving with Shift keeps the anchor; moving without drops it.
--
-- Places are `(y, x)`: line and byte, both from 1, and `x` may be one past
-- the end of its line, which is where the caret sits after the last byte.
--
-- Pure: no `gfx`, no `sys`. `tools/test_textbuf.lua` runs it on the Mac,
-- and `ui.editor` is the view over one of these.

local textbuf = {}
textbuf.__index = textbuf

-- Two spaces, the indent every Lua file in this system is written with.
textbuf.INDENT = "  "

-- An undo step is closed after this many typed bytes, so undo takes a
-- sentence back in phrases rather than all at once or a letter at a time.
local GROUP_MOST = 32

local function split(text)
  local lines = {}

  for line in (tostring(text or "") .. "\n"):gmatch("([^\n]*)\n") do
    lines[#lines + 1] = line
  end

  -- A file's last newline ends its last line rather than starting another.
  if #lines > 1 and lines[#lines] == "" then lines[#lines] = nil end
  if #lines == 0 then lines[1] = "" end

  return lines
end

function textbuf.new(text)
  local b = setmetatable({}, textbuf)

  b:set(text)
  return b
end

-- A different text, with nothing to undo back into the one before.
function textbuf:set(text)
  self.lines = split(text)
  self.cy, self.cx = 1, 1
  self.anchor = nil
  self.want = nil           -- the column Up and Down aim for
  self.undos, self.redos = {}, {}
  self.clean = 0            -- the undo depth the text was saved at
  self.dirty = false
  self.changed_from = 1     -- the first line an edit touched, for a colourer
  self.open = nil           -- the undo step typing is still adding to
end

function textbuf:content()
  return table.concat(self.lines, "\n") .. "\n"
end

-- Saved: this is the text as it is on disk, and undoing back to here makes
-- it so again.
function textbuf:saved()
  self.clean = #self.undos
  self.dirty = false
  self.open = nil
end

--------------------------------------------------------------------------
-- Places.
--------------------------------------------------------------------------

-- A place, kept inside the text.
function textbuf:clamp(y, x)
  if y < 1 then y, x = 1, 1 end
  if y > #self.lines then y, x = #self.lines, #self.lines[#self.lines] + 1 end
  if x < 1 then x = 1 end
  if x > #self.lines[y] + 1 then x = #self.lines[y] + 1 end

  return y, x
end

local function before(y1, x1, y2, x2)
  return y1 < y2 or (y1 == y2 and x1 < x2)
end

-- The selection in reading order, or nil when there is none. An anchor on
-- the cursor is not a selection: that is what a plain click leaves behind.
function textbuf:selection()
  local a = self.anchor

  if not a then return nil end

  local y1, x1, y2, x2 = a[1], a[2], self.cy, self.cx

  if before(y2, x2, y1, x1) then y1, x1, y2, x2 = y2, x2, y1, x1 end
  if y1 == y2 and x1 == x2 then return nil end

  return y1, x1, y2, x2
end

-- The text between two places, joined with "\n" and never ending in one.
function textbuf:between(y1, x1, y2, x2)
  if y1 == y2 then return self.lines[y1]:sub(x1, x2 - 1) end

  local out = { self.lines[y1]:sub(x1) }

  for n = y1 + 1, y2 - 1 do out[#out + 1] = self.lines[n] end

  out[#out + 1] = self.lines[y2]:sub(1, x2 - 1)
  return table.concat(out, "\n")
end

function textbuf:selected()
  local y1, x1, y2, x2 = self:selection()

  return y1 and self:between(y1, x1, y2, x2) or nil
end

-- Where you land `text` after `(y, x)`.
local function after(y, x, text)
  local lines, last = 0, nil

  for part in (text .. "\n"):gmatch("([^\n]*)\n") do
    lines = lines + 1
    last = part
  end

  if lines == 1 then return y, x + #last end

  return y + lines - 1, #last + 1
end

--------------------------------------------------------------------------
-- The one edit.
--------------------------------------------------------------------------

-- The bytes between two places out, `text` in, with nothing recorded.
-- Returns what came out and where the insertion ends.
local function splice(self, y1, x1, y2, x2, text)
  local removed = self:between(y1, x1, y2, x2)
  local head = self.lines[y1]:sub(1, x1 - 1)
  local tail = self.lines[y2]:sub(x2)

  for _ = y1 + 1, y2 do table.remove(self.lines, y1 + 1) end

  local parts = split(text .. "\n")

  -- `split` drops a final empty line, which here is real: "a\n" is two
  -- parts, "a" and "". So split "text\n" and the parts are exactly text's.
  if text == "" then parts = { "" } end

  self.lines[y1] = head .. parts[1]

  for i = 2, #parts do table.insert(self.lines, y1 + i - 1, parts[i]) end

  local ey, ex = after(y1, x1, text)

  self.lines[ey] = self.lines[ey] .. tail

  if y1 < self.changed_from then self.changed_from = y1 end

  return removed, ey, ex
end

--
-- **Replace the bytes between two places with `text`**, leave the caret
-- after it, and record it. `kind` says what sort of edit it was, so typing
-- can gather into one undo step: "type" for a character typed, "erase" for
-- one taken back, anything else - or nil - for an edit that is a step of
-- its own.
--
function textbuf:replace(y1, x1, y2, x2, text, kind)
  y1, x1 = self:clamp(y1, x1)
  y2, x2 = self:clamp(y2, x2)
  if before(y2, x2, y1, x1) then y1, x1, y2, x2 = y2, x2, y1, x1 end

  text = tostring(text or "")

  local was = { self.cy, self.cx, self.anchor }
  local removed, ey, ex = splice(self, y1, x1, y2, x2, text)
  local record = { y = y1, x = x1, removed = removed, inserted = text,
                   before = was }

  self.cy, self.cx, self.anchor, self.want = ey, ex, nil, nil
  record.after = { ey, ex }

  --
  -- Typing gathers: a character typed right where the last one ended, into
  -- the same step, until a space or a line ends a word - so undo takes back
  -- a word at a time, as every editor does. Erasing gathers the same way.
  --
  local open = self.open

  if open and kind and open.kind == kind and #open.records < GROUP_MOST
     and #removed <= 1 and #text <= 1 then
    local last = open.records[#open.records]
    local joins = (kind == "type" and last.after[1] == y1 and last.after[2] == x1)
                  or (kind == "erase" and ((y1 == last.y and x2 == last.x)
                                           or (y1 == last.y and x1 == last.x)))

    if joins and not (kind == "type" and last.inserted:match("[%s]")) then
      open.records[#open.records + 1] = record
      self.dirty = true
      self.redos = {}
      return removed
    end
  end

  local step = { kind = kind, records = { record } }

  self.undos[#self.undos + 1] = step
  self.redos = {}
  self.open = kind and step or nil
  self.dirty = (#self.undos ~= self.clean)

  return removed
end

--
-- Several edits as one undo step - an indent of forty lines is one thing to
-- take back. Nested groups are one step with the outermost.
--
function textbuf:group(fn)
  local base = #self.undos
  local keep = self.grouping

  self.grouping = true
  self.open = nil
  fn()
  self.grouping = keep

  if keep or #self.undos <= base + 1 then return end

  local step = { records = {} }

  for i = base + 1, #self.undos do
    for _, r in ipairs(self.undos[i].records) do
      step.records[#step.records + 1] = r
    end

    self.undos[i] = nil
  end

  self.undos[base + 1] = step
  self.open = nil
  self.dirty = (#self.undos ~= self.clean)
end

function textbuf:undo()
  local step = table.remove(self.undos)

  if not step then return false end

  for i = #step.records, 1, -1 do
    local r = step.records[i]
    local ey, ex = after(r.y, r.x, r.inserted)

    splice(self, r.y, r.x, ey, ex, r.removed)
  end

  local first = step.records[1].before

  self.cy, self.cx, self.anchor = first[1], first[2], first[3]
  self.want = nil
  self.redos[#self.redos + 1] = step
  self.open = nil
  self.dirty = (#self.undos ~= self.clean)
  return true
end

function textbuf:redo()
  local step = table.remove(self.redos)

  if not step then return false end

  for _, r in ipairs(step.records) do
    local ey, ex = after(r.y, r.x, r.removed)

    splice(self, r.y, r.x, ey, ex, r.inserted)
  end

  local last = step.records[#step.records].after

  self.cy, self.cx, self.anchor = last[1], last[2], nil
  self.want = nil
  self.undos[#self.undos + 1] = step
  self.open = nil
  self.dirty = (#self.undos ~= self.clean)
  return true
end

--------------------------------------------------------------------------
-- What the keys do, each in terms of `replace`.
--------------------------------------------------------------------------

-- Put text in at the caret, over the selection if there is one.
function textbuf:insert(text, kind)
  local y1, x1, y2, x2 = self:selection()

  if y1 then
    return self:replace(y1, x1, y2, x2, text)
  end

  return self:replace(self.cy, self.cx, self.cy, self.cx, text, kind)
end

function textbuf:delete_selected()
  local y1, x1, y2, x2 = self:selection()

  if not y1 then return false end

  self:replace(y1, x1, y2, x2, "")
  return true
end

-- Backspace: the selection, or the byte before the caret - joining onto the
-- line above at the start of one.
function textbuf:backspace()
  if self:delete_selected() then return true end

  if self.cx > 1 then
    self:replace(self.cy, self.cx - 1, self.cy, self.cx, "", "erase")
  elseif self.cy > 1 then
    local above = #self.lines[self.cy - 1] + 1

    self:replace(self.cy - 1, above, self.cy, 1, "", "erase")
  else
    return false
  end

  return true
end

-- Delete: the selection, or the byte after the caret.
function textbuf:delete_forward()
  if self:delete_selected() then return true end

  if self.cx <= #self.lines[self.cy] then
    self:replace(self.cy, self.cx, self.cy, self.cx + 1, "", "erase")
  elseif self.cy < #self.lines then
    self:replace(self.cy, self.cx, self.cy + 1, 1, "", "erase")
  else
    return false
  end

  return true
end

-- Enter, keeping the line's indent when `indent` is asked for - which is
-- what a code editor wants and a letter does not.
function textbuf:newline(indent)
  local lead = indent and self.lines[self.cy]:match("^[ \t]*") or ""
  local y1 = self:selection()

  -- The indent of the line the selection starts on, if there is one.
  if y1 then lead = indent and self.lines[y1]:match("^[ \t]*") or "" end

  self:insert("\n" .. lead)
end

--
-- **Text put on the end, and nothing recorded**: an Output panel's, which a
-- program writes to and nobody edits, and which would otherwise keep every
-- line it was ever sent twice - once in the text and once in the undo.
--
function textbuf:append(text)
  local y = #self.lines

  splice(self, y, #self.lines[y] + 1, y, #self.lines[y] + 1, tostring(text or ""))
end

function textbuf:select_all()
  self.anchor = { 1, 1 }
  self.cy = #self.lines
  self.cx = #self.lines[self.cy] + 1
end

--------------------------------------------------------------------------
-- Moving. Each takes `extend`: with it, the selection grows from where it
-- began; without it, there is no selection afterwards.
--------------------------------------------------------------------------

local function moving(self, extend)
  if extend then
    self.anchor = self.anchor or { self.cy, self.cx }
  else
    self.anchor = nil
  end

  self.open = nil
end

-- A selection collapsed by a plain Left or Right lands at its own start or
-- end, as it does everywhere - not one byte past it.
local function collapse(self, to_start)
  local y1, x1, y2, x2 = self:selection()

  if not y1 then return false end

  self.anchor = nil
  self.cy, self.cx = to_start and y1 or y2, to_start and x1 or x2
  return true
end

function textbuf:left(extend)
  if not extend and collapse(self, true) then self.want = nil return end

  moving(self, extend)

  if self.cx > 1 then
    self.cx = self.cx - 1
  elseif self.cy > 1 then
    self.cy = self.cy - 1
    self.cx = #self.lines[self.cy] + 1
  end

  self.want = nil
end

function textbuf:right(extend)
  if not extend and collapse(self, false) then self.want = nil return end

  moving(self, extend)

  if self.cx <= #self.lines[self.cy] then
    self.cx = self.cx + 1
  elseif self.cy < #self.lines then
    self.cy, self.cx = self.cy + 1, 1
  end

  self.want = nil
end

-- Up and down by `n` lines, aiming for the column the caret had when the
-- run of vertical moves began, so a short line on the way does not pull it
-- to the left for good.
function textbuf:vertical(n, extend)
  moving(self, extend)

  self.want = self.want or self.cx
  self.cy = math.max(1, math.min(#self.lines, self.cy + n))
  self.cx = math.min(self.want, #self.lines[self.cy] + 1)
end

function textbuf:up(extend) self:vertical(-1, extend) end
function textbuf:down(extend) self:vertical(1, extend) end

-- Home goes to the first thing on the line that is not space, and from
-- there to the line's very start - the way every code editor does it,
-- since in indented code the first is almost always where you meant.
function textbuf:home(extend)
  moving(self, extend)

  local first = (self.lines[self.cy]:find("[^ \t]") or 1)

  self.cx = (self.cx == first) and 1 or first
  self.want = nil
end

function textbuf:line_end(extend)
  moving(self, extend)
  self.cx = #self.lines[self.cy] + 1
  self.want = nil
end

function textbuf:text_start(extend)
  moving(self, extend)
  self.cy, self.cx, self.want = 1, 1, nil
end

function textbuf:text_end(extend)
  moving(self, extend)
  self.cy = #self.lines
  self.cx, self.want = #self.lines[self.cy] + 1, nil
end

-- A word is letters, digits and `_`; a run of anything else that is not
-- space counts as a word too, so a jump stops at `..` and at `(`.
local function class(ch)
  if ch == "" then return nil end
  if ch:match("[%w_]") then return "w" end
  if ch:match("%s") then return "s" end
  return "p"
end

-- To the start of this word or the one before it, across a line break.
function textbuf:word_left(extend)
  if not extend and collapse(self, true) then return end

  moving(self, extend)

  if self.cx == 1 then
    if self.cy > 1 then
      self.cy = self.cy - 1
      self.cx = #self.lines[self.cy] + 1
    end
    return
  end

  local line = self.lines[self.cy]
  local x = self.cx - 1

  while x > 1 and class(line:sub(x, x)) == "s" do x = x - 1 end

  local kind = class(line:sub(x, x))

  while x > 1 and class(line:sub(x - 1, x - 1)) == kind do x = x - 1 end

  self.cx, self.want = x, nil
end

-- To the end of this word or the next, across a line break.
function textbuf:word_right(extend)
  if not extend and collapse(self, false) then return end

  moving(self, extend)

  local line = self.lines[self.cy]

  if self.cx > #line then
    if self.cy < #self.lines then self.cy, self.cx = self.cy + 1, 1 end
    return
  end

  local x = self.cx

  while x <= #line and class(line:sub(x, x)) == "s" do x = x + 1 end

  local kind = class(line:sub(x, x))

  while x <= #line and class(line:sub(x, x)) == kind do x = x + 1 end

  self.cx, self.want = x, nil
end

-- A place, from a click; `extend` keeps the anchor, as a Shift-click does.
function textbuf:place(y, x, extend)
  moving(self, extend)
  self.cy, self.cx = self:clamp(y, x)
  self.want = nil
end

--------------------------------------------------------------------------
-- Lines at a time: indent, outdent, comment. Each is one undo step.
--------------------------------------------------------------------------

-- The lines a line-wise command works on: the selection's, not counting a
-- last line the selection only reaches the start of - selecting three whole
-- lines by dragging down leaves the caret at the start of the fourth.
function textbuf:selected_lines()
  local y1, x1, y2, x2 = self:selection()

  if not y1 then return self.cy, self.cy end
  if y2 > y1 and x2 == 1 then y2 = y2 - 1 end

  return y1, y2, x1
end

-- Something done to each of the lines, with the selection kept on them.
local function each_line(self, fn)
  local y1, y2 = self:selected_lines()
  local had = self:selection() ~= nil
  local cy, cx, anchor = self.cy, self.cx, self.anchor

  self:group(function()
    for y = y1, y2 do fn(y) end
  end)

  -- The caret and the anchor where they were, moved by what their own line
  -- gained or lost, so the same lines stay selected and a second Tab
  -- indents them again.
  -- A place at a line's very start stays there, so lines selected whole
  -- stay selected whole.
  local function kept(y, x, delta)
    if x == 1 then return y, 1 end

    return y, math.max(1, math.min(x + (delta[y] or 0), #self.lines[y] + 1))
  end

  local delta = self.shift or {}

  self.cy, self.cx = kept(cy, cx, delta)

  if had and anchor then
    self.anchor = { kept(anchor[1], anchor[2], delta) }
  else
    self.anchor = nil
  end

  self.shift = nil
end

-- A line's indent one step deeper, or its first step off.
function textbuf:indent()
  self.shift = {}

  each_line(self, function(y)
    if self.lines[y] == "" then return end

    self:replace(y, 1, y, 1, textbuf.INDENT)
    self.shift[y] = #textbuf.INDENT
  end)
end

function textbuf:outdent()
  self.shift = {}

  each_line(self, function(y)
    local take = #(self.lines[y]:match("^\t") or self.lines[y]:match("^  ")
                   or self.lines[y]:match("^ ") or "")

    if take > 0 then
      self:replace(y, 1, y, take + 1, "")
      self.shift[y] = -take
    end
  end)
end

-- Tab: a selection across lines is indented, and otherwise the caret moves
-- to the next indent stop with spaces.
function textbuf:tab()
  local y1, _, y2 = self:selection()

  if y1 and y2 > y1 then return self:indent() end

  local w = #textbuf.INDENT
  local spaces = w - (self.cx - 1) % w

  self:insert((" "):rep(spaces))
end

--
-- **Comment or uncomment the lines**, Ctrl+/ in the drawing: if every line
-- that has something on it already starts with `--`, those are taken off;
-- otherwise `-- ` goes on every such line, at the indent of the least
-- indented, so a block reads as one block with its comment marks in a
-- column.
--
function textbuf:toggle_comment()
  local y1, y2 = self:selected_lines()
  local all, least = true, nil

  for y = y1, y2 do
    local line = self.lines[y]

    if line:find("%S") then
      local lead = #line:match("^[ \t]*")

      least = least and math.min(least, lead) or lead
      if not line:find("^[ \t]*%-%-") then all = false end
    end
  end

  if not least then return end

  self.shift = {}

  each_line(self, function(y)
    local line = self.lines[y]

    if not line:find("%S") then return end

    if all then
      local _, e = line:find("^[ \t]*%-%- ?")
      local lead = #line:match("^[ \t]*")

      self:replace(y, lead + 1, y, e + 1, "")
      self.shift[y] = -(e - lead)
    else
      self:replace(y, least + 1, y, least + 1, "-- ")
      self.shift[y] = 3
    end
  end)
end

return textbuf
