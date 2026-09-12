-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Super Nintendo.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: section demos
--
--   wm snes                        the first ROM in /home/roms/snes
--   wm snes:Top Gear 2.sfc         that one, from the same directory
--   wm snes:/ramfs/other.smc       anywhere else
--
-- In an image built with `FULL=1`, the default, or `SNES=1`. The core is
-- LakeSnes; `runtime/upstream/lakesnes/README.kosmos.md` is the account.
--
-- **This file is the loop, the way `doom.lua` is Doom's.** The core does
-- the console; `snes_kosmos.c` hands it a ROM, a surface and a pad; and what
-- is left - which ROM, which window, when a frame happens, when to stop - is
-- policy, so it is here.
--
-- ROMs live on the drive, in /home/roms/snes, the way Doom's WAD lives in
-- /home. None is in the repository, and none will be:
--
--   make image FILES="game.sfc:/home/roms/snes/game.sfc"

local ui = use("/lib/ui.lua")
local wmproto = use("/lib/wmproto.lua")

if type(snes) ~= "table" then
  print("snes: this image was not built with SNES=1")
  return
end

local ROMS = "/home/roms/snes"

local function is_rom(name)
  local ext = name:lower():match("%.(%w+)$")

  return ext == "sfc" or ext == "smc"
end

--
-- Which ROM.
--
-- The whole argument rather than its first word, because ROMs are named
-- the way No-Intro names them - "Super Mario World (USA).sfc" - and a name
-- cut at its first space is a file that does not exist.
--
local wanted = (args or ""):match("^%s*(.-)%s*$")
local path

if wanted == "" then
  local names, why = fs.list(ROMS)

  if not names then
    print("snes: no " .. ROMS .. ": " .. tostring(why))
    print("      put ROMs there: make image FILES=\"game.sfc:" .. ROMS .. "/game.sfc\"")
    return
  end

  local roms = {}

  for _, name in ipairs(names) do
    if is_rom(name) then roms[#roms + 1] = name end
  end

  table.sort(roms)

  if #roms == 0 then
    print("snes: " .. ROMS .. " holds no .sfc or .smc")
    return
  end

  path = ROMS .. "/" .. roms[1]

  if #roms > 1 then
    print(("snes: %d ROMs in %s; wm snes:<name> opens another"):format(#roms, ROMS))
  end
elseif wanted:sub(1, 1) == "/" then
  path = wanted
else
  path = ROMS .. "/" .. wanted
end

local attrs, why = fs.getattr(path)

if not attrs then
  print("snes: no " .. path .. ": " .. tostring(why))
  return
end

local size = attrs.size or 0

--
-- The ROM, in a region, a window at a time through a scratch region - for
-- the reason `doom.lua` gives: `fs.read_into` has no offset into the region
-- it fills.
--
local rom = sys.memory((size + 4095) // 4096)

if not rom then
  print(("snes: no room for %d KB of ROM"):format(size // 1024))
  return
end

local at = sys.memory_map(rom)

if not at then
  print("snes: the ROM region would not map")
  return
end

do
  local WINDOW = 256 * 1024
  local scratch = sys.memory(WINDOW // 4096)

  if not scratch then
    print("snes: no room for a staging window")
    return
  end

  local done = 0

  while done < size do
    local got = fs.read_into(path, scratch, done, math.min(WINDOW, size - done))

    if not got or got == 0 then
      print(("snes: %s stopped after %d of %d bytes"):format(path, done, size))
      return
    end

    sys.region_write(rom, done, sys.region_read(scratch, 0, got))
    done = done + got
  end
end

-- What the core printed, onto this program's console. See `doom.lua`.
local function drained()
  local said = snes.log()

  while said do
    print(said)
    said = snes.log()
  end
end

local ok, started, pal = pcall(snes.start, at, size)

drained()

if not ok or not started then
  print("snes: " .. tostring(ok and pal or started))
  return
end

local W, H = snes.width, snes.height
local title = path:match("([^/]+)$"):gsub("%.%w+$", "")

local win, err = ui.window{ title = title, w = W, h = H, x = 60, y = 60,
                            direct = true }

if not win then
  print("snes: " .. tostring(err))
  return
end

if not win:surface() then
  print("snes: this window did not get a shared surface")
  return
end

print(("snes: %s, %d KB, %s"):format(path, size // 1024, pal and "50 Hz" or "60 Hz"))

--
-- The pad, from the Linux keycodes virtio-input speaks. The layout is
-- LakeSnes's own: arrows, Enter for Start, Shift for Select, and the face
-- buttons where they sit on a pad - Y and X above B and A - on A S Z X.
--
-- C is R, which is why Control-C does not close this window the way it
-- closes Doom's. `Super + Q` does, as it does every window.
--
local B = snes.buttons

local KEYS = {
  [103] = B.up,     [108] = B.down,   [105] = B.left,  [106] = B.right,
  [28]  = B.start,  [96]  = B.start,                     -- enter, keypad enter
  [42]  = B.select, [54]  = B.select,                    -- either shift
  [30]  = B.y,      [31]  = B.x,                         -- a s
  [44]  = B.b,      [45]  = B.a,                         -- z x
  [32]  = B.l,      [46]  = B.r,                         -- d c
}

--
-- The console's own rate, which is not sixty.
--
-- An NTSC Super Nintendo draws 60.0988 frames a second and a PAL one
-- 50.007, from its master clock. It matters more than it looks once there
-- is sound, which is paced by samples rather than by frames.
--
local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local period = counter_hz / (pal and 50.007 or 60.0988)

--
-- How fast it is really going, every ten seconds.
--
-- The question this port was started to answer: whether an emulator's core
-- keeps up under QEMU's TCG, which is at its worst on exactly this kind of
-- code. `emulating` is the frame and the copy into the surface, so what is
-- left of the period is everything else on the machine.
--
local REPORT = counter_hz * 10
local report_at = sys.ticks()
local frames, busy = 0, 0

local due = sys.ticks()

while win.running do
  local now = sys.ticks()

  if now >= due then
    local fine, oops = pcall(snes.frame, win:surface())
    local after = sys.ticks()

    drained()

    if not fine then
      print("snes: " .. tostring(oops))
      break
    end

    if not win:commit{ x = 0, y = 0, w = W, h = H } then
      break
    end

    frames = frames + 1
    busy = busy + (after - now)
    due = due + period

    -- Behind by more than a few frames: stop owing them. Racing to repay
    -- a debt the machine cannot pay is a game that never answers a key.
    if now - due > 4 * period then
      due = now
    end
  end

  if now - report_at >= REPORT then
    local seconds = (now - report_at) / counter_hz

    print(("snes: %.1f frames a second of %.1f, %.1f ms emulating each")
          :format(frames / seconds, counter_hz / period,
                  frames > 0 and busy / frames / counter_hz * 1000 or 0))

    report_at, frames, busy = now, 0, 0
  end

  --
  -- Wait for the next frame without spinning, and without oversleeping it.
  --
  -- One scheduler tick when the frame is more than half a period away, and
  -- a yield when it is closer - so an idle console is an idle process, and
  -- a tick's length is never assumed, only that it is shorter than half a
  -- frame.
  --
  local reply = wmproto.poll(win.handle, (due - sys.ticks() > period / 2) and 1 or 0)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "rawkey" then
      local b = KEYS[ev.code]

      if b then snes.button(b, ev.down) end
    end
  end

  if due - sys.ticks() > 0 and due - sys.ticks() <= period / 2 then
    sys.yield()
  end
end

win:close()
