-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs audio
-- kosmos: image media
--
-- `listen <file>` - a film's or a song's sound, at the prompt, to its end.
--
--   listen /Home/song.mp4             plays it and says how long it was
--   listen --report /Home/song.mp4    and how the playing went
--
-- No window and no picture: the Video library (`video.lua`) opened for its
-- sound and played by its own clock - the clock is the sound's, so the
-- film is over when what was played reaches its length - then the ring let
-- play out. A program that makes sound, so it declares `needs audio` and
-- its sound thread is in the audio band, above every program on the
-- desktop, as the Video app's and Music's are.
--
-- `--report` is what `x86-film` reads (`tools/run_film.py`): the codec,
-- whether it was heard, its length, how long it took, the frames handed
-- over and taken, and the sound thread's band, its longest time away and
-- how often its input ran dry - and the machine's own part beside them:
-- how much the device holds, and how long the machine was held off by its
-- own ticks, an emulator's host stopping it, which no band can help. Its
-- suite played a program written to
-- `/Temporary` until 18 November 2026, which no file the image serves
-- could be, so it was never in the band and a busy host made it gap.
--

local video = use("/Kosmos/Libraries/video.lua")
local audio = use("/Kosmos/Libraries/audio.lua")
local files = use("/Kosmos/Libraries/files.lua")

local path, report = nil, false

for _, word in ipairs(files.words(args)) do
  if word == "--report" then
    report = true
  elseif not path then
    path = word
  end
end

if not path then
  print("listen [--report] <file>")
  print("  for instance: listen /Home/song.mp4")
  return
end

local f, why = video.open(path)

if not f then
  print("listen: " .. tostring(why))
  return
end

if not f.sound then
  print("listen: " .. path .. " has no sound")
  f:close()
  return
end

local heard, silent = f:audible()
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
local began = sys.ticks()
local held_off = audio.held_off_since()

-- Played by the sound's clock to its length, and never for ever: a film
-- that stopped being heard ends twenty seconds past its length.
f:play(0)

while f:position() < f.duration and (sys.ticks() - began) / hz < f.duration + 20 do
  f:tick()
  sys.sleep(1)
end

local took = (sys.ticks() - began) / hz

-- What is still in the ring and the device, played out.
for _ = 1, 150 do
  f:tick()
  sys.sleep(1)
end

local v = f.voice_state or {}

if report then
  local h = f:heard() or {}

  print(("listen: %s heard %s, %.2f s in %.2f s, %d frames out, %d in, silent %s")
        :format(f.sound.codec or "none", tostring(heard), f.duration, took,
                v.frames_out or 0, v.frames_in or 0, tostring(silent)))
  print(("listen: its sound thread %s the audio band, away at most %.1f ms, "
         .. "its input dry %d times")
        :format(h.audio_band and "in" or "not in", (h.worst_pass or 0) / hz * 1000,
                h.dry or 0))
  print(("listen: the device holds %.1f ms; the machine was held off %.1f ms, by its own ticks")
        :format(audio.device_holds(), held_off()))
else
  print(("listen: %s, %s, %.1f s"):format(path, f.sound.codec or "sound", f.duration))
end

f:close()
