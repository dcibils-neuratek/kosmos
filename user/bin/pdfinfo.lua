-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What is inside a PDF.
--
--   pdfinfo                     /home/odyssey.pdf
--   pdfinfo /home/other.pdf
--
-- The object layer running on the machine rather than on the host. Until
-- this existed `pdf.lua` had only ever been exercised by `test_pdf.lua`,
-- which reads the file with `io` on a Mac - so everything it proved was
-- about the parser and nothing about whether the parser can reach a file
-- from in here.
--
-- Three separate things are being tried at once, which is deliberate:
--
--   * the object layer, against a real document on a real disk,
--   * `fs.read_into`, which is how anything bigger than a message crosses,
--   * `sys.inflate`, which is new and has never run.
--
-- Any of the three failing says so plainly rather than showing an empty
-- window and leaving which layer broke to be guessed at.

local pdf      = use("/lib/pdf.lua")
local compress = use("/kits/compress")

local path = args[1] or "/home/odyssey.pdf"

--------------------------------------------------------------------------
-- A source over a file in the namespace.
--
-- `pdf.lua` wants `read(offset, length)` and `size`, and this is the whole
-- of what it takes to give it those in here. A file does not arrive as a
-- string: `fs.read` would return the lot, and the lot is 1.6 MB against a
-- 2 MB heap. `fs.read_into` puts a range into pages this process owns and
-- says how many bytes landed, which is `pread` with the buffer named by a
-- capability because a server at EL0 cannot follow the caller's pointer.
--------------------------------------------------------------------------

local WINDOW_PAGES = 16                      -- 64 KB, the biggest single read

local attrs, why = fs.getattr(path)
if not attrs then
  print("pdfinfo: " .. tostring(why or "no such file") .. ": " .. path)
  return
end

local buffer = sys.memory(WINDOW_PAGES)
if not buffer then
  print("pdfinfo: no memory for a read buffer")
  return
end

local capacity = WINDOW_PAGES * 4096

local source = {
  size = attrs.size,

  read = function (offset, length)
    -- Longer than the buffer arrives in pieces. Nothing in the parser asks
    -- for more than a window, but a content stream does.
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
}

--------------------------------------------------------------------------

local ok, doc = pcall(pdf.open, source)

if not ok then
  print("pdfinfo: " .. tostring(doc))
  return
end

print(("%s  %d bytes"):format(path, attrs.size))
print(("  PDF %s, %d pages"):format(doc.version, #doc.pages))

-- The first two pages of the document this was written for are a cover and
-- a frontispiece with no font between them, so "the first page" is the
-- wrong page to report on.
local text_pages, image_pages, first_text = 0, 0, nil

for i = 1, #doc.pages do
  local resources = doc:resolve(doc:page(i).Resources) or {}

  if doc:resolve(resources.Font) then
    text_pages = text_pages + 1
    first_text = first_text or i
  elseif doc:resolve(resources.XObject) then
    image_pages = image_pages + 1
  end
end

print(("  %d with text, %d a bare image"):format(text_pages, image_pages))

if not first_text then
  print("  no page carries a font")
  return
end

local page  = doc:page(first_text)
local box   = doc:resolve(page.MediaBox)
local fonts = doc:resolve(doc:resolve(page.Resources).Font)

local names = {}
for name in pairs(fonts) do names[#names + 1] = name end
table.sort(names)

print(("  page %d: %g x %g points, fonts %s")
      :format(first_text, box[3], box[4], table.concat(names, " ")))

for _, name in ipairs(names) do
  local font = doc:resolve(fonts[name])
  local kid  = doc:resolve(doc:resolve(font.DescendantFonts)[1])

  print(("    %s  %s  %s/%s  embedded=%s")
        :format(name, tostring(font.BaseFont), tostring(font.Subtype),
                tostring(kid and kid.Subtype),
                tostring(kid and doc:resolve(kid.FontDescriptor)
                             and doc:resolve(kid.FontDescriptor).FontFile2 ~= nil)))
end

--------------------------------------------------------------------------
-- And the part that has never run: the content stream, decompressed.
--------------------------------------------------------------------------

local offset, length, filter = doc:stream_range(page.dict.Contents)

if not offset then
  print("  that page has no content stream")
  return
end

print(("  content: %d bytes at %d, filter %s")
      :format(length, offset, table.concat(filter, " ")))

local raw = source.read(offset, length)

if #raw ~= length then
  print(("  FAILED to read the stream: %d bytes of %d"):format(#raw, length))
  return
end

if filter[1] ~= "FlateDecode" then
  print("  not Flate, so nothing to inflate here")
  return
end

local inflated, err = pcall(compress.inflate, raw)

if not inflated then
  print("  inflate failed: " .. tostring(err))
  return
end

print(("  inflated: %d bytes -> %d"):format(length, #err))

-- The first few operators, which is what the interpreter will walk next.
local shown = 0
for line in err:gmatch("[^\n]+") do
  print("    | " .. line)
  shown = shown + 1
  if shown >= 8 then break end
end
