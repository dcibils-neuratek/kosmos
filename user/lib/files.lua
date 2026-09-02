-- Listing a directory, in the one place that has to know how.
--
-- Every path question a file manager and a file panel both ask: what is in
-- here, what is above here, and what is this. Shared because the two would
-- otherwise disagree about the answers, and the way that shows up is a
-- panel that will not open a directory the browser lists.
--
-- Nothing here caches. A directory read twice is asked twice, because
-- something else may have written a file in between and a file manager that
-- shows a stale directory is worse than a slow one.

local files = {}

-- "/home/a/b" -> "/home/a", and "/home" -> "/". The root's parent is the
-- root, so walking up from it stops rather than producing "".
function files.parent(path)
  local up = tostring(path):match("^(.*)/[^/]+/?$")

  if not up or up == "" then return "/" end

  return up
end

function files.join(dir, name)
  if dir == "/" then return "/" .. name end

  return (dir:gsub("/$", "")) .. "/" .. name
end

-- What is in a directory: name, kind and size, directories first and each
-- group in name order.
--
-- Directories first because that is what every file manager since the
-- eighties has done and because a directory is a place rather than a thing
-- - a list that interleaves them makes you read the whole list to find
-- where you can go next.
function files.entries(path)
  local names, err = fs.list(path)

  if not names then return nil, err end

  local out = {}

  for _, name in ipairs(names) do
    local attrs = fs.getattr(files.join(path, name))

    out[#out + 1] = {
      name = name,
      kind = attrs and attrs.kind or "file",
      size = attrs and attrs.size or 0,
      extents = attrs and attrs.extents,
    }
  end

  table.sort(out, function(a, b)
    local a_dir = (a.kind == "directory")
    local b_dir = (b.kind == "directory")

    if a_dir ~= b_dir then return a_dir end

    return a.name < b.name
  end)

  return out
end

-- How an entry reads in a list. The trailing slash is the only marker, and
-- it is enough: it is what a path would need anyway.
function files.label(entry)
  if entry.kind == "directory" then return entry.name .. "/" end

  return entry.name
end


--------------------------------------------------------------------------
-- Copying a file, which is the one thing a file manager must be able to do
-- and this system could not.
--
-- Through a shared region rather than through Lua. `fs.read` hands back a
-- string and a process has a 2 MB heap, so a copy that went that way would
-- hold the whole file twice - once as the string and once inside the write
-- - and fall over on anything real. `read_into` puts the bytes in pages
-- this process owns and `write_from` sends the same pages out; the content
-- never enters the interpreter at all.
--
-- **One region, one read, one write, and a limit that is honest about
-- why.** `write_from` writes a whole file and takes no offset, so a copy
-- larger than the region cannot be assembled - it would need either an
-- append or a second region, and both are a change to the filesystem
-- protocol rather than to this function. Until then a file bigger than the
-- window is refused with its size, which is a copy that did not happen
-- rather than one that half did.
--------------------------------------------------------------------------
-- The icons.
--
-- Vendored pictures, not shapes drawn from `g:fill`. What was here first
-- was the latter - a folder built from a tab and five bevel edges, a
-- document from a sheet, a three-step fold and three grey lines - and it
-- had two problems. It looked hand-drawn, because it was. And it could
-- only ever say "directory" or "not a directory": drawing a *script*
-- distinctly would have meant drawing a script, and then an image, and
-- then a font, each in eleven rectangles, by hand.
--
-- So: `assets/icons/`, which is the Tango Icon Library at the size it was
-- drawn for, public domain, byte for byte as released. Nothing converts
-- them. `gfx.png` decodes PNG already and `surface:blend` composites
-- source-over already, both in C because both are pixel loops, so the
-- release's own bytes are what the system carries.
--
-- The names are freedesktop's, which is the point of using them: `ICONS`
-- below maps what Kosmos knows about a file onto a name that a *different*
-- icon theme would also answer to, so a second theme is seven files and no
-- new code.
--------------------------------------------------------------------------

files.ICON = 32

-- What Kosmos can actually tell apart, and nothing else. An entry here for
-- a distinction the system cannot make would be a picture that lies about
-- what it knows - which is the same rule `filetypes.by_extension` follows
-- and for the same reason.
local ICONS = {
  directory = "folder",

  lua  = "text-x-script",
  txt  = "text-x-generic",
  conf = "text-x-generic",
  md   = "text-x-generic",

  png  = "image-x-generic",

  ttf  = "font-x-generic",
  otf  = "font-x-generic",
  bdf  = "font-x-generic",
}

-- Two directories that are not just directories. Every desktop since the
-- Macintosh has drawn home and a volume differently from a folder, because
-- they are places rather than containers, and Tango ships both.
local BY_PATH = {
  ["/home"] = "user-home",
  ["/data"] = "drive-harddisk",
}

--
-- `path` is optional and only decides between a folder and a place.
--
-- Nothing is decoded here. `gc:icon` sends the *name* and the compositor
-- loads and caches the picture, which is the same division the rest of this
-- kit follows: an application says what it wants drawn and never holds what
-- is drawn. It also means seven icons are decoded once for the whole
-- desktop rather than once per application that shows a directory.
--
function files.icon(g, x, y, entry, path)
  local name

  if path and BY_PATH[path] then
    name = BY_PATH[path]
  elseif entry.kind == "directory" then
    name = ICONS.directory
  else
    local ext = tostring(entry.name):sub(2):match("%.([%w]+)$")

    name = ext and ICONS[ext:lower()] or "text-x-generic"
  end

  g:icon(x, y, name .. ".png", files.ICON)
end

return files
