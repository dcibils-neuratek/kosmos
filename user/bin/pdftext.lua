-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A page of a PDF, as text.
--
--   pdftext                        page 3 of /home/odyssey.pdf
--   pdftext /home/odyssey.pdf 12
--
-- The interpreter's output read back as words, which is the cheapest way to
-- see whether it put things where the producer meant them to go. A run in
-- the wrong place is invisible in a list of coordinates and obvious the
-- moment the lines are supposed to be sentences.

local pdf     = use("/lib/pdf.lua")
local pdfpage = use("/lib/pdfpage.lua")

local path   = args[1] or "/home/odyssey.pdf"
local wanted = tonumber(args[2]) or 3

local attrs = fs.getattr(path)
if not attrs then print("pdftext: no such file: " .. path) return end

local buffer = sys.memory(16)
local capacity = 16 * 4096

local source = {
  size = attrs.size,
  read = function (offset, length)
    local out, done = {}, 0
    while done < length do
      local want = length - done
      if want > capacity then want = capacity end
      local got = fs.read_into(path, buffer, offset + done, want)
      if not got or got == 0 then break end
      out[#out + 1] = sys.region_read(buffer, 0, got)
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
if not ok then print("pdftext: " .. tostring(doc)) return end

local page = doc:page(wanted)
if not page then
  print(("pdftext: page %d of %d"):format(wanted, #doc.pages))
  return
end

local started = sys.ticks()
local got, lines = pcall(pdfpage.text, doc, page)

if not got then print("pdftext: " .. tostring(lines)) return end

local elapsed = sys.ticks() - started

print(("%s, page %d of %d"):format(path, wanted, #doc.pages))
print(("%d lines, %d ticks"):format(#lines, elapsed))
print("")

for _, line in ipairs(lines) do
  print(line)
end
