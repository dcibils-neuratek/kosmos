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
--   groove --size 2560x1440   a window that size; `--size full` the whole
--                       screen. 1920x1080 otherwise, or the whole screen
--                       when that is no bigger
--   groove --report 30  and after thirty seconds, says on the console how
--                       the sound held: how often its ring ran dry, and the
--                       audio server's worst turn and starvations
--   groove --redraw-check  every frame drawn whole as well, and the pixels
--                       that differ from the one drawn only where it changed
--                       counted, for `--report` to say
--
-- A MIDI keyboard plays it (`roadmap.md` 6zg): every one there is when it
-- opens, and again whenever its MIDI button is pressed. `needs audio` puts
-- the Synth Kit's thread in the audio band, above every window (4i).

local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local audio = use("/Kosmos/Libraries/audio.lua")
local synth = use("/Kosmos/Kits/synth")
local midi = use("/Kosmos/Libraries/midi.lua")
local E = use("/Kosmos/Libraries/groove/engine.lua")
local U = use("/Kosmos/Libraries/groove/ui.lua")
local Demos = use("/Kosmos/Libraries/groove/demos.lua")
local app = use("/Kosmos/Libraries/groove/app.lua")

local want = {}
for word in tostring(args or ""):gmatch("%S+") do want[word] = true end
local report = tonumber(tostring(args or ""):match("%-%-report%s+(%d+)"))
U.checking = want["--redraw-check"] or nil
local asked = tostring(args or ""):match("%-%-size%s+(%S+)")
local carried = tostring(args or ""):match("%-%-carry%s+(%S+)")

--------------------------------------------------------------------------
-- **The window: 1920 by 1080**, centred, or the whole work area where that
-- is no bigger. It was maximised, as Cafesa3D is, and on the M700's
-- 3440x1440 that was five million pixels drawn again every frame while a
-- song played - Diego, 28 September: "the app feels laggy", "It should open
-- at 1920x1080 by default", "Or have the launcher parameter to open at a
-- certain resolution", "Also a 3 dot menu option to go full screen". So
-- `--size WxH` or `--size full`, and Full screen in the bar's menu, which
-- starts Groove again at the new size with the song carried over (`again`
-- below): a window that draws its own pixels cannot be resized, because its
-- buffers are this process's, as Video's are. PulseMusic's own 1400 by 860
-- when there is nobody to ask.
--
-- **Its top bar is its title bar** (`roadmap.md` 6zj), as Diego chose:
-- "GROOVE bar is the title bar". It says it has a header, so where the
-- look takes the title bars off the window manager draws close, minimise
-- and maximise at the bar's right end, and a press on the bar's empty band
-- moves the window; where the look keeps them, Groove wears a tab like
-- anything else. `workarea` is asked with the header too, since a window
-- with no tab and no border has the whole width.
--------------------------------------------------------------------------

local DEFAULT_W, DEFAULT_H = 1920, 1080
local W, H, whole = 1400, 860, false

do
  local ok, got = pcall(fs.send, "/Running/wm", { type = "workarea", header = true })

  if ok and type(got) == "table" and got.ok and tonumber(got.w) and tonumber(got.h) then
    local aw, ah = got.w, got.h
    local w, h = tostring(asked or ""):match("^(%d+)[xX](%d+)$")

    if asked == "full" then
      W, H = aw, ah
    elseif w then
      W, H = math.min(tonumber(w), aw), math.min(tonumber(h), ah)
    else
      W, H = math.min(DEFAULT_W, aw), math.min(DEFAULT_H, ah)
    end

    whole = W == aw and H == ah
  end
end

local win = ui.window{ title = "Groove", w = W, h = H, direct = true, header = true,
                       maximised = whole or nil, centre = not whole or nil }

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

-- **Another size**: this Groove's song saved where the next will find it,
-- Groove asked for at that size, and this window closed once the window
-- manager has said yes - not before, so a refused start leaves the song on
-- the screen and a line saying why. As Video does for its sizes.
-- In Groove's own folder, where its project is, and taken away once read:
-- `/Temporary` keeps 16 KB a file and a song is more (`roadmap.md` 6zh).
-- `/Temporary` all the same on a machine with no disk to put it on.
local CARRIES = { "/Home/Documents/Groove/.carried.groove", "/Temporary/groove-carried.groove" }

local function again(size)
  fs.send("/Home/Documents", { type = "mkdir" })
  fs.send("/Home/Documents/Groove", { type = "mkdir" })

  local ok, why, CARRY
  local said = {}

  for _, path in ipairs(CARRIES) do
    ok, why = E.save(path)
    if ok then CARRY = path break end
    said[#said + 1] = path .. ": " .. tostring(why)
  end

  why = table.concat(said, "; ")

  if not ok then
    app.say("The song could not be carried across: " .. tostring(why))
    print("groove: the song could not be carried across: " .. tostring(why))
    return
  end

  local reply, sent = fs.send("/Running/wm", {
    type = "launch", program = "groove",
    args = ("--size %s --carry %s%s"):format(size, CARRY, E.playing and " --play" or ""),
  })

  if reply and reply.ok then
    win:close()
    return
  end

  app.say("Groove could not start again at " .. size .. ": "
          .. tostring(reply and reply.error or sent))
  print("groove: could not start again at " .. size .. ": "
        .. tostring(reply and reply.error or sent))
end

app.onSize = again
app.sizing = { whole = whole, w = W, h = H }

-- The song a Groove at another size handed over, and then gone.
if carried then
  local ok, why = E.load(carried)

  if not ok then app.say("The song did not come across: " .. tostring(why)) end
  print(ok and ("groove: the song carried across, at %dx%d"):format(W, H)
           or ("groove: the song did not come across: " .. tostring(why)))
  fs.send(carried, { type = "delete" })
end

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
local soundSince = nil              -- the counter when the kit's thread began

if out then
  local ok, err = pcall(synth.start, out.ring, out.rate)

  soundSince = sys.ticks()

  if not ok then
    out:close()
    out, why = nil, err
  end
end

if not out then app.say("No sound: " .. tostring(why)) end

print(("groove: %dx%d, %s"):format(W, H, out and "playing into the audio stream" or ("silent, " .. tostring(why))))

-- **A keyboard's notes, taken by the kit itself** (`roadmap.md` 4i, step
-- d): a `/Devices/midi` page of its own, every device, which the kit's
-- thread reads each pass - so a key waits for no window. The window keeps
-- its own page for what it shows and records. Without a sound thread, or a
-- page, the window plays the notes as it did.
local kitMidi = out and midi.open() or nil

if kitMidi and synth.listen(kitMidi.at) then
  app.kitPlays(true)
  print("groove: the kit takes a keyboard's notes from its own page")
elseif kitMidi then
  kitMidi:close()
  kitMidi = nil
end

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

-- How long each processor's own ticks had been held off when the window
-- began, for `--report` to say how much of an absence was the machine's -
-- an emulator's host stopping it, never a thread the scheduler starved
-- (`roadmap.md` 4i-f).
local heldOffFrom = {}

if report then
  for _, c in ipairs(sys.cpuload() or {}) do
    heldOffFrom[c.index] = c.held_off_counter or 0
  end
end

-- **Where a frame's time goes**, for `--report` (Diego, on the M700: "the
-- app feels laggy", "Are we redrawing the entire ui every frame"): drawing
-- - Groove's Lua and the Graphics Kit under it - and handing the window to
-- the window manager, counted apart, because the answer to a slow frame is
-- different for each.
local frames = { n = 0, draw = 0, draw_worst = 0, commit = 0, commit_worst = 0,
                 since = sys.ticks() }

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
  local period = (info.audio_period or 0) // (2 * math.max(1, info.audio_channels or 2))
  -- What the audio server keeps in the device now (`depth.h`), which can
  -- be fewer than it will hold.
  local kept = stats.kept or info.audio_periods or 0
  local holds = kept * period / math.max(1, info.audio_rate or 44100) * 1000

  -- The DSP load, all told: the time spent rendering over the time it
  -- rendered. Past one, no scheduler can keep the sound whole.
  local load = (st.busy and st.rendered and st.rendered > 0)
               and (st.busy / hz) / (st.rendered / E.SR) or 0

  -- Away since its last pass counts too: a thread held off for good has no
  -- second pass to measure a gap by, and published nothing since. Asked of
  -- the kit now rather than from the frame's copy, which under load is as
  -- old as this window's last turn - half a second, once.
  local fresh = synth.state()
  local away = math.max(fresh.worst_pass or 0,
                        (fresh.last_pass and fresh.last_pass > 0)
                        and (sys.ticks() - fresh.last_pass) or 0)

  print(("groove: after %d s: the device holds %.1f ms; the kit's worst pass %.1f ms, "
         .. "the audio server's worst turn %.1f ms; the kit %s, %d periods kept, "
         .. "its ring ran dry %d times; DSP %.0f%%; the device ran dry %d times")
        :format(report, holds, away / hz * 1000, (stats.late or 0) / 1000,
                st.audio_band and "in the audio band" or "not in the audio band",
                st.ahead or 0, st.dry or 0, load * 100, info.audio_dry or -1))

  -- The machine's part: the most any processor's ticks were held off in
  -- this window. A party away longer than the device holds skipped only
  -- past this.
  local held = 0

  for _, c in ipairs(sys.cpuload() or {}) do
    held = math.max(held, (c.held_off_counter or 0) - (heldOffFrom[c.index] or 0))
  end

  print(("groove: the machine was held off %.1f ms in the %d s, by its own ticks")
        :format(held / hz * 1000, report))

  local n = math.max(1, frames.n)
  local ms = 1000 / hz

  -- How much sound came through, by the kit's count: it renders only into
  -- the room the audio server makes, and the server takes at the device's
  -- pace - so this is the device's, and under QEMU's emulation, loaded, it
  -- runs at about half the time that passed (`testing.md` 18.265). Said,
  -- not held to anything.
  local since = soundSince and (sys.ticks() - soundSince) / hz or 0

  print(("groove: the kit rendered %.2f s of sound in the %.2f s since it began")
        :format((st.rendered or 0) / E.SR, since))

  -- How the two depths settled: the kit's ring and the server's device,
  -- each kept by `depth.h`'s rule.
  print(("groove: the kit keeps %d periods, last changed %.2f s after it began; "
         .. "the audio server keeps %d in the device, and found it empty %d times")
        :format(fresh.ahead or 0,
                (soundSince and fresh.ahead_changed and fresh.ahead_changed > 0)
                and math.max(0, (fresh.ahead_changed - soundSince) / hz) or 0,
                kept, stats.device_dry or 0))

  local d = U.drawn

  print(("groove: of %d frames %d drawn whole; %.1f%% of the window's pixels drawn a frame")
        :format(d.frames, d.whole, d.px / math.max(1, d.frames * d.window) * 100))

  if U.checking then
    print(("groove: redraw check: %d frames drawn both ways, %d pixels differ")
          :format(U.checked or 0, U.wrong or 0))
  end

  print(("groove: %dx%d, %.1f frames a second; drawing %.1f ms a frame, %.1f at worst; "
         .. "handing it over %.1f ms, %.1f at worst")
        :format(W, H, frames.n / math.max(1e-9, (sys.ticks() - frames.since) / hz),
                frames.draw / n * ms, frames.draw_worst * ms,
                frames.commit / n * ms, frames.commit_worst * ms))
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

  -- Drawn only where it changed, and only that handed over; nothing, when
  -- nothing did (`groove/ui.lua`).
  local damage = U.flush()
  local drawn = sys.ticks()

  if damage and not win:commit(damage) then return false end

  local handed = sys.ticks()

  frames.n = frames.n + 1
  frames.draw = frames.draw + (drawn - t)
  frames.commit = frames.commit + (handed - drawn)
  frames.draw_worst = math.max(frames.draw_worst, drawn - t)
  frames.commit_worst = math.max(frames.commit_worst, handed - drawn)

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
if kitMidi then kitMidi:close() end
if out then out:close() end
win:close()
print("groove: closed")
