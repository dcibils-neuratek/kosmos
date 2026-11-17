-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A document's file: the document as text and its pictures, in a zip
-- (`docs/write.md`, W1).
--
--   local docfile = use("/Kosmos/Libraries/docfile.lua")
--   docfile.save("/Home/Letter.write", doc, { { name = "pictures/1.png",
--                                               path = "/Home/sea.png" } })
--   local doc, pictures = docfile.open("/Home/Letter.write")
--
--   docfile.keep("write", "/Home/Letter.write", doc, pictures)   -- a kept copy
--   docfile.kept("write")                -- { { file, path, name, epoch, boot } ... }
--   docfile.forget("write", "/Home/Letter.write")                -- saved: gone
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
-- `doc` and `pictures` - `{ name = "pictures/...", path = file }` or
-- `{ name, bytes }` each -
-- into the file at `path`. Written whole in one write, so a save that does
-- not finish leaves the file as it was. True, or nil and why.
--
local function write(path, doc, pictures, extra)
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

    -- A picture as a file somewhere, or as its bytes already read.
    entries[#entries + 1] = { name = p.name, path = p.path, text = p.bytes }
  end

  for _, e in ipairs(extra or {}) do entries[#entries + 1] = e end

  return zip.write{ entries = entries, to = path }
end

function docfile.save(path, doc, pictures)
  return write(path, doc, pictures)
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

--------------------------------------------------------------------------
-- **Kept copies** (`docs/office.md` O4): nothing typed is lost.
--------------------------------------------------------------------------

--
-- A document with changes not saved is kept, whole, in
-- `/Home/System/Recovery/<app>/`, as the file it would be saved as - with
-- one entry more, `kept`, saying whose copy it is: the path it belongs to
-- (none for a document never saved), its name, and when, as the clock's
-- epoch and the epoch the machine started at. Written in one request, so a
-- power cut leaves the old copy or the new one.
--
-- **The application decides when** - after a quiet spell, on its own
-- clock, never inside a paint - and **saving takes the copy away**. What is
-- still there when the application next opens the document is what the
-- machine lost, and the application offers it back.
--
-- Not here yet: versions, every state saved and browsable. A kept copy is
-- the smaller promise, kept first.
--

docfile.RECOVERY = "/Home/System/Recovery"
docfile.KEPT = "kept"

-- An application's folder of kept copies; its name is a word.
local function folder(app)
  if type(app) ~= "string" or not app:find("^[%w][%w_%-]*$") then
    error(("docfile: %q is not an application's name"):format(tostring(app)), 3)
  end

  return docfile.RECOVERY .. "/" .. app
end

--
-- What a document's kept copy is called in its folder: its path with every
-- byte that is not a letter, a digit, a dot or a dash as `_`, so each
-- path has one name and no name reaches past the folder - or `Untitled`,
-- for a document never saved. Pure, for the host's test.
--
function docfile.kept_name(path)
  if type(path) ~= "string" or path == "" then return "Untitled" end

  return (path:gsub("[^%w%.%-]", "_"))
end

-- Now, and when the machine started, as the clock's epochs: nil, nil on a
-- machine with no clock.
local function now()
  local clock = fs.read("/Devices/clock")
  local cpu = fs.read("/Devices/cpu")

  if type(clock) ~= "table" or not clock.epoch then return nil, nil end

  local hz = type(cpu) == "table" and cpu.counter_hz or nil
  local up = hz and hz > 0 and sys.ticks() // hz or 0

  return clock.epoch, clock.epoch - up
end

docfile.now = now

--
-- `doc` and its `pictures` (as `save` takes them) kept for `path` - nil for
-- a document never saved, `name` what a person calls it. True, or nil and
-- why.
--
function docfile.keep(app, path, doc, pictures, name)
  local dir = folder(app)

  -- Each level made, and an answer that it is there already is no answer.
  fs.send("/Home/System", { type = "mkdir" })
  fs.send(docfile.RECOVERY, { type = "mkdir" })
  fs.send(dir, { type = "mkdir" })

  local epoch, boot = now()
  local note = tabletext.encode({ path = path or false, name = tostring(name or "Untitled"),
                                  epoch = epoch or 0, boot = boot or 0 })

  return write(dir .. "/" .. docfile.kept_name(path), doc, pictures,
               { { name = docfile.KEPT, text = note } })
end

--
-- What a kept copy's `kept` entry says, read back: `{ path, name, epoch,
-- boot }`, `path` nil for a document never saved; or nil. Pure.
--
function docfile.kept_note(text)
  if type(text) ~= "string" or not tabletext.is(text) then return nil end

  local t = tabletext.decode(text)

  if type(t) ~= "table" then return nil end

  return { path = type(t.path) == "string" and t.path or nil,
           name = type(t.name) == "string" and t.name or "Untitled",
           epoch = math.tointeger(t.epoch) or 0, boot = math.tointeger(t.boot) or 0 }
end

-- The copies `app` has kept, each its note and the `file` it is in.
function docfile.kept(app)
  local dir = folder(app)
  local out = {}

  for _, n in ipairs(fs.list(dir) or {}) do
    local file = dir .. "/" .. n
    local note = docfile.kept_note(zip.read(file, docfile.KEPT, 64 * 1024))

    if note then
      note.file = file
      out[#out + 1] = note
    end
  end

  return out
end

-- The copy kept for `path` taken away: saved, or turned down.
function docfile.forget(app, path)
  return fs.send(folder(app) .. "/" .. docfile.kept_name(path), { type = "delete" })
end

return docfile
