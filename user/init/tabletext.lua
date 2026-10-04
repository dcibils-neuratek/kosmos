-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- tabletext: a table as text a person can read and edit, and back again,
-- values only.
--
-- Diego, 4 October 2026: "Nothing is stored in binary format for settings
-- and preferences", "I don't like binary files for settings for anything in
-- the system". A table written to a file was the serialiser's bytes -
-- `\0KTV`, a type byte and a four-byte length before each value - which no
-- editor shows and a slip in one damages. So a table written to a file is
-- this:
--
--   -- kosmos: table
--   {
--     bar = "dock",
--     palette = "night",
--     pins = { "tracker", "terminal", "music" },
--     shadow = true,
--   }
--
-- **Lua's own table syntax**, because Lua is the language this system is
-- written for a person in, and **read as values only**: strings, numbers,
-- true and false, and tables of them. Nothing in it is run - there is no
-- `load` here - so a settings file cannot carry a program, and one broken
-- by hand is refused with its line, rather than taking down what read it.
--
-- The first line says what the file is: without it a file is text, and is
-- read as text, so nothing a person writes is taken for a table by
-- accident. Comments (`--` to the end of a line) and blank lines may go
-- anywhere a space may. Keys are written sorted, so the same table is the
-- same file.
--
-- Compiled into every image beside `init.lua`, which stores tables with it
-- (`DISK`'s write and read), and held on the Mac by `tools/test_tabletext.lua`.

local M = {}

M.MARK = "-- kosmos: table"

local DEPTH = 32                    -- tables inside tables, at most
local WIDTH = 60                    -- a list of plain values fits on a line

local KEYWORDS = {}

for w in ("and break do else elseif end false for function goto if in local "
          .. "nil not or repeat return then true until while"):gmatch("%a+") do
  KEYWORDS[w] = true
end

--------------------------------------------------------------------------
-- Writing.
--------------------------------------------------------------------------

local ESCAPES = { ["\\"] = "\\\\", ['"'] = '\\"', ["\n"] = "\\n", ["\r"] = "\\r",
                  ["\t"] = "\\t" }

-- A string as a literal: printable bytes as they are - UTF-8 included, so a
-- name with an accent reads as one - and the rest escaped.
local function quote(s)
  return '"' .. s:gsub('[%c"\\]', function(c)
    return ESCAPES[c] or ("\\%03d"):format(c:byte())
  end) .. '"'
end

-- A number that reads back as itself: an integer as one, a float in the
-- fewest digits that come back exact, and always with a point or an
-- exponent so it comes back a float.
local function number(n)
  if math.type(n) == "integer" then return ("%d"):format(n) end

  if n ~= n or n == math.huge or n == -math.huge then
    return nil, "a number that is not a number cannot be stored"
  end

  local s

  for digits = 14, 17 do
    s = ("%." .. digits .. "g"):format(n)
    if tonumber(s) == n then break end
  end

  if not s:find("[%.eEn]") then s = s .. ".0" end

  return s
end

local function scalar(v)
  local t = type(v)

  if t == "string" then return quote(v) end
  if t == "number" then return number(v) end
  if t == "boolean" then return tostring(v) end

  return nil, "a " .. t .. " cannot be stored in a file"
end

-- Keys in an order that does not change: numbers, then strings, then
-- booleans, each in its own order.
local RANK = { number = 1, string = 2, boolean = 3 }

local function before(a, b)
  local ta, tb = type(a), type(b)

  if ta ~= tb then return (RANK[ta] or 9) < (RANK[tb] or 9) end
  if ta == "boolean" then return (not a) and b end

  return a < b
end

local function key_text(k)
  if type(k) == "string" and k:match("^[%a_][%w_]*$") and not KEYWORDS[k] then
    return k
  end

  local s, why = scalar(k)

  if not s then return nil, why end

  return "[" .. s .. "]"
end

local encode

-- How many of `t`'s keys are 1, 2, ... with nothing missing; and whether
-- those are all it has. Raw, as everything here is: a metatable is
-- behaviour, and what is stored is the data (`serialize.c`'s rule).
local function sequence(t)
  local n, count = 0, 0

  while rawget(t, n + 1) ~= nil do n = n + 1 end

  for _ in next, t do count = count + 1 end

  return n, count == n
end

encode = function(v, indent, path)
  if type(v) ~= "table" then return scalar(v) end

  if path[v] then return nil, "a table that holds itself cannot be stored" end
  if #indent // 2 >= DEPTH then return nil, "tables more than 32 deep cannot be stored" end

  local n, only = sequence(v)

  if next(v) == nil then return "{}" end

  path[v] = true

  -- A short list of plain values on one line: `pins = { "a", "b" }`.
  if only then
    local parts, plain = {}, true

    for i = 1, n do
      if type(rawget(v, i)) == "table" then plain = false break end

      local s, why = scalar(rawget(v, i))

      if not s then path[v] = nil return nil, why end

      parts[i] = s
    end

    local line = plain and ("{ " .. table.concat(parts, ", ") .. " }")

    if line and #line <= WIDTH then
      path[v] = nil
      return line
    end
  end

  local inner = indent .. "  "
  local lines = { "{" }

  for i = 1, n do
    local s, why = encode(rawget(v, i), inner, path)

    if not s then path[v] = nil return nil, why end

    lines[#lines + 1] = inner .. s .. ","
  end

  local keys = {}

  for k in next, v do
    local listed = math.type(k) == "integer" and k >= 1 and k <= n

    if not listed then keys[#keys + 1] = k end
  end

  for _, k in ipairs(keys) do
    if type(k) ~= "string" and type(k) ~= "number" and type(k) ~= "boolean" then
      path[v] = nil
      return nil, "a key that is a " .. type(k) .. " cannot be stored"
    end
  end

  table.sort(keys, before)

  for _, k in ipairs(keys) do
    local kt, why = key_text(k)

    if not kt then path[v] = nil return nil, why end

    local s, why2 = encode(rawget(v, k), inner, path)

    if not s then path[v] = nil return nil, why2 end

    lines[#lines + 1] = inner .. kt .. " = " .. s .. ","
  end

  lines[#lines + 1] = indent .. "}"
  path[v] = nil

  return table.concat(lines, "\n")
end

-- The whole file for `t`: its first line, then the table. Nil and why for
-- what cannot be stored.
function M.encode(t)
  if type(t) ~= "table" then return nil, "only a table is stored as a table" end

  local s, why = encode(t, "", {})

  if not s then return nil, why end

  return M.MARK .. "\n" .. s .. "\n"
end

--------------------------------------------------------------------------
-- Reading: values, and nothing else.
--------------------------------------------------------------------------

-- Whether `text` is a stored table: its first line, exactly.
function M.is(text)
  return type(text) == "string"
         and (text:sub(1, #M.MARK + 1) == M.MARK .. "\n"
              or text:sub(1, #M.MARK + 2) == M.MARK .. "\r\n")
end

local UNESCAPE = { n = "\n", t = "\t", r = "\r", a = "\a", b = "\b", f = "\f",
                   v = "\v", ["\\"] = "\\", ['"'] = '"', ["'"] = "'" }

-- The table in `text`, or nil and "line N: what was wrong".
function M.decode(text)
  if not M.is(text) then return nil, "line 1: not a stored table (" .. M.MARK .. ")" end

  local at, line = #M.MARK + 1, 1

  local function fail(what)
    error({ line = line, what = what }, 0)
  end

  -- Spaces, blank lines and comments, counting the lines.
  local function skip()
    while true do
      local c = text:sub(at, at)

      if c == "\n" then
        line = line + 1
        at = at + 1
      elseif c == " " or c == "\t" or c == "\r" then
        at = at + 1
      elseif text:sub(at, at + 3) == "--[[" then
        local close = text:find("]]", at + 4, true)

        if not close then fail("a comment that does not end") end

        for _ in text:sub(at, close):gmatch("\n") do line = line + 1 end

        at = close + 2
      elseif text:sub(at, at + 1) == "--" then
        local eol = text:find("\n", at, true) or (#text + 1)

        at = eol
      else
        return
      end
    end
  end

  local function str()
    local q = text:sub(at, at)
    local out = {}

    at = at + 1

    while true do
      local c = text:sub(at, at)

      if c == "" or c == "\n" then fail("a string that does not end on its line") end

      if c == q then
        at = at + 1
        return table.concat(out)
      end

      if c == "\\" then
        local e = text:sub(at + 1, at + 1)

        if UNESCAPE[e] then
          out[#out + 1] = UNESCAPE[e]
          at = at + 2
        elseif e:match("%d") then
          local digits = text:match("^%d%d?%d?", at + 1)
          local n = tonumber(digits)

          if n > 255 then fail("an escape past 255") end

          out[#out + 1] = string.char(n)
          at = at + 1 + #digits
        elseif e == "x" then
          local hex = text:match("^%x%x", at + 2)

          if not hex then fail("\\x without two hex digits") end

          out[#out + 1] = string.char(tonumber(hex, 16))
          at = at + 4
        else
          fail("an escape this does not read: \\" .. e)
        end
      else
        out[#out + 1] = c
        at = at + 1
      end
    end
  end

  local function num()
    local s = text:match("^-?0[xX]%x+", at)
              or text:match("^-?%d*%.?%d+[eE][-+]?%d+", at)
              or text:match("^-?%d+%.?%d*[eE][-+]?%d+", at)
              or text:match("^-?%d+%.%d*", at)
              or text:match("^-?%.%d+", at)
              or text:match("^-?%d+", at)

    if not s then fail("a value was expected") end

    local n = tonumber(s)

    if not n then fail("not a number: " .. s) end

    at = at + #s
    return n
  end

  local value

  local function tbl(depth)
    if depth > DEPTH then fail("tables more than 32 deep") end

    local t, n = {}, 0

    at = at + 1                     -- the {

    while true do
      skip()

      local c = text:sub(at, at)

      if c == "}" then
        at = at + 1
        return t
      end

      if c == "" then fail("a table that does not end") end

      local key

      if c == "[" then
        at = at + 1
        skip()
        key = value(depth)
        skip()

        if text:sub(at, at) ~= "]" then fail("']' was expected after a key") end

        at = at + 1
        skip()

        if text:sub(at, at) ~= "=" then fail("'=' was expected after a key") end

        at = at + 1

        if type(key) == "table" then fail("a table cannot be a key") end
        if key ~= key then fail("a key that is not a number") end
      else
        local name = text:match("^[%a_][%w_]*", at)

        if name and not KEYWORDS[name] then
          local after = at + #name
          local rest = text:match("^[ \t\r]*()", after)

          if text:sub(rest, rest) ~= "=" or text:sub(rest, rest + 1) == "==" then
            fail("a name is only a key, before '=': " .. name)
          end

          key = name
          at = rest + 1
        end
      end

      skip()
      local v = value(depth)

      if key == nil then
        n = n + 1
        t[n] = v
      else
        t[key] = v
      end

      skip()

      local sep = text:sub(at, at)

      if sep == "," or sep == ";" then
        at = at + 1
      elseif sep == "" then
        fail("a table that does not end")
      elseif sep ~= "}" then
        fail("',' or '}' was expected")
      end
    end
  end

  value = function(depth)
    local c = text:sub(at, at)

    if c == "{" then return tbl(depth + 1) end
    if c == '"' or c == "'" then return str() end

    if text:match("^true%f[^%w_]", at) then at = at + 4 return true end
    if text:match("^false%f[^%w_]", at) then at = at + 5 return false end

    if c:match("[%d%.%-]") then return num() end

    local word = text:match("^[%a_][%w_]*", at)

    if word then fail("only values are read, not '" .. word .. "'") end

    fail("a value was expected")
  end

  local ok, result = pcall(function()
    skip()

    if text:sub(at, at) ~= "{" then fail("the table was expected, '{'") end

    local t = value(0)

    skip()

    if at <= #text then fail("something after the table") end

    return t
  end)

  if ok then return result end

  if type(result) == "table" then
    return nil, ("line %d: %s"):format(result.line, result.what)
  end

  return nil, tostring(result)
end

return M
