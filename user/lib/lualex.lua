-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Lua, a line at a time, as the colours an editor draws it in.
--
-- The IDE's editor (`roadmap.md` 6n, step 1) colours Lua as it is typed:
-- keywords, numbers, strings, comments, calls, and the names a program is
-- born with in a colour of their own, as `docs/kosmos-ide.html` draws them.
-- This says which bytes of a line are which; the editor chooses the colours.
--
-- **A line at a time, with what carries over.** A long string or a long
-- comment can run across lines, and so can a short string ended with a
-- backslash, so each line starts in the state the one before it ended in
-- and says the state it ends in. An editor keeps the state at the start of
-- every line and, after an edit, lexes again only until a line ends in the
-- state it ended in before - typing inside a function is one line of work,
-- and opening a `--[[` is the rest of the file, which is what it means.
--
-- The state is a string or nil, so two can be compared with `==`:
--
--   nil      ordinary code
--   "c2"     inside a long comment `--[==[`, of level 2
--   "s0"     inside a long string `[[`, of level 0
--   'q"'     inside a short string, carried on by a backslash at the end
--
-- **It colours; it does not check.** An unterminated short string ends
-- where its line does, as Lua would refuse it; saying so is the checker's
-- (step 4), with the line marked. A tokenizer that stopped at the first
-- mistake would draw half a file grey while it was being typed.
--
-- Pure: `tools/test_lualex.lua` runs it on the Mac.

local lex = {}

local KEYWORDS = {}

for word in ([[and break do else elseif end false for function goto if in
               local nil not or repeat return then true until while]])
            :gmatch("%a+") do
  KEYWORDS[word] = true
end

lex.KEYWORDS = KEYWORDS

--
-- The names a program is born with, which the drawing colours as a
-- library's: Lua's own less what this system takes out, and Kosmos's -
-- `tools/luaglobals.py` is the account of both, and this list is its
-- program environment. What a program gets from `use` is step 5's to add.
--
local LIBRARY = {}

for name in ([[_G _VERSION assert collectgarbage coroutine error getmetatable
               ipairs load math next pairs pcall print rawequal rawget rawlen
               rawset select setmetatable string table tonumber tostring type
               utf8 xpcall
               sys gfx fs args cwd run interrupted use write]])
            :gmatch("[%w_]+") do
  LIBRARY[name] = true
end

lex.LIBRARY = LIBRARY

-- Where a long bracket of `level` closes at or after `at`, or nil.
local function long_close(text, at, level)
  local _, e = text:find("]" .. ("="):rep(level) .. "]", at, true)

  return e
end

--
-- The end of a short string whose first byte inside is `i`, and whether it
-- carries on to the next line - a backslash last on the line, or `\z` with
-- nothing but space after it.
--
local function short(text, i, quote)
  local n = #text

  while i <= n do
    local c = text:byte(i)

    if c == 92 then                                           -- \
      if i == n then return n, true end

      if text:byte(i + 1) == 122 then                         -- \z
        local j = text:find("%S", i + 2)

        if not j then return n, true end

        i = j
      else
        i = i + 2
      end
    elseif c == quote then
      return i, false
    else
      i = i + 1
    end
  end

  return n, false
end

-- The last byte of the number that starts at `i`.
local function number_end(text, i)
  local _, e = text:find("^0[xX][%x%.]*", i)

  if e then
    local _, p = text:find("^[pP][%+%-]?%d+", e + 1)

    return p or e
  end

  _, e = text:find("^%d*%.?%d*", i)

  local _, x = text:find("^[eE][%+%-]?%d+", e + 1)

  return x or e
end

--
-- One line: its coloured spans, `{ from, to, kind }` in order and never
-- overlapping, and the state it ends in. `kind` is "keyword", "number",
-- "string", "comment", "call" or "library"; what no span covers is plain.
--
function lex.line(text, state)
  local spans = {}
  local n = #text
  local i = 1

  local function span(from, to, kind)
    spans[#spans + 1] = { from, to, kind }
  end

  --
  -- Carried over from the line before.
  --
  if state then
    local kind, rest = state:sub(1, 1), state:sub(2)

    if kind == "q" then
      local e, open = short(text, 1, rest:byte())

      span(1, e, "string")

      if open then return spans, state end

      i = e + 1
    else
      local e = long_close(text, 1, tonumber(rest) or 0)
      local what = (kind == "c") and "comment" or "string"

      if not e then
        if n > 0 then span(1, n, what) end
        return spans, state
      end

      span(1, e, what)
      i = e + 1
    end
  end

  -- The last byte before `i` that is not space, for a name after `.` or `:`.
  local previous = nil

  while i <= n do
    local c = text:byte(i)

    if c == 32 or c == 9 or c == 13 then                      -- space
      i = i + 1
    elseif c == 45 and text:byte(i + 1) == 45 then            -- --
      local level = text:match("^%[(=*)%[", i + 2)

      if level then
        local e = long_close(text, i + 4 + #level, #level)

        if not e then
          span(i, n, "comment")
          return spans, "c" .. #level
        end

        span(i, e, "comment")
        i = e + 1
      else
        span(i, n, "comment")
        return spans, nil
      end
    elseif c == 34 or c == 39 then                            -- " or '
      local e, open = short(text, i + 1, c)

      span(i, e, "string")

      if open then return spans, "q" .. string.char(c) end

      i = e + 1
      previous = c
    elseif c == 91 and text:find("^%[=*%[", i) then           -- [[ or [=[
      local level = text:match("^%[(=*)%[", i)
      local e = long_close(text, i + 2 + #level, #level)

      if not e then
        span(i, n, "string")
        return spans, "s" .. #level
      end

      span(i, e, "string")
      i = e + 1
      previous = 93
    elseif (c >= 48 and c <= 57)
           or (c == 46 and (text:byte(i + 1) or 0) >= 48
               and (text:byte(i + 1) or 0) <= 57) then       -- a digit, .5
      local e = number_end(text, i)

      span(i, e, "number")
      i = e + 1
      previous = 48
    elseif (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or c == 95 then
      local s, e = text:find("^[%w_]+", i)
      local word = text:sub(s, e)
      local after = text:match("^%s*(.)", e + 1)
      local member = (previous == 46 or previous == 58)       -- . or :

      if KEYWORDS[word] then
        span(s, e, "keyword")
      elseif LIBRARY[word] and not member then
        span(s, e, "library")
      elseif previous == 58 or after == "(" or after == "{"
             or after == '"' or after == "'"
             or (after == "[" and text:find("^%s*%[=*%[", e + 1)) then
        span(s, e, "call")
      end

      i = e + 1
      previous = 97
    else
      -- Punctuation: remembered, for the `.` or `:` before a name. A `..`
      -- is concatenation, and a name after it is not a member.
      if c == 46 and text:byte(i + 1) == 46 then
        previous = 0
        i = i + 2
      else
        previous = c
        i = i + 1
      end
    end
  end

  return spans, nil
end

return lex
