-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JSON, read and written: RFC 8259, and nothing looser.
--
--   local json = use("/lib/json.lua")
--   local value, why = json.decode(text)   -- nil and a sentence if it is not JSON
--   local text = json.encode(value)
--
-- **For files that arrive from outside**, which is what it is for first:
-- Cafesa3D's scenes are glTF, and a glTF is JSON (`roadmap.md` 4l). So it
-- is strict - a trailing comma, a single quote, a comment or a bare word is
-- refused and says where - and bounded: nesting deeper than a hundred is
-- refused rather than followed down the stack, since a file of ten thousand
-- `[` is one line of somebody's making.
--
-- **Lua, not C.** A scene file is structure - a few kilobytes of names and
-- numbers - and neither a loop over a stream nor on a deadline, which is
-- where `CLAUDE.md` puts the line; and a reader that cannot overflow a
-- buffer is the right one for untrusted input.
--
-- `json.null` stands for JSON's null, which a Lua table cannot hold as a
-- value. An array decodes to a sequence and an object to a table of string
-- keys; encoding asks the same question the other way round, so a table
-- whose keys are 1 to n is an array and anything else an object, its keys
-- in order so the same scene writes the same bytes twice.

local json = {}

json.null = setmetatable({}, { __tostring = function() return "null" end })

local MAX_DEPTH = 100

--------------------------------------------------------------------------
-- Reading.
--------------------------------------------------------------------------

local ESCAPES = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f",
                  n = "\n", r = "\r", t = "\t" }

-- A code point as UTF-8.
local function utf8_of(cp)
  if cp < 0x80 then return string.char(cp) end
  if cp < 0x800 then return string.char(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f)) end
  if cp < 0x10000 then
    return string.char(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
  end

  return string.char(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f),
                     0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
end

function json.decode(text)
  if type(text) ~= "string" then return nil, "not text" end

  local at = 1
  local value

  -- Where in the text, as a person counts: line and column.
  local function where(i)
    local line, col = 1, 1

    for k = 1, math.min(i, #text + 1) - 1 do
      if text:byte(k) == 10 then line, col = line + 1, 1 else col = col + 1 end
    end

    return ("line %d, column %d"):format(line, col)
  end

  local function fail(why, i)
    error({ json = why .. " at " .. where(i or at) }, 0)
  end

  local function space()
    at = text:find("[^ \t\r\n]", at) or (#text + 1)
  end

  local function string_()
    local out, i = {}, at + 1

    while true do
      local j = text:find('["\\%c]', i)

      if not j then fail("a string that never ends", at) end

      out[#out + 1] = text:sub(i, j - 1)

      local c = text:sub(j, j)

      if c == '"' then
        at = j + 1
        return table.concat(out)
      elseif c == "\\" then
        local e = text:sub(j + 1, j + 1)

        if ESCAPES[e] then
          out[#out + 1] = ESCAPES[e]
          i = j + 2
        elseif e == "u" then
          local hex = text:match("^%x%x%x%x", j + 2)

          if not hex then fail("\\u without four hex digits", j) end

          local cp = tonumber(hex, 16)

          i = j + 6

          -- A pair of surrogates is one character; half of one is not text.
          if cp >= 0xd800 and cp <= 0xdbff then
            local low = text:match("^\\u(%x%x%x%x)", i)
            local lo = low and tonumber(low, 16)

            if not lo or lo < 0xdc00 or lo > 0xdfff then
              fail("half of a surrogate pair", j)
            end

            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00)
            i = i + 6
          elseif cp >= 0xdc00 and cp <= 0xdfff then
            fail("half of a surrogate pair", j)
          end

          out[#out + 1] = utf8_of(cp)
        else
          fail("an unknown escape \\" .. e, j)
        end
      else
        fail("a control character inside a string", j)
      end
    end
  end

  local function number()
    local s = text:match("^-?%d+%.?%d*[eE]?[-+]?%d*", at)

    -- The grammar exactly: no leading zeros, a digit either side of a point,
    -- and an exponent that has digits.
    if not s or not (s:match("^-?0$") or s:match("^-?0[.eE]") or s:match("^-?[1-9]"))
       or s:match("%.$") or s:match("%.[eE]") or s:match("[eE][-+]?$") then
      fail("a number that is not one")
    end

    local n = tonumber(s)

    if not n or n ~= n or n == math.huge or n == -math.huge then
      fail("a number that is not one")
    end

    at = at + #s
    return n
  end

  function value(depth)
    if depth > MAX_DEPTH then fail("nesting deeper than " .. MAX_DEPTH) end

    space()

    local c = text:sub(at, at)

    if c == "{" then
      local obj = {}

      at = at + 1
      space()

      if text:sub(at, at) == "}" then at = at + 1 return obj end

      while true do
        space()
        if text:sub(at, at) ~= '"' then fail("a name in quotes") end

        local key = string_()

        space()
        if text:sub(at, at) ~= ":" then fail("a colon after a name") end

        at = at + 1
        obj[key] = value(depth + 1)
        space()

        local d = text:sub(at, at)

        at = at + 1

        if d == "}" then return obj end
        if d ~= "," then fail("a comma or a closing brace", at - 1) end
      end
    elseif c == "[" then
      local arr = {}

      at = at + 1
      space()

      if text:sub(at, at) == "]" then at = at + 1 return arr end

      while true do
        arr[#arr + 1] = value(depth + 1)
        space()

        local d = text:sub(at, at)

        at = at + 1

        if d == "]" then return arr end
        if d ~= "," then fail("a comma or a closing bracket", at - 1) end
      end
    elseif c == '"' then
      return string_()
    elseif c == "-" or c:match("%d") then
      return number()
    elseif text:sub(at, at + 3) == "true" then
      at = at + 4
      return true
    elseif text:sub(at, at + 4) == "false" then
      at = at + 5
      return false
    elseif text:sub(at, at + 3) == "null" then
      at = at + 4
      return json.null
    elseif c == "" then
      fail("the end of the text where a value should be")
    end

    fail("a value")
  end

  local ok, result = pcall(function()
    local v = value(1)

    space()
    if at <= #text then fail("more after the value") end

    return v
  end)

  if ok then return result end
  if type(result) == "table" and result.json then return nil, result.json end

  error(result, 0)
end

--------------------------------------------------------------------------
-- Writing.
--------------------------------------------------------------------------

local function quote(s)
  return '"' .. s:gsub('[%c"\\]', function(c)
    local named = { ['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b", ["\f"] = "\\f",
                    ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }

    return named[c] or ("\\u%04x"):format(c:byte())
  end) .. '"'
end

local function is_array(t)
  local n = #t

  for k in pairs(t) do
    if type(k) ~= "number" or k < 1 or k > n or k ~= math.floor(k) then return false end
  end

  return n > 0 or next(t) == nil
end

-- `indent` is a string to indent by, or nil for one line.
function json.encode(v, indent, depth)
  depth = depth or 0

  local t = type(v)

  if v == json.null or v == nil then return "null" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "string" then return quote(v) end

  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then
      error("JSON has no " .. tostring(v), 2)
    end

    if math.type(v) == "integer" then return tostring(v) end

    -- Ten significant digits: a scene's millimetres to a kilometre.
    local s = ("%.10g"):format(v)

    return s
  end

  if t ~= "table" then error("JSON has no " .. t, 2) end
  if depth > MAX_DEPTH then error("nesting deeper than " .. MAX_DEPTH, 2) end

  local nl = indent and ("\n" .. indent:rep(depth + 1)) or ""
  local close = indent and ("\n" .. indent:rep(depth)) or ""
  local sep = indent and ": " or ":"
  local parts = {}

  if is_array(v) then
    if #v == 0 then return "[]" end

    for _, x in ipairs(v) do parts[#parts + 1] = json.encode(x, indent, depth + 1) end

    return "[" .. nl .. table.concat(parts, "," .. nl) .. close .. "]"
  end

  local keys = {}

  for k in pairs(v) do
    if type(k) ~= "string" then error("a JSON object's names are strings", 2) end
    keys[#keys + 1] = k
  end

  table.sort(keys)

  for _, k in ipairs(keys) do
    parts[#parts + 1] = quote(k) .. sep .. json.encode(v[k], indent, depth + 1)
  end

  return "{" .. nl .. table.concat(parts, "," .. nl) .. close .. "}"
end

return json
