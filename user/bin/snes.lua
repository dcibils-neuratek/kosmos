-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Super Nintendo.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: section demos
--
--   wm snes                        the first ROM in /home/roms/snes
--   wm snes:Top Gear 2.sfc         that one, from the same directory
--   wm snes:/ramfs/other.smc       anywhere else
--   wm snes:--scale 2              the first ROM, in a window twice the size
--   wm snes:--scale 2 Top Gear 2.sfc
--
-- A launcher carries the same words, so the bigger window is a launcher of
-- its own: `launcher /home/Desktop/Mario snes --scale 2 Super Mario World.sfc`.
--
-- In an image built with `FULL=1`, the default, or `SNES=1`. The core is
-- LakeSnes; `runtime/upstream/lakesnes/README.kosmos.md` is the account.
--
-- **This file is the loop, the way `doom.lua` is Doom's.** The core does
-- the console; `snes_kosmos.c` hands it a ROM, a surface, a ring and a pad;
-- and what is left - which ROM, which window, when a frame happens, when to
-- stop - is policy, so it is here.
--
-- ROMs live on the drive, in /home/roms/snes, the way Doom's WAD lives in
-- /home. None is in the repository, and none will be:
--
--   make image FILES="game.sfc:/home/roms/snes/game.sfc"

local ui = use("/lib/ui.lua")
local panel = use("/lib/panel.lua")
local wmproto = use("/lib/wmproto.lua")
local audio = use("/lib/audio.lua")

--
-- The core is a kit, and only in an image built with it. It used to be a
-- global, and a global named `snes` hid this program from the prompt: the
-- shell gives a word that already names something to Lua.
--
local have, snes = pcall(use, "/kits/snes")

if not have or type(snes) ~= "table" then
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

--
-- **`--scale 2`, before the ROM, for a window twice the size.**
--
-- The one option, and written first for the reason the ROM is the whole rest
-- of the line: a No-Intro name has spaces in it, so the option must be
-- something no file is called, and it must come off the front. Anything else
-- starting `--` is refused rather than looked for as a file.
--
-- Twice and no more: 1024 by 960 fits under the bar on a 1080-line screen,
-- and three times would fit on no screen this system has run on. Each of the
-- console's pixels becomes a block, in C (`snes_blit.c`). The window manager
-- then composes four times the pixels; what that costs in frames is a
-- measurement, not a guess, and has not been made yet.
--
local scale = 1

do
  local given, rest = wanted:match("^%-%-scale%s+(%S+)%s*(.-)$")

  if given then
    local number = tonumber(given)

    scale = number and math.tointeger(number)

    if scale ~= 1 and scale ~= 2 then
      print(("snes: --scale is 1 or 2, and %s is neither"):format(given))
      return
    end

    wanted = rest
  elseif wanted:sub(1, 2) == "--" then
    print("snes: the one option is --scale 1 or --scale 2, before the ROM")
    return
  end
end

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
-- A file into a region, a window at a time through a scratch region - for
-- the reason `doom.lua` gives: `fs.read_into` has no offset into the region
-- it fills. The ROM comes in this way, and so do the saves below.
--
local WINDOW = 256 * 1024
local scratch = sys.memory(WINDOW // 4096)

if not scratch then
  print("snes: no room for a staging window")
  return
end

local function read_file(file, bytes, region)
  local done = 0

  while done < bytes do
    local got = fs.read_into(file, scratch, done, math.min(WINDOW, bytes - done))

    if not got or got == 0 then
      return nil, ("%s stopped after %d of %d bytes"):format(file, done, bytes)
    end

    sys.region_write(region, done, sys.region_read(scratch, 0, got))
    done = done + got
  end

  return true
end

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
  local ok, oops = read_file(path, size, rom)

  if not ok then
    print("snes: " .. oops)
    return
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

--
-- `fps` is the console's rate as the core keeps it - 60, or 50 for a PAL
-- cartridge - and it comes from the core rather than from here, because the
-- core's sound is pitched to it and a second copy of the number would be a
-- second thing to get wrong.
--
local ok, started, fps = pcall(snes.start, at, size)

drained()

if not ok or not started then
  print("snes: " .. tostring(ok and fps or started))
  return
end

local W, H = snes.width * scale, snes.height * scale
local title = path:match("([^/]+)$"):gsub("%.%w+$", "")

--
-- **Kept when it closes, and continued when it opens** - Diego's "Yes" on 19
-- September to continuing a game after quitting (`roadmap.md` 4g).
--
-- Beside the ROM, where most emulators keep them. `Name.srm` is the
-- cartridge's own save, its battery-backed RAM, which is where a game writes
-- its save slots; `Name.state` is the whole machine at the moment it was
-- closed. The state is what continues a game where it was. The cartridge's
-- save is read first, so a state that will not load - from another version
-- of the core, or of the cartridge - still leaves the game's own saves.
--
-- Written when the console stops rather than on a timer: Quit, the close
-- box, `Super + Q`, and before Open ROM or View starts another Super
-- Nintendo - which is what lets Double Size keep your place, because the
-- new one reads what this one has just written. A console killed from
-- Processes is not kept; it never got the chance.
--
-- Through one region, as big as a state, held for the whole run: a region
-- here is not given back until the process ends, so one is made and used
-- for every read and write.
--
local base = path:gsub("%.%w+$", "")
local SRM, STATE = base .. ".srm", base .. ".state"
local state_bytes = snes.size("state")
local keep = sys.memory((state_bytes + 4095) // 4096)
local keep_at = keep and sys.memory_map(keep)
local kept_at                           -- the frame last kept, or read

-- "loaded" and its size, "refused" and why, or nil when there is none.
local function read_kept(file, kind)
  local attrs = fs.getattr(file)

  if not attrs or (attrs.size or 0) == 0 then return nil end

  if attrs.size > state_bytes then
    return "refused", "larger than anything this cartridge keeps"
  end

  local ok, oops = read_file(file, attrs.size, keep)

  if not ok then return "refused", oops end

  if not snes.load(kind, keep_at, attrs.size) then
    return "refused", "the emulator would not take it: from another "
                      .. "cartridge, or another version of the emulator"
  end

  return "loaded", attrs.size
end

if not keep_at then
  print("snes: no room to keep this game, so it will not be kept")
else
  local had, why = read_kept(SRM, "battery")

  if had == "refused" then
    print("snes: the cartridge's save in " .. SRM .. " was not read: " .. why)
  end

  local got, detail = read_kept(STATE, "state")

  if got == "loaded" then
    kept_at = snes.frames()
    print(("snes: continuing %s from frame %d, a state of %d KB")
          :format(title, kept_at, detail // 1024))
  else
    if got == "refused" then
      print("snes: " .. STATE .. " was not continued: " .. detail)
    end

    print(("snes: starting %s fresh%s"):format(title, had == "loaded"
          and ", with the cartridge's own save" or ""))
  end
end

-- The game into its two files. Nothing when no frame has run since the
-- last time, which is every second call on the way out of a relaunch.
local function keep_game()
  if not keep_at or snes.frames() == kept_at then return end

  local frame = snes.frames()
  local cart = ""

  if snes.size("battery") > 0 then
    local n, why = snes.save("battery", keep_at, state_bytes)
    local ok, oops = n and fs.write_from(SRM, keep, n)

    cart = ok and (", and the cartridge's own save of %d KB"):format(n // 1024)
           or ("; the cartridge's save was not written: "
               .. tostring(oops or why))
  end

  local n, why = snes.save("state", keep_at, state_bytes)
  local ok, oops = n and fs.write_from(STATE, keep, n)

  if ok then
    kept_at = frame
    print(("snes: kept %s at frame %d, a state of %d KB%s")
          :format(title, frame, n // 1024, cart))
  else
    print(("snes: %s was not kept: %s%s"):format(title,
          tostring(oops or why), cart))
  end
end

--
-- **A File menu**, Diego's on 18 September: "we should add a menu to that
-- app as well to open roms and exit the app". The window draws its own
-- pixels, so the window manager draws the menu bar above them and the kit
-- opens the menus (`window:direct_event`, `strips` in `wm.lua`).
--
-- **Open ROM... starts another Super Nintendo** on the chosen file, at the
-- same scale, and closes this one, rather than putting a second ROM into a
-- console that is running the first: the core is started once per process,
-- and a fresh one is the reset a cartridge swap is anyway. The game pauses
-- while the Open window is up, because it is a window this loop waits on.
--
-- **View changes the size the same way**, Diego's choice on 19 September:
-- "Do the 2x option in snes emulator app menu and it will restart the app".
-- A window that draws its own pixels cannot be resized - its buffers are
-- this process's region, sized when it opened - so the other size is a
-- fresh Super Nintendo on the same ROM, and the game starts again. One item,
-- naming the size it goes to, because a menu here has no check marks to say
-- which size this is.
--
-- **Game pauses and resumes** - "so I can pause a game and restart it later
-- on" - as does P. A paused console runs no frames, so its sound stops with
-- them, and "Paused" is drawn over the last picture. **Reset** is the
-- console's button: the game starts again from its title, with its own
-- saves where they were - which is how to start again, now that closing
-- keeps your place.
--
local win
local paused = false
local ran = 0                           -- frames run, for the log and the test

local function relaunch(at_scale, rom)
  local words = (at_scale > 1 and ("--scale " .. at_scale .. " ") or "") .. rom

  -- Kept before the other one starts, because it reads what this writes.
  keep_game()

  local reply, why = fs.send("/app/wm", { type = "launch",
                                          program = "snes", args = words })

  if reply and reply.ok then
    win:close()
    return true
  end

  print("snes: could not start " .. rom .. ": "
        .. tostring(reply and reply.error or why))
  return false
end

local function open_rom()
  local chooser = panel.open{
    start = ROMS, title = "Open ROM",
    filter = is_rom,
    on_choose = function(chosen) relaunch(scale, chosen) end,
  }

  if chooser then chooser:run() end
end

local other = (scale == 1) and 2 or 1
local size_item = {
  text = ((other == 2) and "Double Size (%d x %d)" or "Normal Size (%d x %d)")
         :format(snes.width * other, snes.height * other),
  on_choose = function()
    print(("snes: restarting at %dx, %d by %d"):format(other,
          snes.width * other, snes.height * other))
    relaunch(other, path)
  end,
}

--
-- "Paused", over the picture that was showing.
--
-- The buffer this draws into is the one *not* on the screen, which holds the
-- frame before last - so the frame on the screen is copied into it first,
-- and the box goes over that. Committed whole, because the copy changed
-- every pixel of the buffer even where it changed nothing a person can see.
--
local function show_paused()
  local s = win:surface()
  local region = win.region

  if not s or not region then return end

  local shown = region[(region.draw_into == 1) and 2 or 1]

  if shown then s:blit(shown, 0, 0, W, H, 0, 0) end

  local label = "Paused"
  local bw, bh = gfx.measure(label) + 48, gfx.height("ui") + 20
  local bx, by = (W - bw) // 2, (H - bh) // 2

  s:fill(bx, by, bw, bh, 0xff000000)
  s:fill(bx + 1, by + 1, bw - 2, bh - 2, 0xff303030)
  s:text(bx + 24, by + 10, label, 0xffffffff)

  win:commit{ x = 0, y = 0, w = W, h = H }
end

local pause_item = { text = "Pause" }
local due                               -- the clock's next frame, set below

local function set_paused(on)
  paused = on
  pause_item.text = on and "Resume" or "Pause"

  if on then
    print(("snes: paused at frame %d"):format(ran))
    show_paused()
  else
    print(("snes: resumed at frame %d"):format(ran))
    due = sys.ticks()                   -- no frames owed for the pause
  end
end

pause_item.on_choose = function() set_paused(not paused) end

local reset_item = {
  text = "Reset",
  on_choose = function()
    print(("snes: reset at frame %d"):format(snes.frames()))
    snes.reset()
    if paused then set_paused(false) end
  end,
}

local err

win, err = ui.window{ title = title, w = W, h = H, x = 60, y = 60,
                      direct = true,
                      menubar = {
                        { title = "File", items = {
                          { text = "Open ROM...", on_choose = open_rom },
                          { separator = true },
                          { text = "Quit", on_choose = function() win:close() end },
                        } },
                        { title = "View", items = { size_item } },
                        { title = "Game", items = { pause_item, reset_item } },
                      } }

if not win then
  print("snes: " .. tostring(err))
  return
end

if not win:surface() then
  print("snes: this window did not get a shared surface")
  return
end

print(("snes: %s, %d KB, %g Hz, a %d by %d window"):format(path, size // 1024,
                                                        fps, W, H))

--
-- Sound, when the machine has a device - and then the device decides when a
-- frame runs.
--
-- **The device keeps time, so nothing else has to.** Each frame the core
-- makes one frame's worth of samples and the device takes them at its own
-- rate. Running a frame whenever less than one frame's worth is waiting holds
-- the console at exactly the rate its sound needs: no clock arithmetic, and no
-- second clock for the picture to drift against. Without a device, the
-- counter paces it as it always did.
--
-- The ring is sized from the numbers rather than left at the default: a frame
-- is run when under one frame of sound is waiting, so no more than two are
-- ever queued, and two slots of slack cover the period being filled.
--
local out
local per_frame = 0

do
  local fmt = audio.format()

  if fmt.period == 0 then
    print("snes: no sound device, so the clock paces it")
  else
    per_frame = fmt.rate / fps

    local depth = math.ceil(2 * per_frame / fmt.frames) + 2
    local stream, oops = audio.open(title, depth)

    if stream then
      snes.sound(stream.ring, stream.rate)
      out = stream
      print(("snes: sound, %d Hz, in a ring of %d periods")
            :format(stream.rate, stream.ring_periods))
    else
      print("snes: no sound (" .. tostring(oops) .. "), so the clock paces it")
    end
  end
end

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

  --
  -- **And a game controller** - the USB driver's keys (`pad_decode.h`),
  -- evdev's gamepad codes, which name a button by where it sits. An 8BitDo
  -- SN30 Pro has a Super Nintendo's buttons in a Super Nintendo's places,
  -- so the bottom one is B, the right A, the left Y and the top X - and on
  -- an Xbox pad the same four places are the same four buttons.
  --
  [0x130] = B.b,    [0x131] = B.a,                       -- south, east
  [0x134] = B.y,    [0x133] = B.x,                       -- west, north
  [0x136] = B.l,    [0x137] = B.r,                       -- the shoulders
  [0x13a] = B.select, [0x13b] = B.start,
  [0x220] = B.up,   [0x221] = B.down,                    -- the D-pad, and
  [0x222] = B.left, [0x223] = B.right,                   -- the left stick
}

-- P, and the key marked Pause, as the Game menu's item.
local PAUSE_KEYS = { [25] = true, [119] = true }

local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local period = counter_hz / fps
due = sys.ticks()

-- Whether the console owes the device, or the clock, a frame.
local function wanted(now)
  if out then
    return out:queued() * out.frames < per_frame
  end

  return now >= due
end

--
-- How fast it is really going, every ten seconds.
--
-- The question this port was started to answer: whether an emulator's core
-- keeps up under QEMU's TCG, which is at its worst on exactly this kind of
-- code. `emulating` is the frame and the copy into the surface. Dropped sound
-- is frames the ring had no room for, which pacing by the ring should keep at
-- none; a core too slow for its device shows up instead as the server's
-- `starved`, and as silence between the frames.
--
local REPORT = counter_hz * 10
local report_at = sys.ticks()
local frames, busy, lost = 0, 0, 0
local broken = false                    -- the core failed: nothing to keep

while win.running do
  local now = sys.ticks()

  if not paused and wanted(now) then
    local fine, dropped = pcall(snes.frame, win:surface(), scale)
    local after = sys.ticks()

    drained()

    if not fine then
      print("snes: " .. tostring(dropped))
      broken = true
      break
    end

    if not win:commit{ x = 0, y = 0, w = W, h = H } then
      break
    end

    frames = frames + 1
    ran = ran + 1
    busy = busy + (after - now)
    lost = lost + dropped

    if not out then
      due = due + period

      -- Behind by more than a few frames: stop owing them. Racing to repay
      -- a debt the machine cannot pay is a game that never answers a key.
      if now - due > 4 * period then
        due = now
      end
    end
  end

  if now - report_at >= REPORT then
    local seconds = (now - report_at) / counter_hz

    print(("snes: %.1f frames a second of %g, %.1f ms emulating each, "
           .. "%d frames of sound dropped")
          :format(frames / seconds, fps,
                  frames > 0 and busy / frames / counter_hz * 1000 or 0, lost))

    report_at, frames, busy, lost = now, 0, 0, 0
  end

  --
  -- Wait for the next frame without spinning, and without oversleeping it.
  --
  -- One scheduler tick when nothing is owed - a tick is shorter than a
  -- period of sound and than half a frame, which is all that is assumed
  -- about it - and none when a frame is.
  --
  local idle

  if out then
    idle = not wanted(sys.ticks())
  else
    idle = due - sys.ticks() > period / 2
  end

  -- Paused, the loop has nothing to do until somebody presses something,
  -- so it waits for that rather than for a frame; an event ends the wait.
  local reply = wmproto.poll(win.handle, paused and 25 or (idle and 1 or 0))

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      -- The menu bar's, or a menu's.
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "rawkey" then
      local b = KEYS[ev.code]

      if PAUSE_KEYS[ev.code] then
        if ev.down then set_paused(not paused) end
      elseif b then
        snes.button(b, ev.down)
      end
    end
  end

  if not paused and not out and due - sys.ticks() > 0
     and due - sys.ticks() <= period / 2 then
    sys.yield()
  end
end

if out then out:close() end

-- However it stopped - Quit, the close box, Super + Q - unless the core
-- itself failed, when the machine is not worth continuing from.
if not broken then keep_game() end

win:close()
