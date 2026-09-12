-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Quake.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: section demos
--
--   wm quake                        /home/id1/pak0.pak
--   wm quake:/ramfs/pak0.pak        somewhere else
--
-- Only in an image built with `make QUAKE=1`; see
-- `runtime/upstream/quake/README.kosmos.md` for why that is a build option
-- and not part of the desktop.
--
-- **This file is the loop, the way `doom.lua` is Doom's.** Chocolate Quake
-- does the game; `quake_kosmos.c` is the platform under it; and what is left
-- - which window, which pak, when a frame happens, when to stop - is policy,
-- so it is here in Lua where policy goes.
--
-- The pak does not pass through Lua. The shareware `pak0.pak` is eighteen
-- megabytes and the Lua heap starts at two, so it goes into a region the way
-- Doom's WAD does, and the libc is told that region is `pak0.pak`.

local ui = use("/lib/ui.lua")
local wmproto = use("/lib/wmproto.lua")

if type(quake) ~= "table" then
  print("quake: this image was not built with QUAKE=1")
  return
end

local path = (args or ""):match("^%s*(%S+)") or "/home/id1/pak0.pak"

local attrs, why = fs.getattr(path)

if not attrs then
  print("quake: no " .. path .. ": " .. tostring(why))
  print("       put one on the disk: make image FILES=\"pak0.pak:/home/id1/pak0.pak\"")
  return
end

local size = attrs.size or 0

if size < 12 then
  print("quake: " .. path .. " is " .. size .. " bytes, which is not a pak")
  return
end

--
-- The pak, in a region of its own, a window at a time through a scratch
-- region - for the reason `doom.lua` gives: `fs.read_into` has no offset
-- into the region it fills.
--
local pak = sys.memory((size + 4095) // 4096)

if not pak then
  print(("quake: no room for %d KB of pak"):format(size // 1024))
  return
end

local at = sys.memory_map(pak)

if not at then
  print("quake: the pak region would not map")
  return
end

do
  local WINDOW = 256 * 1024
  local scratch = sys.memory(WINDOW // 4096)

  if not scratch then
    print("quake: no room for a staging window")
    return
  end

  local done = 0

  while done < size do
    local got = fs.read_into(path, scratch, done, math.min(WINDOW, size - done))

    if not got or got == 0 then
      print(("quake: %s stopped after %d of %d bytes"):format(path, done, size))
      return
    end

    sys.region_write(pak, done, sys.region_read(scratch, 0, got))
    done = done + got
  end
end

-- A pak starts with "PACK". Checked here, because what goes wrong on this
-- system is the transfer, and Quake's own complaint would be about the file.
if sys.region_read(pak, 0, 4) ~= "PACK" then
  print(("quake: %s does not start with PACK - the read did not land"):format(path))
  return
end

local W, H = quake.width, quake.height

local win, err = ui.window{ title = "Quake", w = W, h = H, x = 60, y = 60,
                            direct = true }

if not win then
  print("quake: " .. tostring(err))
  return
end

if not win:surface() then
  print("quake: this window did not get a shared surface")
  return
end

print(("quake: %s, %d KB"):format(path, size // 1024))

-- What Quake printed, onto this program's console. See `doom.lua`.
local function drained()
  local said = quake.log()

  while said do
    print(said)
    said = quake.log()
  end
end

local ok, started, reason = pcall(quake.start, at, size)

drained()

if not ok or not started then
  print("quake: " .. tostring(ok and reason or started))
  win:close()
  return
end

--
-- Keys, as transitions, from the Linux codes virtio-input speaks to Quake's
-- own numbers in `keys.h`: a letter or a digit is its lower-case character,
-- the rest are Quake's `K_` constants.
--
local KEYS = {
  [1]   = 27,  [14]  = 127, [15]  = 9,   [28]  = 13,  [96]  = 13,  [57] = 32,
  [103] = 128, [108] = 129, [105] = 130, [106] = 131,             -- arrows
  [56]  = 132, [100] = 132,                                       -- alt
  [29]  = 133, [97]  = 133,                                       -- ctrl
  [42]  = 134, [54]  = 134,                                       -- shift
  [87]  = 145, [88]  = 146,                                       -- F11, F12
  [110] = 147, [111] = 148, [109] = 149, [104] = 150,             -- ins del pgdn pgup
  [102] = 151, [107] = 152, [119] = 255,                          -- home end pause
}

local function row(first, chars)
  for i = 1, #chars do
    KEYS[first + i - 1] = chars:byte(i)
  end
end

row(2, "1234567890-=")
row(16, "qwertyuiop[]")
row(30, "asdfghjkl;'`")
row(43, "\\zxcvbnm,./")

for i = 0, 9 do
  KEYS[59 + i] = 135 + i                                          -- F1 to F10
end

local K_MOUSE1 = 200

-- Control-C closes the window - not the way the rest of the system uses
-- that key, which is copy, but because this reads `rawkey` keycodes on a
-- path the window manager does not take characters from. `Super + Q` is
-- the consistent way and works as well. Closing here is done by
-- keycode, so it does not depend on the character path.
local CTRL = { [29] = true, [97] = true }
local ctrl_down = false

-- The window manager reports the pointer's position, and movement only while
-- the first button is held, so looking around is a drag.
local mouse_x, mouse_y

local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local last = sys.ticks()

while win.running do
  local now = sys.ticks()
  local dt = (now - last) / counter_hz

  last = now

  -- Asked for on every pass: `commit` flips which buffer is live.
  local fine, running, drawn = pcall(quake.frame, win:surface(), dt)

  drained()

  if not fine then
    print("quake: " .. tostring(running))
    break
  end

  if not running then
    if drawn then print("quake: " .. drawn) end
    break
  end

  if drawn and not win:commit{ x = 0, y = 0, w = W, h = H } then
    break
  end

  -- One scheduler tick of waiting: Quake holds itself to its own frame rate
  -- and a frame that draws nothing new should not spin the processor.
  local reply = wmproto.poll(win.handle, 1)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "rawkey" then
      if CTRL[ev.code] then ctrl_down = ev.down end

      if ctrl_down and ev.code == 46 and ev.down then
        win:close()
      else
        local k = KEYS[ev.code]

        if k then quake.key(k, ev.down) end
      end
    elseif ev.type == "mouse" and not ev.menu then
      if ev.action == "press" then
        mouse_x, mouse_y = ev.x, ev.y
        quake.key(K_MOUSE1, true)
      elseif ev.action == "release" then
        quake.key(K_MOUSE1, false)
        mouse_x, mouse_y = nil, nil
      elseif ev.action == "move" and mouse_x then
        quake.mouse(ev.x - mouse_x, ev.y - mouse_y)
        mouse_x, mouse_y = ev.x, ev.y
      end
    end
  end
end

win:close()
