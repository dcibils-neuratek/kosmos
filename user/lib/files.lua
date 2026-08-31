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

function files.describe(entry)
  if entry.kind == "directory" then return "directory" end

  local extents = entry.extents and entry.extents > 0
                  and (", %d extent%s"):format(entry.extents,
                                               entry.extents == 1 and "" or "s")
                  or ""

  return ("%d bytes%s"):format(entry.size, extents)
end

return files
