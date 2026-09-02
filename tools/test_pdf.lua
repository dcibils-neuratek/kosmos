-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The PDF object layer, tested on this machine against a real document.
--
--   build/host/lua tools/test_pdf.lua [file.pdf]
--
-- `pdf.lua` asks its caller for exactly two things - read a range, and say
-- how big the file is - so given those over an ordinary file it can be
-- exercised here in a fraction of a second, against a document somebody
-- actually produced rather than one written to make the test pass.
--
-- That distinction matters more for a PDF than for most formats. The
-- specification permits a great deal that no real producer emits, and real
-- producers emit a great deal the specification barely describes. A fixture
-- would test the parser against my reading of the specification. This tests
-- it against ConvertAPI's output, which is the thing it has to survive.

local path = ... or "/Users/diego/Downloads/pg1727-images-3.pdf"

package.path = "user/lib/?.lua;" .. package.path
local pdf = dofile("user/lib/pdf.lua")

--------------------------------------------------------------------------
-- A source over a file. The whole of what pdf.lua needs from the world.
--------------------------------------------------------------------------

local file = assert(io.open(path, "rb"), "cannot open " .. path)
local size = file:seek("end")

local reads, bytes_read = 0, 0

local source = {
  size = size,
  read = function (offset, length)
    reads = reads + 1
    bytes_read = bytes_read + length
    file:seek("set", offset)
    return file:read(length) or ""
  end,
}

--------------------------------------------------------------------------

local checks, failed = 0, 0

local function ok(what, got, want)
  checks = checks + 1
  if got ~= want then
    failed = failed + 1
    print(("  FAIL: %s\n        got %s, wanted %s")
          :format(what, tostring(got), tostring(want)))
  end
end

local function report(what, value)
  print(("  %-34s %s"):format(what, tostring(value)))
end

--------------------------------------------------------------------------

print(("PDF: %s (%d bytes)"):format(path, size))

local doc = pdf.open(source)

print("\n-- what it found --")
report("version", doc.version)
report("objects in the xref", (function ()
  local n = 0
  for _ in pairs(doc.xref) do n = n + 1 end
  return n
end)())
report("pages", #doc.pages)
report("catalog /Type", doc.catalog.Type)

-- The first page is the cover and the second the frontispiece: both are a
-- single image and neither has a font. Assuming page one has text is
-- exactly the assumption a fixture would have hidden.
local census = { text = 0, image = 0, empty = 0 }
local first_text

for i = 1, #doc.pages do
  local r = doc:resolve(doc:page(i).Resources) or {}

  if doc:resolve(r.Font) then
    census.text = census.text + 1
    first_text = first_text or i
  elseif doc:resolve(r.XObject) then
    census.image = census.image + 1
  else
    census.empty = census.empty + 1
  end
end

report("pages with text", census.text)
report("pages with only an image", census.image)
report("pages with neither", census.empty)
report("first page with text", first_text)

local page = doc:page(first_text)
report("that page /Type", page.dict.Type)
report("its /MediaBox", table.concat(doc:resolve(page.MediaBox) or {}, " "))

local resources = doc:resolve(page.Resources)
local fonts     = doc:resolve(resources.Font)

local names = {}
for name in pairs(fonts) do names[#names + 1] = name end
table.sort(names)
report("its fonts", table.concat(names, " "))

local first = doc:resolve(fonts[names[1]])
report(names[1] .. " /Subtype", first.Subtype)
report(names[1] .. " /BaseFont", first.BaseFont)
report(names[1] .. " /Encoding", doc:resolve(first.Encoding))

local descendant = doc:resolve(doc:resolve(first.DescendantFonts)[1])
report("descendant /Subtype", descendant.Subtype)
report("descendant /CIDToGIDMap", doc:resolve(descendant.CIDToGIDMap))

local descriptor = doc:resolve(descendant.FontDescriptor)
report("an embedded /FontFile2", descriptor.FontFile2 ~= nil)

local offset, length, filter = doc:stream_range(page.dict.Contents)
report("content stream at", offset)
report("content stream length", length)
report("content stream filter", table.concat(filter, " "))

print("\n-- checks --")
ok("the header says 1.4",            doc.version, "1.4")
ok("the catalog is a catalog",       doc.catalog.Type, "Catalog")
ok("254 pages, as /Count says",      #doc.pages, 254)
ok("251 of them carry text",         census.text, 251)
ok("two are a bare image",           census.image, 2)
ok("the page census adds up",        census.text + census.image + census.empty, #doc.pages)
ok("an embedded TrueType font",      descriptor.FontFile2 ~= nil, true)
ok("the page is a page",             page.dict.Type, "Page")
ok("a page inherits its MediaBox",   doc:resolve(page.MediaBox) ~= nil, true)
ok("the font is Type0",              first.Subtype, "Type0")
ok("the encoding is Identity-H",     doc:resolve(first.Encoding), "Identity-H")
ok("the descendant is CIDFontType2", descendant.Subtype, "CIDFontType2")
ok("CIDToGIDMap is Identity",        doc:resolve(descendant.CIDToGIDMap), "Identity")
ok("the content stream is Flate",    filter[1], "FlateDecode")
ok("the content stream has bytes",   length > 0, true)

-- Every page reachable, not only the first: a page tree that works for
-- page 1 and loses page 200 is the failure this catches.
local no_contents, no_resources = 0, 0
for i = 1, #doc.pages do
  local p = doc:page(i)
  if p.dict.Contents == nil then no_contents  = no_contents  + 1 end
  if p.Resources     == nil then no_resources = no_resources + 1 end
end
ok("every page has contents",  no_contents, 0)
ok("every page has resources", no_resources, 0)

-- The point of the whole design: the document is 1.6 MB and the parser
-- never held it. If this ever exceeds the file size, something started
-- reading the lot.
print("\n-- what it cost --")
report("read calls", reads)
report("bytes read", bytes_read)
report("as a share of the file", ("%.1f%%"):format(100 * bytes_read / size))

print()
if failed == 0 then
  print(("PASS: %d checks on the PDF object layer, on this machine."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
