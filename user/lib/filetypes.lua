-- What opens what.
--
-- One table, in one place, so that Tracker, the file panel and anything
-- else that opens a file agree - the alternative is each of them holding an
-- opinion and a `.md` opening in the editor from one and the reader from
-- another.
--
-- **By extension, and that is the temporary half.** BeOS did this properly:
-- a file's *type* was an attribute of the file, set when it was written and
-- travelling with it, and a separate table mapped a type to its preferred
-- application. A name is a much weaker thing to ask - renaming a file
-- changes what opens it, and a file with no extension has no type at all.
--
-- The machinery for the real version half exists. `kfs`'s inode carries an
-- `attrs` block that nothing writes yet, and the ramfs has typed attributes
-- already; when the disk grows attributes, `kind_of` reads the type
-- attribute first and falls back to the extension for files that were
-- written before anything set one. That fallback is why this is worth
-- building now rather than waiting.

local filetypes = {}

-- extension -> the program that opens it.
--
-- Deliberately short. Every entry is a claim that an application handles
-- something, and an entry for a program that does not exist is a file that
-- appears to open and does not.
filetypes.by_extension = {
  lua  = "editor",
  txt  = "editor",
  conf = "editor",

  md   = "reader",

  png  = "photo",
  pdf  = "pdfview",

  mp3  = "music",
  wav  = "play",
}

-- What a path is, as a type name rather than a program.
function filetypes.kind_of(path, attrs)
  -- The attribute wins when there is one, because it was set by whoever
  -- wrote the file and a name is only a guess about it. Nothing writes one
  -- yet; this is the branch that will matter and it is here so that adding
  -- attributes to the disk does not mean revisiting every caller.
  if attrs and attrs.type then return attrs.type end

  local name = tostring(path):match("([^/]+)$") or ""

  -- A leading dot is not an extension. `.appearance` is a settings file
  -- whose whole name happens to start with one, and reading it as a file
  -- of type "appearance" put a type in Tracker's Kind column that nothing
  -- in the system has ever heard of.
  local ext = name:sub(2):match("%.([%w]+)$")

  return ext and ext:lower() or nil
end

--
-- Which program opens a path, or nil if nothing claims it.
--
-- Tracker has always called this and this file has never had it, so opening
-- a file from the file manager failed on the call rather than on the answer:
-- `types.opener(full)` on a nil field, every time, for every file. Nothing
-- caught it because nothing tests opening a file from Tracker - the display
-- harness starts applications from the Deskbar, which goes a different way.
--
-- Thin on purpose. `kind_of` already decides what a file *is*, attribute
-- first and extension second, and this is only the lookup from that to a
-- program name. Keeping them apart is what lets the type come from an
-- attribute later without this function changing at all.
--
function filetypes.opener(path, attrs)
  local kind = filetypes.kind_of(path, attrs)

  return kind and filetypes.by_extension[kind] or nil
end

return filetypes
