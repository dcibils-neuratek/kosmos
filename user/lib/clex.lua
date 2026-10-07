-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- C, a line at a time, as the colours an editor draws it in.
--
-- The IDE's editor colours a project's C as it colours its Lua
-- (`lualex.lua`, whose shape this is): the same kinds, so the same palette,
-- and the same contract - each line starts in the state the one before it
-- ended in and says the state it ends in, so an edit lexes again only until
-- a line ends as it did before.
--
-- The state is a string or nil:
--
--   nil      ordinary code
--   "c"      inside a `/* ... */` comment
--   "p"      a preprocessor line carried on by a backslash at its end
--   'q"'     inside a string carried on by a backslash at its end
--
-- **The kinds**, as Lua's are: "keyword" - C's words and a preprocessor
-- directive; "number"; "string" - a string, a character, and the header an
-- `#include` names; "comment"; "call" - a name before `(`; and "library" -
-- the names a C app is handed rather than writes: the standard types, and
-- Lua's, the Window Kit's and Kosmos's, which all wear a prefix of their own.
--
-- **It colours; it does not check.** An unterminated string ends where its
-- line does; TinyCC, at F6, says what is wrong and where.
--
-- Pure: `tools/test_clex.lua` runs it on the Mac.

local lex = {}

local KEYWORDS = {}

for word in ([[auto break case char const continue default do double else enum
               extern float for goto if inline int long register restrict return
               short signed sizeof static struct switch typedef union unsigned
               void volatile while _Bool _Static_assert _Alignas _Alignof
               _Noreturn _Thread_local bool true false NULL]])
            :gmatch("[%w_]+") do
  KEYWORDS[word] = true
end

lex.KEYWORDS = KEYWORDS

-- The standard library's types, and Lua's: names a program is handed.
local LIBRARY = {}

for name in ([[int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t
               intptr_t uintptr_t size_t ssize_t ptrdiff_t FILE va_list
               lua_State lua_Integer lua_Number lua_CFunction luaL_Reg luaL_Buffer]])
            :gmatch("[%w_]+") do
  LIBRARY[name] = true
end

lex.LIBRARY = LIBRARY

-- And by the prefix every kit's names wear: Lua's, the Window Kit's, Kosmos's.
local PREFIXES = { "lua_", "luaL_", "LUA_", "kw_", "KW_", "kosmos_", "KOSMOS_", "WM_" }

local function handed(word)
  if LIBRARY[word] then return true end

  for _, p in ipairs(PREFIXES) do
    if word:sub(1, #p) == p then return true end
  end

  return false
end

lex.handed = handed

-- The end of a string or character whose first byte inside is `i`, and
-- whether a backslash at the end of the line carries it on.
local function quoted(text, i, quote)
  local n = #text

  while i <= n do
    local c = text:byte(i)

    if c == 92 then                                           -- \
      if i == n then return n, true end
      i = i + 2
    elseif c == quote then
      return i, false
    else
      i = i + 1
    end
  end

  return n, false
end

-- The last byte of the number that starts at `i`, its suffixes included.
local function number_end(text, i)
  local _, e = text:find("^0[xX][%x]*", i)

  if not e then
    _, e = text:find("^%d*%.?%d*", i)

    local _, x = text:find("^[eE][%+%-]?%d+", e + 1)

    e = x or e
  end

  local _, s = text:find("^[uUlLfF]+", e + 1)

  return s or e
end

--
-- One line: its coloured spans, `{ from, to, kind }` in order and never
-- overlapping, and the state it ends in.
--
function lex.line(text, state)
  local spans = {}
  local n = #text
  local i = 1
  local directive = (state == "p")

  local function span(from, to, kind)
    spans[#spans + 1] = { from, to, kind }
  end

  if state == "c" then
    local _, e = text:find("*/", 1, true)

    if not e then
      if n > 0 then span(1, n, "comment") end
      return spans, "c"
    end

    span(1, e, "comment")
    i = e + 1
  elseif state and state:sub(1, 1) == "q" then
    local e, open = quoted(text, 1, state:byte(2))

    span(1, e, "string")
    if open then return spans, state end
    i = e + 1
  end

  -- A `#` first on the line: the directive is a keyword, and the header an
  -- `#include` names is a string, `<stdio.h>` as much as `"mine.h"`.
  local hash = text:find("^%s*#", i)

  if hash and not state then
    local s, e = text:find("^%s*#%s*[%a_]*", i)

    span(text:find("#", s, true), e, "keyword")
    directive = true
    i = e + 1

    if text:sub(s, e):match("include$") then
      local hs, he = text:find("^%s*<[^>]*>", i)

      if hs then
        hs = text:find("<", hs, true)
        span(hs, he, "string")
        i = he + 1
      end
    end
  end

  while i <= n do
    local c = text:byte(i)

    if c == 32 or c == 9 or c == 13 then                      -- space
      i = i + 1
    elseif c == 47 and text:byte(i + 1) == 47 then            -- //
      span(i, n, "comment")
      return spans, nil
    elseif c == 47 and text:byte(i + 1) == 42 then            -- /*
      local _, e = text:find("*/", i + 2, true)

      if not e then
        span(i, n, "comment")
        return spans, "c"
      end

      span(i, e, "comment")
      i = e + 1
    elseif c == 34 or c == 39 then                            -- " or '
      local e, open = quoted(text, i + 1, c)

      span(i, e, "string")
      if open then return spans, "q" .. string.char(c) end
      i = e + 1
    elseif (c >= 48 and c <= 57)
           or (c == 46 and (text:byte(i + 1) or 0) >= 48
               and (text:byte(i + 1) or 0) <= 57) then       -- a digit, .5
      local e = number_end(text, i)

      span(i, e, "number")
      i = e + 1
    elseif (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or c == 95 then
      local s, e = text:find("^[%w_]+", i)
      local word = text:sub(s, e)
      local after = text:match("^%s*(.)", e + 1)

      if KEYWORDS[word] then
        span(s, e, "keyword")
      elseif handed(word) then
        span(s, e, "library")
      elseif after == "(" then
        span(s, e, "call")
      end

      i = e + 1
    else
      i = i + 1
    end
  end

  -- A directive whose last byte is a backslash carries on.
  if directive and text:byte(n) == 92 then return spans, "p" end

  return spans, nil
end

return lex
