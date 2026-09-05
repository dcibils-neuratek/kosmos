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
-- A size a person reads, rather than a number a filesystem counts in.
--
-- 4510970 is a true answer to how big that song is and a useless one: the
-- question anybody actually asks of a size column is "big or small", and
-- eight digits answer it more slowly than "4.3 MB" does.
--
-- Binary units, because that is what the filesystem allocates in - blocks
-- are 4096 bytes and a "1 KB" that meant 1000 would put a one-block file at
-- 4.1 KB. Written KB and MB rather than KiB and MiB, which is what every
-- desktop shows and what the person reading it expects.
--
-- One decimal under 10 and none above, so the column is at most five
-- characters wide and the numbers line up: 9.4 KB, 66 KB, 4.3 MB. Bytes
-- stay exact below a kilobyte, where the digits are short enough to read
-- and rounding "999 B" to "1.0 KB" would be a lie about a small file.
--------------------------------------------------------------------------

local UNITS = { "KB", "MB", "GB", "TB" }

function files.size(n)
  n = tonumber(n) or 0

  if n < 1024 then return ("%d B"):format(n) end

  local scaled = n / 1024

  for i = 1, #UNITS do
    if scaled < 1024 or i == #UNITS then
      return ((scaled < 10) and "%.1f %s" or "%.0f %s")
             :format(scaled, UNITS[i])
    end

    scaled = scaled / 1024
  end
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

local COPY_PAGES = 256                    -- 1 MB
local COPY_MAX = COPY_PAGES * 4096

local buffer                              -- allocated on the first copy

function files.copy(from, to)
  local attrs = fs.getattr(from)

  if not attrs then return nil, from .. ": no such file" end

  if attrs.kind == "directory" then
    return nil, "copying a directory is not done yet"
  end

  local size = attrs.size or 0

  if size > COPY_MAX then
    return nil, ("%s is %d KB and the copy window is %d KB")
                :format(from, size // 1024, COPY_MAX // 1024)
  end

  -- Allocated once and kept. A file manager copies more than one thing, and
  -- a region per copy is a region per copy that nothing gives back.
  if not buffer then
    buffer = sys.memory(COPY_PAGES)

    if not buffer then return nil, "no memory for a copy buffer" end
  end

  --
  -- The region first, and a plain read if the server does not do regions.
  --
  -- Not every filesystem implements `read_into`. The program store serves
  -- `/bin` out of the image and answers `read` with the source as a value;
  -- it has no pages to hand over, so a copy from `/bin` failed outright -
  -- which is exactly what the first copy attempted here did, and the status
  -- bar said "could not be read" without saying that the *path* was the
  -- reason.
  --
  -- So: the efficient way when it is available, and the ordinary way when
  -- it is not. The ordinary way puts the file through this process's heap,
  -- which is what the region exists to avoid - hence the same size limit
  -- applying to both.
  --
  local got = fs.read_into(from, buffer, 0, size)

  if got then
    local put, why = fs.write_from(to, buffer, got)

    if not put then return nil, to .. ": " .. tostring(why) end

    return put
  end

  local data = fs.read(from)

  if type(data) ~= "string" then
    return nil, from .. ": could not be read"
  end

  local ok, why = fs.write(to, data)

  if not ok then return nil, to .. ": " .. tostring(why) end

  return #data
end

--------------------------------------------------------------------------
-- Moving one, which is not copying it and then deleting it if it can be
-- helped.
--
-- **The cheap path first.** A move inside one filesystem is two directory
-- entries being edited - the name leaves one directory and joins another -
-- and the blocks holding the file never move at all. `rename` is that
-- edit, it happens inside one journal transaction, and it costs the same
-- for a four-megabyte song as for an empty file.
--
-- **The dear path when it has to be.** A filesystem that has no `rename`,
-- or a destination on a *different* filesystem, cannot do that edit: the
-- namespace sends this message to whoever owns `from`, and that server
-- has no way to write into somebody else's tree. So the fallback copies
-- the bytes and then removes the original - and only ever in that order,
-- because a move that removes the source and then fails to write the
-- destination has destroyed the file, which is the one failure a file
-- manager must not have.
--
-- Which one happened is returned, because the caller usually wants to say.
--------------------------------------------------------------------------

function files.move(from, to)
  if from == to then return nil, "it is already there" end

  --
  -- Refused here rather than by the filesystem, because only one of the
  -- two answers is right and the filesystem does not know which. A move of
  -- `/home/a` to `/home/a/b` would either loop or orphan a subtree, and the
  -- string test is exact: both are absolute paths through the same tree.
  --
  if to:sub(1, #from + 1) == from .. "/" then
    return nil, "a directory cannot be moved into itself"
  end

  if fs.getattr(to) then return nil, to .. " already exists" end

  local ok = fs.send(from, { type = "rename", to = to })

  if ok then return true, "moved" end

  local put, why = files.copy(from, to)

  if not put then return nil, why end

  local gone, gwhy = fs.send(from, { type = "delete" })

  if not gone then
    return nil, ("copied it, but the original is still there: %s")
                :format(tostring(gwhy))
  end

  return true, "copied"
end

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
