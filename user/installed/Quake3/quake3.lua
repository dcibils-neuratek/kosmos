-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Quake III Arena.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Quake III Arena
-- kosmos: section applications
-- kosmos: kind game
-- kosmos: about id Software's Quake III Arena, as Quake3e plays it, drawn by Vulkan
-- kosmos: image quake3.elf
-- kosmos: image on intel-gpu quake3-anv.elf
--
--   wm quake3                        the game beside this file
--   wm quake3:+map q3dm1             and anything `quake3 +...` would say
--
-- **An installed application** (`docs/quake3e.md`), as Quake 1 is: this
-- file, `quake3.elf` - Quake3e, its Vulkan renderer and the platform under
-- it (`q3_*.c`), linked with a Vulkan driver into an image of their own -
-- and the game's paks, in one folder, `/Home/Apps/Quake3`. No system image
-- carries any of it.
--
-- **This file is the loop**, as `quake.lua` is Quake 1's: the engine does
-- the game, the platform does the window, the keys, the pointer and the
-- sound, and what is left - where the game is, when a frame happens, when
-- to stop - is here. The window is the engine's own, opened from C through
-- the Window Kit, because Vulkan presents into it; so is every event.
--
-- **Which game**: `baseq3/pak0.pk3`, the full game's, if it is here, and
-- otherwise the demo's, `demoq3/pak0.pk3` - which Quake III plays as its
-- base game when it is told so (`fs_basegame`).

local have, q3 = pcall(use, "quake3.elf")

if not have or type(q3) ~= "table" then
  print("quake3: " .. tostring(q3))
  return
end

local here = tostring(sys.program or ""):match("^(.*)/[^/]+$") or "/Home/Apps/Quake3"
local home = here .. "/home"

local function exists(path)
  return fs.getattr(path) ~= nil
end

local line

if exists(here .. "/baseq3/pak0.pk3") then
  line = ""
elseif exists(here .. "/demoq3/pak0.pk3") then
  line = "+set fs_basegame demoq3"
else
  print("quake3: no game here: put baseq3/pak0.pk3, or the demo's")
  print("        demoq3/pak0.pk3, beside quake3.lua in " .. here)
  return
end

-- **The settings this machine starts with**, before anything a person says
-- after them: a window of 1280 by 720, the game's code native - ioquake3's
-- game, cgame and q3_ui, compiled into this image (`docs/quake3e.md` Q3),
-- since the demo's own QVMs are older than the engine - a server that is
-- not pure, since a pure one allows only QVMs and a game on this machine
-- has nobody to be pure for, and the frame rate Quake III is tuned for.
local defaults = "+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720"
    .. " +set r_fullscreen 0 +set vm_game 0 +set vm_cgame 0 +set vm_ui 0 +set sv_pure 0"
    .. " +set com_maxfps 125"

line = defaults .. " " .. line

-- **And what is said after them**, which wins over them: `quake3.args`
-- beside this file, if there is one - a line of `+set`s a person keeps,
-- since `open` hands a program one word - and then whatever `wm
-- quake3:...` said.
do
  local said = exists(here .. "/quake3.args") and fs.read(here .. "/quake3.args")

  if type(said) == "string" then
    said = said:gsub("%s+", " ")
    if said:match("%S") then
      line = line .. " " .. said
    end
  end
end

if type(args) == "string" and args ~= "" then
  line = line .. " " .. args
end

-- What the engine printed, onto this program's console, a line at a time:
-- the engine's log comes in pieces of whatever size it was written in, and
-- a line split between two of them is held until its end arrives.
local pending = ""

local function drained()
  local said = q3.log()

  while said do
    pending = pending .. said
    said = q3.log()
  end

  for whole in pending:gmatch("([^\n]*)\n") do
    print(whole)
  end

  pending = pending:match("[^\n]*$") or ""
end

-- **Its sound**: a stream from the audio server, whose ring the engine's
-- own mixer fills from C as it asks where its sound card has got to
-- (`q3_snd.c`) - eight periods, the six it keeps ahead and two of slack.
-- Before the engine starts, since that is where it looks for a card.
local audio = use("/Kosmos/Libraries/audio.lua")
local sound

do
  local fmt = audio.format()

  if fmt.period == 0 then
    print("quake3: no sound device, so silent")
  else
    local stream, oops = audio.open("Quake III", 8)

    if stream then
      q3.sound(stream.ring, stream.rate)
      sound = stream
    else
      print("quake3: no sound (" .. tostring(oops) .. ")")
    end
  end
end

print("quake3: " .. here .. ", " .. line)

local ok, started, reason = pcall(q3.start, here, home, line)

drained()

if not ok or not started then
  print("quake3: " .. tostring(ok and reason or started))
  if sound then sound:close() end
  return
end

-- One `Com_Frame` a turn. The engine waits inside it for its next frame,
-- in slices short enough to read the window's events between them, so
-- nothing here sleeps.
while true do
  local running, why = q3.frame()

  drained()

  if not running then
    print("quake3: " .. tostring(why))
    break
  end
end

if sound then sound:close() end
