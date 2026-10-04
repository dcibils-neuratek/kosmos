-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What is inside a PDF.
--
--   pdfinfo                     /Home/odyssey.pdf
--   pdfinfo /Home/other.pdf
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

local pdf      = use("/Kosmos/Libraries/pdf.lua")
local compress = use("/Kosmos/Kits/compress")

local path = args[1] or "/Home/odyssey.pdf"

-- A file read a window at a time - sixteen pages, the biggest single read
-- - through the PDF Kit's door, `pdf.file`. This program built its own
-- source until 4 October, as the viewer and `pdfbench` did.
local source, why = pdf.file(path, 16)

if not source then
  print("pdfinfo: " .. tostring(why))
  return
end

--------------------------------------------------------------------------

local ok, doc = pcall(pdf.open, source)

if not ok then
  print("pdfinfo: " .. tostring(doc))
  return
end

print(("%s  %d bytes"):format(path, source.size))
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
