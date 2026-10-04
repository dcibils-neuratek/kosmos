-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A document's file: the document as text and its pictures, in a zip
-- (`docs/write.md`, W1).
--
--   local docfile = use("/Kosmos/Libraries/docfile.lua")
--   docfile.save("/Home/Letter.write", doc, { { name = "pictures/1.png",
--                                               path = "/Home/sea.png" } })
--   local doc, pictures = docfile.open("/Home/Letter.write")
--
-- **A kit**: Kosmos Write's `.write` first, and Present's and Sheets' files
-- after it - each is a document and the pictures it shows, and none of them
-- needs to know how those are kept.
--
-- **A zip, as Pages' own files are** (Diego's third answer, 4 October): an
-- entry `document` first, the document as `tabletext` - a table as text a
-- person can read, values only, never run - and each picture as it arrived,
-- under `pictures/`. Any zip program opens one, and a person who does finds
-- a document they can read.
--
-- What is in the file is not judged here beyond its being one: that is the
-- reader's - `writedoc.check` for Write - since only it knows what a field
-- means.

local docfile = {}

local zip = use("/Kosmos/Libraries/zip.lua")

docfile.DOCUMENT = "document"

-- The document's text, at most: refused before it is inflated, so a file
-- that claims a gigabyte of text costs nothing to turn away. A hundred pages
-- are about 500 KB of it and 870 KB as the tables it becomes (`testing.md`
-- 18.377), so this is about eight hundred pages.
docfile.MOST = 4 * 1024 * 1024

-- A picture's name inside the file: `pictures/` and a plain file name.
local function picture_name(n)
  return type(n) == "string" and #n <= 128
         and n:find("^pictures/[%w][%w_%-%.]*$") ~= nil
         and not n:find("%.%.")
end

docfile.picture_name = picture_name

--
-- `doc` and `pictures` - `{ name = "pictures/...", path = file }` each -
-- into the file at `path`. Written whole in one write, so a save that does
-- not finish leaves the file as it was. True, or nil and why.
--
function docfile.save(path, doc, pictures)
  local text = tabletext.encode(doc)

  if #text > docfile.MOST then
    return nil, ("the document is %d KB as text, and a document file holds "
                 .. "at most %d KB"):format(#text // 1024, docfile.MOST // 1024)
  end

  local entries = { { name = docfile.DOCUMENT, text = text } }

  for _, p in ipairs(pictures or {}) do
    if not picture_name(p.name) then
      return nil, tostring(p.name) .. " is not a picture's name in a document"
    end

    entries[#entries + 1] = { name = p.name, path = p.path }
  end

  return zip.write{ entries = entries, to = path }
end

--
-- The document in the file at `path`, as it was written - not yet checked
-- - and the names of the pictures it holds; or nil and why, which names the
-- file.
--
function docfile.open(path)
  local list, why = zip.entries(path)

  if not list then return nil, why end

  local text
  text, why = zip.read(path, docfile.DOCUMENT, docfile.MOST)

  if not text then return nil, why end

  if not tabletext.is(text) then
    return nil, path .. ": its document is not a stored table"
  end

  local doc
  doc, why = tabletext.decode(text)

  if not doc then return nil, path .. ": its document, " .. why end

  local pictures = {}

  for _, e in ipairs(list) do
    if picture_name(e.name) then pictures[#pictures + 1] = e.name end
  end

  return doc, pictures
end

return docfile
