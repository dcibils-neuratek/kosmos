-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Regions: pages this process owns, mapped, and given back - and a file
-- read into one, or written from one.
--
--   local regions = use("/Kosmos/Libraries/regions.lua")
--   local r = regions.make(bytes)           { cap, at, size }, or nil and why
--   regions.read_file(path, r, size)        how many bytes landed
--   regions.write_file(path, r, size)       true, or nil and why
--   regions.free(r, ...)
--
-- **What a library does when its bytes are not to pass through the
-- interpreter**: a zip's files, a PDF's fonts and pages - a loop over bytes
-- in C, over pages the process owns, with Lua holding only the capability
-- and the address. It was written twice, in `zip.lua` and again in
-- `pdfwrite.lua`, and that second copy is what `CLAUDE.md`'s premise - kits
-- supply, applications orchestrate, a second copy is a defect - was written
-- the day of (4 October 2026). One copy, here, and both use it.

local regions = {}

regions.PAGE = 4096

--
-- `bytes` rounded up to whole pages, asked for and mapped: `{ cap, at,
-- size }`, or nil and why. Never less than a page, so an empty thing still
-- has somewhere to be.
--
function regions.make(bytes)
  local pages = math.max(1, (bytes + regions.PAGE - 1) // regions.PAGE)
  local cap = sys.memory(pages)

  if not cap then
    return nil, ("no memory for %d KB"):format(pages * regions.PAGE // 1024)
  end

  local at = sys.memory_map(cap)

  if not at then
    sys.release(cap)
    return nil, "could not map a region"
  end

  return { cap = cap, at = at, size = pages * regions.PAGE }
end

-- Each region given back; nil ones are passed over.
function regions.free(...)
  for i = 1, select("#", ...) do
    local r = select(i, ...)
    if r then sys.release(r.cap) end
  end
end

--
-- **A file into a region, and a region into a file**: straight where the
-- filesystem hands over pages, and through a string where it does not -
-- `/Home` in memory on a machine with no disk, or `/Temporary` - which is
-- what `files.copy` does, and for its reason: those hold small files, so
-- the string is small too.
--
function regions.read_file(path, r, size)
  local got = fs.read_into(path, r.cap, 0, size)

  if got then return got end

  local data = fs.read(path)

  if type(data) ~= "string" then return nil end

  sys.region_write(r.cap, 0, data)

  return #data
end

function regions.write_file(path, r, size)
  local ok, why = fs.write_from(path, r.cap, size)

  if ok then return ok end

  local put, oops = fs.write(path, sys.region_read(r.cap, 0, size))

  if put then return put end

  return nil, oops or why
end

return regions
