-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Regions: pages this process owns, mapped, and given back - and a file
-- read into one, or written from one.
--
--   local regions = use("/Kosmos/Libraries/regions.lua")
--   local r = regions.make(bytes)           { cap, at, size }, or nil and why
--   local b = regions.unmapped(bytes)       { cap, size }: no address here
--   regions.read_file(path, r, size)        how many bytes landed, or nil and why
--   regions.fill(path, r, size)             the same, a window at a time
--   local r, size = regions.read_whole(path)    a file in a region its size
--   regions.write_file(path, r, size)       how many bytes went, or nil and why
--   regions.write_string(path, text)        the same, from a string
--   regions.free(r, ...)
--
-- **What a library does when its bytes are not to pass through the
-- interpreter**: a zip's files, a PDF's fonts and pages - a loop over bytes
-- in C, over pages the process owns, with Lua holding only the capability
-- and the address. It was written twice, in `zip.lua` and again in
-- `pdfwrite.lua`, and that second copy is what `CLAUDE.md`'s premise - kits
-- supply, applications orchestrate, a second copy is a defect - was written
-- the day of (4 October 2026). One copy, here, and both use it.
--
-- **And so does everything else that made a region by hand**: the PDF
-- reader's pages and fonts, a film's reads, the camera's and a MIDI
-- keyboard's rings, a direct window's surfaces, Groove's export, a copy,
-- the wallpaper, the screen `screenshot` and `vncd` borrow, the files `log
-- save`, `diagnose`, `acpi` and telnet's `put` write, the textures Solar
-- System reads, and the WAD, pak and ROM the three games load. Several
-- kept the pages when the mapping failed, or never asked whether it had.

local regions = {}

regions.PAGE = 4096

-- How a file comes in a window at a time: a quarter of a megabyte, which is
-- what the three games used when each had its own copy of `fill`.
regions.WINDOW = 256 * 1024

--
-- `bytes` rounded up to whole pages and asked for, with no address in this
-- process: `{ cap, size }`, or nil and why. Never less than a page, so an
-- empty thing still has somewhere to be.
--
-- **The why carries the kernel's reason beside the size**, because "no
-- memory" is the same sentence whether the machine is out of pages or this
-- process is out of capability slots, and those are different problems -
-- telling them apart took `pdfpage.lua` an evening once.
--
-- **For a region nothing here needs an address for**: one a server fills
-- or empties, or `sys.region_read`, `region_write` and `region_copy` reach
-- by its capability, which map it themselves when they first need to.
--
function regions.unmapped(bytes)
  local pages = math.max(1, (bytes + regions.PAGE - 1) // regions.PAGE)
  local cap, why = sys.memory(pages)

  if not cap then
    return nil, ("no region of %d KB: %s"):format(pages * regions.PAGE // 1024,
                                                   tostring(why))
  end

  return { cap = cap, size = pages * regions.PAGE }
end

--
-- The same, mapped: `{ cap, at, size }`, for C in this process to work on
-- at `at` - a decoder, a surface, a game's engine. Nil and why, with the
-- pages given back, when it will not map.
--
function regions.make(bytes)
  local r, why = regions.unmapped(bytes)

  if not r then return nil, why end

  local at, oops = sys.memory_map(r.cap)

  if not at then
    sys.release(r.cap)
    return nil, ("a region of %d KB that would not map: %s")
                :format(r.size // 1024, tostring(oops))
  end

  r.at = at

  return r
end

--
-- Each region given back, mapped or not; nil ones are passed over. Nothing
-- may use its address after: `sys.release` unmaps it.
--
-- **This kept a mapped region's pages until 5 October 2026**: `sys.memory_map`
-- went to the kernel without a record, and a mapping holds its region
-- (`sharemap.h`), so `free` dropped the capability and the pages stayed.
-- Found by this file's own move, and mended in `sys_user.c`, where every
-- mapping is now remembered and `sys.release` takes it down.
--
function regions.free(...)
  for i = 1, select("#", ...) do
    local r = select(i, ...)
    if r then sys.release(r.cap) end
  end
end

--
-- **A file into a region, and a region into a file**: straight where the
-- filesystem hands over pages, and through a string where it does not -
-- `/Home` in memory on a machine with no disk, `/Temporary`, or the program
-- store, which answers `read` with a value and has no pages to hand over -
-- which is what `files.copy` does, and for its reason: those hold small
-- files, so the string is small too. A string too large for the heap is a
-- refusal rather than an error raised in the caller.
--
-- How many bytes landed, or nil and why.
--
function regions.read_file(path, r, size)
  local got, why = fs.read_into(path, r.cap, 0, size)

  if got then return got end

  local ok, data, oops = pcall(fs.read, path)

  if not ok then return nil, tostring(data) end

  if type(data) ~= "string" then
    return nil, tostring(oops or why or "not a file that can be read")
  end

  local put, refused = sys.region_write(r.cap, 0, data)

  if not put then return nil, refused end

  return #data
end

--
-- How many bytes went, or nil and why.
--
function regions.write_file(path, r, size)
  local ok, why = fs.write_from(path, r.cap, size)

  if ok then return ok end

  local read, data = pcall(sys.region_read, r.cap, 0, size)
  local put, oops

  if read and type(data) == "string" then put, oops = fs.write(path, data) end

  if put then return size end

  return nil, oops or why
end

--
-- **A string into a file, through pages**: a log of a quarter of a megabyte,
-- an ACPI table, a file sent over a connection - where a message holds two
-- kilobytes. What `log save`, `diagnose`, `acpi` and telnet's `put` each
-- did by hand. A region the string's size, unmapped, and given back after.
--
function regions.write_string(path, text)
  local r, why = regions.unmapped(#text)

  if not r then return nil, why end

  local put, oops = sys.region_write(r.cap, 0, text)

  if put then put, oops = regions.write_file(path, r, #text) end

  regions.free(r)

  return put, oops
end

--
-- **A file into a region already made, a window at a time**, the bytes
-- never a Lua string: each window lands in a scratch region and is copied
-- across by `sys.region_copy`, region to region.
--
-- The scratch is there because `fs.read_into` takes an offset into the
-- *file* and always writes at the start of the region - there is no offset
-- into the region in the protocol. A loop that forgets it writes every
-- window over the last, and what ends up at the front is the file's final
-- window: Doom's WAD began "02_8" where "IWAD" belongs until that was
-- found. The window manager's wallpaper and `pdfbench` looped that way
-- too, and were right only while the first read brought the whole file.
--
-- How many bytes landed, which is `size`, or nil and why.
--
function regions.fill(path, r, size, window)
  window = window or regions.WINDOW

  local scratch, why = regions.unmapped(math.min(window, size))

  if not scratch then return nil, why end

  local done = 0

  while done < size do
    local got = fs.read_into(path, scratch.cap, done,
                             math.min(window, size - done))

    if not got or got == 0 then
      regions.free(scratch)
      return nil, ("%s stopped after %d of %d bytes"):format(path, done, size)
    end

    local copied, failed = sys.region_copy(r.cap, done, scratch.cap, 0, got)

    if not copied then
      regions.free(scratch)
      return nil, tostring(failed)
    end

    done = done + got
  end

  regions.free(scratch)

  return done
end

--
-- **A whole file in a region of its own**, mapped, for C to read where it
-- lies: Doom's WAD, Quake's pak, a Super Nintendo ROM - files of megabytes
-- on a Lua heap that starts at two. The region and the file's size, or nil
-- and why.
--
function regions.read_whole(path, window)
  local attrs, why = fs.getattr(path)

  if not attrs or attrs.kind == "directory" then
    return nil, ("%s: %s"):format(path, tostring(why or "no such file"))
  end

  local size = attrs.size or 0
  local r, oops = regions.make(size)

  if not r then return nil, oops end

  local got, failed = regions.fill(path, r, size, window)

  if not got then
    regions.free(r)
    return nil, failed
  end

  return r, size
end

return regions
