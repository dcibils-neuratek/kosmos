-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon File_Audio
-- kosmos: section applications
-- kosmos: needs audio midi
-- Groove: making music - eight tracks of drums and synthesisers, clips
-- launched in scenes, a song arranged from them with automation, and a WAV
-- at the end (`roadmap.md` 6zh).
--
-- Diego, 28 September 2026: "i want to add a music creation tool which i
-- developed in Love2d", "i dont want to vendor it in, just convert it to
-- Kosmos and it will come bundled to Kosmos as an app called Groove". It is
-- his PulseMusic: the window is `groove/app.lua`, PulseMusic's own; the
-- sound is the Synth Kit, on a thread of its own; this file is the window
-- and the loop - what LÖVE was to PulseMusic.
--
--   groove              the techno demo, ready to play
--   groove --house      the house demo
--   groove --open       the saved project
--   groove --play       and playing, which is how `make shot` pictures it
--   groove --report 30  and after thirty seconds, says on the console how
--                       the sound held: how often its ring ran dry, and the
--                       audio server's worst turn and starvations
--
-- A MIDI keyboard plays it (`roadmap.md` 6zg): every one there is when it
-- opens, and again whenever its MIDI button is pressed. `needs audio` puts
-- the Synth Kit's thread in the audio band, above every window (4i).

local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local audio = use("/Kosmos/Libraries/audio.lua")
local synth = use("/Kosmos/Kits/synth")
local E = use("/Kosmos/Libraries/groove/engine.lua")
local U = use("/Kosmos/Libraries/groove/ui.lua")
local Demos = use("/Kosmos/Libraries/groove/demos.lua")
local app = use("/Kosmos/Libraries/groove/app.lua")

local want = {}
for word in tostring(args or ""):gmatch("%S+") do want[word] = true end
local report = tonumber(tostring(args or ""):match("%-%-report%s+(%d+)"))

--------------------------------------------------------------------------
-- The window, maximised as Cafesa3D's is: a workstation wants the room,
-- and a window that draws its own pixels is opened at its size and never
-- resized. PulseMusic's own 1400 by 860 when there is nobody to ask.
--
-- **Its top bar is its title bar** (`roadmap.md` 6zj), as Diego chose:
-- "GROOVE bar is the title bar". It says it has a header, so where the
-- look takes the title bars off the window manager draws close, minimise
-- and maximise at the bar's right end, and a press on the bar's empty band
-- moves the window; where the look keeps them, Groove wears a tab like
-- anything else. `workarea` is asked with the header too, since a window
-- with no tab and no border has the whole width.
--------------------------------------------------------------------------

local W, H, area = 1400, 860, false

do
  local ok, got = pcall(fs.send, "/Running/wm", { type = "workarea", header = true })

  if ok and type(got) == "table" and got.ok and tonumber(got.w) and tonumber(got.h) then
    W, H, area = got.w, got.h, true
  end
end

local win = ui.window{ title = "Groove", w = W, h = H, direct = true, header = true,
                       maximised = area or nil, centre = not area or nil }

if not win or not win:surface() then
  print("groove: no window")
  return
end

W, H = win:surface():size()

-- The pointer when no button is down, which PulseMusic's hover needs: a
-- launcher lights its row, a knob its label.
wmproto.track(win.handle, true)

--------------------------------------------------------------------------
-- The sound: a stream of the machine's periods, eight deep, and the kit's
-- thread filling it. Without a sound device Groove still opens, and says so.
--------------------------------------------------------------------------

app.size(W, H)
app.load()

-- The three, in the bar's right end, when the bar is the title bar; and a
-- press on its empty band handed to the window manager as a drag, or a
-- double one as maximise - `take_hold`, as a kit's header does.
local function place_lights()
  app.chrome(win.headed, win.lights)

  if win.headed and win.lights then
    fs.send("/Running/wm", { type = "lights", window = win.handle,
                             x = W - win.lights.w - 12, y = (48 - win.lights.h) // 2 })
  end
end

place_lights()
app.onTitle = function(x, y) win:take_hold(x, y) end

if want["--house"] then E.setSong(Demos.house()) end
if want["--open"] then app.open() end

local out, why = audio.open("Groove", 8)

if out then
  local ok, err = pcall(synth.start, out.ring, out.rate)

  if not ok then
    out:close()
    out, why = nil, err
  end
end

if not out then app.say("No sound: " .. tostring(why)) end

print(("groove: %dx%d, %s"):format(W, H, out and "playing into the audio stream" or ("silent, " .. tostring(why))))

if want["--play"] then E.play() end

--------------------------------------------------------------------------
-- Keys: a `rawkey` carries the key's code, and PulseMusic asks for LÖVE's
-- names - its note keys are the letters where they sit on the keyboard.
--------------------------------------------------------------------------

local KEYS = {
  [57] = "space", [15] = "tab", [28] = "return", [14] = "backspace", [111] = "delete",
  [105] = "left", [106] = "right", [103] = "up", [108] = "down", [1] = "escape",
  [11] = "0", [2] = "1", [3] = "2", [4] = "3", [5] = "4",
  [6] = "5", [7] = "6", [8] = "7", [9] = "8", [10] = "9",
  [30] = "a", [48] = "b", [46] = "c", [32] = "d", [18] = "e",
  [33] = "f", [34] = "g", [35] = "h", [23] = "i", [36] = "j",
  [37] = "k", [38] = "l", [50] = "m", [49] = "n", [24] = "o",
  [25] = "p", [16] = "q", [19] = "r", [31] = "s", [20] = "t",
  [22] = "u", [47] = "v", [17] = "w", [45] = "x", [21] = "y",
  [44] = "z",
}
local SHIFT = { [42] = true, [54] = true }
local CTRL = { [29] = true, [97] = true }
local ALT = { [56] = true, [100] = true }

local down = {}

local function modifiers()
  local shift = down[42] or down[54] or false
  local ctrl = down[29] or down[97] or false
  local alt = down[56] or down[100] or false
  app.modifiers(shift, ctrl, alt)
end

--------------------------------------------------------------------------
-- The loop. **A frame after anything a person does**, and every pass while
-- the song moves; otherwise it waits for the next event, a tenth of a second
-- at most, for a meter still falling.
--
-- A press is drawn at once, where it happened, before the moves that
-- follow it in the same batch - an immediate-mode window reads a press as
-- "the pointer is here and the button went down", and a press read at the
-- end of a drag is a press somewhere else.
--
-- **The song goes to the kit after the frame**, when anything was touched:
-- the frame is where a knob or a step changes, and `E.sync` hands the
-- whole song over at most once a frame.
--
-- **With a MIDI keyboard open it looks every tick**, four milliseconds,
-- because its events arrive in a page rather than as messages and nothing
-- wakes the window for one. A note heard is played at once and the frame
-- follows, as a key typed on the computer's keyboard is.
--------------------------------------------------------------------------

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
local last = sys.ticks()
local reportAt = report and (last + report * hz)

-- `--report`: how the sound held, from both of the places it could fail -
-- the Synth Kit's thread keeping its ring, and the audio server keeping the
-- device (4i). Said once.
--
-- **What decides a skip is whether each party came back in time**: the
-- kit's thread between two of its passes, and the audio server between two
-- turns, each against what the device holds. Past that a gap is certain on
-- any device; within it none is owed to this machine. The device's own
-- count of periods that found it empty (`hal_snd_dry`) is said too, and is
-- the real thing on hardware - under QEMU it is not: its WAV writer drains
-- the queue in bursts, and an idle machine counts a hundred (`testing.md`
-- 18.263).
local function reportSound()
  local st = E.kitState()
  local stats = audio.stats() or {}
  local info = sys.info() or {}
  local frames = (info.audio_period or 0) // (2 * math.max(1, info.audio_channels or 2))
  local holds = (info.audio_periods or 0) * frames / math.max(1, info.audio_rate or 44100) * 1000

  -- The DSP load, all told: the time spent rendering over the time it
  -- rendered. Past one, no scheduler can keep the sound whole.
  local load = (st.busy and st.rendered and st.rendered > 0)
               and (st.busy / hz) / (st.rendered / E.SR) or 0

  print(("groove: after %d s: the device holds %.1f ms; the kit's worst pass %.1f ms, "
         .. "the audio server's worst turn %.1f ms; the kit %s, %d periods kept, "
         .. "its ring ran dry %d times; DSP %.0f%%; the device ran dry %d times")
        :format(report, holds, (st.worst_pass or 0) / hz * 1000, (stats.late or 0) / 1000,
                st.audio_band and "in the audio band" or "not in the audio band",
                st.ahead or 0, st.dry or 0, load * 100, info.audio_dry or -1))
end
local lastPress, lastX, lastY = -1, -100, -100
local dirty = true

local function frame(touched)
  local t = sys.ticks()
  local dt = (t - last) / hz
  last = t

  app.update(dt, hz)
  U.target(win:surface())
  app.draw()

  if not win:commit{ x = 0, y = 0, w = W, h = H } then return false end

  if touched then E.changed() end
  E.sync()
  return true
end

while win.running do
  local busy = app.busy()
  local wait = (busy or app.midiOpen() or reportAt) and 1 or (dirty and 0 or 25)
  local reply = wmproto.poll(win.handle, wait)

  if not reply then break end

  local touched = false
  local draw = dirty or busy

  dirty = false

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      draw = true
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "theme" then
      -- A look that takes the title bars off, or puts them back.
      if ev.headed ~= nil and ev.headed ~= win.headed then
        win.headed = ev.headed
        place_lights()
      end

      draw = true
    elseif ev.type == "rawkey" then
      local was = down[ev.code]

      down[ev.code] = ev.down or nil

      if SHIFT[ev.code] or CTRL[ev.code] or ALT[ev.code] then
        modifiers()
      elseif KEYS[ev.code] and ev.down and not was then
        app.keypressed(KEYS[ev.code], ev.at)
        touched, draw = true, true
      elseif KEYS[ev.code] and not ev.down and was then
        app.keyreleased(KEYS[ev.code])
        touched, draw = true, true
      end
    elseif ev.type == "mouse" and not ev.menu then
      local x, y = ev.x or 0, ev.y or 0

      if ev.action == "press" then
        local b = ev.button == "right" and 2 or 1
        local t = sys.ticks()
        local presses = 1

        if b == 1 and (t - lastPress) < hz * 0.4 and math.abs(x - lastX) < 6 and math.abs(y - lastY) < 6 then
          presses = 2
        end

        if b == 1 then lastPress, lastX, lastY = t, x, y end

        app.mousepressed(x, y, b, presses)
        if not frame(true) then break end
        draw = false
      elseif ev.action == "release" then
        if ev.button ~= "right" then
          app.mousereleased(x, y, 1)
          if not frame(true) then break end
          draw = false
        end
      elseif ev.action == "move" then
        app.mousemoved(x, y)
        draw = true
        if U.down then touched = true end
      end
    elseif ev.type == "wheel" then
      if ev.x and ev.y then app.mousemoved(ev.x, ev.y) end
      app.wheelmoved(ev.n or 0)
      touched, draw = true, true
    end
  end

  if not win.running then break end

  if app.midiPoll(sys.ticks() / hz) > 0 then touched, draw = true, true end

  if reportAt and sys.ticks() >= reportAt then
    reportAt = nil
    reportSound()
  end

  if draw or touched then
    if not frame(touched) then break end
  else
    app.update(0, hz)
  end
end

app.quit()
synth.close()
if out then out:close() end
win:close()
print("groove: closed")
