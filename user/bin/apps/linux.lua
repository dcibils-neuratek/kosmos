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
-- Each key the window is given, said - for following a key from the
-- keyboard or a VNC viewer to Linux (`testing.md` 18.655).
local KEYS = ARGS:find("--keys", 1, true) ~= nil

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
-- Inside a D-Bus session of its own: Epiphany ends at once without one
-- ("Cannot autolaunch D-Bus without X11 $DISPLAY", the M700).
local APP = option("app") or "dbus-run-session epiphany"
local PACKAGES = option("packages")
                 or "alpine-base eudev seatd cage wlr-randr dbus mesa-dri-gallium font-dejavu epiphany"
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

-- The largest the picture may become: the desktop's own size, its width
-- rounded up to the 16 a screen's width is a multiple of (L5).
local MOST_W, MOST_H

do
  local screen = fs.read("/Devices/screen") or {}

  MOST_W = math.max(SW, ((screen.width or 1920) + 15) // 16 * 16)
  MOST_H = math.max(SH, screen.height or 1080)
end

-- **One Linux**: a second opened brings the first forward and goes, rather
-- than starting a second sandbox of the same name beside it (the M700, 15
-- October 2026: three Linux windows, each redrawing at once).
do
  local open = wmproto.windows()

  for _, w in ipairs(open and open.windows or {}) do
    if w.title == "Linux" and w.handle then
      fs.send("/Running/wm", { type = "raise", window = w.handle })
      tell("already open; brought forward")
      return
    end
  end
end

local win = ui.window{ title = "Linux", w = W, h = H, x = 80, y = 60,
                       direct = true, header = true, resizable = true }

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

-- **Setting Linux up, shown** (Diego, 14 October 2026: "a progress bar
-- showing the disk creation process", "a modal window", "from the linux
-- app", "to show the progress of the filesystem being created"): a panel
-- over the picture while it is set up, a step a row - its disk, its
-- filesystem, Alpine and the browser - each with a bar, as the mockup's
-- setup page draws it (`docs/linux.html`).
-- Each step says what it is doing now, under its bar (Diego: "why dont the
-- setting up linux says what is doing in every step").
local steps = {
  { name = "Linux's disk", done = 0, of = 1, said = "",
    detail = ("Reserving %d MB on %s"):format(DISK_MB, DISK:match("^(/[^/]+)") or DISK) },
  { name = "Its filesystem", done = 0, of = 1, said = "",
    detail = "Waiting for the disk" },
  { name = "Alpine and the browser", done = 0, of = 100, said = "",
    detail = "Downloaded from Alpine's servers and installed onto Linux's disk" },
}

local function panel(surface)
  local pw, ph = math.min(480, W - 40), 64 + #steps * 74
  local px, py = (W - pw) // 2, L.head + math.max(20, (H - L.head - ph) // 2)
  local th = gfx.height()

  surface:fill_round(px - 1, py - 1, pw + 2, ph + 2, 0xff3a3f4a, 13)
  surface:fill_round(px, py, pw, ph, 0xfff7f8fa, 12)
  surface:text(px + 22, py + 18, "Setting up Linux", 0xff1b1d22, 0xfff7f8fa, "heading")

  for i, step in ipairs(steps) do
    local y = py + 52 + (i - 1) * 74
    local whole = step.of > 0 and step.done >= step.of
    local share = step.of > 0 and math.min(1, step.done / step.of) or 0
    local ink = whole and 0xff2f9e5b or 0xff1b1d22

    surface:text(px + 22, y, step.name, ink, 0xfff7f8fa, "ui")

    if step.said ~= "" then
      local sw = gfx.measure(step.said)
      surface:text(px + pw - 22 - sw, y, step.said, 0xff6b7484, 0xfff7f8fa, "ui")
    end

    surface:fill_round(px + 22, y + th + 8, pw - 44, 8, 0xffdfe3ea, 4)

    if share > 0 then
      surface:fill_round(px + 22, y + th + 8, math.max(8, math.floor((pw - 44) * share)), 8,
                         whole and 0xff2f9e5b or 0xff3b82f6, 4)
    end

    -- What it is doing now, cut to the panel's width.
    local detail = tostring(step.detail or "")

    while #detail > 3 and gfx.measure(detail) > pw - 44 do
      detail = detail:sub(1, -5) .. "..."
    end

    surface:text(px + 22, y + th + 22, detail, 0xff6b7484, 0xfff7f8fa, "ui")
  end
end

-- Whole frames still owed: a change to the header or the size is drawn
-- into both surfaces, a frame each.
local whole = 2

--
-- **How Linux feels, measured** (`docs/linux.md` L6a; Diego, 15 October
-- 2026: "lets make the measurements and understand if its a real thing").
-- Each second: the frames Linux handed over, the longest wait between two
-- of them while it was drawing, and the time from each key handed to Linux
-- to the next frame it drew - shown in the header, and with `--tell` said.
-- A frame after a key is not proof it was the key's (a cursor blinks), so
-- the key's time is an upper bound only when nothing else is drawing.
--
local meter = { seen = -1, frames = 0, last_at = nil, gap = 0, key_at = nil,
                keys = {}, second_at = 0, shown = "" }

local function header_text()
  return meter.shown ~= "" and (state .. "  ·  " .. meter.shown) or state
end

local function draw_all()
  local surface = win:surface()

  pk.header(surface, 0, 0, W, "Linux", header_text())

  surface:fill(0, L.head, W, H - L.head, 0xff101114)

  if picture then
    surface:blit(picture, 0, 0, SW, SH, 0, L.head)
  end

  if stage == "disk" or stage == "setup" or (stage == "failed" and steps[1].done > 0) then
    panel(surface)
  end

  return win:commit{ x = 0, y = 0, w = W, h = H }
end

local function say(text)
  state = text
  whole = 2
  tell(text)
  draw_all()
end

-- A sandbox for this stage: `console`, the disk's `setup`, or `run` from it.
local function start(how)
  -- The last one's screen given back: a new one is made for this.
  if s and s.screen_region then
    picture = nil
    sys.release(s.screen_region.cap)
  end

  s = sandboxes.parse("", "linux")
  s.image, s.memory = "alpine", MEMORY
  s.screen = ("%dx%d"):format(SW, SH)
  s.screen_most = ("%dx%d"):format(MOST_W, MOST_H)
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
  last, whole = -1, 2
  return true
end

--------------------------------------------------------------------------
-- Keys and the pointer, to Linux (`vmmscreen.h`'s input ring).
--------------------------------------------------------------------------

local INPUT_AT, KEYBOARD, TABLET = 1024, 0, 1
local EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
local BUTTON = { left = 0x110, right = 0x111, middle = 0x112 }

-- **Held, not lost, when the ring is full** (`testing.md` 18.655): the
-- ring to Linux holds 128 events and a key is four - its press, its
-- release and a sync after each - so an address typed at full speed filled
-- it, and every event after was dropped: on the M700 an address arrived as
-- one letter, and a Shift released in the lost part stayed held in Linux.
-- What does not fit waits here, in order, and goes as the ring empties.
local waiting, first, after = {}, 1, 1     -- events held: [first, after)
local held = 0                             -- how many have been, said with --keys

local function drain()
  while first < after do
    if not (region and id) then
      waiting, first, after = {}, 1, 1
      return
    end

    if not sys.ring_push(region.cap, INPUT_AT, waiting[first]) then return end

    waiting[first] = nil
    first = first + 1
  end

  waiting, first, after = {}, 1, 1
end

local function push(device, type, code, value)
  if region and id then
    local event = string.pack("<I2I2I2I2I4I4", device, type, code, 0, value & 0xffffffff, 0)

    if first < after or not sys.ring_push(region.cap, INPUT_AT, event) then
      waiting[after] = event
      after = after + 1
      held = held + 1
    end
  end
end

local function key(code, down)
  if down and region and id and not meter.key_at then meter.key_at = sys.ticks() end

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

--------------------------------------------------------------------------
-- **Its size, following the window's** (`docs/linux.md` L5).
--------------------------------------------------------------------------

local sized = 0

-- The picture the window's size less its header, the width a multiple of
-- 16, inside the room the region has: said in the header and counted, which
-- the monitor tells the guest as its display changing.
local function follow(w, h)
  local nw = math.max(64, math.min(MOST_W, w // 16 * 16))
  local nh = math.max(64, math.min(MOST_H, h - L.head))

  W, H = w, h

  if nw == SW and nh == SH then return end

  SW, SH = nw, nh

  if region then
    sys.region_write(region.cap, 4, string.pack("<I4I4I4", SW, SH, SW * 4))
    sized = sized + 1
    sys.region_write(region.cap, 48, string.pack("<I4", sized))
    picture = gfx.wrap{ at = region.at + 4096, w = SW, h = SH,
                        region = region.cap, offset = 4096 }
  end

  tell(("size %dx%d"):format(SW, SH))
end

-- How many frames the monitor has laid in: four bytes of its header.
local function frames()
  local head = region and sys.region_read(region.cap, 16, 4)

  return head and string.unpack("<I4", head) or -1
end

--------------------------------------------------------------------------
-- The stages, a step each pass, so the window keeps drawing.
--------------------------------------------------------------------------

-- **Its disk made by `sandbox disk new`, in a process of its own**: a
-- drive that is writing zeroes answers nobody else until it is done, so a
-- window that made the disk itself waited, and dropped the pointer, a step
-- at a time (the M700, 14 October 2026). The program says how far it has
-- got in a file in `/Temporary`, which this reads a pass at a time.
local REPORT = "/Temporary/linux-disk.txt"
local launched, make_disk_at = false, nil
local hz = (sys.info() or {}).counter_hz or 62500000

local function make_disk()
  if not launched then
    launched = true
    fs.write(REPORT, "")
    steps[1].of = DISK_MB

    local reply, why = fs.send("/Running/wm", { type = "launch", program = "sandbox",
                                                args = ("disk new %s %d --report %s")
                                                       :format(DISK, DISK_MB, REPORT),
                                                wait = false })

    if not reply or not reply.ok then
      stage = "failed"
      say("Linux's disk could not be made: " .. tostring(why or (reply and reply.error)))
    else
      say("Making Linux's disk")
    end

    return
  end

  -- Four times a second is often enough to move a bar, and a read every
  -- pass made `/Temporary` the busiest drive on the machine.
  if sys.ticks() - (make_disk_at or 0) < hz // 4 then return end

  make_disk_at = sys.ticks()

  local text = fs.read(REPORT) or ""
  local failed = text:match("^failed (.*)")
  local done, of = text:match("^(%d+) (%d+)")

  if failed then
    stage = "failed"
    say("Linux's disk could not be made: " .. failed)
    return
  end

  done = math.tointeger(tonumber(done or ""))

  if not done or done == made then return end

  made = done
  steps[1].done, steps[1].said = made, ("%d of %d MB"):format(made, DISK_MB)
  steps[1].detail = ("Writing zeroes, so Linux never reads what was on the drive")
  say(("Making Linux's disk, %d of %d MB"):format(made, DISK_MB))

  if made >= DISK_MB then
    stage = "setup"
    steps[1].detail = ("%d MB on %s, made"):format(DISK_MB, DISK:match("^(/[^/]+)") or DISK)
    steps[2].said, steps[2].detail = "waiting", "Starting Linux to make it"
    if start("setup") then say("Setting up Linux") end
  end
end

-- What the setup says on its serial line, as the header's state.
local function heard_setup(line)
  local what = line:match("^kosmos%-setup: (.*)$")

  if not what then return end

  if what == "done" then
    steps[3].done, steps[3].said = 100, "done"
    fs.write(READY, "set up from " .. REPO .. " with " .. PACKAGES .. "\n")
    tell("set up")
  elseif what:match("^failed") then
    stage = "failed"
    say("Setting up Linux " .. what)
  elseif what == "preparing the disk" then
    -- ext4 being made: the filesystem's step, half way while it runs.
    steps[2].done, steps[2].said = 0.5, "making ext4"
    steps[2].detail = "Making an ext4 filesystem on Linux's disk"
    say("Making Linux's filesystem")
  elseif what:match("^package ") then
    -- The package apk is on: downloaded, then unpacked onto the disk.
    local name, n, of = what:match("^package (%S+) (%d+) (%d+)")

    if name then
      steps[2].done, steps[2].said, steps[2].detail = 1, "ext4", "Made"
      steps[3].detail = ("Downloading and installing %s - %s of %s packages"):format(name, n, of)
    end
  else
    local percent = math.tointeger(tonumber(what:match("^(%d+)%%$") or ""))

    -- Installing begins once the filesystem is made.
    if what:match("^installing") or percent then
      steps[2].done, steps[2].said, steps[2].detail = 1, "ext4", "Made"
    end

    if percent then
      steps[3].done, steps[3].said = percent, percent .. "%"
      say(("Installing Linux, %d%%"):format(percent))
    else
      say("Setting up Linux: " .. what)
    end
  end

  draw_all()
end

if not draw_all() then return end

if stage == "console" or stage == "run" then
  if start(stage) then
    say(stage == "console" and ("Alpine Linux, " .. s.screen)
        or ("Alpine Linux, " .. APP))
  end
elseif stage == "disk" and fs.getattr(DISK) then
  -- A disk there from a setup that did not finish. Whole: set up again on
  -- it. Part made, its window closed while it grew: made again.
  local there = fs.getattr(DISK)

  if (there.size or 0) >= DISK_MB * 1024 * 1024 then
    made, stage = DISK_MB, "setup"
    steps[1].done, steps[1].of, steps[1].said = DISK_MB, DISK_MB, DISK_MB .. " MB"
    steps[1].detail = "Made already"
    steps[2].detail = "Starting Linux to make it"
    if start("setup") then say("Setting up Linux") end
  else
    fs.send(DISK, { type = "delete" })
  end
end

-- Moves with no button held too: Linux draws its own pointer's hover.
wmproto.track(win.handle, true)

local looked, heard, partial = sys.ticks(), 0, ""
local drew_at, prev = 0, { 0, 0, 0, 0 }

while win.running do
  if stage == "disk" then make_disk() end

  -- **At most sixty times a second, and only what changed** (the M700, 15
  -- October 2026: the window drew and handed over its whole 3 MB each time
  -- the guest flushed, up to 250 times a second, in the display band - and
  -- held its core, and the network with it). The monitor keeps everything
  -- flushed since `cleared`; the window draws that and what it drew last
  -- time, since a commit flips two surfaces and each must catch up.
  local now = id and frames() or last

  drain()

  -- The meter: every frame Linux hands over counted as it is seen, drawn
  -- or not, and a second's worth said.
  do
    local t = sys.ticks()

    if id and meter.seen >= 0 and now > meter.seen then
      if meter.last_at and t - meter.last_at < hz then
        meter.gap = math.max(meter.gap, t - meter.last_at)
      end

      if meter.key_at then
        if t - meter.key_at < 2 * hz then
          meter.keys[#meter.keys + 1] = (t - meter.key_at) * 1000 // hz
        end
        meter.key_at = nil
      end

      meter.frames, meter.last_at = meter.frames + (now - meter.seen), t
    end

    meter.seen = id and now or -1

    if t - meter.second_at >= hz then
      local keys, shown = meter.keys, ""

      if meter.frames > 0 or #keys > 0 then
        shown = ("%d fps, gap %d ms"):format(meter.frames, meter.gap * 1000 // hz)

        if #keys > 0 then
          table.sort(keys)
          shown = shown .. (", key %d ms"):format(keys[(#keys + 1) // 2])
        end

        if TELL then
          print(("linux: meter fps=%d gap=%d keys=%s"):format(meter.frames,
                meter.gap * 1000 // hz, table.concat(keys, ",")))
        end
      end

      if shown ~= meter.shown then
        meter.shown, whole = shown, 2
      end

      meter.frames, meter.gap, meter.keys, meter.second_at = 0, 0, {}, t
    end
  end

  -- A changed header is the whole window twice, not the header alone: a
  -- commit flips the two surfaces, and one of the header only would leave
  -- the other a frame of the picture behind.
  if region and (now ~= last or whole > 0) and sys.ticks() - drew_at >= hz // 60 then
    if TELL and last <= 0 and now > 0 then
      print(("linux: picture at %d,%d, %dx%d; frames %d")
            :format(win.origin_x, win.origin_y + L.head, SW, SH, now))
    end

    local head = sys.region_read(region.cap, 16, 24)
    local _, _, x, y, w, h = string.unpack("<I4I4I4I4I4I4", head)

    sys.region_write(region.cap, 52, string.pack("<I4", now))
    last, drew_at = now, sys.ticks()

    if whole > 0 or w == 0 then
      whole = math.max(0, whole - 1)
      draw_all()
    else
      local ux, uy = math.min(x, prev[1]), math.min(y, prev[2])
      local ur = math.max(x + w, prev[1] + prev[3])
      local ub = math.max(y + h, prev[2] + prev[4])
      local surface = win:surface()

      ux, uy = math.max(0, ux), math.max(0, uy)
      ur, ub = math.min(SW, ur), math.min(SH, ub)

      if ur > ux and ub > uy then
        surface:blit(picture, ux, uy, ur - ux, ub - uy, ux, L.head + uy)
        win:commit{ x = ux, y = L.head + uy, w = ur - ux, h = ub - uy }
      end
    end

    prev = { x, y, w, h }
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

  -- Resting when there is nothing to watch: Linux stopped, or failed.
  local reply = wmproto.poll(win.handle, (id or stage == "disk") and 1 or 25)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      -- a menu's own
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "resize" and ev.w and ev.h then
      follow(ev.w, ev.h)
      whole = 2
      draw_all()
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
      if KEYS then
        print(("linux: key %d %s, %d held so far"):format(ev.code, ev.down and "down" or "up", held))
      end
      key(ev.code, ev.down)
    end
  end
end

if id then sandboxes.stop(s.name) end
