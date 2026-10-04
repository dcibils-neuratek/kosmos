-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Liang's hyphenation on the host (`user/lib/hyphen.lua`), against the
-- patterns the image carries (`assets/hyphenation/`): words TeX is known to
-- break, the exceptions winning over the patterns, the shortest pieces the
-- languages allow, Spanish with its accents, and a capital.

local hyphen = dofile("user/lib/hyphen.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then checks = checks + 1 else fails = fails + 1 print("  " .. what) end
end

local function read(name)
  local f = assert(io.open("assets/hyphenation/" .. name, "rb"))
  local text = f:read("a")
  f:close()
  return text
end

local en = hyphen.parse(read("hyph-en-us.pat.txt"), read("hyph-en-us.hyp.txt"), 2, 3)
local es = hyphen.parse(read("hyph-es.pat.txt"), nil, 2, 2)

-- A word with its breaks written in, from the offsets.
local function shown(f, word)
  local out, last = {}, 0
  for _, at in ipairs(f(word)) do
    out[#out + 1] = word:sub(last + 1, at)
    last = at
  end
  out[#out + 1] = word:sub(last + 1)
  return table.concat(out, "-")
end

for word, want in pairs{
  -- "com-put-er" would leave two letters, and American English leaves no
  -- fewer than three: a plain Liang in Python over the same file agrees.
  hyphenation = "hy-phen-ation", computer = "com-puter",
  algorithm = "al-go-rithm", typesetting = "type-set-ting",
  -- From the exceptions, over the patterns.
  associate = "as-so-ciate", table = "ta-ble", project = "project",
  -- Too short to break at all, with two left and three right.
  cat = "cat", into = "into",
} do
  local got = shown(en, word)
  check(got == want, ("English %s is %s, not %s"):format(word, got, want))
end

for word, want in pairs{
  -- As these patterns break it, checked against a plain Liang in Python.
  computadora = "compu-tado-ra", ["Espa\u{f1}a"] = "Es-pa-\u{f1}a",
  ["canci\u{f3}n"] = "can-ci\u{f3}n", ventana = "ven-ta-na",
} do
  local got = shown(es, word)
  check(got == want, ("Spanish %s is %s, not %s"):format(word, got, want))
end

if fails > 0 then
  print(("hyphen: %d of %d checks failed"):format(fails, checks + fails))
  os.exit(1)
end

print(("hyphen: %d checks pass"):format(checks))
