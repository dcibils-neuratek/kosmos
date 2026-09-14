-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
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

  html = "browser",

  mp3  = "music",
  wav  = "play",

  --
  -- Not an extension, and the first entry here that never was one.
  --
  -- A launcher's type comes from its attributes rather than its name -
  -- `kind_of` below - because the name is the label somebody sees in the
  -- menu and on the desktop, and `Drive.launcher` under an icon is the
  -- machinery showing through. This is the type-to-program half of that.
  --
  -- **`launcheredit` edits one; it does not open one.** Opening a launcher
  -- means starting what it points at, which is what Tracker does when you
  -- double-click it and what the Deskbar does when you choose it - both
  -- test `kind == "launcher"` before they ever ask here. So this is the
  -- answer to "what handles this type", which is the question a Get Info or
  -- a right-click asks.
  --
  launcher = "launcheredit",
}

-- What a path is, as a type name rather than a program.
function filetypes.kind_of(path, attrs)
  -- The attribute wins when there is one, because it was set by whoever
  -- wrote the file and a name is only a guess about it. Nothing writes one
  -- yet; this is the branch that will matter and it is here so that adding
  -- attributes to the disk does not mean revisiting every caller.
  if attrs and attrs.type then return attrs.type end

  --
  -- And a launcher is one, whoever wrote it.
  --
  -- `kind` is what the node *is* - file, directory, launcher - and for a
  -- launcher that is also the whole of what it is worth saying. Reading it
  -- here as well as `type` means every launcher already on a disk is a
  -- launcher to this function without anybody rewriting its attributes:
  -- there were thirty-eight of them on the first machine this ran on, and a
  -- migration to teach them a word they already knew would have been work
  -- for nothing.
  --
  -- New ones set `type` too, so the general branch above stays the one that
  -- matters and this is only the floor under it.
  --
  if attrs and attrs.kind == "launcher" then return "launcher" end

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

--
-- **What a program declares about itself**, in its opening comment block:
-- `kosmos: application`, for one, means it draws a window.
--
-- The rule `/bin`'s server reads, and it has to be the same rule or a program
-- would be an application in the Deskbar and a console program in Tracker
-- (`user/servers/binfs.c`): the block is every line from the top that is
-- empty or begins with `--`, and it ends at the first that is neither - so
-- the same words in a string further down declare nothing.
--
function filetypes.declares(source, word)
  for line in (tostring(source or "") .. "\n"):gmatch("(.-)\n") do
    if line ~= "" and line:sub(1, 2) ~= "--" then
      return false
    end

    if line:match("kosmos:%s*(%a+)") == word then
      return true
    end
  end

  return false
end

--
-- **How to open a path**: the program to start, and what to hand it - or nil
-- when nothing claims it.
--
-- A Lua file is a program, so opening one runs it. An application - its
-- opening comment says `kosmos: application` - starts as itself and opens its
-- own window. Anything else is a console program, and runs in a Terminal of
-- its own, which is where its output has somewhere to go. `source` is the
-- file's beginning, and only a Lua file needs it.
--
-- Editing one is still `opener`'s answer - the editor - which is what
-- Tracker's File menu asks when you choose Edit.
--
function filetypes.how_to_open(path, attrs, source)
  local kind = filetypes.kind_of(path, attrs)

  if kind == "lua" then
    if filetypes.declares(source, "application") then
      return { program = path, args = "" }
    end

    return { program = "terminal", args = path }
  end

  local program = kind and filetypes.by_extension[kind]

  return program and { program = program, args = path } or nil
end

return filetypes
