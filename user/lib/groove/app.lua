-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Groove: a session-view groove workstation (`roadmap.md` 6zh) - Diego's
-- PulseMusic, converted to Kosmos rather than vendored into it, and looking
-- as PulseMusic looks ("PulseMusic's look, faithfully").
--
-- **This is PulseMusic's `app.lua`**, function for function: the top bar,
-- eight track columns with their clips and mixers, the scenes, the master
-- effects, the song arrangement, the clip editors - drum grid, piano roll,
-- automation lane - and the device panel. What changed is underneath it:
--
-- - the sound is the Synth Kit's, on a thread of its own, so there is no
--   `pump` here and nothing this window does can make the audio late;
-- - `E` is `groove/engine.lua`, which keeps the song and hands it to the kit,
--   and says what is being heard - already past the latency PulseMusic
--   subtracted, so `latency` is gone from every call;
-- - `U` draws on a surface, and the window (`/Kosmos/Apps/groove.lua`) feeds
--   it the pointer and the keys;
-- - files are Kosmos's: the project in `/Home/Documents/Groove`, exports in
--   `/Home/Music`, where Music finds them.
--
-- MIDI - the Launchkey, and any keyboard - is the next step (`roadmap.md`
-- 6zg), after Groove plays; its button is here and says so.

local E = use("/Kosmos/Libraries/groove/engine.lua")
local P = use("/Kosmos/Libraries/groove/presets.lua")
local U = use("/Kosmos/Libraries/groove/ui.lua")
local Demos = use("/Kosmos/Libraries/groove/demos.lua")
local clock = use("/Kosmos/Libraries/clock.lua")

local app = {}
local C = U.C
local floor, min, max = math.floor, math.min, math.max

local sel = { track = 1, scene = 1, row = 1 }
local prTop, lastLen, pr, paint = {}, {}, nil, nil
local clipboard, toast, toastT, exportState = nil, "", 0, nil
local recArm, octave, held, songScroll = false, 3, {}, 0
local meters, mMeterL, mMeterR = {}, 0, 0
local cpu, loadT, loadBusy, loadFrames = 0, 0, nil, nil
local L = {}
local now = 0                                   -- seconds, for the blink

-- Whether the top bar is the window's title bar, and the room the three
-- take at its right end when it is (`/Kosmos/Apps/groove.lua`).
local chrome = { headed = false, lights = nil }

function app.chrome(headed, lights) chrome.headed, chrome.lights = headed, lights end

local DIR = "/Home/Documents/Groove"
local PROJECT = DIR .. "/Project.groove"
local MUSIC = "/Home/Music"

local NOTE_KEYS = { a = 0, w = 1, s = 2, e = 3, d = 4, f = 5, t = 6, g = 7, y = 8, h = 9, u = 10, j = 11, k = 12, o = 13, l = 14 }
local DRUM_KEYS = { a = 1, s = 2, d = 3, f = 4, g = 5, h = 6, j = 7, k = 8 }
local BLACK = { [1] = true, [3] = true, [6] = true, [8] = true, [10] = true }

local function say(msg) toast, toastT = msg, 3.5 end
local function track() return E.song.tracks[sel.track] end
local function selClip() return track().clips[sel.scene] end

app.say = say

---------------------------------------------------------------- automation (app side)
-- autoFocus: the lane shown in the editor follows the last control touched.
local autoFocus, selEntry, lanePaint = nil, 1, nil
local recLast, chainBackup = nil, nil

local function songRecording() return recArm and E.playing and E.mode == "song" end

-- A control was moved by hand. `old` is its value before the move.
local function touched(target, label, old, spec, color)
  autoFocus = { target = target, label = label, color = color }
  if songRecording() then
    if not E.isAutomated(target) and old ~= nil then
      E.song.autoBase = E.song.autoBase or {}
      E.song.autoBase[target] = P.toNorm(spec, old)
    end
    E.recTouched = E.recTouched or {}
    E.recTouched[target] = true
  elseif E.isAutomated(target) then
    local cp = E.playing and E.mode == "song" and E.heardSong()
    local entry = cp and E.song.chain[cp]
    local lane = entry and entry.auto and entry.auto[target]
    if lane and next(lane) then E.hold(target)                  -- your hand wins until the next section
    else E.song.autoBase[target] = E.autoGet(target) end       -- plain edit: new resting value
  end
end

local function autoBadge(target, x, y, w, h)
  if not E.isAutomated(target) then return end
  U.col(E.autoHold[target] and C.dim or C.rec)
  U.circle(x + w - 9, y + 7, 3)
  if U.hit(x, y, w, h) and U.rpressed and not U.active then
    E.autoClearTarget(target); U.rpressed = false
    say("Automation cleared for this control")
  end
end

local function aknob(id, target, label, x, y, w, h, spec, c, k, color)
  local old = c[k]
  local v = U.knob(id, x, y, w, h, spec, old, color)
  if U.active == id and (not autoFocus or autoFocus.target ~= target) then
    autoFocus = { target = target, label = label, color = color }
  end
  if v and v ~= old then c[k] = v; touched(target, label, old, spec, color) end
  autoBadge(target, x, y, w, h)
  return v
end

-- REC + song playing: every control touched during the pass writes its value on each
-- heard step until the pass ends (latch). Called once per frame.
local function autoRecordTick()
  if not songRecording() then
    if E.recTouched then E.recTouched = nil end
    recLast = nil
    return
  end
  if not E.recTouched then return end
  local cp, ss = E.heardSong()
  if not cp or cp < 1 then return end
  local key = cp * 100000 + ss
  if key == recLast then return end
  recLast = key
  local entry = E.song.chain[cp]
  if not entry then return end
  for target in pairs(E.recTouched) do
    local n = E.autoGet(target)
    if n then E.autoWrite(entry, target, ss + 1, n) end
  end
end

---------------------------------------------------------------- export
local function exportName()
  local t = clock.now()
  local base = t and ("Groove %04d-%02d-%02d %02d.%02d"):format(t.year, t.month, t.day, t.hour, t.min)
               or "Groove"
  local name, n = base .. ".wav", 1
  while fs.getattr(MUSIC .. "/" .. name) do
    n = n + 1
    name = ("%s %d.wav"):format(base, n)
  end
  return name
end

---------------------------------------------------------------- lifetime
function app.load()
  U.load()
  E.setSong(Demos.techno())
  for i = 1, E.NT do meters[i] = 0 end
end

-- `busy` and `rendered` are the kit's, all told; the load is their change
-- over half a second - render time over the time it rendered.
function app.update(dt, counter_hz)
  if exportState == "go" then
    exportState = nil
    fs.send(MUSIC, { type = "mkdir" })
    local name = exportName()
    local ok, res = pcall(E.export, MUSIC .. "/" .. name)
    if ok then say(string.format("Exported %s (%.0fs) to %s", name, res, MUSIC))
    else say("Export failed: " .. tostring(res)) end
  end
  autoRecordTick()
  E.update()
  now = now + dt
  local st = E.kitState()
  loadT = loadT + dt
  if loadT >= 0.5 and st.busy then
    if loadBusy and st.rendered > loadFrames and counter_hz then
      cpu = ((st.busy - loadBusy) / counter_hz) / ((st.rendered - loadFrames) / E.SR)
    end
    loadBusy, loadFrames, loadT = st.busy, st.rendered, 0
  end
  toastT = max(0, toastT - dt)
  for i = 1, E.NT do
    meters[i] = max(meters[i] * 0.88, E.rt[i].peak)
  end
  mMeterL = max(mMeterL * 0.88, E.masterPeakL); mMeterR = max(mMeterR * 0.88, E.masterPeakR)
end

-- Whether a frame would show anything new without a person doing anything:
-- the song moving, a clip blinking to say it waits for the bar, a message
-- fading, a meter falling.
function app.busy()
  if E.playing or toastT > 0 or exportState then return true end
  if mMeterL > 0.0005 or mMeterR > 0.0005 then return true end
  for i = 1, E.NT do if meters[i] > 0.0005 or E.rt[i].queued then return true end end
  return false
end

---------------------------------------------------------------- clip ops
local function createClip(ti, sc)
  local tr = E.song.tracks[ti]
  if tr.clips[sc] then return end
  local n = 0
  for _ in pairs(tr.clips) do n = n + 1 end
  tr.clips[sc] = E.newClip(tr.type, 16, tr.name:sub(1, 1) .. tr.name:sub(2):lower() .. " " .. (n + 1))
end

local function doubleClip(c)
  if c.len >= 64 then return say("Clip is already 64 steps") end
  local l = c.len
  if c.steps then for r = 1, 8 do for s = 1, l do c.steps[r][s + l] = c.steps[r][s] end end
  else
    for i = 1, #c.notes do
      local n = c.notes[i]
      c.notes[#c.notes + 1] = { step = n.step + l, pitch = n.pitch, len = n.len, vel = n.vel }
    end
  end
  c.len = l * 2
end

local function clearClip(c)
  if c.steps then for r = 1, 8 do for s = 1, 64 do c.steps[r][s] = 0 end end else c.notes = {} end
end

local function heardPos(ti)
  local rt = E.rt[ti]
  local clip = rt.playing and E.song.tracks[ti].clips[rt.playing]
  if not clip or not E.playing then return end
  local h = E.heardStep()
  if not h or h < rt.clipStart then return end
  return clip, (h - rt.clipStart) % clip.len, h
end

---------------------------------------------------------------- layout
function app.size(w, h) L.W, L.H = w, h end

local function layout()
  L.topH, L.botH, L.rightW, L.sceneW, L.masterW = 48, 300, 240, 112, 100
  L.sesY, L.sesH = L.topH, L.H - L.topH - L.botH
  L.trackW = floor((L.W - L.rightW - L.sceneW - L.masterW) / E.NT)
  L.headH, L.stopH = 26, 20
  L.slotH = max(18, min(26, floor((L.sesH - L.headH - L.stopH - 176) / E.NS)))
  L.slotY = L.sesY + L.headH
  L.stopY = L.slotY + L.slotH * E.NS
  L.mixY = L.stopY + L.stopH
  L.mixH = L.sesY + L.sesH - L.mixY
  L.devW = 9 * 62 + 20
end

---------------------------------------------------------------- top bar
local PLAY_O = { glyph = "play", color = C.play, ic = C.play }
local STOP_O = { glyph = "stop" }
local REC_O = { glyph = "rec", color = C.rec, ic = C.rec }
local SESSION_O, SONG_O = { color = C.blue }, { color = C.blue }
local BPM_O, SWING_O = { wheelStep = 1 }, { wheelStep = 0.01 }
local EXPORT_O, MIDI_O = { tc = C.accent }, {}

local function swingLabel(s) return string.format("%d%%", floor(s * 100 + 0.5)) end

local function drawTop()
  U.rect(0, 0, L.W, L.topH, C.panel, 0)
  U.text("GROOVE", 14, 12, C.accent, U.fL)
  local x, y, h = 106, 10, 28
  PLAY_O.on = E.playing
  if U.button(x, y, 36, h, nil, PLAY_O) then
    if E.playing then E.stop() end
    E.play()
  end
  x = x + 40
  if U.button(x, y, 36, h, nil, STOP_O) then E.stop() end
  x = x + 40
  REC_O.on = recArm
  if U.button(x, y, 36, h, nil, REC_O) then
    recArm = not recArm
    say(recArm and "REC armed: notes record into the playing clip. In SONG mode, knob moves record too." or "REC off")
  end
  x = x + 50
  U.text("BPM", x, 18, C.dim, U.fS); x = x + 26
  local v = U.dragNum("bpm", x, y, 58, h, E.song.bpm, 60, 200, 0.25, "%.1f", BPM_O)
  if v then E.song.bpm = floor(v * 2 + 0.5) / 2 end
  x = x + 68
  U.text("SWING", x, 18, C.dim, U.fS); x = x + 40
  v = U.dragNum("swing", x, y, 48, h, E.song.swing, 0, 0.4, 0.003, swingLabel, SWING_O)
  if v then E.song.swing = v end
  x = x + 60
  local hs = E.playing and E.heardStep() or 0
  hs = floor(hs or 0)
  U.rect(x, y, 78, h, C.dark)
  U.text(string.format("%d . %d . %d", floor(hs / 16) + 1, floor(hs / 4) % 4 + 1, hs % 4 + 1), x, 17, E.playing and C.play or C.dim, U.fM, 78)
  x = x + 92
  SESSION_O.on, SONG_O.on = E.mode == "session", E.mode == "song"
  if U.button(x, y, 66, h, "SESSION", SESSION_O) then E.setMode("session") end
  if U.button(x + 68, y, 54, h, "SONG", SONG_O) then E.setMode("song") end
  x = x + 140
  U.text("DEMOS", x, 18, C.dim, U.fS); x = x + 42
  if U.button(x, y, 60, h, "TECHNO") then E.setSong(Demos.techno()); say("Loaded techno demo - press SPACE") end
  if U.button(x + 62, y, 54, h, "HOUSE") then E.setSong(Demos.house()); say("Loaded house demo - press SPACE") end
  if U.button(x + 118, y, 44, h, "NEW") then E.setSong(E.newSong()); say("Empty project") end
  x = x + 180
  if U.button(x, y, 46, h, "SAVE") then app.save() end
  if U.button(x + 48, y, 46, h, "LOAD") then app.open() end
  if U.button(x + 96, y, 86, h, "EXPORT WAV", EXPORT_O) then exportState = "pending" end
  if U.button(x + 196, y, 46, h, "MIDI", MIDI_O) then
    say("MIDI keyboards are not supported yet: they come in the next step")
  end
  local right = (chrome.headed and chrome.lights) and (L.W - chrome.lights.w - 24) or (L.W - 10)
  U.text(string.format("DSP %2.0f%%", cpu * 100), right - 70, 18, cpu > 0.7 and C.rec or C.dim, U.fS, 70, "right")

  -- The bar's empty band is the title bar's, when it is one: a press there
  -- is handed to the window manager, which moves the window until the
  -- button comes up - so this window hears no more of it.
  if chrome.headed and app.onTitle and U.pressed and not U.active and U.hit(0, 0, L.W, L.topH) then
    U.pressed, U.down = false, false
    app.onTitle(U.mx, U.my)
  end
end

function app.save()
  fs.send(DIR, { type = "mkdir" })
  local ok, err = E.save(PROJECT)
  say(ok and ("Saved to " .. PROJECT) or ("Save failed: " .. tostring(err)))
end

function app.open()
  local ok, err = E.load(PROJECT)
  say(ok and "Project loaded" or ("Load failed: " .. tostring(err)))
end

---------------------------------------------------------------- session + mixer
local STOPCLIP_O = { glyph = "stop", is = 3.5, bg = C.panel }
local MUTE_O, SOLO_O = { color = C.accent, bg = C.dark }, { color = C.blue, bg = C.dark }

local function drawTrackColumn(ti, x)
  local tr, rt, w = E.song.tracks[ti], E.rt[ti], L.trackW - 2
  local col = U.trackColors[tr.color]
  local selected = sel.track == ti
  -- header
  U.rect(x, L.sesY + 2, w, L.headH - 4, col, 3, selected and 1 or 0.55)
  U.text(tr.name, x + 6, L.sesY + 7, C.dark, U.fM)
  U.text(tr.type == "drum" and "DRM" or "SYN", x, L.sesY + 9, C.dark, U.fS, w - 5, "right")
  if U.hit(x, L.sesY, w, L.headH) and U.pressed and not U.active then sel.track = ti; U.pressed = false end
  -- slots
  for sc = 1, E.NS do
    local y = L.slotY + (sc - 1) * L.slotH
    local clip, sh = tr.clips[sc], L.slotH - 2
    local isSel = selected and sel.scene == sc
    local hot = U.hit(x, y, w, sh) and not U.active
    if clip then
      local playing = rt.playing == sc and E.playing
      U.rect(x, y, w, sh, col, 3, playing and 1 or (hot and 0.75 or 0.55))
      local queued = rt.queued == sc
      if queued and floor(now * 6) % 2 == 0 then U.col(C.text) else U.col(C.dark, playing and 1 or 0.7) end
      U.icon("play", x + 10, y + sh / 2, 4.5)
      U.text(clip.name, x + 20, y + (sh - 12) / 2, C.dark, U.fS, nil, nil, w - 22)
      if playing then
        local _, pos = heardPos(ti)
        if pos then U.rect(x, y + sh - 3, w * pos / clip.len, 3, C.dark, 0, 0.75) end
      end
      if hot and U.pressed then
        sel.track, sel.scene = ti, sc
        if U.mx < x + 20 then E.launchClip(ti, sc) end
        U.pressed = false
      end
    else
      U.rect(x, y, w, sh, C.panel, 3)
      if hot then U.text("+", x, y + (sh - 14) / 2, C.dim, U.fM, w) end
      if hot and U.pressed then
        if isSel then createClip(ti, sc) end
        sel.track, sel.scene = ti, sc; U.pressed = false
      end
    end
    if isSel then
      U.col(C.text); U.frame(x, y, w, sh, 3); U.frame(x + 1, y + 1, w - 2, sh - 2, 2)
    end
  end
  -- stop
  STOPCLIP_O.ic = (rt.queued == 0) and C.accent or C.dim
  if U.button(x, L.stopY + 1, w, L.stopH - 3, nil, STOPCLIP_O) then
    E.stopClip(ti)
  end
  -- mixer
  U.rect(x, L.mixY, w, L.mixH - 2, selected and C.panel2 or C.panel, 3)
  local kw, kh, y = floor(w / 2), 50, L.mixY + 4
  local mp = P.mixParams
  local tp = "t" .. ti .. ".m."
  aknob("sa" .. ti, tp .. "sendA", tr.name .. " / DELAY SEND", x, y, kw, kh, mp.sendA, tr, "sendA", C.blue)
  aknob("sb" .. ti, tp .. "sendB", tr.name .. " / REVERB SEND", x + kw, y, kw, kh, mp.sendB, tr, "sendB", C.blue)
  aknob("dk" .. ti, tp .. "duck", tr.name .. " / DUCK", x, y + kh, kw, kh, mp.duck, tr, "duck", C.play)
  aknob("pn" .. ti, tp .. "pan", tr.name .. " / PAN", x + kw, y + kh, kw, kh, mp.pan, tr, "pan", col)
  local v
  local by = L.mixY + L.mixH - 26
  local fy, fh = y + kh * 2 + 10, by - (y + kh * 2) - 20
  if fh > 20 then
    local oldVol = tr.vol
    v = U.fader("vol" .. ti, x + w / 2 - 16, fy, 22, fh, tr.vol, 0.7, col)
    if U.active == "vol" .. ti and (not autoFocus or autoFocus.target ~= tp .. "vol") then
      autoFocus = { target = tp .. "vol", label = tr.name .. " / VOLUME", color = col }
    end
    if v and v ~= oldVol then tr.vol = v; touched(tp .. "vol", tr.name .. " / VOLUME", oldVol, P.volParam, col) end
    autoBadge(tp .. "vol", x + w / 2 - 22, fy - 8, 40, fh + 16)
    U.meter(x + w / 2 + 12, fy, 6, fh, meters[ti])
  end
  MUTE_O.on, SOLO_O.on = tr.mute, tr.solo
  if U.button(x + 4, by, kw - 6, 20, "M", MUTE_O) then tr.mute = not tr.mute end
  if U.button(x + kw + 2, by, kw - 6, 20, "S", SOLO_O) then tr.solo = not tr.solo end
end

local STOPALL_O = { bg = C.panel, tc = C.dim }
local HELP = { "SPACE  play / stop", "1-8  launch scene", "0  stop all clips", "TAB  session / song", "A..L  play notes",
  "Z X  octave", "ENTER  new clip", "CTRL C V D  clip", "DEL  delete clip", "SHIFT  accent / fine" }

local function drawSceneColumn(x)
  local w = L.sceneW - 2
  -- same header box geometry as the track columns, so the column reads as header + 8 rows
  U.rect(x, L.sesY + 2, w, L.headH - 4, C.panel2, 3)
  U.text("SCENES", x, L.sesY + 8, C.dim, U.fS, w)
  for sc = 1, E.NS do
    local y = L.slotY + (sc - 1) * L.slotH
    local sh = L.slotH - 2
    local hot = U.hit(x, y, w, sh) and not U.active
    -- hovering a launcher lights the whole row it will launch
    if hot then U.rect(4, y, x - 6, sh, C.text, 3, 0.07) end
    U.rect(x, y, w, sh, hot and C.panel3 or C.panel2, 3)
    U.col(C.play); U.icon("play", x + 10, y + sh / 2, 4.5)
    U.text(E.song.scenes[sc], x + 20, y + (sh - 12) / 2, sel.scene == sc and C.text or C.dim, U.fS, nil, nil, w - 22)
    if hot and U.pressed then
      sel.scene = sc; E.launchScene(sc)
      if not E.playing then E.play() end
      U.pressed = false
    end
  end
  if U.button(x, L.stopY + 1, w, L.stopH - 3, "STOP ALL", STOPALL_O) then
    for ti = 1, E.NT do E.stopClip(ti) end
  end
  local y = L.mixY + 8
  for i, s in ipairs(HELP) do
    if i == 6 then s = s .. " (" .. octave .. ")" end
    if y + 12 < L.mixY + L.mixH then U.text(s, x + 4, y, C.dim, U.fS) end
    y = y + 14
  end
end

local function drawMasterColumn(x)
  local w, fx = L.masterW - 2, E.song.fx
  U.text("MASTER FX", x, L.sesY + 8, C.dim, U.fS, w)
  local areaH = L.slotH * E.NS + L.stopH
  local kw, kh = floor(w / 2), floor(areaH / 3)
  U.rect(x, L.slotY, w, areaH - 2, C.panel, 3)
  for i = 1, 6 do
    local s = P.fxParams[i]
    local kx, ky = x + ((i - 1) % 2) * kw, L.slotY + floor((i - 1) / 2) * kh + 2
    aknob("fx" .. i, "fx." .. s.k, "MASTER FX / " .. s.l, kx, ky, kw, kh - 2, s, fx, s.k, C.blue)
  end
  U.rect(x, L.mixY, w, L.mixH - 2, C.panel, 3)
  for i = 7, 8 do
    local s = P.fxParams[i]
    aknob("fx" .. i, "fx." .. s.k, "MASTER FX / " .. s.l, x + (i - 7) * kw, L.mixY + 4, kw, 50, s, fx, s.k, C.blue)
  end
  local fy = L.mixY + 64
  local fh = L.mixY + L.mixH - 12 - fy
  if fh > 20 then
    local v = U.fader("master", x + w / 2 - 22, fy, 24, fh, E.song.master, 0.8, C.text); if v then E.song.master = v end
    U.meter(x + w / 2 + 8, fy, 6, fh, mMeterL); U.meter(x + w / 2 + 16, fy, 6, fh, mMeterR)
  end
end

---------------------------------------------------------------- song chain panel
local CHAIN_O = { int = true, bg = C.panel }
local X_O = { bg = C.panel, tc = C.dim }
local FROMSTART_O = { tc = C.play }
local KEEP_O = { on = true, color = C.rec }
local RECPERF_O, UNDO_O = { tc = C.rec }, { tc = C.dim }

local function sceneLabel(s) s = floor(s + 0.5); return s .. "  " .. E.song.scenes[s] end
local function barsLabel(b) return floor(b + 0.5) .. " bars" end

local function drawSongPanel()
  local x, y, w, h = L.W - L.rightW, L.sesY, L.rightW, L.sesH
  CHAIN_O.font = U.fS
  U.rect(x + 2, y + 2, w - 4, h - 4, C.panel, 4)
  U.text("SONG ARRANGEMENT", x + 12, y + 9, C.text, U.fS)
  local secs, bars = E.songSeconds()
  U.text(string.format("%d bars  %d:%02d", bars, floor(secs / 60), floor(secs % 60)), x, y + 9, C.dim, U.fS, w - 12, "right")
  local chain = E.song.chain
  local ly, lh, rowH = y + 30, h - 30 - 92, 24
  local visible = floor(lh / rowH)
  songScroll = max(0, min(songScroll, #chain - visible))
  local heardCp, heardSs = nil, nil
  if E.mode == "song" and E.playing then heardCp, heardSs = E.heardSong() end
  local remove
  for i = 1 + songScroll, min(#chain, songScroll + visible) do
    local e, ry = chain[i], ly + (i - 1 - songScroll) * rowH
    local cur = heardCp == i
    U.rect(x + 8, ry, w - 16, rowH - 2, cur and C.panel3 or C.panel2, 3)
    if cur then
      local prog = (heardSs + 1) / (e.bars * 16)
      U.rect(x + 8, ry + rowH - 4, (w - 16) * max(0, min(1, prog)), 2, C.play, 0)
    end
    local isSel = E.mode == "song" and selEntry == i
    if U.hit(x + 8, ry, 22, rowH - 2) and U.pressed and not U.active then selEntry = i; U.pressed = false end
    if isSel then U.rect(x + 8, ry, 3, rowH - 2, C.accent, 1) end
    U.text(tostring(i), x + 10, ry + 5, isSel and C.text or C.dim, U.fS, 18, "center")
    if e.auto and next(e.auto) then U.col(C.rec); U.circle(x + 26, ry + 5, 2) end
    local v = U.dragNum("chs" .. i, x + 30, ry + 1, 108, rowH - 4, e.scene, 1, E.NS, 0.06, sceneLabel, CHAIN_O)
    if v then e.scene = floor(v + 0.5) end
    v = U.dragNum("chb" .. i, x + 140, ry + 1, 58, rowH - 4, e.bars, 1, 64, 0.12, barsLabel, CHAIN_O)
    if v then e.bars = floor(v + 0.5) end
    if U.button(x + 200, ry + 1, 28, rowH - 4, "x", X_O) then remove = i end
  end
  if remove and #chain > 1 then table.remove(chain, remove); selEntry = max(1, min(#chain, selEntry)) end
  if U.hit(x, ly, w, lh) and U.wheel ~= 0 then songScroll = songScroll - U.wheel; U.wheel = 0 end
  local by = y + h - 88
  if U.button(x + 8, by, w - 16, 22, "+ ADD SCENE  [" .. E.song.scenes[sel.scene] .. "]") then
    chain[#chain + 1] = { scene = sel.scene, bars = 8 }
    songScroll = #chain
  end
  if U.button(x + 8, by + 26, w - 16, 24, "PLAY SONG FROM START", FROMSTART_O) then
    E.perform = false
    E.setMode("song"); E.play()
  end
  -- perform the arrangement: launch scenes live, every bar is written into the song
  if E.perform then
    local _, pbars = E.songSeconds()
    if U.button(x + 8, by + 54, w - 16, 24, string.format("STOP + KEEP  (%d bars)", pbars), KEEP_O) then
      E.perform = false; E.stop()
      if #E.song.chain == 0 then E.song.chain = chainBackup; chainBackup = nil; say("Nothing performed, song unchanged")
      else selEntry = 1; say("Song written from your performance. UNDO brings the old one back.") end
    end
  else
    local bw = chainBackup and (w - 16 - 54) or (w - 16)
    if U.button(x + 8, by + 54, bw, 24, "REC PERFORMANCE", RECPERF_O) then
      chainBackup = E.copy(E.song.chain)
      if E.mode ~= "session" then E.setMode("session") else E.stop() end
      E.song.chain = {}
      E.perform = true
      say("Launch scenes now (click or keys 1-8). Every bar goes into the song.")
    end
    if chainBackup and U.button(x + 8 + bw + 4, by + 54, 50, 24, "UNDO", UNDO_O) then
      E.song.chain = chainBackup; chainBackup = nil; selEntry = 1
      say("Previous song restored")
    end
  end
end

---------------------------------------------------------------- clip editors
local ROW_O = {}

local function drawDrumEditor(x, y, w, h, tr, clip, ti)
  local keyW = 64
  local gx, gw, rowH = x + keyW, w - keyW, floor(h / 8)
  local cw = gw / clip.len
  local col = U.trackColors[tr.color]
  ROW_O.color = col
  for r = 1, 8 do
    local ry = y + (r - 1) * rowH
    ROW_O.on = sel.row == r
    if U.button(x, ry + 1, keyW - 4, rowH - 2, P.rowNames[r], ROW_O) then
      sel.row = r; E.triggerDrum(ti, r, 0.9)
    end
    for s = 1, clip.len do
      local cx = gx + (s - 1) * cw
      local beat = floor((s - 1) / 4) % 2 == 0
      U.rect(cx + 1, ry + 1, cw - 2, rowH - 2, beat and C.panel3 or C.panel2, 2, beat and 0.75 or 1)
      local vel = clip.steps[r][s]
      if vel > 0 then
        U.rect(cx + 1, ry + 1, cw - 2, rowH - 2, col, 2, 0.35 + 0.65 * vel)
        if vel >= 0.95 then U.rect(cx + 1, ry + 1, cw - 2, 3, C.text, 1, 0.9) end
      end
    end
  end
  local inGrid = U.hit(gx, y, gw, rowH * 8)
  if inGrid then
    local s, r = floor((U.mx - gx) / cw) + 1, floor((U.my - y) / rowH) + 1
    s, r = max(1, min(clip.len, s)), max(1, min(8, r))
    if U.pressed and not U.active then
      local on = clip.steps[r][s] > 0
      paint = on and 0 or (U.shift and 1.0 or (U.alt and 0.42 or 0.8))
      U.active, U.pressed = "drumgrid", false
      if not on and not E.playing then E.triggerDrum(ti, r, paint) end
      sel.row = r
    elseif U.rpressed then clip.steps[r][s] = 0 end
    if U.active == "drumgrid" and U.down then clip.steps[r][s] = paint end
  end
  local _, pos = heardPos(ti)
  if pos and E.rt[ti].playing == sel.scene then U.rect(gx + floor(pos) * cw, y, cw, rowH * 8, C.text, 0, 0.12) end
end

local function noteAt(clip, step, pitch)
  for i = #clip.notes, 1, -1 do
    local n = clip.notes[i]
    if n.pitch == pitch and step >= n.step and step < n.step + n.len then return n, i end
  end
end

local KEY_WHITE = { 0.8, 0.8, 0.83 }

local function drawPianoRoll(x, y, w, h, tr, clip, ti)
  local keyW, rowH = 44, 12
  local rows = floor(h / rowH)
  local gx, gw = x + keyW, w - keyW
  local cw = gw / clip.len
  local col = U.trackColors[tr.color]
  local key = ti .. ":" .. sel.scene
  local top = prTop[key]
  if not top then
    local hi, lo = 0, 127
    for _, n in ipairs(clip.notes) do hi = max(hi, n.pitch); lo = min(lo, n.pitch) end
    top = #clip.notes > 0 and floor((hi + lo) / 2 + rows / 2) or (tr.params.mono == 2 and 52 or 76)
  end
  if U.hit(x, y, w, h) and U.wheel ~= 0 then top = top + U.wheel * 2; U.wheel = 0 end
  top = max(rows + 11, min(108, top))
  prTop[key] = top
  for i = 0, rows - 1 do
    local p, ry = top - i, y + i * rowH
    local black = BLACK[p % 12]
    U.rect(gx, ry, gw, rowH - 1, black and C.panel or C.panel2, 0, p % 12 == 0 and 1 or 0.8)
    U.rect(x, ry, keyW - 4, rowH - 1, black and C.dark or KEY_WHITE, 1)
    if p % 12 == 0 then U.text("C" .. (p // 12 - 2), x + 3, ry - 1, C.dark, U.fS) end
  end
  for s = 0, clip.len do
    local a = (s % 16 == 0) and 0.9 or ((s % 4 == 0) and 0.45 or 0.15)
    U.vline(gx + s * cw, y, y + rows * rowH, C.line, a)
  end
  -- The notes inside the grid only, which LÖVE's scissor did.
  local lowest = top - rows + 1
  for _, n in ipairs(clip.notes) do
    if n.pitch <= top and n.pitch >= lowest then
      local ny = y + (top - n.pitch) * rowH
      U.rect(gx + n.step * cw + 1, ny, min(n.len * cw, gw - n.step * cw) - 2, rowH - 1, col, 2, 0.45 + 0.55 * n.vel)
      if n.vel >= 0.95 then U.rect(gx + n.step * cw + 1, ny, 3, rowH - 1, C.text, 1) end
    end
  end
  local _, pos = heardPos(ti)
  if pos and E.rt[ti].playing == sel.scene then U.rect(gx + pos * cw, y, 2, rows * rowH, C.text, 0, 0.8) end

  local step = max(0, min(clip.len - 1, floor((U.mx - gx) / cw)))
  local pitch = top - max(0, min(rows - 1, floor((U.my - y) / rowH)))
  if U.hit(gx, y, gw, rows * rowH) and not U.active then
    local n, idx = noteAt(clip, step, pitch)
    if U.rpressed and n then table.remove(clip.notes, idx)
    elseif U.pressed then
      U.pressed = false
      if n and U.shift then n.vel = n.vel >= 0.95 and 0.8 or 1.0
      else
        local mode = "len"
        if n then
          mode = (U.mx > gx + (n.step + n.len) * cw - 6) and "len" or "move"
        else
          n = { step = step, pitch = pitch, len = lastLen[ti] or 1, vel = 0.8 }
          clip.notes[#clip.notes + 1] = n
        end
        pr = { mode = mode, note = n, d = step - n.step, preview = n.pitch, ti = ti, moved = false }
        U.active = "pr"
        E.noteOn(ti, n.pitch, n.vel)
      end
    end
  end
  if U.active == "pr" and pr then
    local n = pr.note
    if pr.mode == "len" then
      local nl = max(1, step - n.step + 1)
      if nl ~= n.len then n.len = nl; pr.moved = true end
    else
      n.step = max(0, min(clip.len - 1, step - pr.d))
      if pitch ~= n.pitch then
        E.noteOff(ti, pr.preview); n.pitch = pitch; pr.preview = pitch; E.noteOn(ti, pitch, n.vel)
      end
    end
  end
end

local function targetLabel(target)
  local _, _, spec = E.resolve(target)
  if not spec then return target end
  local ti, row = target:match("^t(%d+)%.r(%d+)")
  if ti then return E.song.tracks[tonumber(ti)].name .. " / " .. P.rowNames[tonumber(row)] .. " " .. spec.l end
  ti = target:match("^t(%d+)")
  if ti then return E.song.tracks[tonumber(ti)].name .. " / " .. spec.l end
  return "MASTER FX / " .. spec.l
end

local function targetColor(target)
  local ti = target:match("^t(%d+)")
  return ti and U.trackColors[E.song.tracks[tonumber(ti)].color] or C.blue
end

-- SONG mode editor: one automation lane, one bar per 16th, painted like drum steps.
local CLEAR_O = {}

local function drawLaneEditor(x, y, w, h)
  local chain = E.song.chain
  local heardCp, heardSs
  if E.playing and E.mode == "song" then
    heardCp, heardSs = E.heardSong()
    if heardCp and chain[heardCp] and not lanePaint then selEntry = heardCp end
  end
  if #chain == 0 then
    U.text("The song is empty. Add a scene on the right, or use REC PERFORMANCE.", x, y + h / 2 - 8, C.dim, U.fM, w)
    return
  end
  selEntry = max(1, min(#chain, selEntry))
  local entry = chain[selEntry]
  U.rect(x + 10, y + 10, 8, 18, autoFocus and autoFocus.color or C.dim, 2)
  local where = string.format("section %d  %s  (%d bars)", selEntry, E.song.scenes[entry.scene], entry.bars)
  if autoFocus then
    U.text(autoFocus.label, x + 26, y + 12, autoFocus.color or C.text, U.fM)
    U.text(where, x + 34 + U.fM:getWidth(autoFocus.label), y + 14, C.dim, U.fS)
  else
    U.text("AUTOMATION", x + 26, y + 12, C.text, U.fM)
    U.text(where, x + 120, y + 14, C.dim, U.fS)
  end

  -- chips: the other controls automated in this section, one click to switch lane
  local list = {}
  if entry.auto then
    for target, lane in pairs(entry.auto) do
      if next(lane) and not (autoFocus and autoFocus.target == target) then list[#list + 1] = target end
    end
  end
  table.sort(list)
  local shownChips = min(3, #list)
  local cx = x + w - 92 - shownChips * 122
  if #list > 3 then U.text("+" .. (#list - 3), cx - 22, y + 14, C.dim, U.fS) end
  for i = 1, shownChips do
    local target = list[i]
    if U.button(cx, y + 8, 118, 22, targetLabel(target), { tc = targetColor(target), font = U.fS }) then
      autoFocus = { target = target, label = targetLabel(target), color = targetColor(target) }
    end
    cx = cx + 122
  end

  local gx, gy, gw, gh = x + 10, y + 40, w - 20, h - 68
  U.rect(gx, gy, gw, gh, C.dark, 3)
  if not autoFocus then
    U.text("Touch any knob or fader to see its lane here.", x, y + h / 2 - 18, C.dim, U.fM, w)
    U.text("To record: arm REC, play the song, turn the knob. It replays from then on.", x, y + h / 2 + 2, C.dim, U.fS, w)
    return
  end
  local target = autoFocus.target
  local _, _, spec = E.resolve(target)
  if not spec then autoFocus = nil; return end
  local col = autoFocus.color or C.accent
  local steps = entry.bars * 16
  local sw = gw / steps
  local lane = entry.auto and entry.auto[target]

  for st = 0, steps, 4 do
    local bar = st % 16 == 0
    if bar or sw * 4 >= 7 then U.vline(gx + st * sw, gy, gy + gh, C.line, bar and 1 or 0.4) end
  end
  for b = 0, entry.bars - 1 do
    if sw * 16 >= 22 then U.text(tostring(b + 1), gx + b * 16 * sw + 3, gy + 2, C.dim, U.fS) end
  end
  local base = E.song.autoBase and E.song.autoBase[target]
  if base then U.hline(gx, gx + gw, gy + gh * (1 - base), C.text, 0.25) end
  if lane then
    local bw = max(1, sw - (sw >= 4 and 1 or 0))
    for st = 1, steps do
      local v = lane[st]
      if v then U.rect(gx + (st - 1) * sw, gy + gh * (1 - v), bw, max(2, gh * v), col, 0, 0.85) end
    end
  end

  -- paint
  local over = U.hit(gx, gy, gw, gh)
  local hoverStep = max(1, min(steps, floor((U.mx - gx) / sw) + 1))
  local hoverVal = max(0, min(1, 1 - (U.my - gy) / gh))
  if spec.choices or spec.int then hoverVal = P.toNorm(spec, P.fromNorm(spec, hoverVal)) end
  if over and U.pressed and not U.active then
    lanePaint = { last = hoverStep, erase = U.shift }; U.pressed = false
  end
  if over and U.rpressed and lane then lane[hoverStep] = nil; U.rpressed = false end
  if lanePaint and U.down then
    local a, b = lanePaint.last, hoverStep
    local stepDir = a <= b and 1 or -1
    for st = a, b, stepDir do
      if lanePaint.erase then
        if lane then lane[st] = nil end
      else
        E.autoWrite(entry, target, st, hoverVal)
      end
    end
    lanePaint.last = hoverStep
    lane = entry.auto and entry.auto[target]
  elseif lanePaint then
    lanePaint = nil
    if lane and not next(lane) then E.autoClearLane(entry, target) end
  end

  if heardCp == selEntry and heardSs then U.vline(gx + heardSs * sw, gy, gy + gh, C.play) end
  if over then
    local label = U.format(spec, P.fromNorm(spec, hoverVal))
    local tx = min(gx + gw - 60, max(gx + 4, U.mx + 10))
    U.rect(tx - 4, gy + 4, 56, 16, C.dark, 3, 0.85)
    U.text(label, tx - 4, gy + 6, C.text, U.fS, 56, "center")
  end
  CLEAR_O.font = U.fS
  if U.button(x + w - 86, y + 8, 76, 22, "CLEAR LANE", CLEAR_O) and lane then E.autoClearLane(entry, target) end
  U.text("drag: paint values    shift-drag: erase    right-click: erase one step    right-click a knob: remove all its automation    REC + play song + turn knob: record",
    x + 12, y + h - 19, C.dim, U.fS)
end

local LEN_O = {}

local function drawEditor()
  local x, y, w, h = 0, L.H - L.botH, L.W - L.devW, L.botH
  U.rect(x + 2, y + 2, w - 4, h - 4, C.panel, 4)
  if E.mode == "song" then drawLaneEditor(x, y, w, h); return end
  local tr, clip = track(), selClip()
  local col = U.trackColors[tr.color]
  U.rect(x + 10, y + 10, 8, 18, col, 2)
  if not clip then
    U.text(tr.name .. "  /  " .. E.song.scenes[sel.scene], x + 26, y + 12, C.text, U.fM)
    U.text("Empty slot. Click it again or press ENTER to create a clip.", x, y + h / 2 - 8, C.dim, U.fM, w)
    return
  end
  U.text(tr.name .. "  /  " .. clip.name, x + 26, y + 12, C.text, U.fM)
  local bx = x + 260
  U.text("LENGTH", bx, y + 14, C.dim, U.fS); bx = bx + 48
  LEN_O.color = col
  for _, l in ipairs({ 16, 32, 64 }) do
    LEN_O.on = clip.len == l
    if U.button(bx, y + 8, 34, 22, (l // 16) .. " bar", LEN_O) then clip.len = l end
    bx = bx + 36
  end
  bx = bx + 10
  if U.button(bx, y + 8, 60, 22, "DOUBLE") then doubleClip(clip) end
  if U.button(bx + 62, y + 8, 50, 22, "CLEAR") then clearClip(clip) end
  bx = bx + 122
  if clip.notes then
    if U.button(bx, y + 8, 44, 22, "OCT -") then for _, n in ipairs(clip.notes) do n.pitch = max(12, n.pitch - 12) end; prTop[sel.track .. ":" .. sel.scene] = nil end
    if U.button(bx + 46, y + 8, 44, 22, "OCT +") then for _, n in ipairs(clip.notes) do n.pitch = min(108, n.pitch + 12) end; prTop[sel.track .. ":" .. sel.scene] = nil end
    U.text("drag: draw / move / resize    right-click: delete    shift-click: accent    wheel: scroll    overlapping notes on a MONO synth = slide", x + 12, y + h - 19, C.dim, U.fS)
  else
    U.text("click: step    shift-click: accent    alt-click: ghost    right-click: erase    row name or A..K keys: audition", x + 12, y + h - 19, C.dim, U.fS)
  end
  if clip.steps then drawDrumEditor(x + 10, y + 38, w - 20, h - 62, tr, clip, sel.track)
  else drawPianoRoll(x + 10, y + 38, w - 20, h - 62, tr, clip, sel.track) end
end

---------------------------------------------------------------- device panel
local DEV_ROW_O = {}

local function drawDevice()
  local x, y, w, h = L.W - L.devW, L.H - L.botH, L.devW, L.botH
  U.rect(x + 2, y + 2, w - 4, h - 4, C.panel, 4)
  local tr = track()
  local col = U.trackColors[tr.color]
  local isDrum = tr.type == "drum"
  local list = isDrum and P.kits or P.synths
  local idx = isDrum and tr.kit or tr.preset
  U.text(isDrum and "DRUM KIT" or "SYNTH", x + 12, y + 14, C.dim, U.fS)
  local px = x + 70
  local function setPreset(i)
    i = (i - 1) % #list + 1
    if isDrum then tr.kit = i else tr.preset = i; P.applySynth(tr.params, i) end
  end
  if U.button(px, y + 8, 22, 22, "<") then setPreset(idx - 1) end
  U.rect(px + 24, y + 8, 150, 22, C.dark)
  U.text(list[idx].name, px + 24, y + 13, col, U.fM, 150)
  if U.button(px + 176, y + 8, 22, 22, ">") then setPreset(idx + 1) end
  U.text(string.format("%d / %d", idx, #list), px + 206, y + 14, C.dim, U.fS)
  if U.button(x + w - 130, y + 8, 118, 22, "TYPE: " .. (isDrum and "DRUMS" or "SYNTH")) then
    if next(tr.clips) then say("Delete this track's clips before changing its type")
    else tr.type = isDrum and "synth" or "drum"; E.releaseTrack(sel.track) end
  end
  local gy, gh = y + 38, h - 46
  if isDrum then
    local cw, ch = floor((w - 20) / 8), floor((gh - 22) / 4)
    DEV_ROW_O.color = col
    for r = 1, 8 do
      local cx = x + 10 + (r - 1) * cw
      DEV_ROW_O.on = sel.row == r
      if U.button(cx + 2, gy, cw - 4, 18, P.rowNames[r], DEV_ROW_O) then
        sel.row = r; E.triggerDrum(sel.track, r, 0.9)
      end
      for i, s in ipairs(P.drumParams) do
        aknob("dr" .. r .. s.k, "t" .. sel.track .. ".r" .. r .. "." .. s.k, tr.name .. " / " .. P.rowNames[r] .. " " .. s.l,
          cx, gy + 22 + (i - 1) * ch, cw, ch, s, tr.rows[r], s.k, col)
      end
    end
  else
    local cw, ch = floor((w - 20) / 9), floor(gh / 3)
    for i, s in ipairs(P.synthParams) do
      local cx, cy = x + 10 + ((i - 1) % 9) * cw, gy + floor((i - 1) / 9) * ch
      local v = aknob("sy" .. s.k, "t" .. sel.track .. ".p." .. s.k, tr.name .. " / " .. s.l, cx, cy, cw, ch, s, tr.params, s.k, col)
      if v and s.k == "mono" then E.releaseTrack(sel.track) end
    end
  end
end

---------------------------------------------------------------- frame
function app.draw()
  layout()
  U.begin()
  U.rect(0, 0, L.W, L.H, C.bg, 0)
  drawTop()
  for ti = 1, E.NT do drawTrackColumn(ti, 4 + (ti - 1) * L.trackW) end
  drawSceneColumn(4 + E.NT * L.trackW)
  drawMasterColumn(4 + E.NT * L.trackW + L.sceneW)
  drawSongPanel()
  drawEditor()
  drawDevice()
  if U.released and pr then
    E.noteOff(pr.ti, pr.preview)
    if pr.mode == "len" then lastLen[pr.ti] = pr.note.len end
    pr = nil
  end
  if toastT > 0 then
    local tw = U.fM:getWidth(toast) + 32
    U.rect((L.W - tw) / 2, L.H - L.botH - 40, tw, 28, C.dark, 6, min(1, toastT) * 0.95)
    U.text(toast, (L.W - tw) / 2, L.H - L.botH - 33, C.text, U.fM, tw)
  end
  if exportState then
    U.rect(0, 0, L.W, L.H, C.dark, 0, 0.75)
    U.text("Rendering song to WAV...", 0, L.H / 2 - 12, C.accent, U.fL, L.W)
    if exportState == "pending" then exportState = "go" end
  end
  U.finish()
end

---------------------------------------------------------------- input
function app.mousepressed(x, y, b, presses)
  U.mx, U.my = x, y
  if b == 1 then U.pressed, U.down = true, true; if presses and presses >= 2 then U.dbl = true end
  elseif b == 2 then U.rpressed = true end
end
function app.mousereleased(x, y, b) U.mx, U.my = x, y; if b == 1 then U.released, U.down = true, false end end
function app.mousemoved(x, y) U.mx, U.my = x, y end
function app.wheelmoved(dy) U.wheel = U.wheel + dy end
function app.modifiers(shift, ctrl, alt) U.shift, U.ctrl, U.alt = shift, ctrl, alt end

-- One path for every live input (computer keys now, MIDI keys and pads
-- after). id identifies the physical key so its release finds the right voice and recorded note.
local function liveNote(id, ti, pitch, vel, down)
  if down then
    if held[id] then return false end
    local tr = E.song.tracks[ti]
    E.noteOn(ti, pitch, vel)
    held[id] = { ti = ti, pitch = pitch }
    if tr.type == "drum" and ti == sel.track then sel.row = pitch end
    if recArm then
      local clip, pos, h = heardPos(ti)
      if clip then
        local p = floor(pos + 0.5) % clip.len
        if clip.steps then clip.steps[pitch][p + 1] = vel
        else
          local n = { step = p, pitch = pitch, len = 1, vel = vel }
          clip.notes[#clip.notes + 1] = n
          held[id].note, held[id].h0, held[id].clip = n, h, clip
        end
        E.changed()
      end
    end
    return true
  else
    local k = held[id]
    if not k then return false end
    E.noteOff(k.ti, k.pitch)
    if k.note then
      local h = E.heardStep()
      if h then k.note.len = max(1, min(k.clip.len, floor(h - k.h0 + 0.5))); E.changed() end
    end
    held[id] = nil
    return true
  end
end

local function playKey(sc, down)
  if not down then return liveNote(sc, sel.track, 0, 0, false) end
  local pitch
  if track().type == "drum" then pitch = DRUM_KEYS[sc] else pitch = NOTE_KEYS[sc] and (NOTE_KEYS[sc] + (octave + 2) * 12) end
  if not pitch then return false end
  return liveNote(sc, sel.track, pitch, 0.85, true)
end

function app.keypressed(key)
  local tr = track()
  if U.ctrl then
    if key == "s" then app.save()
    elseif key == "o" then app.open()
    elseif key == "c" and selClip() then
      clipboard = { type = tr.type, clip = E.copy(selClip()) }; say("Clip copied")
    elseif key == "v" and clipboard then
      if clipboard.type == tr.type then tr.clips[sel.scene] = E.copy(clipboard.clip)
      else say("Clipboard holds a " .. clipboard.type .. " clip") end
    elseif key == "d" and selClip() and sel.scene < E.NS then
      tr.clips[sel.scene + 1] = E.copy(selClip()); sel.scene = sel.scene + 1
    end
    return
  end
  if key == "space" then if E.playing then E.stop() else E.play() end
  elseif key == "tab" then
    if E.perform then E.perform = false; if #E.song.chain == 0 and chainBackup then E.song.chain = chainBackup; chainBackup = nil end end
    E.setMode(E.mode == "session" and "song" or "session")
  elseif key == "return" then
    if selClip() then E.launchClip(sel.track, sel.scene) else createClip(sel.track, sel.scene) end
  elseif key == "delete" or key == "backspace" then
    if selClip() then
      if E.rt[sel.track].playing == sel.scene then E.stopClip(sel.track); E.releaseTrack(sel.track) end
      tr.clips[sel.scene] = nil
    end
  elseif key == "up" then sel.scene = max(1, sel.scene - 1)
  elseif key == "down" then sel.scene = min(E.NS, sel.scene + 1)
  elseif key == "left" then sel.track = max(1, sel.track - 1)
  elseif key == "right" then sel.track = min(E.NT, sel.track + 1)
  elseif key == "z" then octave = max(0, octave - 1)
  elseif key == "x" then octave = min(7, octave + 1)
  elseif key == "0" then for ti = 1, E.NT do E.stopClip(ti) end
  elseif tonumber(key) and tonumber(key) >= 1 and tonumber(key) <= E.NS then
    sel.scene = tonumber(key); E.launchScene(sel.scene)
    if not E.playing then E.play() end
  else playKey(key, true) end
end

function app.keyreleased(key) playKey(key, false) end

return app
