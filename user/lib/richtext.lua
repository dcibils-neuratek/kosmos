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
-- page.
--
richtext.PARA = {
  align = enum(ALIGN), spacing_lines = number(0.5, 5),
  before_pt = number(0, 500), after_pt = number(0, 500),
  indent_first_mm = number(-200, 200), indent_left_mm = number(0, 200),
  indent_right_mm = number(0, 200), drop_cap_lines = number(0, 10, true),
  list = enum(LIST), keep_with_next = boolean,
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
-- One paragraph, checked: its style's name - the one it gave if that is a
-- style, and `fallback` if not - its own paragraph fields where they differ
-- from that style, and its runs, joined where they look alike.
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

return richtext
