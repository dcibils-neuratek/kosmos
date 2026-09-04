-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The disk, from this machine rather than from inside Kosmos.
--
--   build/host/lua tools/kfs.lua create out.img 64
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
    local _, node = kfs.find(sb, full)

    if node and node.kind == kfs.KIND_DIR then
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
  local _, node = kfs.find(sb, guest)

  if not node then die(guest .. ": no such file") end

  local out = io.open(host, "wb")

  if not out then die("cannot write " .. host) end

  -- A window at a time, so an image holding something large can be taken
  -- out of it on a machine that would rather not hold it all at once.
  local at = 0

  while at < node.size do
    local piece = kfs.read_range(sb, node, at, 1024 * 1024)

    if not piece or #piece == 0 then break end

    out:write(piece)
    at = at + #piece
  end

  out:close()
  image:close()
  print(("%s -> %s, %d bytes"):format(guest, host, node.size))
elseif command == "rm" then
  local img, path = args[2], args[3]

  if not path then die("usage: rm <image> <path in image>") end

  open(img, "w")

  local sb = mounted()
  local ok, err = kfs.unlink(sb, path)

  if not ok then die(path .. ": " .. tostring(err)) end

  image:close()
  print("removed " .. path)
else
  print("usage: kfs.lua <create|ls|put|get|rm> <image> ...")
  print("")
  print("  create <image> <MB> [host:guest ...]   format, and fill it")
  print("  ls     <image> [path]                  what is in there")
  print("  put    <image> <host> <path>           a file in")
  print("  get    <image> <path> <host>           a file out")
  print("  rm     <image> <path>                  a file gone")
  os.exit(command and 1 or 0)
end
