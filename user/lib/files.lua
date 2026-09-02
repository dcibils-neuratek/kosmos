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

return files
