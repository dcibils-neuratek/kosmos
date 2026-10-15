-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Linux
-- kosmos: section applications
-- **Linux in a Kosmos window** (`docs/linux.md`, `docs/linux.html`): Alpine
-- in a sandbox, a web browser on it, its screen in this window.
--
-- Diego, 14 October 2026: "can we prioritize linux on kosmos so we can run
-- a web browser ?" - in `cage`, on a disk in `/Sandboxes`, 2 GB; and the
-- browser Epiphany rather than Chromium ("chrome is too big", "Your call").
--
-- **The screen** (L2): the guest draws through virtio-gpu, the sandbox's
-- monitor lays each flush into a region as Kosmos's own pixels
-- (`vmmscreen.h`), and this window wraps the region as a surface and blits
-- it when the monitor's count of frames moved - on the window's own clock,
-- never waited for. No pixel passes through this file.
--
-- **The keyboard and the pointer** (L3): each raw key and each move, press
-- and turn of the wheel over the picture, as Linux's own input events, into
-- the ring in the screen region's first page, which the monitor feeds to the
-- guest's virtio-input keyboard and tablet. Kosmos's own keys - Super and
-- its shortcuts - are the window manager's and never arrive here.
--
-- **Linux from its own disk** (L4): the first time, a disk made in
-- `/Sandboxes` and Alpine installed onto it, its progress in the header;
-- every time after, booted from it into `cage` and the browser. Its kernel's
-- modules come from the image's modloop, through a read-only share.
--
--   linux                  Linux and the browser, 1024 by 768
--   linux 1280x800         at that size (a width a multiple of 16)
--   linux --memory 512     with that much memory, in MB (2048)
--   linux --app foot       running that in `cage` rather than the browser
--   linux --packages "..." what the first run installs
--   linux --repo URL       the Alpine repository it installs from
--   linux --disk PATH      its disk (`/Sandboxes/linux.img`), --disk-mb N
--   linux --console        Linux's console on its screen, no disk (L2, L3)
--   linux --tell           and say what it does, for a test to see

local ui = use("/Kosmos/Libraries/ui.lua")
local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
local sandboxes = use("/Kosmos/Libraries/sandboxes.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local L = ui.layout

local ARGS = tostring(args or "")
local TELL = ARGS:find("--tell", 1, true) ~= nil
local CONSOLE = ARGS:find("--console", 1, true) ~= nil

-- `--name value`, or `--name "a value with spaces"`. The name's own `-`
-- escaped, since in a pattern it is a repetition: `disk-mb` was never found.
local function option(name)
  local n = name:gsub("%-", "%%-")

  return ARGS:match("%-%-" .. n .. "%s+\"([^\"]*)\"") or ARGS:match("%-%-" .. n .. "%s+(%S+)")
end

local SW, SH = 1024, 768

do
  local w, h = ARGS:match("(%d+)x(%d+)")

  if w then SW, SH = math.tointeger(tonumber(w)), math.tointeger(tonumber(h)) end
end

local MEMORY = math.tointeger(tonumber(option("memory") or "")) or 2048
local APP = option("app") or "epiphany"
local PACKAGES = option("packages")
                 or "alpine-base eudev seatd cage mesa-dri-gallium font-dejavu " .. APP
local REPO = option("repo") or "http://dl-cdn.alpinelinux.org/alpine/v3.24"
local DISK_MB = math.tointeger(tonumber(option("disk-mb") or "")) or 4096
local IMAGE = "/Home/Sandboxes/Images/alpine"

-- Its disk: in `/Sandboxes`, its own drive, where the machine has one.
local DISK = option("disk")
             or (fs.getattr("/Sandboxes") and "/Sandboxes/linux.img"
                 or "/Home/Sandboxes/linux.img")
local READY = DISK .. ".ready"

local function tell(text)
  if TELL then print("linux: " .. text) end
end

local W, H = SW, L.head + SH

local win = ui.window{ title = "Linux", w = W, h = H, x = 80, y = 60,
                       direct = true, header = true }

if not win or not win:surface() then
  print("linux: no window")
  return
end

--------------------------------------------------------------------------
-- What is running, and what it says.
--------------------------------------------------------------------------

local state = "Starting..."
local s, id, picture, region, last = nil, nil, nil, nil, -1
local stage = CONSOLE and "console" or (fs.getattr(READY) and "run" or "disk")
local made = 0                          -- MB of its disk made so far

local function draw_all()
  local surface = win:surface()

  pk.header(surface, 0, 0, W, "Linux", state)

  if picture then
    surface:blit(picture, 0, 0, SW, SH, 0, L.head)
  else
    surface:fill(0, L.head, W, SH, 0xff101114)
  end

  return win:commit{ x = 0, y = 0, w = W, h = H }
end

local function say(text)
  state = text
  tell(text)
  draw_all()
end

-- A sandbox for this stage: `console`, the disk's `setup`, or `run` from it.
local function start(how)
  s = sandboxes.parse("", "linux")
  s.image, s.memory = "alpine", MEMORY
  s.screen = ("%dx%d"):format(SW, SH)
  s.words = {}

  if how == "console" then
    -- A shell on its screen's console - tty1 - for its prompt to be seen.
    s.kind = "shell"
    s.command = "setsid sh -c 'exec sh -l <>/dev/tty1 >&0 2>&0' & "
                .. "echo 'Alpine Linux in a Kosmos window' > /dev/tty1; wait"

    -- With `--tell`, the guest says its virtio interrupts on its serial line
    -- once a second, which this passes on (`tools/run_linuxwin.py`).
    if TELL then
      s.command = "(while sleep 1; do grep -i virtio /proc/interrupts | tr -s ' ' > /dev/console; done) & "
                  .. s.command
    end
  else
    s.kind = "shell"
    s.network = "outgoing"
    s.disk = DISK
    s.folders = { IMAGE .. ":/kosmos/image:ro" }

    -- Kosmos's Downloads as Linux's own, read and write: what the browser
    -- saves is where Kosmos keeps what it saves (`docs/linux.md`).
    if how == "run" then
      if not fs.getattr("/Home/Downloads") then fs.send("/Home/Downloads", { type = "mkdir" }) end
      s.folders[#s.folders + 1] = "/Home/Downloads:/root/Downloads"
    end
    s.words[#s.words + 1] = "kosmos.modloop=" .. sandboxes.hex("/kosmos/image/modloop")

    if how == "setup" then
      s.words[#s.words + 1] = "kosmos.setup=" .. sandboxes.hex(REPO)
      s.words[#s.words + 1] = "kosmos.packages=" .. sandboxes.hex(PACKAGES)
    else
      s.words[#s.words + 1] = "kosmos.root=1"
      s.words[#s.words + 1] = "kosmos.app=" .. sandboxes.hex(APP)
    end
  end

  local got, why = sandboxes.start(s)

  if not got then
    say("Linux would not start: " .. tostring(why))
    return false
  end

  id = got
  region = s.screen_region
  picture = gfx.wrap{ at = region.at + 4096, w = region.width, h = region.height,
                      region = region.cap, offset = 4096 }
  last = -1
  return true
end

--------------------------------------------------------------------------
-- Keys and the pointer, to Linux (`vmmscreen.h`'s input ring).
--------------------------------------------------------------------------

local INPUT_AT, KEYBOARD, TABLET = 1024, 0, 1
local EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
local BUTTON = { left = 0x110, right = 0x111, middle = 0x112 }

local function push(device, type, code, value)
  if region and id then
    sys.ring_push(region.cap, INPUT_AT,
                  string.pack("<I2I2I2I2I4I4", device, type, code, 0, value & 0xffffffff, 0))
  end
end

local function key(code, down)
  push(KEYBOARD, EV_KEY, code, down and 1 or 0)
  push(KEYBOARD, EV_SYN, 0, 0)
end

-- Where the pointer is over the picture, as the tablet's 0 to 32767; false
-- outside it.
local function point(x, y)
  local px, py = x, y - L.head

  if px < 0 or py < 0 or px >= SW or py >= SH then return false end

  push(TABLET, EV_ABS, 0, px * 32767 // (SW - 1))
  push(TABLET, EV_ABS, 1, py * 32767 // (SH - 1))
  return true
end

-- How many frames the monitor has laid in: four bytes of its header.
local function frames()
  local head = region and sys.region_read(region.cap, 16, 4)

  return head and string.unpack("<I4", head) or -1
end

--------------------------------------------------------------------------
-- The stages, a step each pass, so the window keeps drawing.
--------------------------------------------------------------------------

-- Its disk made a piece at a time: 256 MB a pass, said as it grows.
local function make_disk()
  local step = math.min(made + 256, DISK_MB)
  local ok, why = fs.size(DISK, step * 1024 * 1024, made == 0)

  if not ok then
    stage = "failed"
    say("Linux's disk could not be made: " .. tostring(why))
    return
  end

  made = step
  say(("Making Linux's disk, %d of %d MB"):format(made, DISK_MB))

  if made == DISK_MB then
    stage = "setup"
    if start("setup") then say("Setting up Linux") end
  end
end

-- What the setup says on its serial line, as the header's state.
local function heard_setup(line)
  local what = line:match("^kosmos%-setup: (.*)$")

  if not what then return end

  if what == "done" then
    fs.write(READY, "set up from " .. REPO .. " with " .. PACKAGES .. "\n")
    tell("set up")
  elseif what:match("^failed") then
    stage = "failed"
    say("Setting up Linux " .. what)
  else
    local percent = what:match("^(%d+)%%$")

    say(percent and ("Installing Linux, %s%%"):format(percent) or ("Setting up Linux: " .. what))
  end
end

if not draw_all() then return end

if stage == "console" or stage == "run" then
  if start(stage) then
    say(stage == "console" and ("Alpine Linux, " .. s.screen)
        or ("Alpine Linux, " .. APP))
  end
elseif stage == "disk" and fs.getattr(DISK) then
  -- A disk there from a setup that did not finish: set up again on it.
  made, stage = DISK_MB, "setup"
  if start("setup") then say("Setting up Linux") end
end

-- Moves with no button held too: Linux draws its own pointer's hover.
wmproto.track(win.handle, true)

local hz = (sys.info() or {}).counter_hz or 62500000
local looked, heard, partial = sys.ticks(), 0, ""

while win.running do
  if stage == "disk" then make_disk() end

  local now = frames()

  -- **The whole window when a frame moved**: two surfaces and a commit
  -- that flips them, as the Camera's (`camera.lua`).
  if now ~= last then
    if TELL and last <= 0 and now > 0 then
      print(("linux: picture at %d,%d, %dx%d; frames %d")
            :format(win.origin_x, win.origin_y + L.head, SW, SH, now))
    end

    last = now
    draw_all()
  end

  -- Twice a second: what Linux said on its serial line - the setup's
  -- progress, a test's lines - and whether it is still running.
  if id and sys.ticks() - looked >= hz // 2 then
    looked = sys.ticks()

    local text, next_at, r = sandboxes.read(s.name, heard)

    if text and #text > 0 then
      partial = partial .. text

      for line in partial:gmatch("([^\n]*)\n") do
        line = line:gsub("\r", "")
        if TELL then print("linux: serial: " .. line) end
        if stage == "setup" then heard_setup(line) end
      end

      partial = partial:match("([^\n]*)$") or ""
    end

    heard = next_at or heard

    if not r or not r.running then
      sandboxes.stop(s.name)
      id = nil

      -- Set up: started again, from its disk.
      if stage == "setup" and fs.getattr(READY) then
        stage = "run"
        sys.sleep(25)
        if start("run") then say("Alpine Linux, " .. APP) end
      elseif stage ~= "failed" then
        say("Linux has stopped")
        tell("stopped")
      end
    end
  end

  local reply = wmproto.poll(win.handle, stage == "disk" and 0 or 1)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      -- a menu's own
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "mouse" and not ev.menu and ev.action == "press" and ev.y < L.head then
      win:take_hold(ev.x, ev.y)
    elseif ev.type == "mouse" and not ev.menu then
      if point(ev.x or 0, ev.y or 0) and (ev.action == "press" or ev.action == "release") then
        push(TABLET, EV_KEY, BUTTON[ev.button or "left"] or BUTTON.left,
             ev.action == "press" and 1 or 0)
      end
      push(TABLET, EV_SYN, 0, 0)
    elseif ev.type == "wheel" and point(ev.x or 0, ev.y or 0) then
      push(TABLET, EV_REL, 8, ev.n or 0)
      push(TABLET, EV_SYN, 0, 0)
    elseif ev.type == "rawkey" and ev.code then
      key(ev.code, ev.down)
    end
  end
end

if id then sandboxes.stop(s.name) end
