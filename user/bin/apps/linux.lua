-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Linux
-- kosmos: section applications
-- **Linux in a Kosmos window** (`docs/linux.md`, `docs/linux.html`): Alpine
-- in a sandbox, its screen in this window.
--
-- Diego, 14 October 2026: "can we prioritize linux on kosmos so we can run
-- a web browser ?" - Chromium in `cage`, on a disk in `/Sandboxes`, 2 GB.
-- This is step L2: **the screen**. The guest draws through virtio-gpu, the
-- sandbox's monitor lays each flush into a region as Kosmos's own pixels
-- (`vmmscreen.h`), and this window wraps that region as a surface and blits
-- it whenever the monitor's count of frames moves - asked on the window's
-- own clock, never waited for. No pixel passes through this file.
--
-- **And the keyboard and the pointer** (L3): each raw key and each move,
-- press and turn of the wheel over the picture, as Linux's own input
-- events, into the ring in the screen region's first page (`vmmscreen.h`),
-- which the monitor feeds to the guest's virtio-input keyboard and tablet.
-- Kosmos's own keys - Super and its shortcuts - are the window manager's
-- and never arrive here. What is shown until L4 brings Chromium is Linux's
-- own console on its screen: a shell on `tty1`.
--
--   linux                  a 1024 by 768 screen
--   linux 1280x800         at that size (a width a multiple of 16)
--   linux --memory 256     with that much memory, in MB (512 until L4)
--   linux --tell           and say where its window and picture are, for
--                          a test to look (`tools/run_linuxwin.py`)

local ui = use("/Kosmos/Libraries/ui.lua")
local theme = ui.theme
local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
local sandboxes = use("/Kosmos/Libraries/sandboxes.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local L = ui.layout

local SW, SH, MEMORY = 1024, 768, 512
local TELL = tostring(args or ""):find("--tell", 1, true) ~= nil

do
  local w, h = tostring(args or ""):match("(%d+)x(%d+)")
  local mb = tostring(args or ""):match("%-%-memory%s+(%d+)")

  if w then SW, SH = math.tointeger(tonumber(w)), math.tointeger(tonumber(h)) end
  if mb then MEMORY = math.tointeger(tonumber(mb)) end
end

local W, H = SW, L.head + SH

local win = ui.window{ title = "Linux", w = W, h = H, x = 80, y = 60,
                       direct = true, header = true }

if not win or not win:surface() then
  print("linux: no window")
  return
end

--------------------------------------------------------------------------
-- The sandbox: started with the window, stopped with it.
--------------------------------------------------------------------------

local s = sandboxes.parse("", "linux")

s.image, s.kind, s.memory = "alpine", "shell", MEMORY
s.screen = ("%dx%d"):format(SW, SH)

-- A shell on its screen's console - tty1 - for its prompt to be seen; the
-- serial line keeps the sandbox's own console, as every sandbox's.
s.command = "setsid sh -c 'exec sh -l <>/dev/tty1 >&0 2>&0' & "
            .. "echo 'Alpine Linux in a Kosmos window' > /dev/tty1; wait"

-- With `--tell`, the guest says its virtio interrupts on its serial line
-- once a second, which this passes on - how a test sees the pointer's
-- events arrive (`tools/run_linuxwin.py`).
if TELL then
  s.command = "(while sleep 1; do grep -i virtio /proc/interrupts | tr -s ' ' > /dev/console; done) & "
              .. s.command
end

local state = "Starting Alpine..."
local picture, region, last = nil, nil, -1

local id, why = sandboxes.start(s)

if id then
  region = s.screen_region
  picture = gfx.wrap{ at = region.at + 4096, w = region.width, h = region.height,
                      region = region.cap, offset = 4096 }
  state = "Alpine Linux, " .. s.screen
else
  state = "Linux would not start: " .. tostring(why)
  print("linux: " .. tostring(why))
end

--------------------------------------------------------------------------
-- Keys and the pointer, to Linux (`vmmscreen.h`'s input ring).
--------------------------------------------------------------------------

local INPUT_AT, KEYBOARD, TABLET = 1024, 0, 1
local EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
local BUTTON = { left = 0x110, right = 0x111, middle = 0x112 }

local function push(device, type, code, value)
  if region then
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

if not draw_all() then return end

-- Moves with no button held too: Linux draws its own pointer's hover.
wmproto.track(win.handle, true)

local hz = (sys.info() or {}).counter_hz or 62500000
local looked, heard = sys.ticks(), 0

while win.running do
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

  -- Once a second: whether Linux is still running - and, told to, what
  -- its serial line said.
  if id and sys.ticks() - looked >= hz then
    looked = sys.ticks()

    local text, next_at, r = sandboxes.read(s.name, heard)

    if text and TELL and #text > 0 then
      for line in text:gmatch("[^\n]+") do print("linux: serial: " .. line) end
    end

    heard = next_at or heard

    if not r or not r.running then
      id = nil
      state = "Linux has stopped"
      if TELL then print("linux: stopped") end
      draw_all()
    end
  end

  local reply = wmproto.poll(win.handle, 1)

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
