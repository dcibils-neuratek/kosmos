-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where a word may be broken across two lines (`docs/write.md` W4e).
--
--   local hyphen = use("/Kosmos/Libraries/hyphen.lua")
--   local en = hyphen.language("en-us")
--   en("hyphenation")        { 3, 7 } - after "hy" and "hyphen"
--
-- **Liang's algorithm, with TeX's patterns** (`assets/hyphenation/`, the
-- hyph-utf8 package's, vendored with their terms). A pattern is letters
-- with digits between them - `hen5at` - and every pattern found anywhere in
-- a word, its ends marked with dots, leaves its digits between the letters
-- it matched; where the largest digit between two letters is odd, the word
-- may break there. Franklin Liang's thesis, 1983, and still how TeX does it.
--
-- **A kit**: Kosmos Write's setting first; anything else that sets a column
-- of text after it. What comes back is the byte offsets of the breaks in
-- the word as given - UTF-8 and all - each one a place between two
-- characters, never within `left` characters of the start or `right` of
-- the end.
--
-- The patterns are read from text, so the same file is the test on the Mac
-- (`hyphen.parse`) and the language inside the machine (`hyphen.language`,
-- which reads the image's copy). A language's table is built the first time
-- it is asked for and kept.

local hyphen = {}

-- The languages there are, and the shortest piece each leaves on a line:
-- the hyphenmins its patterns' authors give.
hyphen.LANGUAGES = {
  ["en-us"] = { name = "English", left = 2, right = 3 },
  ["es"] = { name = "Spanish", left = 2, right = 2 },
}

-- The capitals each language's letters may be written in, to their small
-- letters: ASCII's by `lower`, these by hand.
local LOWER = {
  ["\u{C1}"] = "\u{E1}", ["\u{C9}"] = "\u{E9}", ["\u{CD}"] = "\u{ED}",
  ["\u{D3}"] = "\u{F3}", ["\u{DA}"] = "\u{FA}", ["\u{DC}"] = "\u{FC}",
  ["\u{D1}"] = "\u{F1}",
}

-- A string's characters, one per entry, in small letters.
local function letters(word)
  local out = {}

  for ch in word:gmatch(utf8.charpattern) do
    out[#out + 1] = LOWER[ch] or ch:lower()
  end

  return out
end

--
-- **A language from its patterns' text** - one pattern a line, as the
-- package's `.pat.txt` has them - and its exceptions' (`.hyp.txt`, words
-- with their hyphens written in), which win over the patterns.
--
function hyphen.parse(patterns, exceptions, left, right)
  local table_ = {}
  local most = 0

  for line in (patterns or ""):gmatch("[^\n]+") do
    local word = line:match("^%s*(%S+)")

    if word then
      local chars, digits = {}, { 0 }

      for ch in word:gmatch(utf8.charpattern) do
        if ch:match("^%d$") then
          digits[#digits] = tonumber(ch)
        else
          chars[#chars + 1] = ch
          digits[#digits + 1] = 0
        end
      end

      table_[table.concat(chars)] = digits
      most = math.max(most, #chars)
    end
  end

  local fixed = {}

  for line in (exceptions or ""):gmatch("[^\n]+") do
    local written = line:match("^%s*(%S+)")

    if written then
      local breaks, at = {}, 0

      for ch in written:gmatch(utf8.charpattern) do
        if ch == "-" then
          breaks[#breaks + 1] = at
        else
          at = at + #ch
        end
      end

      fixed[(written:gsub("-", ""))] = breaks
    end
  end

  left, right = left or 2, right or 3

  --
  -- **The breaks in `word`**: byte offsets, each after that many bytes of
  -- the word as it was given.
  --
  return function(word)
    local chars = letters(word)

    if #chars < left + right then return {} end

    local key = table.concat(chars)

    if fixed[key] then
      -- An exception says its breaks in its own small letters' bytes; the
      -- word as given may have capitals whose UTF-8 is as long.
      return fixed[key]
    end

    -- The word between dots, and the largest digit found between each two
    -- of its characters.
    local dotted = { "." }
    for _, ch in ipairs(chars) do dotted[#dotted + 1] = ch end
    dotted[#dotted + 1] = "."

    local values = {}
    for i = 1, #dotted + 1 do values[i] = 0 end

    for i = 1, #dotted do
      local piece = ""

      for j = i, math.min(#dotted, i + most - 1) do
        piece = piece .. dotted[j]

        local digits = table_[piece]

        if digits then
          for k, d in ipairs(digits) do
            local at = i + k - 1
            if d > values[at] then values[at] = d end
          end
        end
      end
    end

    -- `values[i + 1]` is between dotted characters i and i + 1, which is
    -- after word character i - 1: odd means a break may be there.
    local out, bytes = {}, 0
    local sizes = {}

    for ch in word:gmatch(utf8.charpattern) do
      sizes[#sizes + 1] = #ch
    end

    for i = 1, #chars - 1 do
      bytes = bytes + sizes[i]

      if i >= left and #chars - i >= right and values[i + 2] % 2 == 1 then
        out[#out + 1] = bytes
      end
    end

    return out
  end
end

local made = {}

--
-- **A language by its tag**, read from the image's copy of its patterns:
-- a function from a word to its breaks, or nil when the language has none.
--
function hyphen.language(tag)
  if made[tag] ~= nil then return made[tag] or nil end

  local spec = hyphen.LANGUAGES[tag]
  local patterns = spec and sys.asset("hyphenation/hyph-" .. tag .. ".pat.txt")

  if not patterns then
    made[tag] = false
    return nil
  end

  local exceptions = sys.asset("hyphenation/hyph-" .. tag .. ".hyp.txt")

  made[tag] = hyphen.parse(patterns, exceptions, spec.left, spec.right)
  return made[tag]
end

return hyphen
