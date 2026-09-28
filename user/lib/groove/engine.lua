-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Groove's engine, as the window sees it (`roadmap.md` 6zh): PulseMusic's
-- `engine.lua` with the sound taken out of it.
--
-- **The song lives here, and the sound lives in the Synth Kit.** PulseMusic
-- rendered in the same Lua that drew its window; Diego chose "All in C, on
-- its own thread", so `/Kosmos/Kits/synth` plays and this file keeps what a
-- person edits - the song as PulseMusic's tables, which the kit reads whole -
-- and a mirror of what is being heard, read back from the kit once a frame.
--
--   E.song                 the tables, edited in place by the window
--   E.changed()            something in them changed: the kit is handed the
--                          song again at the end of the frame (`E.sync`)
--   E.update()             once a frame: what is heard, into `E.rt`,
--                          `E.playing`, `E.heard`, and the knobs a lane moves
--
-- **Why the whole song rather than the one value that changed.** The kit
-- reads a song into a struct on this thread and hands the audio thread a
-- pointer, so the thread never allocates and never sees half an edit. One
-- knob or one step is the same hand-over as a new song, and there is then
-- exactly one way a change reaches the sound.
--
-- The rest is PulseMusic's, unchanged in meaning: the song model, the
-- automation lanes and their resting values, saving and loading.

local P = use("/Kosmos/Libraries/groove/presets.lua")
local synth = use("/Kosmos/Kits/synth")

local E = {}
local floor, min, max = math.floor, math.min, math.max

E.NT, E.NS, E.SR = 8, 8, 44100

---------------------------------------------------------------- song model
function E.newTrack(name, kind, color, preset)
  local t = { name = name, type = kind, color = color, vol = 0.7, pan = 0, sendA = 0, sendB = 0, duck = 0,
    mute = false, solo = false, preset = preset or 1, kit = 1, params = {}, rows = P.newRows(), clips = {} }
  P.applySynth(t.params, kind == "synth" and t.preset or #P.synths)
  if kind == "drum" then t.kit = preset or 1 end
  return t
end

function E.newClip(kind, len, name)
  local c = { name = name or "Clip", len = len or 16 }
  if kind == "drum" then
    c.steps = {}
    for r = 1, 8 do c.steps[r] = {}; for s = 1, 64 do c.steps[r][s] = 0 end end
  else
    c.notes = {}
  end
  return c
end

function E.newSong()
  local s = { bpm = 128, swing = 0, master = 0.8, tracks = {}, scenes = {}, chain = {}, fx = {} }
  P.fillDefaults(s.fx, P.fxParams)
  local names = { "DRUMS", "TOPS", "BASS", "ACID", "STAB", "LEAD", "PAD", "FX" }
  local kinds = { "drum", "drum", "synth", "synth", "synth", "synth", "synth", "synth" }
  local pres = { 1, 1, 1, 3, 8, 10, 13, 16 }
  for i = 1, E.NT do s.tracks[i] = E.newTrack(names[i], kinds[i], i, pres[i]) end
  for i = 1, E.NS do s.scenes[i] = "SCENE " .. i end
  s.chain[1] = { scene = 1, bars = 8 }
  return s
end

function E.copy(v)
  if type(v) ~= "table" then return v end
  local c = {}
  for k, x in pairs(v) do c[k] = E.copy(x) end
  return c
end

function E.songSeconds()
  local bars = 0
  for _, e in ipairs(E.song.chain) do bars = bars + e.bars end
  return bars * 4 * 60 / E.song.bpm, bars
end

---------------------------------------------------------------- the kit
--
-- **A transport command is believed before it is heard.** `synth.play()`
-- answers its number, and the state the kit publishes says how many
-- commands it includes; until it includes this one, the window keeps what
-- it asked for rather than showing a state from before the press - which
-- would light the play button off for a frame, and read "the song ended"
-- for a song that has not started yet.
--
local state = {}
local sent = 0
local dirty = false
local lastChain, lastBar = nil, nil

-- The runtime mirror, in PulseMusic's names. `playing` and `queued` are a
-- scene or nil, as `E.rt` had them.
E.rt = {}
for i = 1, E.NT do
  E.rt[i] = { playing = nil, queued = nil, clipStart = 0, peak = 0, hits = {}, bend = 0 }
end

E.mode, E.playing, E.finished = "session", false, false
E.masterPeakL, E.masterPeakR = 0, 0
E.heard = { step = nil, chain = 0, sectionStep = 0 }
E.autoHold, E.recTouched, E.perform = {}, nil, false

local function track_numbers(n) sent = max(sent, n or 0) end

-- A command posted: `n` is its number.
local function posted(n) track_numbers(n); return n end

function E.changed() dirty = true end

-- What the kit said last, as it said it: `busy` and `rendered` for the load.
function E.kitState() return state end

-- The names a hand is on, handed with the song so the kit's copy is held
-- too: a knob being turned, and in a recording pass every one touched.
local holds = {}

local function held_names()
  for k in pairs(holds) do holds[k] = nil end
  for k, v in pairs(E.autoHold) do if v then holds[k] = true end end
  if E.recTouched then for k in pairs(E.recTouched) do holds[k] = true end end
  return holds
end

-- Once a frame, after the frame's edits: the song to the kit if it changed.
function E.sync()
  if not dirty or not E.song then return end
  dirty = false
  synth.song(E.song, held_names())
end

function E.setSong(song)
  E.stop()
  E.song = song
  P.fillDefaults(song.fx, P.fxParams)
  for i = 1, E.NT do
    local t = song.tracks[i]
    P.fillDefaults(t.params, P.synthParams)
    t.rows = t.rows or P.newRows()
    local rt = E.rt[i]
    rt.playing, rt.queued, rt.clipStart, rt.peak, rt.bend = nil, nil, 0, 0, 0
  end
  E.autoHold, E.recTouched = {}, nil
  dirty = false
  synth.song(song)
end

function E.setMode(mode)
  E.stop()
  E.mode = mode
  posted(synth.mode(mode == "song"))
end

function E.play()
  if E.playing then return end
  E.sync()
  if E.mode == "song" then E.autoRestore() end
  E.playing, E.finished = true, false
  lastChain, lastBar = nil, nil
  posted(synth.play())
end

function E.stop()
  if E.playing and E.mode == "song" then E.autoRestore() end
  E.playing = false
  E.autoHold = {}
  for ti = 1, E.NT do E.rt[ti].queued = nil end
  posted(synth.stop())
end

function E.launchClip(ti, sc)
  E.rt[ti].queued = sc
  if not E.playing then E.playing, E.finished = true, false end
  posted(synth.launch_clip(ti, sc))
end

function E.launchScene(sc)
  for ti = 1, E.NT do
    E.rt[ti].queued = E.song.tracks[ti].clips[sc] and sc or 0
  end
  E.sync()
  posted(synth.launch_scene(sc))
end

function E.stopClip(ti)
  E.rt[ti].queued = 0
  posted(synth.stop_clip(ti))
end

-- `key` is the counter when its key went down, so the kit can say how long
-- the note took to be heard (4i); nil when nobody knows.
function E.noteOn(ti, pitch, vel, key) E.sync(); posted(synth.note_on(ti, pitch, vel or 0.8, key)) end
function E.noteOff(ti, pitch) posted(synth.note_off(ti, pitch)) end
function E.triggerDrum(ti, row, vel) E.sync(); posted(synth.note_on(ti, row, vel or 0.8)) end
function E.releaseTrack(ti) posted(synth.release(ti)) end
function E.bend(ti, semis) E.rt[ti].bend = semis; posted(synth.bend(ti, semis)) end

-- The step being heard, with its fraction; the kit has already taken the
-- sound's way to the speaker off it, which PulseMusic did with `latency`.
function E.heardStep() return E.heard.step end

-- The section being heard and the step inside it, from 0.
function E.heardSong()
  if not E.heard.step then return nil end
  return E.heard.chain, E.heard.sectionStep
end

---------------------------------------------------------------- automation
-- A lane is one normalized value (0..1) per 16th step, stored per song section:
--   song.chain[i].auto[target][step] = n        (step 1..bars*16, missing = hold)
-- Targets: "t3.p.cut" synth param, "t1.r2.decay" drum row param, "t3.m.sendA" mixer, "fx.dFb" master fx.
-- song.autoBase[target] is the value the control has wherever no lane drives it.
--
-- The kit plays the lanes. What is here moves the knobs with them, so the
-- window shows what is heard, and keeps the bookkeeping PulseMusic kept:
-- resting values, a hand that wins until the next section, clearing.
local resolved = {}

local function specIn(list, key)
  for _, sp in ipairs(list) do if sp.k == key then return sp end end
end

function E.resolve(target)
  local r = resolved[target]
  if r == nil then
    local ti, a, b = target:match("^t(%d+)%.p%.(%w+)$")
    if ti then r = { "p", tonumber(ti), a }
    else
      ti, a, b = target:match("^t(%d+)%.r(%d+)%.(%w+)$")
      if ti then r = { "r", tonumber(ti), tonumber(a), b }
      else
        ti, a = target:match("^t(%d+)%.m%.(%w+)$")
        if ti then r = { "m", tonumber(ti), a }
        else
          a = target:match("^fx%.(%w+)$")
          r = a and { "fx", a } or false
        end
      end
    end
    resolved[target] = r
  end
  if not r then return nil end
  local song, kind = E.song, r[1]
  if kind == "fx" then return song.fx, r[2], specIn(P.fxParams, r[2]) end
  local tr = song.tracks[r[2]]
  if not tr then return nil end
  if kind == "p" then
    if tr.type == "drum" then return nil end
    return tr.params, r[3], specIn(P.synthParams, r[3])
  elseif kind == "r" then
    if tr.type ~= "drum" or not tr.rows[r[3]] then return nil end
    return tr.rows[r[3]], r[4], specIn(P.drumParams, r[4])
  end
  return tr, r[3], r[3] == "vol" and P.volParam or P.mixParams[r[3]]
end

function E.autoGet(target)
  local c, k, spec = E.resolve(target)
  if c and spec then return P.toNorm(spec, c[k]) end
end

local function autoSet(target, n)
  local c, k, spec = E.resolve(target)
  if c and spec then c[k] = P.fromNorm(spec, n) end
end

function E.isAutomated(target)
  return E.song.autoBase ~= nil and E.song.autoBase[target] ~= nil
end

function E.autoEnsureBase(target)
  local song = E.song
  song.autoBase = song.autoBase or {}
  if song.autoBase[target] == nil then song.autoBase[target] = E.autoGet(target) end
end

function E.autoWrite(entry, target, step, n)
  if step < 1 or step > entry.bars * 16 then return end
  E.autoEnsureBase(target)
  entry.auto = entry.auto or {}
  entry.auto[target] = entry.auto[target] or {}
  entry.auto[target][step] = n
  dirty = true
end

function E.autoClearLane(entry, target)
  if entry.auto then entry.auto[target] = nil end
  dirty = true
  for _, e in ipairs(E.song.chain) do
    if e.auto and e.auto[target] and next(e.auto[target]) then return end
  end
  -- last lane gone: the control is a plain knob again, left at its base value
  if E.song.autoBase and E.song.autoBase[target] ~= nil then
    autoSet(target, E.song.autoBase[target]); E.song.autoBase[target] = nil
  end
end

function E.autoClearTarget(target)
  for _, e in ipairs(E.song.chain) do if e.auto then e.auto[target] = nil end end
  if E.song.autoBase and E.song.autoBase[target] ~= nil then
    autoSet(target, E.song.autoBase[target]); E.song.autoBase[target] = nil
  end
  E.autoHold[target] = nil
  dirty = true
end

-- every automated control back to its base value (song start, song stop)
function E.autoRestore()
  E.autoHold = {}
  if not E.song or not E.song.autoBase then return end
  for target, n in pairs(E.song.autoBase) do autoSet(target, n) end
end

-- A hand on an automated knob, in a section whose lane drives it: the hand
-- wins until the next section, here and in the kit.
function E.hold(target)
  E.autoHold[target] = true
  posted(synth.hold(target, true))
  dirty = true
end

-- A new section heard: holds end, and what it does not drive returns to base.
local function sectionStart(entry)
  E.autoHold = {}
  local base = E.song.autoBase
  if not base then return end
  for target, n in pairs(base) do
    local lane = entry.auto and entry.auto[target]
    local beingRecorded = E.recTouched and E.recTouched[target]
    if not (lane and next(lane)) and not beingRecorded then autoSet(target, n) end
  end
end

-- The knobs a lane drives, where the lane is heard: the step's value, and
-- a glide toward the next one by the step's fraction, as the kit ramps.
local function followLanes(entry, ss, frac)
  if not entry.auto then return end
  for target, lane in pairs(entry.auto) do
    if not E.autoHold[target] and not (E.recTouched and E.recTouched[target]) then
      local v = lane[ss + 1]
      if v then
        local nxt = lane[ss + 2]
        local _, _, spec = E.resolve(target)
        if nxt and nxt ~= v and spec and not (spec.choices or spec.int) then
          v = v + (nxt - v) * frac
        end
        autoSet(target, v)
      end
    end
  end
end

---------------------------------------------------------------- once a frame
--
-- What the kit says is heard, into PulseMusic's names; the lanes' knobs
-- moved; and a performance being recorded written a bar at a time.
--
function E.update()
  synth.state(state)
  local current = (state.applied or 0) >= sent

  if current then
    if E.playing and not state.playing and E.mode == "song" and state.finished then
      E.finished = true
      E.autoRestore()
    end
    E.playing = state.playing
  end

  E.masterPeakL, E.masterPeakR = state.peak_l or 0, state.peak_r or 0

  for ti = 1, E.NT do
    local rt, st = E.rt[ti], state.tracks[ti]
    if current then
      rt.playing = (st.playing or 0) > 0 and st.playing or nil
      rt.queued = (st.queued or -1) >= 0 and st.queued or nil
    end
    rt.clipStart, rt.peak, rt.hits = st.start or 0, st.peak or 0, st.hits
  end

  local h = E.heard
  h.step = E.playing and state.step or nil
  h.chain, h.sectionStep = state.chain or 0, state.section_step or 0

  if not h.step then lastChain, lastBar = nil, nil; return end

  if E.mode == "song" then
    local entry = E.song.chain[h.chain]
    if entry then
      if h.chain ~= lastChain then lastChain = h.chain; sectionStart(entry) end
      followLanes(entry, h.sectionStep, h.step - floor(h.step))
    end
  elseif E.perform then
    -- One chain entry per run of bars spent on the same scene, as
    -- PulseMusic's `tick` wrote it - here from the bars as they are heard.
    local bar = floor(h.step / 16)
    if bar ~= lastBar then
      lastBar = bar
      local sc = state.scene or 0
      if sc > 0 then
        local chain = E.song.chain
        local last = chain[#chain]
        if last and last.scene == sc and last.bars < 64 then last.bars = last.bars + 1
        else chain[#chain + 1] = { scene = sc, bars = 1 } end
        dirty = true
      end
    end
  end
end

---------------------------------------------------------------- export
--
-- The song as a WAV, rendered by the kit on an engine of its own into a
-- region of this process's, and written whole with `write_from` - which
-- takes a file of any size, where a Lua string of a four-minute song would
-- be forty megabytes through the interpreter. Answers the seconds.
--
local PAGE = 4096

function E.export(path)
  local secs = E.songSeconds()
  local bytes = 44 + floor((secs + 5) * E.SR) * 4 + 4 * 1024 * 2
  local cap, why = sys.memory((bytes + PAGE - 1) // PAGE)
  if not cap then error("no memory to render into: " .. tostring(why)) end
  local at = sys.memory_map(cap)
  if not at then sys.release(cap); error("a region that could not be mapped") end

  local ok, len, seconds = pcall(synth.export, E.song, at, bytes)
  if not ok then sys.release(cap); error(len) end

  local wrote, werr = fs.write_from(path, cap, len)
  sys.release(cap)
  if wrote ~= len then error("the file could not be written: " .. tostring(werr)) end
  return seconds
end

---------------------------------------------------------------- persistence
local function ser(v)
  local t = type(v)
  if t == "table" then
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b)
      if type(a) == type(b) then return a < b end
      return type(a) == "number"
    end)
    local out = {}
    for _, k in ipairs(keys) do
      local ks = type(k) == "number" and ("[" .. k .. "]") or ("[" .. string.format("%q", k) .. "]")
      out[#out + 1] = ks .. "=" .. ser(v[k])
    end
    return "{" .. table.concat(out, ",") .. "}"
  elseif t == "string" then return string.format("%q", v)
  else return tostring(v) end
end

E.serialize = ser

function E.save(path)
  return fs.write(path, "return " .. ser(E.song))
end

-- A project PulseMusic saved reads here too: the same `return { ... }`,
-- run with nothing in its world, as text and never as bytecode.
function E.parse(src, name)
  local chunk, err = load(src, name or "project", "t", {})
  if not chunk then return nil, err end
  local ok, song = pcall(chunk)
  if not ok or type(song) ~= "table" or type(song.tracks) ~= "table" then
    return nil, "not a Groove project"
  end
  for i = 1, E.NT do
    if type(song.tracks[i]) ~= "table" then return nil, "a project with fewer than eight tracks" end
    song.tracks[i].clips = song.tracks[i].clips or {}
  end
  song.scenes = song.scenes or {}
  for i = 1, E.NS do song.scenes[i] = song.scenes[i] or ("SCENE " .. i) end
  song.chain, song.fx = song.chain or {}, song.fx or {}
  return song
end

function E.load(path)
  local src = fs.read(path)
  if type(src) ~= "string" then return false, "no saved project yet" end
  local song, err = E.parse(src, path)
  if not song then return false, err end
  E.setSong(song)
  return true
end

return E
