-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Sandboxes: small operating systems, each in a virtual machine of its own.
--
-- `docs/sandboxes.md` S1. **The one door** for the Sandboxes window and the
-- `sandbox` program alike: what a sandbox is, which ones there are, which
-- are running and how they are doing, and starting, stopping and talking
-- to one. Neither of them does any of it itself.
--
--   /Home/Sandboxes/<name>/sandbox.txt     a sandbox: a line a fact
--   /Home/Sandboxes/Images/<image>/        an image: `Image`, `initramfs`,
--                                          and the package's `package.txt`
--
-- A running sandbox is one process, its monitor `vmm`, found at
-- `/Running/sandbox-<name>` and asked in `vmmproto.h`'s shapes. It keeps
-- its guest's console, so anybody may read it from where they left off.

local files = use("/Kosmos/Libraries/files.lua")

local sandboxes = {}

sandboxes.ROOT = "/Home/Sandboxes"
sandboxes.IMAGES = "/Home/Sandboxes/Images"

local MB, ALIGN = 1024 * 1024, 2 * 1024 * 1024

-- vmmproto.h
local OP_STATE, OP_READ, OP_KEYS, OP_STOP = 1, 2, 3, 4
local RUNNING = 1
local TEXT_MAX = 1536
local REQUEST = "<I4I4I8c" .. TEXT_MAX
local REPLY = "<I4I4i4I4I8I8I8I8I8I8c16"     -- and the text after it
local REPLY_HEAD = string.packsize(REPLY)

-- The monitor's exit codes before its guest ran, as sentences (`vmm.c`).
local MONITOR_SAYS = {
  [11] = "it was given no setup page",
  [12] = "its setup did not describe its RAM",
  [13] = "it could not map the guest's RAM",
  [14] = "the guest's device tree did not fit",
  [15] = "this machine cannot run a guest yet - it was not started with virtualization on",
  [16] = "there was no room for another guest",
  [17] = "the guest's RAM or its processor was refused",
  [18] = "it could not start a thread on processor 0",
  [19] = "its name was taken in /Running - is it running already?",
  [20] = "a folder it was given is not one it can serve",
}

--------------------------------------------------------------------------
-- A sandbox, written down.
--------------------------------------------------------------------------

-- What a sandbox is, from its `sandbox.txt`: a line a fact, as a package's
-- manifest is. `folder` and `port` may come more than once.
function sandboxes.parse(text, name)
  local s = { name = name, image = "alpine", kind = "shell", command = "",
              processors = 1, memory = 256, starts = "by hand",
              folders = {}, ports = {}, network = "none" }

  for line in tostring(text or ""):gmatch("[^\n]+") do
    local key, value = line:match("^(%S+)%s*(.-)%s*$")

    if key == "folder" then
      s.folders[#s.folders + 1] = value
    elseif key == "port" then
      s.ports[#s.ports + 1] = value
    elseif key and key:sub(1, 1) ~= "#" then
      s[key] = value
    end
  end

  s.processors = math.tointeger(tonumber(s.processors)) or 1
  s.memory = math.tointeger(tonumber(s.memory)) or 256
  return s
end

function sandboxes.format(s)
  local lines = { "# A sandbox; docs/sandboxes.md. A line a fact." }

  for _, key in ipairs({ "image", "kind", "command", "processors", "memory", "network", "starts" }) do
    lines[#lines + 1] = ("%-11s %s"):format(key, tostring(s[key] or ""))
  end

  for _, f in ipairs(s.folders or {}) do lines[#lines + 1] = ("%-11s %s"):format("folder", f) end
  for _, p in ipairs(s.ports or {}) do lines[#lines + 1] = ("%-11s %s"):format("port", p) end

  return table.concat(lines, "\n") .. "\n"
end

-- A name a sandbox may have: what fits `sandbox-<name>` in /Running.
function sandboxes.valid_name(name)
  return type(name) == "string" and name:match("^[%w%-]+$") ~= nil and #name <= 15
         and name ~= "Images"
end

function sandboxes.definition(name)
  local text = fs.read(sandboxes.ROOT .. "/" .. name .. "/sandbox.txt")

  if type(text) ~= "string" then return nil end

  return sandboxes.parse(text, name)
end

function sandboxes.save(s)
  if not sandboxes.valid_name(s.name) then
    return nil, "a sandbox's name is letters, digits and hyphens, at most 15"
  end

  files.make_folder(sandboxes.ROOT .. "/" .. s.name)
  return fs.write(sandboxes.ROOT .. "/" .. s.name .. "/sandbox.txt", sandboxes.format(s))
end

function sandboxes.delete(name)
  if sandboxes.state(name) then return nil, name .. " is running - stop it first" end

  return files.remove(sandboxes.ROOT .. "/" .. name)
end

--------------------------------------------------------------------------
-- Images.
--------------------------------------------------------------------------

-- The images there are: a folder each in Images with a kernel in it, its
-- title and size from its package's manifest when it came as one.
function sandboxes.images()
  local out = {}

  for _, name in ipairs(fs.list(sandboxes.IMAGES) or {}) do
    local at = sandboxes.IMAGES .. "/" .. name
    local kernel = fs.getattr(at .. "/Image")

    if kernel then
      local manifest = fs.read(at .. "/package.txt")
      local initrd = fs.getattr(at .. "/initramfs")
      local title = type(manifest) == "string" and manifest:match("\ntitle%s+([^\n]+)") or name
      local version = type(manifest) == "string" and manifest:match("\nabout%s+([^\n]+)") or ""

      out[#out + 1] = { name = name, title = title, about = version,
                        bytes = (kernel.size or 0) + (initrd and initrd.size or 0) }
    end
  end

  table.sort(out, function(a, b) return a.name < b.name end)
  return out
end

--------------------------------------------------------------------------
-- A running sandbox, asked.
--------------------------------------------------------------------------

local function ask(name, op, offset, text)
  local reply, why = fs.raw("/Running/sandbox-" .. name,
                            string.pack(REQUEST, op, #(text or ""), offset or 0,
                                        (text or ""):sub(1, TEXT_MAX)))

  if not reply or #reply < REPLY_HEAD then return nil, why end

  local err, state, code, length, at, written, ram, up, guest, exits, called =
    string.unpack(REPLY, reply)

  return { error = err, running = state == RUNNING, exit_code = code >= 0 and code or nil,
           text = reply:sub(REPLY_HEAD + 1, REPLY_HEAD + length), offset = at,
           written = written, ram = ram, up_ticks = up, guest_ticks = guest,
           exits = exits, name = called:gsub("%z.*", "") }
end

-- How it is doing, or nil when it is not running.
function sandboxes.state(name)
  return ask(name, OP_STATE)
end

-- Its console from `offset`: the text, and where the next read starts.
function sandboxes.read(name, offset)
  local r, why = ask(name, OP_READ, offset or 0)

  if not r then return nil, why end

  return r.text, r.offset + #r.text, r
end

function sandboxes.type(name, text)
  return ask(name, OP_KEYS, 0, text)
end

-- Stops it if it runs; lets its monitor go if it has ended.
function sandboxes.stop(name)
  return ask(name, OP_STOP)
end

-- The running ones, by the names /Running gives them.
function sandboxes.running()
  local out = {}

  for _, entry in ipairs(fs.list("/Running") or {}) do
    local name = tostring(entry):match("^sandbox%-(.+)$")

    if name then out[#out + 1] = name end
  end

  table.sort(out)
  return out
end

-- Every sandbox there is, defined or running, each with its state.
function sandboxes.all()
  local seen, out = {}, {}

  for _, name in ipairs(fs.list(sandboxes.ROOT) or {}) do
    local s = name ~= "Images" and sandboxes.definition(name)

    if s then
      seen[name] = true
      out[#out + 1] = s
    end
  end

  for _, name in ipairs(sandboxes.running()) do
    if not seen[name] then
      out[#out + 1] = sandboxes.parse("", name)
      out[#out].task = true
    end
  end

  for _, s in ipairs(out) do
    s.now = sandboxes.state(s.name)
  end

  table.sort(out, function(a, b) return a.name < b.name end)
  return out
end

--------------------------------------------------------------------------
-- Starting one.
--------------------------------------------------------------------------

--
-- **A folder, read**: `folder /Home/www /srv ro` in a definition, or
-- `--share /Home/www:/srv:ro` at the prompt - the folder in `/Home`, where
-- the guest sees it (`/work` unless said), and `ro` or `rw` (`rw` unless
-- said). Nil and why for one outside `/Home`.
--
function sandboxes.folder(text)
  local host, rest = tostring(text or ""):match("^(%S+)%s*(.-)$")
  local at, mode

  if host and host:find(":", 1, true) and rest == "" then
    host, at, mode = host:match("^([^:]+):?([^:]*):?([^:]*)$")
  else
    at, mode = rest:match("^(%S*)%s*(%S*)$")
  end

  host = host and host:gsub("/+$", "")

  if not host or not host:match("^/Home/[^/]") or host:find("/%.%.") then
    return nil, ("a folder is in /Home, and not %s"):format(tostring(host))
  end

  at = (at and at ~= "") and at or "/work"

  if not at:match("^/[%w/._%-]*$") then return nil, "where the guest sees it is a path: " .. at end

  return { host = host, at = at, read_only = mode == "ro" }
end

function sandboxes.folder_text(f)
  return ("%s %s %s"):format(f.host, f.at, f.read_only and "ro" or "rw")
end

-- Bytes as hexadecimal, which a kernel's command line carries whole.
local function hex(text)
  return (text:gsub(".", function(c) return ("%02x"):format(c:byte()) end))
end

--
-- **Started**: its image's kernel and initramfs read into a region that
-- becomes its RAM - through a window, so the bytes never become a Lua
-- string - a setup page saying where (`vmmproto.h`), and its monitor
-- started through the namespace (`fs.vm_start`), which registers it as
-- `/Running/sandbox-<name>`. Answers once it is found there, or nil and
-- why - the monitor's own reason when it ended before then.
--
function sandboxes.start(s)
  local image = sandboxes.IMAGES .. "/" .. tostring(s.image)
  local kernel = fs.getattr(image .. "/Image")
  local initrd = fs.getattr(image .. "/initramfs")

  if not sandboxes.valid_name(s.name) then return nil, "no such sandbox name: " .. tostring(s.name) end
  if sandboxes.state(s.name) then return nil, s.name .. " is running already" end
  if not kernel then return nil, ("no image called %s - pkg install %s"):format(s.image, s.image) end

  local ram_bytes = s.memory * MB
  local ram, window, setup = sys.memory(ram_bytes // 4096), sys.memory(64), sys.memory(1)

  if not ram or not window or not setup then
    return nil, ("no room for %d MB of RAM"):format(s.memory)
  end

  local function load(path, bytes, at)
    local done, step = 0, 64 * 4096

    while done < bytes do
      local want = math.min(step, bytes - done)

      if fs.read_into(path, window, done, want) ~= want then return false end

      sys.region_copy(ram, at + done, window, 0, want)
      done = done + want
    end

    return true
  end

  if not load(image .. "/Image", kernel.size, 0) then return nil, "could not read " .. image .. "/Image" end

  local head = sys.region_read(ram, 0, 64)

  if head:sub(57, 60) ~= "ARM\x64" then return nil, image .. "/Image is not an arm64 Linux kernel" end

  local initrd_at, initrd_bytes = 0, initrd and initrd.size or 0

  if initrd_bytes > 0 then
    local room = math.max(string.unpack("<I8", head, 17), kernel.size)

    initrd_at = (room + ALIGN - 1) // ALIGN * ALIGN

    if initrd_at + initrd_bytes > ram_bytes - ALIGN or not load(image .. "/initramfs", initrd_bytes, initrd_at) then
      return nil, ("%d MB is not room for %s's kernel and initramfs"):format(s.memory, s.image)
    end
  end

  -- The guest's command and how it says it ended, on its command line: the
  -- command as hexadecimal, so its spaces and quotes are not the line's.
  local token = ("kosmos-end-%x"):format(sys.ticks() % 0x7fffffff)
  -- `quiet` and no `earlycon`: a sandbox's console is its command's, and
  -- the kernel's boot is in it only when the kernel has something to say.
  local line = "console=ttyAMA0 quiet panic=-1 kosmos.name=" .. s.name

  if s.command and s.command ~= "" then
    line = line .. " kosmos.run=" .. hex(s.command) .. " kosmos.end=" .. token
  end

  -- Its folders: each a `share<i>` the monitor serves, and where the guest
  -- mounts it, said on its command line.
  local shares = ""

  for i, text in ipairs(s.folders or {}) do
    local f, why = sandboxes.folder(text)

    if not f then return nil, why end
    if i > 4 then return nil, "four folders at most" end
    if not fs.getattr(f.host) then return nil, f.host .. " is not there" end

    shares = shares .. f.host .. ("\0"):rep(256 - #f.host) .. string.pack("<I4I4", f.read_only and 1 or 0, 0)
    line = line .. (" kosmos.share%d=%s:%s"):format(i - 1, hex(f.at), f.read_only and "ro" or "rw")
  end

  -- Its network: none, or outgoing through the monitor's gateway (S4).
  local outgoing = s.network == "outgoing"

  if outgoing then line = line .. " kosmos.net=1" end

  if #line > 511 then return nil, "its command and folders are too long for a kernel's command line" end

  sys.region_write(setup, 0, string.pack("<I4I4I8I8I8I8I8", 0x4b766d6d, 1, ram_bytes,
                                          0, kernel.size, initrd_at, initrd_bytes)
                             .. line .. ("\0"):rep(512 - #line)
                             .. s.name .. ("\0"):rep(16 - #s.name)
                             .. token .. ("\0"):rep(24 - #token)
                             .. shares .. ("\0"):rep((4 - #(s.folders or {})) * 264)
                             .. string.pack("<I4I4", outgoing and 1 or 0, 0))

  local id, why = fs.vm_start(ram, setup, #(s.folders or {}) > 0, outgoing)

  if not id then return nil, why end

  -- Found in /Running, or ended first and said why.
  for _ = 1, 200 do
    if sandboxes.state(s.name) then return id end

    local done, code = sys.wait(true)

    if done == id then
      return nil, ("its monitor ended: %s"):format(MONITOR_SAYS[code] or ("code " .. tostring(code)))
    end

    sys.sleep(5)
  end

  return nil, "its monitor never answered"
end

--
-- **A task**: an image and a command, run once and thrown away. `say` is
-- given the guest's console as it comes; answers the command's exit code,
-- and how long the sandbox took to come up and to finish, in seconds.
--
function sandboxes.run(image, command, memory, say, folders, network)
  local name = ("task-%d"):format(sys.ticks() % 100000)
  local s = sandboxes.parse("", name)
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local began = sys.ticks()

  s.image, s.command, s.kind, s.memory = image, command or "", "task", memory or 256
  s.folders = folders or {}
  s.network = network and "outgoing" or "none"

  local id, why = sandboxes.start(s)

  if not id then return nil, why end

  local up = (sys.ticks() - began) / hz
  local at, last = 0, nil

  repeat
    local text, next_at, r = sandboxes.read(name, at)

    if not text then break end

    -- The monitor's line about the guest switching itself off, and the
    -- kernel's - which `quiet` does not quieten - are what a task always
    -- does; said for a console watched, not for this.
    text = text:gsub("vmm: the guest switched itself off\n", "")
               :gsub("%[%s*[%d.]+%] reboot: Power down\n", "")

    if #text > 0 then say(text) end

    at, last = next_at, r

    if r.running or #text > 0 then sys.sleep(5) end
  until not r.running and #text == 0

  sandboxes.stop(name)                -- its monitor may go
  return last and last.exit_code or nil, up, (sys.ticks() - began) / hz
end

-- Processor time the guest has had, as a share of `seconds` - for a card.
function sandboxes.busy(before, now)
  if not before or not now or now.up_ticks <= before.up_ticks then return 0 end

  return math.min(1, (now.guest_ticks - before.guest_ticks) / (now.up_ticks - before.up_ticks))
end

return sandboxes
