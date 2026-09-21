-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The disk, from this machine rather than from inside Kosmos.
--
--   build/host/lua tools/kfs.lua create out.img 32
--   build/host/lua tools/kfs.lua ls     out.img /home
--   build/host/lua tools/kfs.lua put    out.img book.pdf /home/book.pdf
--   build/host/lua tools/kfs.lua get    out.img /home/notes.txt notes.txt
--   build/host/lua tools/kfs.lua rm     out.img /home/notes.txt
--
-- **Why this exists.** Kosmos does not use FAT32, and `design.md` gives
-- the reasons - no attributes, no journal, no way to say what a file *is*.
-- That trade has one daily cost, and it is a real one: a Mac cannot mount
-- the image and drop a file onto it.
--
-- This is the answer, and it is a better one than it sounds. `kfs.lua` is
-- the filesystem, it is Lua, and it asks the system for exactly two things
-- - read a block, write a block. Given those over a file instead of a
-- block device, the same code that manages the disk inside the machine
-- manages the image outside it.
--
-- **One implementation, not two.** A separate host tool that understood
-- the format would be a second copy to keep in step, and the two would
-- drift the first time the format changed. Anything this writes that the
-- machine cannot read is a bug in one place, and `tools/test_kfs.lua`
-- tests that one place.
--
-- What is still missing compared to a mounted volume is browsing it in
-- Finder. That would be a FUSE filesystem, and the way to build it without
-- a second implementation is to embed this same file - which is a project
-- for the day somebody wants to drag things onto the icon.

local args = { ... }
local command = args[1]

--------------------------------------------------------------------------
-- A disk, over a file.
--
-- Blocks are read and written where they are rather than the whole image
-- being held in memory: the machine's images are sixty-four megabytes now
-- and will be gigabytes on a real card, and a tool that has to hold the
-- disk to change one file in it is a tool that stops working.
--------------------------------------------------------------------------

local image                              -- the open file
local writable = false

sys = {}

function sys.disk_read(sector, bytes)
  image:seek("set", sector * 512)

  local data = image:read(bytes) or ""

  -- Past the end of a sparse file reads as zeroes, which is what an
  -- unwritten part of a disk is.
  if #data < bytes then
    data = data .. string.rep("\0", bytes - #data)
  end

  return data
end

function sys.disk_write(sector, data)
  if not writable then
    error("this image was opened for reading", 0)
  end

  image:seek("set", sector * 512)
  image:write(data)

  return true
end

function sys.ticks()
  -- Fixed, so two runs over the same inputs produce the same image. An
  -- image that differs because it was built at a different second cannot
  -- be diffed against yesterday's to see what actually changed.
  return 0
end

--------------------------------------------------------------------------

local kfs = assert(loadfile("user/lib/kfs.lua"))()

local function die(message)
  io.stderr:write("kfs: " .. message .. "\n")
  os.exit(1)
end

local function open(path, mode)
  writable = (mode ~= "r")

  image = io.open(path, writable and "r+b" or "rb")

  if not image then die("cannot open " .. path) end
end

local function mounted()
  local sb = kfs.mount()

  if not sb then
    die("that image does not hold a Kosmos filesystem")
  end

  local replayed = kfs.recover(sb)

  if replayed > 0 then
    io.stderr:write(("kfs: the last write to this image did not finish; "
                     .. "%d block(s) replayed\n"):format(replayed))
  end

  return sb
end

-- Directories on the way to a file, made as needed.
local function ensure(sb, path)
  local so_far = ""

  for name in path:sub(2):gmatch("([^/]+)/") do
    so_far = so_far .. "/" .. name

    -- Already there is not an error: two files in one directory both ask.
    if not kfs.find(sb, so_far) then
      local ok, err = kfs.mkdir(sb, so_far, 0)

      if not ok then die("making " .. so_far .. ": " .. tostring(err)) end
    end
  end
end

local function put(sb, host, guest)
  local f = io.open(host, "rb")

  if not f then die("cannot read " .. host) end

  local data = f:read("a")
  f:close()

  ensure(sb, guest)

  local ok, err = kfs.store(sb, guest, data, 0)

  if not ok then die(host .. " -> " .. guest .. ": " .. tostring(err)) end

  return #data
end

-- A file out of the image into one on this machine, a window at a time, so
-- an image holding something large can be taken out of it on a machine that
-- would rather not hold it all at once.
local function take(sb, node, host)
  local out = io.open(host, "wb")

  if not out then die("cannot write " .. host) end

  local at = 0

  while at < node.size do
    local piece = kfs.read_range(sb, node, at, 1024 * 1024)

    if not piece or #piece == 0 then break end

    out:write(piece)
    at = at + #piece
  end

  out:close()
end

--------------------------------------------------------------------------
-- The commands.
--------------------------------------------------------------------------

if command == "create" then
  local out, megabytes = args[2], tonumber(args[3])

  if not out or not megabytes then
    die("usage: create <image> <megabytes> [host:guest ...]")
  end

  --
  -- Every host file is checked *before* anything is written.
  --
  -- This used to format first and read the files as it went, so a typo in a
  -- name - or a file that is simply not in the directory you ran from -
  -- destroyed the disk and then said `cannot read doom1.wad`. The message
  -- is about the file; the damage was to everything that had been on the
  -- image, and there is no undo. It happened.
  --
  -- The pairs are parsed here too, for the same reason: `host:guest` being
  -- malformed is an argument error, and an argument error must not be
  -- reported by a tool that has already reformatted something.
  --
  local pairs_in = {}

  for i = 4, #args do
    local host, guest = args[i]:match("^(.-):(.+)$")

    if not host then die("`" .. args[i] .. "` is not host:guest") end

    local probe = io.open(host, "rb")

    if not probe then
      die("cannot read " .. host .. " - nothing has been written")
    end

    probe:close()

    pairs_in[#pairs_in + 1] = { host = host, guest = guest }
  end

  -- Made at its full size first, so the filesystem lands in a file that is
  -- already as large as it believes the disk to be.
  local f = io.open(out, "wb")

  if not f then die("cannot write " .. out) end

  f:seek("set", megabytes * 1024 * 1024 - 1)
  f:write("\0")
  f:close()

  open(out, "w")

  local sb, err = kfs.mkfs(megabytes * 1024 * 1024 // 512, 0)

  if not sb then die("formatting: " .. tostring(err)) end

  sb = mounted()

  local count, total = 0, 0

  for _, one in ipairs(pairs_in) do
    total = total + put(sb, one.host, one.guest)
    count = count + 1
  end

  image:close()

  print(("%s: %d MB, %d file(s), %d bytes"):format(out, megabytes, count,
                                                   total))
elseif command == "ls" then
  local img, path = args[2], args[3] or "/"

  if not img then die("usage: ls <image> [path]") end

  open(img, "r")

  local sb = mounted()
  local names, err = kfs.list(sb, path)

  if not names then die(path .. ": " .. tostring(err)) end

  for _, name in ipairs(names) do
    local full = (path == "/") and ("/" .. name) or (path .. "/" .. name)
    local number, node = kfs.find(sb, full)

    if number and node.kind == kfs.KIND_DIR then
      print(("  %-28s %10s"):format(name, "folder"))
    else
      print(("  %-28s %10d"):format(name, node and node.size or 0))
    end
  end

  image:close()
elseif command == "put" then
  local img, host, guest = args[2], args[3], args[4]

  if not guest then die("usage: put <image> <host file> <path in image>") end

  open(img, "w")

  local sb = mounted()
  local n = put(sb, host, guest)

  image:close()
  print(("%s -> %s, %d bytes"):format(host, guest, n))
elseif command == "get" then
  local img, guest, host = args[2], args[3], args[4]

  if not host then die("usage: get <image> <path in image> <host file>") end

  open(img, "r")

  local sb = mounted()
  local number, node = kfs.find(sb, guest)

  --
  -- **`number`, not `node`.** `kfs.find` answers `number, node` when it
  -- finds something and `nil, reason` when it does not - so the second
  -- value is a *string* on failure, and `if not node` never fires on it.
  -- This read `local _, node = ...` and then tested `node`, so a file that
  -- was simply not there became `attempt to compare number with nil` four
  -- frames away in `take`, with the reason thrown on the floor.
  --
  -- It hid a plain answer behind a Lua error for as long as it existed,
  -- and it cost an evening on 21 September: `make stick-log` crashed on a
  -- ThinkPad's stick and the message said nothing about the only thing
  -- that was wrong, which was that `/home/diagnose.txt` was not on it.
  --
  if not number then die(guest .. ": " .. tostring(node)) end

  if node.kind == kfs.KIND_DIR then
    die(guest .. " is a folder; `getdir` takes one of those")
  end

  take(sb, node, host)
  image:close()
  print(("%s -> %s, %d bytes"):format(guest, host, node.size))
elseif command == "getdir" then
  -- Every file in one folder, into a folder here that already exists: what
  -- `acpi save` leaves on a stick is one file a table, and `make stick-log`
  -- takes them all with one read of the stick rather than one each.
  -- Folders inside it are not followed.
  local img, guest, host = args[2], args[3], args[4]

  if not host then
    die("usage: getdir <image> <folder in image> <folder here>")
  end

  open(img, "r")

  local sb = mounted()
  local names, err = kfs.list(sb, guest)

  if not names then die(guest .. ": " .. tostring(err)) end

  for _, name in ipairs(names) do
    local number, node = kfs.find(sb, guest .. "/" .. name)

    if number and node.kind ~= kfs.KIND_DIR then
      take(sb, node, host .. "/" .. name)
      print(("%s/%s -> %s/%s, %d bytes"):format(guest, name, host, name,
                                               node.size))
    end
  end

  image:close()
elseif command == "df" then
  local img = args[2]

  if not img then die("usage: df <image>") end

  open(img, "r")

  local sb = mounted()
  local free, err = kfs.free_blocks(sb)

  if not free then die(tostring(err)) end

  image:close()

  -- The same words the machine's `df` ends its line with, so the two can be
  -- held side by side - by a person, and by `run_interchange.py`.
  print(("%d blocks free of %d, %d KB"):format(free, sb.blocks,
                                                free * kfs.BLOCK // 1024))
elseif command == "rm" then
  local img, path = args[2], args[3]

  if not path then die("usage: rm <image> <path in image>") end

  open(img, "w")

  local sb = mounted()
  local ok, err = kfs.unlink(sb, path)

  if not ok then die(path .. ": " .. tostring(err)) end

  image:close()
  print("removed " .. path)
elseif command == "copy" then
  --
  -- **Every file from one image into another, attributes and all.**
  --
  -- Written on 21 September after `get` and `put` were used to move a disk
  -- into a bigger one and quietly made a different disk. The header above
  -- says what this format has that FAT32 does not - "no way to say what a
  -- file *is*" is the thing kfs fixes - and a launcher on the Deskbar's
  -- menu is an ordinary empty file whose *attributes* say `kind =
  -- "launcher"`. Copy the bytes and you have copied nothing that mattered:
  -- the menus came up empty and the Drive on the desktop stopped opening.
  --
  -- So this is a copy that carries what a file *is*, and it exists because
  -- a pair of verbs that each do half the job will be used for the whole
  -- one again.
  --
  local from, to = args[2], args[3]

  if not to then die("usage: copy <image from> <image to>") end

  local function walk(sb, path, out)
    for _, name in ipairs(kfs.list(sb, path) or {}) do
      local full = (path == "/") and ("/" .. name) or (path .. "/" .. name)
      local number, node = kfs.find(sb, full)

      if number then
        if node.kind == kfs.KIND_DIR then
          walk(sb, full, out)
        else
          out[#out + 1] = full
        end
      end
    end

    return out
  end

  open(from, "r")

  local source = mounted()
  local paths = walk(source, "/", {})
  local held = {}

  for _, path in ipairs(paths) do
    local number, node = kfs.find(source, path)

    if not number then die(path .. ": " .. tostring(node)) end

    local data = {}
    local at = 0

    while at < node.size do
      local piece = kfs.read_range(source, node, at, 1024 * 1024)

      if not piece or #piece == 0 then break end

      data[#data + 1] = piece
      at = at + #piece
    end

    --
    -- **The attribute block verbatim, not unpacked and packed again.**
    --
    -- It is `<I4 length>` and then the serialiser's bytes, padded out to
    -- the block - so copying the block copies the attributes exactly,
    -- without this tool needing `sys.pack` at all. The host's Lua is stock
    -- upstream and has no serialiser; teaching it one would be a second
    -- implementation of a format that already has one, which is the thing
    -- this repository keeps refusing to do.
    --
    local raw = nil

    if node.attrs ~= 0 then
      raw = kfs.read_block(node.attrs)
    end

    held[#held + 1] = { path = path, data = table.concat(data), raw = raw }
  end

  image:close()

  open(to, "w")

  local target = mounted()
  local carried = 0

  for _, file in ipairs(held) do
    ensure(target, file.path)

    local ok, err = kfs.store(target, file.path, file.data, 0)

    if not ok then die(file.path .. ": " .. tostring(err)) end

    if file.raw then
      local number, node = kfs.find(target, file.path)
      local block, aerr = kfs.alloc_block(target)

      if not block then die(file.path .. ": " .. tostring(aerr)) end

      local fine, werr = kfs.write_block(block, file.raw)

      if not fine then die(file.path .. ": " .. tostring(werr)) end

      node.attrs = block

      local ok2, ierr = kfs.write_inode(target, number, node)

      if not ok2 then die(file.path .. ": " .. tostring(ierr)) end

      carried = carried + 1
    end
  end

  image:close()
  print(("%s -> %s: %d files, %d of them with attributes")
        :format(from, to, #held, carried))
else
  print("usage: kfs.lua <create|ls|put|get|rm|copy> <image> ...")
  print("")
  print("  create <image> <MB> [host:guest ...]   format, and fill it")
  print("  ls     <image> [path]                  what is in there")
  print("  put    <image> <host> <path>           a file in")
  print("  get    <image> <path> <host>           a file out")
  print("  rm     <image> <path>                  a file gone")
  print("  df     <image>                         how much room is left")
  print("  copy   <image from> <image to>         every file, attributes too")
  os.exit(command and 1 or 0)
end
