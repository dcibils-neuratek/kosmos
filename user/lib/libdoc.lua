-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- What a library offers, read from its source: each name it defines, how it
-- is called, and the comment above it as what it is.
--
-- The IDE's suggestions (`roadmap.md` 6n, step 5) come from the system the
-- IDE is running on rather than from a list somebody keeps: after `ui.` the
-- names `ui.lua` actually defines, after `win:` a window's methods. And the
-- libraries here are written to be read - a paragraph above almost every
-- function - so their comments are the documentation, and nothing has to be
-- written twice.
--
-- **Read, never run.** A library's source is scanned for the definitions a
-- person would look for - `function ui.button(spec)`, `ui.UP = -1`,
-- `function window:add(child)` - and nothing in it is executed, so asking
-- what a library has cannot open a window or change a setting.
--
-- Pure: `tools/test_libdoc.lua` reads the tree's own libraries on the Mac.

local libdoc = {}

-- The comment block ending on the line above `n`, as text: "-- " taken off,
-- and the Markdown bold the comments here use taken out.
local function comment_above(lines, n)
  local out = {}
  local i = n - 1

  while i >= 1 and lines[i]:match("^%s*%-%-") do
    local text = lines[i]:gsub("^%s*%-%- ?", "")

    -- A section's banner, a row of dashes, is nobody's words.
    if not text:match("^%-+$") then table.insert(out, 1, text) end

    i = i - 1
  end

  -- The blank comment lines that frame a block are not part of it.
  while out[1] == "" do table.remove(out, 1) end
  while out[#out] == "" do table.remove(out) end

  return (table.concat(out, "\n"):gsub("%*%*", ""))
end

-- The comment block that opens a function's body, within its first few
-- lines, for one whose words are inside it rather than above - `ui.button`'s
-- come after the line that makes its view.
local INSIDE_MOST = 4

local function comment_inside(lines, n)
  local i = n + 1

  while lines[i] and not lines[i]:match("^%s*%-%-") do
    if i > n + INSIDE_MOST or lines[i]:match("^end") then return "" end
    i = i + 1
  end

  local j = i

  while lines[j] and lines[j]:match("^%s*%-%-") do j = j + 1 end

  return comment_above(lines, j)
end

-- The first sentence of the first paragraph, for a list's right-hand side,
-- cut to a line's length. A paragraph that opens with a heading and a colon
-- - "A slider: the drawings' level ..." - runs on past the colon.
local SUMMARY_MOST = 90

local function summary(doc)
  -- The first paragraph that says something: not a title of two words
  -- or fewer, and not an example indented in the comment.
  local para = ""

  for p in (doc .. "\n\n"):gmatch("(.-)\n\n+") do
    local words = select(2, p:gsub("%S+", ""))

    if p:match("^%S") and words > 2 then
      para = p
      break
    end
  end

  local line = para:gsub("\n", " ")
  local first = line:match("^(.-%.)%s") or line:match("^(.-%.)$") or line

  if #first > SUMMARY_MOST then
    first = first:sub(1, SUMMARY_MOST):gsub("%s+%S*$", "") .. "..."
  end

  return first
end

-- An example written in the comment - `--   ui.header{ x = 0, ... }` - is a
-- better signature than the parameter list, when there is one.
local function example(doc, owner, name)
  for line in (doc .. "\n"):gmatch("([^\n]*)\n") do
    local shown = line:match("^%s%s+(" .. owner .. "[%.:]" .. name .. "[%({].*)$")

    if shown then return shown end
  end
end

--
-- A library's source into what it offers:
--
--   { owner = "ui", names = { [name] = entry }, list = { entries in order },
--     tables = { window = { names =, list = } } }
--
-- An entry is `{ name, kind = "function" | "method" | "value", signature,
-- summary, doc, line }`. `owner` is the table the source returns - the one
-- a program gets from `use` - and `tables` the local tables whose methods
-- are defined with `:`, which is how an object's kind is written here.
--
function libdoc.read(source)
  local lines = {}

  for line in (tostring(source or "") .. "\n"):gmatch("([^\n]*)\n") do
    lines[#lines + 1] = line
  end

  local owner = nil

  for i = #lines, 1, -1 do
    owner = lines[i]:match("^return%s+([%a_][%w_]*)%s*$")
    if owner then break end
    if lines[i]:match("%S") then break end
  end

  local result = { owner = owner, names = {}, list = {}, tables = {} }

  local function add(into, entry)
    if into.names[entry.name] then return end

    into.names[entry.name] = entry
    into.list[#into.list + 1] = entry
  end

  local function table_for(name)
    if name == owner then return result end

    result.tables[name] = result.tables[name] or { names = {}, list = {} }
    return result.tables[name]
  end

  for n, line in ipairs(lines) do
    local t, sep, name, params = line:match("^function%s+([%a_][%w_]*)([%.:])([%a_][%w_]*)%s*(%b())")
    local vt, vname = line:match("^([%a_][%w_]*)%.([%a_][%w_]*)%s*=[^=]")

    if t and (t == owner or sep == ":") then
      local doc = comment_above(lines, n)

      -- Past the one-line definitions a function's own constants make,
      -- blank lines between: `ui.tabs`'s words are above its three.
      if doc == "" then
        local i = n - 1

        while i >= 1 and (lines[i] == "" or lines[i]:match("^local%s+[%w_]+%s*=%s*[^{]*$")) do
          i = i - 1
        end

        if i >= 1 and lines[i]:match("^%s*%-%-") then doc = comment_above(lines, i + 1) end
      end

      if doc == "" then doc = comment_inside(lines, n) end

      add(table_for(t), {
        name = name, kind = (sep == ":") and "method" or "function",
        signature = example(doc, t, name) or (t .. sep .. name .. params),
        summary = summary(doc), doc = doc, line = n,
      })
    elseif vt and vt == owner and vname then
      local doc = comment_above(lines, n)
      local is_fn = line:match("=%s*function")

      add(result, {
        name = vname, kind = is_fn and "function" or "value",
        signature = vt .. "." .. vname, summary = summary(doc), doc = doc,
        line = n,
      })
    end
  end

  return result
end

--
-- The names in `set` that begin with `prefix`, in the order they were
-- defined, the exact case first.
--
function libdoc.matching(set, prefix)
  local out = {}

  for _, e in ipairs(set and set.list or {}) do
    if e.name:sub(1, #prefix) == prefix then out[#out + 1] = e end
  end

  return out
end

-- How far apart two names are: the edits from one to the other.
local function distance(a, b)
  local prev = {}

  for j = 0, #b do prev[j] = j end

  for i = 1, #a do
    local cur = { [0] = i }

    for j = 1, #b do
      local cost = (a:sub(i, i) == b:sub(j, j)) and 0 or 1

      cur[j] = math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
    end

    prev = cur
  end

  return prev[#b]
end

-- The name in `set` nearest to `name`, when one is near enough to be the
-- one meant - a slip of a letter or two - or nil.
function libdoc.nearest(set, name)
  local best, far = nil, math.max(2, #name // 3) + 1

  for _, e in ipairs(set and set.list or {}) do
    local d = distance(name, e.name)

    if d < far then best, far = e.name, d end
  end

  return best
end

return libdoc
