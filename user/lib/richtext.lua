-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Text in paragraphs, runs and styles (`docs/write.md`, W1).
--
--   local richtext = use("/Kosmos/Libraries/richtext.lua")
--   local styles, by_name = richtext.styles(list, defaults)
--   local p = richtext.paragraph(raw, by_name, "Body")
--
-- **A kit, not Write's**: Kosmos Write's body is its first user, Present's
-- text boxes and Sheets' cells the next (Diego, 4 October 2026: "We will
-- reuse most of this technology"). What is here is the shape of styled text
-- and nothing about a page.
--
-- **A style is whole.** Every field is present, so a paragraph's look is its
-- style and its own changes, never a chain of parents to follow. **A
-- paragraph** is a style's name, the paragraph fields that differ from that
-- style, and runs. **A run** is text and the character fields that differ
-- from its paragraph's style. `"\n"` in a run is a line broken inside the
-- paragraph; nothing else below a space is kept but a tab.
--
-- **Everything here is somebody else's until it is checked**, because a
-- document is a file and a file came from anywhere. Each function takes a
-- table as it arrived and gives back a new one with the declared fields of
-- the declared types, each number held to its range, and nothing else - so
-- the editor never meets a field it did not declare. And the result is the
-- one way of writing that text: a field equal to the style's is left out,
-- runs that look alike are joined, an empty run is dropped. **Checking a
-- checked paragraph changes nothing**, which is what makes a document saved
-- and opened again equal to itself.
--
-- Units are in the names (`size_pt`, `indent_left_mm`): a document holds
-- millimetres and points side by side, and a number that does not say which
-- it is will one day be read as the other.

local richtext = {}

richtext.WEIGHTS = { "Light", "Regular", "Medium", "SemiBold", "Bold" }
richtext.ALIGNS = { "left", "center", "right", "justify" }
richtext.LISTS = { "none", "bullet", "number" }

local function set_of(list)
  local s = {}
  for _, v in ipairs(list) do s[v] = true end
  return s
end

local WEIGHT, ALIGN, LIST = set_of(richtext.WEIGHTS), set_of(richtext.ALIGNS),
                            set_of(richtext.LISTS)

--------------------------------------------------------------------------
-- The fields, and what each may hold.
--------------------------------------------------------------------------

-- A name a person reads: a face, a style. Printable, and short enough for a
-- menu.
local function name(v)
  if type(v) ~= "string" or #v == 0 or #v > 64 or v:find("%c") then
    return nil
  end
  return v
end

local function enum(set)
  return function(v)
    if type(v) == "string" and set[v] then return v end
  end
end

local function boolean(v)
  if type(v) == "boolean" then return v end
end

-- A number in [lo, hi], held to it: a size of 5000 points is a large size,
-- not a broken file. Not-a-number and the infinities are not numbers here.
local function number(lo, hi, integer)
  return function(v)
    if type(v) ~= "number" or v ~= v or v == math.huge or v == -math.huge then
      return nil
    end

    if integer then v = math.floor(v) end

    return math.max(lo, math.min(hi, v))
  end
end

-- A colour as a person reads it in the file, `#rrggbb`, kept in lower case
-- so the same colour is the same text.
local function colour(v)
  if type(v) == "string" and v:find("^#%x%x%x%x%x%x$") then
    return v:lower()
  end
end

--
-- **What a character may have**: the Format panel's Style tab, a face and
-- its weight and size, bold being a weight, italic, underline, strike and a
-- colour.
--
richtext.CHAR = {
  face = name, weight = enum(WEIGHT), italic = boolean,
  size_pt = number(1, 1000), colour = colour,
  underline = boolean, strike = boolean,
}

--
-- **What a paragraph may have**: its Layout tab - alignment, line spacing in
-- lines, the space before and after it, its three indents, a drop cap so
-- many lines deep, and a list - and its More tab's *keep with the next
-- paragraph*, which is what stops a heading standing alone at the foot of a
-- page, and *a page break before*, which Add Page makes.
--
richtext.PARA = {
  align = enum(ALIGN), spacing_lines = number(0.5, 5),
  before_pt = number(0, 500), after_pt = number(0, 500),
  indent_first_mm = number(-200, 200), indent_left_mm = number(0, 200),
  indent_right_mm = number(0, 200), drop_cap_lines = number(0, 10, true),
  list = enum(LIST), keep_with_next = boolean, page_break_before = boolean,
}

-- Both sets, in one order, so what is written and compared is always the
-- same fields in the same order.
local CHAR_KEYS, PARA_KEYS = {}, {}

for k in pairs(richtext.CHAR) do CHAR_KEYS[#CHAR_KEYS + 1] = k end
for k in pairs(richtext.PARA) do PARA_KEYS[#PARA_KEYS + 1] = k end

table.sort(CHAR_KEYS)
table.sort(PARA_KEYS)

richtext.CHAR_KEYS, richtext.PARA_KEYS = CHAR_KEYS, PARA_KEYS

--
-- A style with nothing chosen: what a field of a style falls back to when
-- the file did not say, so a style is whole even when its file is not.
--
richtext.PLAIN = {
  face = "IBM Plex Sans", weight = "Regular", italic = false, size_pt = 11,
  colour = "#000000", underline = false, strike = false,
  align = "left", spacing_lines = 1, before_pt = 0, after_pt = 0,
  indent_first_mm = 0, indent_left_mm = 0, indent_right_mm = 0,
  drop_cap_lines = 0, list = "none", keep_with_next = false,
  page_break_before = false,
}

--------------------------------------------------------------------------
-- Styles.
--------------------------------------------------------------------------

--
-- One style, whole: each field from `t` where it is one, and from `base`
-- where it is not. Nil when `t` has no name it could be known by.
--
function richtext.style(t, base)
  if type(t) ~= "table" then return nil end

  base = base or richtext.PLAIN

  local out = { name = name(t.name) }

  if not out.name then return nil end

  for _, k in ipairs(CHAR_KEYS) do
    local v = richtext.CHAR[k](t[k])
    if v == nil then v = base[k] end
    out[k] = v
  end

  for _, k in ipairs(PARA_KEYS) do
    local v = richtext.PARA[k](t[k])
    if v == nil then v = base[k] end
    out[k] = v
  end

  -- The style Return gives the next paragraph: its own name when it is
  -- the same one, written so rather than left out, since a field left out
  -- would be filled from the defaults the next time it is read.
  out.next = name(t.next) or base.next or out.name

  return out
end

--
-- A document's styles, as a list in the order a person sees them, and by
-- name. A style missing a field takes the default style of its name, or
-- the plain one; a name seen twice keeps the first; a `next` that names no
-- style becomes the style's own name. **No styles at all
-- is the defaults**, since a paragraph has to wear something.
--
function richtext.styles(list, defaults)
  -- Each default whole first, since a default may say only what differs
  -- from the plain style.
  local base = {}

  for _, d in ipairs(defaults or {}) do base[d.name] = richtext.style(d) end

  local out, by_name = {}, {}

  for _, raw in ipairs(type(list) == "table" and list or {}) do
    local s = type(raw) == "table"
              and richtext.style(raw, base[raw.name] or richtext.PLAIN)

    if s and not by_name[s.name] then
      out[#out + 1] = s
      by_name[s.name] = s
    end
  end

  if #out == 0 then
    for _, d in ipairs(defaults or {}) do
      local s = richtext.style(d)
      out[#out + 1] = s
      by_name[s.name] = s
    end
  end

  for _, s in ipairs(out) do
    if not by_name[s.next] then s.next = s.name end
  end

  return out, by_name
end

--------------------------------------------------------------------------
-- Paragraphs and runs.
--------------------------------------------------------------------------

-- Text as it may stand in a run: bytes below a space are dropped but a line
-- break and a tab. UTF-8 is not judged here - a face that has no glyph for
-- something draws its missing glyph, which is the reader's problem and not
-- the file's.
local function text_of(v)
  if type(v) ~= "string" then return nil end
  return (v:gsub("[%z\1-\8\11-\31\127]", ""))
end

-- Whether two runs look the same: every character field equal.
local function alike(a, b)
  for _, k in ipairs(CHAR_KEYS) do
    if a[k] ~= b[k] then return false end
  end
  return true
end

richtext.alike = alike

--
-- One run against its paragraph's style: the text, and the character
-- fields that are valid and differ from the style's. Nil when it holds no
-- text.
--
function richtext.run(t, style)
  if type(t) ~= "table" then return nil end

  local text = text_of(t.text)

  if not text or text == "" then return nil end

  local out = { text = text }

  for _, k in ipairs(CHAR_KEYS) do
    local v = richtext.CHAR[k](t[k])

    if v ~= nil and v ~= style[k] then out[k] = v end
  end

  return out
end

--
-- **A picture** a paragraph is (W5): a file inside the document's own -
-- `pictures/` and a plain name, as `docfile` keeps them - and the size it
-- is shown at, in millimetres. Nil when it is not one.
--
local function picture_of(t)
  if type(t) ~= "table" then return nil end

  local name = t.name

  if type(name) ~= "string" or #name > 128 or not name:find("^pictures/[%w][%w_%-%.]*$")
     or name:find("%.%.") then
    return nil
  end

  local size = number(1, 1000)
  local w, h = size(t.width_mm), size(t.height_mm)

  if not w or not h then return nil end

  return { name = name, width_mm = w, height_mm = h }
end

richtext.picture_of = picture_of

--
-- **A shape** a paragraph is (W7b): one of Pages' plain shapes, its size in
-- millimetres and the colour it is filled with. Nil when it is not one.
--
richtext.SHAPES = { "rectangle", "rounded", "oval", "triangle", "star", "arrow" }

local SHAPE = set_of(richtext.SHAPES)

local function shape_of(t)
  if type(t) ~= "table" or not SHAPE[t.kind] then return nil end

  local size = number(1, 1000)
  local w, h = size(t.width_mm), size(t.height_mm)

  if not w or not h then return nil end

  return { kind = t.kind, width_mm = w, height_mm = h, fill = colour(t.fill) or "#2a55c9" }
end

richtext.shape_of = shape_of

--
-- **A table** a paragraph is (W5b): rows of cells, each cell a paragraph of
-- its own - its style, its own fields and its runs, never a picture or a
-- table - every row as many cells as the table has columns, and whether
-- its first row is a header. Nil when it is not one.
--
-- **The header row is a rule of the table, said by its name**: drawn over a
-- tint, its text bold, and repeated at the head of each page the table runs
-- on to, as Pages' and Word's are.
--
richtext.TABLE_ROWS, richtext.TABLE_COLUMNS = 1000, 20

local function cell_of(t, by_name, style_name)
  if type(t) ~= "table" then t = {} end

  local raw = { style = t.style, runs = t.runs }
  for _, k in ipairs(PARA_KEYS) do raw[k] = t[k] end

  return richtext.paragraph(raw, by_name, style_name)
end

--
-- **A text box** (W7a) is a table of one cell drawn as a box: `box` says
-- how wide it is, whether it has a border and what it is filled with -
-- nothing, when it has no `fill`. Placed as its paragraph aligns.
--
local function box_of(t)
  if type(t) ~= "table" then return nil end

  local border = boolean(t.border)

  return { width_mm = number(10, 1000)(t.width_mm) or 80,
           border = border == nil and true or border, fill = colour(t.fill) }
end

--
-- **A chart** (W7c) is a table whose numbers are drawn: `chart` says as
-- what - columns, bars, lines or a pie - and how tall. Its first row names
-- the series and is always its header; its first column names the
-- categories; the cells between are the numbers.
--
richtext.CHARTS = { "column", "bar", "line", "pie" }

local CHART = set_of(richtext.CHARTS)

local function chart_of(t)
  if type(t) ~= "table" or not CHART[t.kind] then return nil end

  return { kind = t.kind, height_mm = number(20, 250)(t.height_mm) or 70 }
end

local function table_of(t, by_name, style_name)
  if type(t) ~= "table" or type(t.rows) ~= "table" then return nil end

  local columns = number(1, richtext.TABLE_COLUMNS, true)(t.columns)

  if not columns then return nil end

  local box = box_of(t.box)
  local chart = not box and chart_of(t.chart) or nil
  local most = richtext.TABLE_ROWS

  if box then columns, most = 1, 1 end

  local out = { columns = columns, header = chart ~= nil or (not box and boolean(t.header) or false),
                rows = {}, box = box, chart = chart }

  for _, raw in ipairs(t.rows) do
    if #out.rows >= most then break end

    if type(raw) == "table" then
      local row = {}
      for c = 1, columns do row[c] = cell_of(raw[c], by_name, style_name) end
      out.rows[#out.rows + 1] = row
    end
  end

  if #out.rows == 0 then return nil end

  return out
end

--
-- One paragraph, checked: its style's name - the one it gave if that is a
-- style, and `fallback` if not - its own paragraph fields where they differ
-- from that style, and its runs, joined where they look alike. A picture's
-- paragraph holds the picture and no text, and a table's the table.
--
function richtext.paragraph(t, by_name, fallback)
  if type(t) ~= "table" then t = {} end

  local style_name = by_name[t.style] and t.style or fallback
  local style = by_name[style_name] or richtext.PLAIN
  local out = { style = style_name, runs = {} }

  for _, k in ipairs(PARA_KEYS) do
    local v = richtext.PARA[k](t[k])

    if v ~= nil and v ~= style[k] then out[k] = v end
  end

  out.picture = picture_of(t.picture)

  if out.picture then return out end

  out.shape = shape_of(t.shape)

  if out.shape then return out end

  out.table = table_of(t.table, by_name, style_name)

  if out.table then return out end

  local runs = out.runs

  for _, raw in ipairs(type(t.runs) == "table" and t.runs or {}) do
    local r = richtext.run(raw, style)

    if r then
      local last = runs[#runs]

      if last and alike(last, r) then
        last.text = last.text .. r.text
      else
        runs[#runs + 1] = r
      end
    end
  end

  return out
end

-- A paragraph's words, without their looks.
function richtext.plain(p)
  local parts = {}
  for i, r in ipairs(p.runs or {}) do parts[i] = r.text end
  return table.concat(parts)
end

--
-- What a run looks like, whole: its style's character fields with its own
-- on top. What a page is set from, and what the Format panel shows.
--
function richtext.look(run, style)
  local out = {}

  for _, k in ipairs(CHAR_KEYS) do
    local v = run[k]
    if v == nil then v = style[k] end
    out[k] = v
  end

  return out
end

-- And a paragraph's: its style's paragraph fields with its own on top.
function richtext.layout(p, style)
  local out = {}

  for _, k in ipairs(PARA_KEYS) do
    local v = p[k]
    if v == nil then v = style[k] end
    out[k] = v
  end

  return out
end

--------------------------------------------------------------------------
-- **Editing** (`docs/write.md` W4b): text typed, taken out, a paragraph
-- broken in two and two made one - over a body, a list of paragraphs, as
-- Write's document, a slide's text box and a spreadsheet's cell all are.
--
-- **A place is a paragraph and a byte**: `{ para = n, at = i }`, the caret
-- before the `i`th byte of paragraph `n`'s text - one past its end at the
-- end. Always at the start of a UTF-8 character.
--
-- **Nothing is changed; each edit returns a new body.** The paragraphs it
-- did not touch are the same tables in the new body as in the old, so an
-- undo is the old body kept, and a page that set them once need not set
-- them again (`pageset`'s cache is keyed by the paragraph).
--------------------------------------------------------------------------

-- A run's character fields, without its text.
local function fields_of(run)
  local out = {}
  for _, k in ipairs(CHAR_KEYS) do out[k] = run[k] end
  return out
end

-- A paragraph like `p` - its style and its own fields - with `runs`, those
-- made tidy: empty ones dropped and neighbours that look alike joined.
local function with_runs(p, runs)
  local out = {}

  for k, v in pairs(p) do
    if k ~= "runs" then out[k] = v end
  end

  local tidy = {}

  for _, r in ipairs(runs) do
    if r.text ~= "" then
      local last = tidy[#tidy]

      if last and alike(last, r) then
        last.text = last.text .. r.text
      else
        local copy = fields_of(r)
        copy.text = r.text
        tidy[#tidy + 1] = copy
      end
    end
  end

  out.runs = tidy
  return out
end

-- The runs of `p` from byte `from` up to (not including) byte `to`.
local function slice(p, from, to)
  local out, start = {}, 1

  for _, r in ipairs(p.runs) do
    local stop = start + #r.text
    local a, b = math.max(from, start), math.min(to, stop)

    if a < b then
      local piece = fields_of(r)
      piece.text = r.text:sub(a - start + 1, b - start)
      out[#out + 1] = piece
    end

    start = stop
  end

  return out
end

richtext.slice = slice

local function concat(...)
  local out = {}
  for _, list in ipairs({ ... }) do
    for _, r in ipairs(list) do out[#out + 1] = r end
  end
  return out
end

-- The fields a run typed at `at` takes: the text before the caret's, or the
-- text after it at a paragraph's start.
local function look_at(p, at)
  local start, before, after = 1, nil, nil

  for _, r in ipairs(p.runs) do
    local stop = start + #r.text

    if at > start and at <= stop then before = r end
    if at >= start and at < stop and not after then after = r end

    start = stop
  end

  local r = before or after
  return r and fields_of(r) or {}
end

-- A copy of a body, so an edit can replace paragraphs in it.
local function copy_body(body)
  local out = {}
  for i, p in ipairs(body) do out[i] = p end
  return out
end

-- A paragraph that is a thing rather than text: a picture, a shape or a
-- table. It goes whole or not at all.
local function block(p)
  return p.picture ~= nil or p.table ~= nil or p.shape ~= nil
end

richtext.block = block

--
-- **A place in a table's cell** (W5b) says the cell too: `{ para, at, row,
-- col }`, `at` a byte of that cell's text. An edit there is the same edit
-- on a body of one paragraph - the cell - put back into a new table whose
-- other rows and cells are the same tables as before, so every function
-- here works in a cell as it does on the page, and a page sets again only
-- the cell that changed.
--
local function same_cell(a, b)
  return a.para == b.para and a.row == b.row and a.col == b.col
end

richtext.same_cell = same_cell

local function in_cell(body, a, b, edit)
  local p = body[a.para]
  local t = p.table
  local got, place = edit({ t.rows[a.row][a.col] }, { para = 1, at = a.at },
                          b and { para = 1, at = b.at })

  local rows, row = {}, {}
  for r, x in ipairs(t.rows) do rows[r] = x end
  for c, x in ipairs(t.rows[a.row]) do row[c] = x end

  row[a.col] = got[1]
  rows[a.row] = row

  local q = {}
  for k, v in pairs(p) do q[k] = v end
  q.table = { columns = t.columns, header = t.header, rows = rows, box = t.box,
              chart = t.chart }

  local out = copy_body(body)
  out[a.para] = q

  return out, place and { para = a.para, at = place.at, row = a.row, col = a.col }
end

-- Text a body may hold: line breaks go between paragraphs, so what comes
-- from a clipboard is split there, and bytes below a space are dropped but
-- a tab.
local function clean(text)
  return (text:gsub("\r\n?", "\n"):gsub("[%z\1-\8\11-\31\127]", ""))
end

--
-- **`text` typed at `place`**: in the look of what is before the caret -
-- with `with` over it, when Bold or a face was chosen with nothing
-- selected - a line break in it starting a new paragraph in the same style.
-- The new body and the caret after what was typed.
--
function richtext.type(body, place, text, with)
  if place.row then
    return in_cell(body, place, nil, function(cell, at)
      return richtext.type(cell, at, (text:gsub("[\r\n]+", " ")), with)
    end)
  end

  local out = copy_body(body)
  local p = body[place.para]
  local plain = richtext.plain(p)
  local fields = look_at(p, place.at)

  for k, v in pairs(with or {}) do fields[k] = v end
  local tail = slice(p, place.at, #plain + 1)
  local head = slice(p, 1, place.at)
  local lines = {}

  for line in (clean(text) .. "\n"):gmatch("([^\n]*)\n") do
    lines[#lines + 1] = line
  end

  local function typed(line)
    local r = {}
    for k, v in pairs(fields) do r[k] = v end
    r.text = line
    return { r }
  end

  if #lines == 1 then
    out[place.para] = with_runs(p, concat(head, typed(lines[1]), tail))
    return out, { para = place.para, at = place.at + #lines[1] }
  end

  -- Several lines: the first ends this paragraph, the last begins the one
  -- that holds the tail, and each between is a paragraph of its own.
  local new = { with_runs(p, concat(head, typed(lines[1]))) }

  for i = 2, #lines - 1 do
    new[#new + 1] = with_runs(p, typed(lines[i]))
  end

  new[#new + 1] = with_runs(p, concat(typed(lines[#lines]), tail))

  table.remove(out, place.para)

  for i, q in ipairs(new) do table.insert(out, place.para + i - 1, q) end

  return out, { para = place.para + #new - 1, at = #lines[#lines] + 1 }
end

--
-- **A line broken inside a paragraph** (Shift-Return, and Return in a text
-- box): a `"\n"` in its text, in the look before the caret. The new body
-- and the caret after it.
--
function richtext.line_break(body, place, with)
  if place.row then
    return in_cell(body, place, nil, function(cell, at)
      return richtext.line_break(cell, at, with)
    end)
  end

  local out = copy_body(body)
  local p = body[place.para]
  local fields = look_at(p, place.at)

  for k, v in pairs(with or {}) do fields[k] = v end
  fields.text = "\n"

  out[place.para] = with_runs(p, concat(slice(p, 1, place.at), { fields },
                                        slice(p, place.at, #richtext.plain(p) + 1)))

  return out, { para = place.para, at = place.at + 1 }
end

--
-- **Return**: the paragraph broken at `place`. At its end the new one is in
-- the style's `next` - a heading is followed by Body - and with none of the
-- paragraph's own fields; anywhere else both halves keep the style and the
-- fields, as Pages does. `by_name` is the document's styles by name.
-- In a table's cell it does nothing: what Return means there - the cell
-- below - is the window's.
--
function richtext.split(body, place, by_name)
  if place.row then return body, place end

  local out = copy_body(body)
  local p = body[place.para]
  local plain = richtext.plain(p)
  local first = with_runs(p, slice(p, 1, place.at))
  local second

  if place.at > #plain then
    local style = by_name and by_name[p.style]
    second = { style = style and style.next or p.style, runs = {} }

    -- A list goes on: the next item is in the list too, until Return on an
    -- empty one ends it (the window's).
    if p.list and p.list ~= "none" then second.list = p.list end
  else
    second = with_runs(p, slice(p, place.at, #plain + 1))
  end

  out[place.para] = first
  table.insert(out, place.para + 1, second)

  return out, { para = place.para + 1, at = 1 }
end

-- Whether place `a` comes before place `b`: in a table, row by row and
-- cell by cell.
function richtext.before(a, b)
  if a.para ~= b.para then return a.para < b.para end
  if (a.row or 0) ~= (b.row or 0) then return (a.row or 0) < (b.row or 0) end
  if (a.col or 0) ~= (b.col or 0) then return (a.col or 0) < (b.col or 0) end
  return a.at < b.at
end

--
-- **What is between two places taken out**: the paragraphs between them
-- gone, and the first and last made one, in the first one's style. The new
-- body and the caret where the range began. Both in one cell, it is that
-- cell's text; a range from a cell to outside it takes its table whole.
--
function richtext.delete(body, a, b)
  if richtext.before(b, a) then a, b = b, a end

  if a.row and b.row and same_cell(a, b) then
    return in_cell(body, a, b, richtext.delete)
  end

  -- From a cell to anywhere else, the table goes whole.
  if a.row then a = { para = a.para, at = 1 } end
  if b.row then b = { para = b.para, at = 1 } end

  local out = copy_body(body)
  local first, last = body[a.para], body[b.para]

  if a.para == b.para and block(first) then return out, a end

  -- **A picture or a table at either end goes with the range**: what stays
  -- is the text before the range and the text after it, in the style of
  -- whichever of the two ends is text.
  local head = block(first) and {} or slice(first, 1, a.at)
  local tail = block(last) and {} or slice(last, b.at, #richtext.plain(last) + 1)
  local keep = first

  if block(first) then keep = block(last) and { style = first.style } or last end

  out[a.para] = with_runs(keep, concat(head, tail))

  for _ = a.para + 1, b.para do table.remove(out, a.para + 1) end

  return out, { para = a.para, at = a.at }
end

--
-- The text between two places, paragraphs ended by line breaks: what a
-- copy puts on the clipboard.
--
function richtext.text(body, a, b)
  if richtext.before(b, a) then a, b = b, a end

  if a.row and b.row and same_cell(a, b) then
    local cell = { body[a.para].table.rows[a.row][a.col] }
    return richtext.text(cell, { para = 1, at = a.at }, { para = 1, at = b.at })
  end

  local parts = {}

  for n = a.para, b.para do
    local t = body[n].table

    if t then
      local rows = {}

      for r, row in ipairs(t.rows) do
        local cells = {}
        for c, cell in ipairs(row) do cells[c] = richtext.plain(cell) end
        rows[r] = table.concat(cells, "\t")
      end

      parts[#parts + 1] = table.concat(rows, "\n")
      goto next
    end

    do
    local plain = richtext.plain(body[n])
    local from = n == a.para and a.at or 1
    local to = n == b.para and b.at or #plain + 1

    parts[#parts + 1] = plain:sub(from, to - 1)
    end

    ::next::
  end

  return table.concat(parts, "\n")
end

--------------------------------------------------------------------------
-- **Formatting** (W4c, the Format panel): a range's paragraphs given a
-- style or a paragraph field, a range's characters given a character
-- field, and the look at a place for the panel to show. Each returns a new
-- body, and each paragraph it touches is checked again against its style
-- (`richtext.paragraph`), so a field set to what the style already says is
-- left out and runs that end up alike are joined.
--------------------------------------------------------------------------

-- The range's ends in order, and the paragraphs it covers.
local function ordered(a, b)
  if richtext.before(b, a) then return b, a end
  return a, b
end

-- A table paragraph like `p` with `change` made to each of its cells.
local function each_cell(p, change)
  local rows = {}

  for r, row in ipairs(p.table.rows) do
    rows[r] = {}
    for c, cell in ipairs(row) do rows[r][c] = change(cell) end
  end

  local q = {}
  for k, v in pairs(p) do q[k] = v end
  q.table = { columns = p.table.columns, header = p.table.header, rows = rows,
              box = p.table.box, chart = p.table.chart }

  return q
end

-- The whole of a cell as a range, for a format over a whole table.
local function all_of(cell)
  return { para = 1, at = 1 }, { para = 1, at = #richtext.plain(cell) + 1 }
end

--
-- **A paragraph style** for every paragraph the range touches, their own
-- paragraph fields given up - choosing Title makes a title - and their runs'
-- character fields kept.
--
function richtext.restyle(body, a, b, name, by_name)
  a, b = ordered(a, b)

  if a.row and b.row and same_cell(a, b) then
    return (in_cell(body, a, b, function(cell, x, y)
      return richtext.restyle(cell, x, y, name, by_name)
    end))
  end

  local out = copy_body(body)

  for n = a.para, b.para do
    local p = body[n]

    if p.table then
      out[n] = each_cell(p, function(cell)
        local x, y = all_of(cell)
        return richtext.restyle({ cell }, x, y, name, by_name)[1]
      end)
    else
      out[n] = richtext.paragraph({ style = name, runs = p.runs, picture = p.picture,
                                    shape = p.shape }, by_name, p.style)
    end
  end

  return out
end

--
-- **Paragraph fields** - alignment, spacing, indents, a list, a drop cap -
-- for every paragraph the range touches.
--
function richtext.arrange(body, a, b, fields, by_name)
  a, b = ordered(a, b)

  if a.row and b.row and same_cell(a, b) then
    return (in_cell(body, a, b, function(cell, x, y)
      return richtext.arrange(cell, x, y, fields, by_name)
    end))
  end

  local out = copy_body(body)

  for n = a.para, b.para do
    if body[n].table then
      out[n] = each_cell(body[n], function(cell)
        local x, y = all_of(cell)
        return richtext.arrange({ cell }, x, y, fields, by_name)[1]
      end)
      goto next
    end

    do
    local p = {}
    for k, v in pairs(body[n]) do p[k] = v end
    for k, v in pairs(fields) do p[k] = v end
    out[n] = richtext.paragraph(p, by_name, body[n].style)
    end

    ::next::
  end

  return out
end

--
-- **Character fields** for the characters in the range: the runs it cuts
-- are split where it starts and ends, the fields set on what is inside.
--
function richtext.format(body, a, b, fields, by_name)
  a, b = ordered(a, b)

  if a.row and b.row and same_cell(a, b) then
    return (in_cell(body, a, b, function(cell, x, y)
      return richtext.format(cell, x, y, fields, by_name)
    end))
  end

  local out = copy_body(body)

  for n = a.para, b.para do
    local p = body[n]
    local plain = richtext.plain(p)

    if p.table and not (n == a.para and a.row) and not (n == b.para and b.row) then
      out[n] = each_cell(p, function(cell)
        local x, y = all_of(cell)
        return richtext.format({ cell }, x, y, fields, by_name)[1]
      end)
    end

    local from = n == a.para and a.at or 1
    local to = n == b.para and b.at or #plain + 1

    if to > from then
      local inside = slice(p, from, to)

      for _, r in ipairs(inside) do
        for k, v in pairs(fields) do r[k] = v end
      end

      local q = {}
      for k, v in pairs(p) do q[k] = v end
      q.runs = concat(slice(p, 1, from), inside, slice(p, to, #plain + 1))
      out[n] = richtext.paragraph(q, by_name, p.style)
    end
  end

  return out
end

--
-- **The look at a place**, whole: the paragraph's style, its layout, and
-- the character look of the text just before the caret - what typing there
-- would be, and what the Format panel shows.
--
function richtext.look_at(body, place, by_name)
  if place.row then
    return richtext.look_at({ body[place.para].table.rows[place.row][place.col] },
                            { para = 1, at = place.at }, by_name)
  end

  local p = body[place.para]
  local style = by_name[p.style] or richtext.PLAIN

  return richtext.look(look_at(p, place.at), style),
         richtext.layout(p, style), p.style
end

-- The place one character before or after `place` - across a paragraph's
-- end - or the place itself at the body's ends. **Through a table cell by
-- cell**: past a cell's end is the next cell's start, past the last the
-- paragraph after the table; into one from either side, its first cell or
-- its last.
function richtext.step(body, place, forward)
  local t = body[place.para].table

  if t and place.row then
    local plain = richtext.plain(t.rows[place.row][place.col])

    if forward and place.at <= #plain or not forward and place.at > 1 then
      local cell = { t.rows[place.row][place.col] }
      local got = richtext.step(cell, { para = 1, at = place.at }, forward)
      return { para = place.para, at = got.at, row = place.row, col = place.col }
    end

    local other = richtext.next_cell(body, place, forward and 1 or -1)

    if other then return other end

    if forward then
      if body[place.para + 1] then
        return richtext.enter(body, place.para + 1, true)
      end
    elseif place.para > 1 then
      return richtext.enter(body, place.para - 1, false)
    end

    return place
  end

  local plain = richtext.plain(body[place.para])

  if forward then
    if place.at <= #plain then
      local i = place.at + 1
      while i <= #plain and plain:byte(i) >= 0x80 and plain:byte(i) < 0xC0 do
        i = i + 1
      end
      return { para = place.para, at = i }
    end

    if body[place.para + 1] then return richtext.enter(body, place.para + 1, true) end

    return place
  end

  if place.at > 1 then
    local i = place.at - 1
    while i > 1 and plain:byte(i) >= 0x80 and plain:byte(i) < 0xC0 do
      i = i - 1
    end
    return { para = place.para, at = i }
  end

  if place.para > 1 then
    return richtext.enter(body, place.para - 1, false)
  end

  return place
end

--
-- **Paragraph `n` entered** from before it (`from_start`) or after it: its
-- start or its end - a table's first cell's start or its last cell's end;
-- a chart itself.
--
function richtext.enter(body, n, from_start)
  local p = body[n]
  local t = p.table

  -- A chart is entered as a picture is: the caret on it, not in a number.
  if t and not t.chart then
    local r = from_start and 1 or #t.rows
    local c = from_start and 1 or t.columns
    local at = from_start and 1 or #richtext.plain(t.rows[r][c]) + 1

    return { para = n, at = at, row = r, col = c }
  end

  return { para = n, at = from_start and 1 or #richtext.plain(p) + 1 }
end

--------------------------------------------------------------------------
-- **Tables** (W5b): one made, its size changed, and the cell after or
-- before a place - what the Table tool, the Format panel's Table part and
-- Tab do. Each returns a new body, as every edit does.
--------------------------------------------------------------------------

-- An empty cell, in `style`.
local function empty_cell(style)
  return { style = style, runs = {} }
end

--
-- **A table's paragraph**: `rows` by `columns` empty cells in `style`, the
-- first row a header when `header` says so.
--
function richtext.new_table(rows, columns, style, header)
  local t = { columns = columns, header = header and true or false, rows = {} }

  for r = 1, rows do
    t.rows[r] = {}
    for c = 1, columns do t.rows[r][c] = empty_cell(style) end
  end

  return { style = style, runs = {}, table = t }
end

--
-- **A shape's paragraph**: `kind`, 40 by 30 mm, in the drawing's blue,
-- centred.
--
function richtext.new_shape(kind, style)
  return { style = style, runs = {}, align = "center",
           shape = { kind = kind, width_mm = 40, height_mm = 30, fill = "#2a55c9" } }
end

--
-- **A chart's paragraph**: `kind`, 70 mm tall, over a year's seasons in
-- two series - numbers a person changes rather than a blank they start
-- from, as Pages' charts begin.
--
function richtext.new_chart(kind, style)
  local words = { { "", "2025", "2026" }, { "Spring", "12", "18" }, { "Summer", "20", "26" },
                  { "Autumn", "15", "21" }, { "Winter", "9", "14" } }
  local t = { columns = 3, header = true, rows = {}, chart = { kind = kind, height_mm = 70 } }

  for r, row in ipairs(words) do
    t.rows[r] = {}

    for c, text in ipairs(row) do
      t.rows[r][c] = { style = style, runs = text ~= "" and { { text = text } } or {} }
    end
  end

  return { style = style, runs = {}, align = "center", table = t }
end

--
-- **A chart's numbers**, read from its table: the series' names, the
-- categories' names, and each series' number for each category - a cell
-- that is not a number counts as nought, and a thousands comma is read
-- past. What the setting draws and Word's chart holds, from one reading.
--
function richtext.chart_data(t)
  local series, categories, values = {}, {}, {}

  for c = 2, t.columns do series[c - 1] = richtext.plain(t.rows[1][c]) end
  for r = 2, #t.rows do categories[r - 1] = richtext.plain(t.rows[r][1]) end

  for s = 1, #series do
    values[s] = {}

    for k = 1, #categories do
      local text = richtext.plain(t.rows[k + 1][s + 1]):gsub(",", "")
      local v = tonumber(text)

      values[s][k] = (v and v == v and v ~= math.huge and v ~= -math.huge) and v or 0
    end
  end

  return { series = series, categories = categories, values = values }
end

--
-- **A text box's paragraph**: one empty cell in `style`, `width_mm` wide,
-- bordered, centred.
--
function richtext.new_box(style, width_mm)
  return { style = style, runs = {}, align = "center",
           table = { columns = 1, header = false, rows = { { empty_cell(style) } },
                     box = { width_mm = width_mm or 80, border = true } } }
end

--
-- **The cell after or before `place`'s** - `step` 1 or -1, row by row - its
-- text's start going forward and its end going back; nil past the table's
-- ends.
--
function richtext.next_cell(body, place, step)
  local t = body[place.para].table
  local r, c = place.row, place.col + step

  if c > t.columns then r, c = r + 1, 1 end
  if c < 1 then r, c = r - 1, t.columns end

  if r < 1 or r > #t.rows then return nil end

  local at = step > 0 and 1 or #richtext.plain(t.rows[r][c]) + 1

  return { para = place.para, at = at, row = r, col = c }
end

--
-- **Table `n` reshaped**: `fields.rows` and `fields.columns` - rows and
-- columns added at the end, empty, in the table's style, or taken from the
-- end - `fields.header`, and a text box's `fields.box`, each only when
-- given. Never smaller than one cell, never larger than the checks allow,
-- and a text box always one cell.
--
function richtext.reshape(body, n, fields, by_name)
  local p = body[n]
  local t = p.table
  local rows = math.max(1, math.min(richtext.TABLE_ROWS, fields.rows or #t.rows))
  local columns = math.max(1, math.min(richtext.TABLE_COLUMNS, fields.columns or t.columns))
  local header = t.header

  if fields.header ~= nil then header = fields.header end

  local out_rows = {}

  for r = 1, rows do
    local old = t.rows[r]

    if old and columns == t.columns then
      out_rows[r] = old
    else
      out_rows[r] = {}
      for c = 1, columns do
        out_rows[r][c] = old and old[c] or empty_cell(p.style)
      end
    end
  end

  local q = {}
  for k, v in pairs(p) do q[k] = v end
  q.table = { columns = columns, header = header, rows = out_rows,
              box = fields.box or t.box, chart = fields.chart or t.chart }

  local out = copy_body(body)
  out[n] = richtext.paragraph(q, by_name, p.style)

  return out
end

return richtext
