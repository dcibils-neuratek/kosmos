-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- sandbox: a small operating system, in a virtual machine of its own.
--
--   sandbox run alpine               /Home/Sandboxes/alpine, on this console
--   sandbox run alpine --memory 512  with 512 MB of RAM (256 otherwise)
--
-- `docs/virtualization.md` V3, toward Diego's first goal for Sandboxes: "the
-- initial goal for sandboxes is to run a linux distro like alpine". A
-- sandbox is a folder in /Home/Sandboxes holding `Image`, an arm64 Linux
-- kernel, and optionally `initramfs` and `cmdline`; Alpine's comes as a
-- package (`pkg install alpine`).
--
-- **It reads and hands over; `vmm` runs the machine.** The kernel and the
-- initramfs are read into a region that becomes the guest's RAM - through
-- a small window, so their bytes go from the disk server into regions and
-- are never a Lua string - a setup page says where they are
-- (`vmmproto.h`), and `fs.vm_start` starts the monitor with both and this
-- console. Then this waits for it.

local files = use("/Kosmos/Libraries/files.lua")

local words = files.words(args)
local name, ram_mb = nil, 256

local i = 1

while i <= #words do
  local w = words[i]

  if w == "run" then
    name = words[i + 1]
    i = i + 2
  elseif w == "--memory" then
    ram_mb = math.tointeger(tonumber(words[i + 1] or "")) or ram_mb
    i = i + 2
  else
    i = i + 1
  end
end

if not name then
  print("sandbox: sandbox run <name> [--memory MB] - a folder of /Home/Sandboxes")
  return
end

local folder = "/Home/Sandboxes/" .. name
local MB, ALIGN = 1024 * 1024, 2 * 1024 * 1024

local function size_of(path)
  local a = fs.getattr(path)

  return a and math.tointeger(a.size) or nil
end

local kernel_bytes = size_of(folder .. "/Image")

if not kernel_bytes then
  print(("sandbox: %s has no Image - a sandbox is a folder with an arm64 kernel in it"):format(folder))
  sys.exit(1)
end

local initrd_bytes = size_of(folder .. "/initramfs") or 0
local cmdline = fs.read(folder .. "/cmdline")

cmdline = type(cmdline) == "string" and cmdline:gsub("%s+$", "")
          or "console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1"

local ram_bytes = ram_mb * MB
local ram = sys.memory(ram_bytes // 4096)
local window = sys.memory(64)

if not ram or not window then
  print(("sandbox: no room for %d MB of RAM"):format(ram_mb))
  sys.exit(1)
end

-- A file into the RAM at `at`, a window at a time.
local function load(path, bytes, at)
  local done, step = 0, 64 * 4096

  while done < bytes do
    local want = math.min(step, bytes - done)

    if fs.read_into(path, window, done, want) ~= want then
      return false
    end

    sys.region_copy(ram, at + done, window, 0, want)
    done = done + want
  end

  return true
end

if not load(folder .. "/Image", kernel_bytes, 0) then
  print("sandbox: could not read " .. folder .. "/Image")
  sys.exit(1)
end

-- The kernel's own idea of how much room it needs, from its header (the
-- arm64 boot protocol: `image_size` at 16, the magic "ARM\x64" at 56).
local head = sys.region_read(ram, 0, 64)

if head:sub(57, 60) ~= "ARM\x64" then
  print("sandbox: " .. folder .. "/Image is not an arm64 Linux kernel")
  sys.exit(1)
end

local image_size = math.max(string.unpack("<I8", head, 17), kernel_bytes)
local initrd_at = 0

if initrd_bytes > 0 then
  initrd_at = (image_size + ALIGN - 1) // ALIGN * ALIGN

  if initrd_at + initrd_bytes > ram_bytes - ALIGN
     or not load(folder .. "/initramfs", initrd_bytes, initrd_at) then
    print("sandbox: the initramfs would not fit, or could not be read")
    sys.exit(1)
  end
end

local setup = sys.memory(1)

sys.region_write(setup, 0, string.pack("<I4I4I8I8I8I8I8", 0x4b766d6d, 1, ram_bytes,
                                        0, kernel_bytes, initrd_at, initrd_bytes)
                           .. cmdline:sub(1, 511) .. "\0")

print(("sandbox: %s, %d MB, a %d KB kernel%s"):format(name, ram_mb, kernel_bytes // 1024,
      initrd_bytes > 0 and (" and a %d KB initramfs"):format(initrd_bytes // 1024) or ""))

local id, why = fs.vm_start(ram, setup)

if not id then
  print("sandbox: " .. tostring(why))
  sys.exit(1)
end

local _, code = sys.wait()

print(("sandbox: %s ended, code %s"):format(name, tostring(code)))
