-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where a second goes, when a page of PDF takes one.
--
--   pdfbench                      page 3 of /home/odyssey.pdf
--   pdfbench /home/odyssey.pdf 40
--
-- A page came out in about 1.1 seconds and the obvious suspect was the
-- tokenizer, which reads a byte at a time through Lua string.sub. Obvious
-- is not measured, and `CLAUDE.md` is explicit that nothing moves to C
-- without a profile saying it should. This is that profile.
--
-- Four phases, timed separately, each run on its own so one does not hide
-- inside another:
--
--   read      the compressed stream off the disk
--   inflate   both ways: through a Lua string, and region to region
--   tokenize  the scanner alone, every token thrown away
--   walk      the scanner plus the interpreter plus the decode

local pdf      = use("/lib/pdf.lua")
local pdfpage  = use("/lib/pdfpage.lua")
local compress = use("/kits/compress")
local pdfkit   = use("/kits/pdf")

local path   = args[1] or "/home/odyssey.pdf"
local wanted = tonumber(args[2]) or 3

local attrs = fs.getattr(path)
if not attrs then print("pdfbench: no such file: " .. path) return end

-- Two regions: the compressed bytes land in one, the inflated in the other.
local RAW_PAGES, OUT_PAGES = 16, 64

local raw_cap = sys.memory(RAW_PAGES)
local out_cap = sys.memory(OUT_PAGES)
local raw_at  = sys.memory_map(raw_cap)
local out_at  = sys.memory_map(out_cap)

local capacity = RAW_PAGES * 4096

local source = {
  size = attrs.size,
  read = function (offset, length)
    local out, done = {}, 0
    while done < length do
      local want = length - done
      if want > capacity then want = capacity end
      local got = fs.read_into(path, raw_cap, offset + done, want)
      if not got or got == 0 then break end
      out[#out + 1] = sys.region_read(raw_cap, 0, got)
      done = done + got
    end
    return table.concat(out)
  end,

  -- The same range, straight into a region the caller owns. This is the
  -- path a page takes; `read` above is for the small structural reads the
  -- object layer makes, where a string is the right answer.
  read_into = function (region, offset, length)
    return fs.read_into(path, region, offset, length)
  end,
}

local ok, doc = pcall(pdf.open, source)
if not ok then print("pdfbench: " .. tostring(doc)) return end

local page = doc:page(wanted)
if not page then print("pdfbench: no page " .. wanted) return end

local offset, length = doc:stream_range(page.dict.Contents)
if not offset then print("pdfbench: that page has no content") return end

local HZ = sys.info and sys.info().counter_hz or 62500000

local function ms(ticks)
  return ("%8.1f ms"):format(ticks * 1000 / HZ)
end

local function time(what, fn)
  local t = sys.ticks()
  local value = fn()
  print(("  %-34s %s"):format(what, ms(sys.ticks() - t)))
  return value
end

print(("%s page %d: %d compressed bytes"):format(path, wanted, length))

-- 1. off the disk, the way the parser does it (into a Lua string)
local raw = time("read, into a Lua string", function ()
  return source.read(offset, length)
end)

-- 2. and into a region, with no string anywhere
time("read, into a region", function ()
  local done = 0
  while done < length do
    local got = fs.read_into(path, raw_cap, offset + done, length - done)
    if not got or got == 0 then break end
    done = done + got
  end
  return done
end)

-- 3. inflate, both ways
local plain = time("inflate, string to string", function ()
  return compress.inflate(raw)
end)

local size = time("inflate, region to region", function ()
  return compress.inflate_into(raw_at, length, out_at, OUT_PAGES * 4096)
end)

print(("  %-34s %d -> %d bytes"):format("(inflated)", length, #plain))
if size ~= #plain then
  print(("  MISMATCH: region form gave %d"):format(size))
end

-- 4. the scanner alone: every token parsed and discarded
local counted = time("tokenize only", function ()
  local cur = pdf.cursor(pdf.from_string(plain), 0)
  local n = 0
  while true do
    cur:space()
    if cur:at() == nil then break end
    local got, value = pcall(pdf.parse_value, cur)
    if not got then break end
    if value ~= nil then n = n + 1 end
  end
  return n
end)

print(("  %-34s %d"):format("(tokens)", counted))

-- 5. the C scanner alone, over the region, every token thrown away
local scanned = time("scan in C, over the region", function ()
  local n, offset = 0, 0
  while true do
    local kinds, _, next_at = pdfkit.scan(out_at, size, offset, 1024)
    if #kinds == 0 then break end
    n = n + #kinds
    offset = next_at
  end
  return n
end)

print(("  %-34s %d"):format("(tokens, in C)", scanned))

-- 6. scanner + interpreter, emitting nothing
local fonts = {}
do
  local resources = doc:resolve(page.Resources) or {}
  for name, dict in pairs(doc:resolve(resources.Font) or {}) do
    fonts[name] = pdfpage.font(doc, dict)
  end
end

local shows = 0
time("scan + interpret", function ()
  shows = 0
  pdfpage.walk(out_at, size, fonts, function () shows = shows + 1 end)
end)

print(("  %-34s %d"):format("(shows)", shows))
