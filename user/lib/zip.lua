-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Zip archives, made and opened (`roadmap.md` 6v, `docs/rightclick.html`).
--
--   local zip = use("/Kosmos/Libraries/zip.lua")
--   zip.write{ paths = { "/Home/roms" }, to = "/Home/roms.zip" }
--   zip.extract{ from = "/Home/roms.zip", into = "/Home/roms 2" }
--
-- And for a document's file (`docfile.lua`, `docs/write.md`), whose text is
-- made in memory and whose pictures are files somewhere else: an archive
-- from named entries, each a string or a file, and one entry read back.
--
--   zip.write{ entries = { { name = "document", text = text },
--                          { name = "pictures/1.png", path = "/Home/a.png" } },
--              to = "/Home/Letter.write" }
--   local text = zip.read("/Home/Letter.write", "document", 1024 * 1024)
--
-- **The structure here, the bytes in C.** A zip is a local header before
-- each file, the file, and a directory of them all at the end - a few dozen
-- bytes an entry and a decision about each, which is this side of the line
-- `CLAUDE.md` draws. What is a loop over bytes - DEFLATE, inflating, CRC-32,
-- moving a stored file - is the compress kit's, run over regions: a file is
-- read into pages this process owns (`fs.read_into`), deflated from them
-- into the archive's pages, and the archive written from those
-- (`fs.write_from`), so no file's bytes pass through the interpreter, whose
-- heap is 2 MB.
--
-- **What that costs, said.** A file is read whole, and the archive is made
-- whole before it is written - `write_from` writes a file and takes no
-- offset - so both have to fit in memory, and a file over 4 GB or an
-- archive of more than 65,535 things would want ZIP64, which is not here.
-- Each is refused with the reason, before anything is written.
--
-- **Extracting never writes outside the folder it was asked to**: a name
-- with `..` in it, or one that starts at the root, is a zip trying to reach
-- somewhere it was not given, and the archive is refused.
--
-- `progress(files_done, files, bytes_done, bytes)` is called after each
-- file, and `stopped()`, asked before each, ends the work when it says so -
-- what `zip` and `unzip` report and hear from Tracker.

local zip = {}

local kit = use("/Kosmos/Kits/compress")
local files = use("/Kosmos/Libraries/files.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local regions = use("/Kosmos/Libraries/regions.lua")


local LOCAL, CENTRAL, EOCD = 0x04034b50, 0x02014b50, 0x06054b50

-- A name as a place under a folder, or nil (*Opening one*, below); used by
-- the writing half too.
local safe

-- Bit 11: the names are UTF-8, as a FAT long name read here is.
local UTF8 = 0x0800

-- Regions, a file read into one and written from one: `regions.lua`'s,
-- which this file had its own copy of until 4 October.
local region, free = regions.make, regions.free
local read_in, write_out = regions.read_file, regions.write_file

--
-- **A date as a zip keeps it**: MS-DOS's two words, in local time, to two
-- seconds. A file with no date - written before the disk had the clock -
-- is 1 January 1980, the first day the format has.
--
local function dos_time(epoch)
  local t = epoch and clock.at(epoch)

  if not t or t.year < 1980 then return 0, (1 << 5) | 1 end

  return (t.hour << 11) | (t.min << 5) | (t.sec // 2),
         ((t.year - 1980) << 9) | (t.month << 5) | t.day
end

--------------------------------------------------------------------------
-- Making one.
--------------------------------------------------------------------------

--
-- **Entries by name**, in the order given: `{ name, text }` is a string
-- made here, `{ name, path }` a file. A name that would open outside the
-- folder it was opened into, or that is there twice, is refused - an
-- archive this writes is one `extract` would take.
--
local function named(entries)
  local out, seen = {}, {}
  local dev = fs.read("/Devices/clock")
  local now = type(dev) == "table" and dev.epoch or nil

  for _, e in ipairs(entries) do
    local name = type(e.name) == "string" and safe(e.name)

    if not name or name ~= e.name or name:sub(-1) == "/" then
      return nil, tostring(e.name) .. " is not a name a zip may hold"
    end

    if seen[name] then return nil, name .. " is in it twice" end

    seen[name] = true

    if type(e.text) == "string" then
      out[#out + 1] = { name = name, text = e.text, size = #e.text,
                        modified = now }
    else
      local attrs = type(e.path) == "string" and fs.getattr(e.path)

      if not attrs or attrs.kind == "directory" then
        return nil, tostring(e.path) .. ": no such file"
      end

      out[#out + 1] = { path = e.path, name = name, size = attrs.size or 0,
                        modified = attrs.modified }
    end
  end

  return out
end

--
-- Every file and folder under `paths`, named as the archive names them:
-- from the folder that holds each one given, with `/` between, and a
-- folder's name ending in one. In name order, so an archive of the same
-- things is the same archive.
--
local function gather(paths)
  local out = {}

  local function walk(path, name, depth)
    local attrs = fs.getattr(path)

    if not attrs then return nil, path .. ": no such file" end

    if attrs.kind == "directory" then
      if depth > 32 then return nil, path .. ": folders too deep" end

      out[#out + 1] = { path = path, name = name .. "/", dir = true,
                        modified = attrs.modified }

      local names = fs.list(path) or {}

      table.sort(names)

      for _, n in ipairs(names) do
        local ok, why = walk(files.join(path, n), name .. "/" .. n, depth + 1)

        if not ok then return nil, why end
      end
    else
      out[#out + 1] = { path = path, name = name, size = attrs.size or 0,
                        modified = attrs.modified }
    end

    return true
  end

  for _, p in ipairs(paths) do
    local ok, why = walk(p, p:match("([^/]+)/?$") or p, 0)

    if not ok then return nil, why end
  end

  return out
end

--
-- `spec.paths` - or `spec.entries`, by name - into a zip at `spec.to`.
-- True, or nil and why; nothing is written unless all of it is.
--
function zip.write(spec)
  local items, why

  if spec.entries then
    items, why = named(spec.entries)
  else
    items, why = gather(spec.paths or {})
  end

  if not items then return nil, why end

  if #items > 65535 then
    return nil, ("%d things will not go in one zip without ZIP64")
                :format(#items)
  end

  local total, biggest, room, count = 0, 0, 22, 0

  for _, e in ipairs(items) do
    local n = e.size or 0

    if n >= 0xffffffff then
      return nil, (e.path or e.name) .. " is 4 GB or more, which a zip "
                  .. "without ZIP64 cannot hold"
    end

    if not e.dir then count = count + 1 end

    total = total + n
    biggest = math.max(biggest, n)
    room = room + 30 + 46 + 2 * #e.name + n
  end

  local input, out

  input, why = region(math.max(biggest, 1))
  if not input then return nil, why end

  out, why = region(room)
  if not out then free(input) return nil, why end

  local at, central, files_done, bytes_done = 0, {}, 0, 0

  for _, e in ipairs(items) do
    if spec.stopped and spec.stopped() then
      free(input, out)
      return nil, "stopped"
    end

    local time, date = dos_time(e.modified)
    local crc, method, csize, usize = 0, 0, 0, 0
    local head = 30 + #e.name

    if not e.dir and e.size > 0 then
      local got

      if e.text then
        sys.region_write(input.cap, 0, e.text)
        got = #e.text
      else
        got = read_in(e.path, input, e.size)
      end

      if got ~= e.size then
        free(input, out)
        return nil, (e.path or e.name) .. ": could not be read"
      end

      usize = got
      crc = kit.crc32(input.at, usize)

      -- Deflated when that makes it smaller, and stored as it is when it
      -- does not: a photograph or a film is compressed already.
      local data = out.at + at + head
      local squeezed = kit.deflate_into(input.at, usize, data, usize - 1)

      if squeezed then
        method, csize = 8, squeezed
      else
        kit.copy_into(input.at, data, usize)
        method, csize = 0, usize
      end
    end

    sys.region_write(out.cap, at,
      string.pack("<I4I2I2I2I2I2I4I4I4I2I2", LOCAL, 20, UTF8, method,
                  time, date, crc, csize, usize, #e.name, 0) .. e.name)

    central[#central + 1] =
      string.pack("<I4I2I2I2I2I2I2I4I4I4I2I2I2I2I2I4I4", CENTRAL, 20, 20,
                  UTF8, method, time, date, crc, csize, usize, #e.name, 0, 0,
                  0, 0, e.dir and 0x10 or 0, at) .. e.name

    at = at + head + csize

    if not e.dir then
      files_done = files_done + 1
      bytes_done = bytes_done + usize

      if spec.progress then
        spec.progress(files_done, count, bytes_done, total)
      end
    end
  end

  local directory = table.concat(central)

  sys.region_write(out.cap, at, directory
    .. string.pack("<I4I2I2I2I2I4I4I2", EOCD, 0, 0, #items, #items,
                   #directory, at, 0))

  local ok
  ok, why = write_out(spec.to, out, at + #directory + 22)

  free(input, out)

  if not ok then return nil, spec.to .. ": " .. tostring(why) end

  return true
end

--------------------------------------------------------------------------
-- Opening one.
--------------------------------------------------------------------------

--
-- A name as a place under the folder being extracted into, or nil when it
-- would not be under it: from the root, with a drive's letter, or through
-- `..`. Backslashes are what some zips separate with.
--
function safe(name)
  name = name:gsub("\\", "/")

  if name == "" or name:sub(1, 1) == "/" or name:find("^%a:") then
    return nil
  end

  for part in name:gmatch("[^/]+") do
    if part == ".." or part == "." then return nil end
  end

  return name
end

--
-- The archive's entries, read from its directory: `{ name, dir, method,
-- crc, csize, usize, offset }`, and the region holding the whole archive,
-- which the caller gives back. Nil and why for an archive this does not
-- read - damaged, encrypted, split, or compressed some other way.
--
local function read_directory(path)
  local attrs = fs.getattr(path)

  if not attrs or attrs.kind == "directory" then
    return nil, path .. ": no such file"
  end

  local size = attrs.size or 0

  if size < 22 then return nil, path .. " is not a zip" end

  local whole, why = region(size)

  if not whole then return nil, why end

  if read_in(path, whole, size) ~= size then
    free(whole)
    return nil, path .. ": could not be read"
  end

  -- The end record, the last one: it is followed only by a comment.
  local n = math.min(size, 65535 + 22)
  local tail = sys.region_read(whole.cap, size - n, n)
  local at, from = nil, 1

  repeat
    local found = tail:find("PK\5\6", from, true)

    if found then at, from = found, found + 1 end
  until not found

  local function damaged(what)
    free(whole)
    return nil, path .. (what or " is damaged, or is not a zip")
  end

  if not at or at + 21 > #tail then return damaged() end

  local _, disk, cd_disk, _, entries, cd_size, cd_at =
    string.unpack("<I4I2I2I2I2I4I4", tail, at)

  if disk ~= 0 or cd_disk ~= 0 then
    return damaged(" is split across several files, which is not read here")
  end

  if cd_at + cd_size > size then return damaged() end

  local cd = sys.region_read(whole.cap, cd_at, cd_size)
  local out, pos = {}, 1

  for _ = 1, entries do
    if pos + 45 > #cd or string.unpack("<I4", cd, pos) ~= CENTRAL then
      return damaged()
    end

    local _, _, _, flags, method, _, _, crc, csize, usize, nlen, elen, clen,
          _, _, _, offset = string.unpack("<I4I2I2I2I2I2I2I4I4I4I2I2I2I2I2I4I4",
                                          cd, pos)
    local raw = cd:sub(pos + 46, pos + 45 + nlen)

    pos = pos + 46 + nlen + elen + clen

    if flags & 1 == 1 then
      return damaged(" is encrypted, which is not read here")
    end

    if method ~= 0 and method ~= 8 then
      return damaged((" holds %s compressed with method %d, which is not "
                      .. "read here"):format(raw, method))
    end

    local name = safe(raw)

    if not name then
      return damaged(" names " .. raw .. ", which is outside the folder it "
                     .. "would be opened into")
    end

    if offset + 30 > size then return damaged() end

    local sig, _, _, _, _, _, _, _, _, lnlen, lelen =
      string.unpack("<I4I2I2I2I2I2I4I4I4I2I2",
                    sys.region_read(whole.cap, offset, 30))

    if sig ~= LOCAL then return damaged() end

    local data = offset + 30 + lnlen + lelen

    if data + csize > size then return damaged() end

    out[#out + 1] = { name = name, dir = name:sub(-1) == "/",
                      method = method, crc = crc, csize = csize,
                      usize = usize, data = data }
  end

  return out, whole
end

-- What is in an archive, without opening it: the names and their sizes.
function zip.entries(path)
  local list, whole = read_directory(path)

  if not list then return nil, whole end

  free(whole)

  return list
end

--
-- One entry's bytes, from the archive in `whole` into the region `out`:
-- inflated or copied, and held to the CRC the archive gave it. True, or nil
-- and why.
--
local function unpack_entry(whole, e, out)
  if e.method == 8 then
    local done, got = pcall(kit.inflate_into, whole.at + e.data, e.csize,
                            out.at, e.usize, true)

    if not done or got ~= e.usize then
      return nil, e.name .. " would not inflate: " .. tostring(got)
    end
  else
    kit.copy_into(whole.at + e.data, out.at, e.usize)
  end

  if kit.crc32(out.at, e.usize) ~= e.crc then
    return nil, e.name .. " is not what the archive says it is - its CRC "
                .. "differs"
  end

  return true
end

--
-- **One entry of an archive, as a string**: for a document's text, which
-- is small, rather than for a picture, which is not. `most` bytes at most,
-- refused before anything is inflated. Nil and why, naming the archive.
--
function zip.read(path, name, most)
  local list, whole = read_directory(path)

  if not list then return nil, whole end

  local e

  for _, x in ipairs(list) do
    if x.name == name and not x.dir then e = x break end
  end

  if not e then
    free(whole)
    return nil, path .. " holds no " .. name
  end

  if most and e.usize > most then
    free(whole)
    return nil, ("%s: its %s is %d KB, more than the %d KB it may be")
                :format(path, name, e.usize // 1024, most // 1024)
  end

  if e.usize == 0 then
    free(whole)
    return ""
  end

  local out, why = region(e.usize)

  if not out then free(whole) return nil, why end

  local ok
  ok, why = unpack_entry(whole, e, out)

  local bytes = ok and sys.region_read(out.cap, 0, e.usize)

  free(whole, out)

  if not ok then return nil, path .. ": " .. why end

  return bytes
end

--
-- **One folder at the top is the archive's folder**: a zip made from `roms`
-- holds `roms/...`, and opened into a folder called `roms` it should not be
-- `roms/roms/...`. So when every name is under one folder, that folder is
-- the one being made, and the names inside it are what go in it.
--
local function under_one(list)
  local top = nil

  for _, e in ipairs(list) do
    local first = e.name:match("^([^/]+)/")

    if not first then return nil end
    if top and first ~= top then return nil end

    top = first
  end

  return top
end

--
-- `spec.from` opened into a new folder, `spec.into`, which must not exist
-- yet: made here, and taken away again if the work does not finish, since
-- nothing in it was anybody's before.
--
function zip.extract(spec)
  if fs.getattr(spec.into) then
    return nil, spec.into .. " is there already"
  end

  local list, whole = read_directory(spec.from)

  if not list then return nil, whole end

  local top = under_one(list)
  local count, total, biggest = 0, 0, 0

  for _, e in ipairs(list) do
    if not e.dir then
      count = count + 1
      total = total + e.usize
      biggest = math.max(biggest, e.usize)
    end
  end

  local out, why = region(math.max(biggest, 1))

  if not out then free(whole) return nil, why end

  local made = {}

  local function folder(path)
    if made[path] or fs.getattr(path) then made[path] = true return true end

    local up = files.parent(path)

    if up ~= path and not made[up] and not fs.getattr(up) then
      local ok, oops = folder(up)

      if not ok then return nil, oops end
    end

    local ok, oops = fs.send(path, { type = "mkdir" })

    if ok then made[path] = true end

    return ok, oops
  end

  local function fail(what)
    free(whole, out)
    files.remove(spec.into)
    return nil, what
  end

  local ok, oops = folder(spec.into)

  if not ok then free(whole, out) return nil, spec.into .. ": " .. tostring(oops) end

  local files_done, bytes_done = 0, 0

  for _, e in ipairs(list) do
    if spec.stopped and spec.stopped() then return fail("stopped") end

    local name = top and e.name:sub(#top + 2) or e.name
    local dest = name ~= "" and files.join(spec.into, (name:gsub("/$", "")))
                 or spec.into

    if e.dir then
      ok, oops = folder(dest)
      if not ok then return fail(dest .. ": " .. tostring(oops)) end
    else
      ok, oops = folder(files.parent(dest))
      if not ok then return fail(dest .. ": " .. tostring(oops)) end

      if e.usize == 0 then
        ok, oops = fs.write(dest, "")
      else
        ok, oops = unpack_entry(whole, e, out)

        if not ok then return fail(oops) end

        ok, oops = write_out(dest, out, e.usize)
      end

      if not ok then return fail(dest .. ": " .. tostring(oops)) end

      files_done = files_done + 1
      bytes_done = bytes_done + e.usize

      if spec.progress then
        spec.progress(files_done, count, bytes_done, total)
      end
    end
  end

  free(whole, out)

  return true
end

--------------------------------------------------------------------------
-- **A job**: the work handed to `zip` or `unzip` by whoever watches it.
--
-- Tracker writes what to do into a file of its own choosing and starts the
-- program with `--job <file>` - a list of paths as a table, where a command
-- line would have split a name with a space in it. The program keeps
-- `<file>.state` current as it goes, and stops when `<file>.stop` appears,
-- which is Tracker's Stop: a message nobody has to be listening for at the
-- moment it is sent.
--
--   state      "working", "done", "stopped" or "failed"
--   files, files_done, bytes, bytes_done
--   why        what went wrong, when it did
--   made       the archive or the folder, when it is done
--
-- Said at most five times a second, and always at the end: a folder of ten
-- thousand small files would otherwise be ten thousand writes of progress.
--------------------------------------------------------------------------

function zip.job(file, work)
  local spec = fs.read(file)

  if type(spec) ~= "table" then
    print(("%s: no job in it"):format(file))
    return nil
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local said_at = 0
  local state = { state = "working", files = 0, files_done = 0, bytes = 0,
                  bytes_done = 0 }

  local function tell(final)
    local now = sys.ticks()

    if final or now - said_at >= hz // 5 then
      said_at = now
      fs.write(file .. ".state", state)
    end
  end

  tell(true)

  spec.progress = function(files_done, files, bytes_done, bytes)
    state.files_done, state.files = files_done, files
    state.bytes_done, state.bytes = bytes_done, bytes
    tell(false)
  end

  spec.stopped = function()
    return fs.getattr(file .. ".stop") ~= nil
  end

  local ok, why = work(spec)

  if ok then
    state.state, state.made = "done", spec.to or spec.into
  elseif why == "stopped" then
    state.state = "stopped"
  else
    state.state, state.why = "failed", tostring(why)
  end

  tell(true)

  return ok, why
end

return zip
