-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Synth Kit's engine, heard on this machine (`roadmap.md` 6zh, Groove).
--
--   build/host/test_synth tools/test_synth.lua
--
-- Songs made here in Groove's shape, played by the kit's C, and held to what
-- a person would hear: a kick where its step is, swing moving the off-beat, a
-- sine at the pitch it was asked for, a scene launched on the next bar and
-- not before, a muted track silent, a song that ends, a lane that turns a
-- track up, a closed hat that stops the open one - and the same song the same
-- twice. Whether the C is PulseMusic's engine was checked once against the
-- engine itself, block by block (`testing.md` 18.254); this is what stays.

local passed, failed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local RATE = 44100

-- A song of eight silent tracks, one scene each empty; `edit` fills it in.
local function song(edit)
  local s = { bpm = 120, swing = 0, master = 0.8, fx = { dRet = 0, rRet = 0 },
              tracks = {}, chain = { { scene = 1, bars = 8 } } }

  for t = 1, 8 do
    s.tracks[t] = { type = "synth", vol = 0.7, params = {}, clips = {} }
  end

  edit(s)
  return s
end

-- A drum clip with velocity 1 on the steps listed for each row.
local function drums(len, rows)
  local c = { len = len, steps = {} }

  for r = 1, 8 do
    c.steps[r] = {}
    for st = 1, 64 do c.steps[r][st] = 0 end
    for _, st in ipairs(rows[r] or {}) do c.steps[r][st] = 1 end
  end

  return c
end

local STEP = RATE * 60 / 120 / 4          -- 5512.5 frames at 120 a minute

-- Silence, when nothing is in the song.
synth.song(song(function() end))
synth.play()
synth.render(RATE)
check(synth.peak(0, RATE) == 0, "an empty song made a sound")

-- A rim shot on steps 1 and 5: at the start, and a beat later. The rim,
-- because it is gone in a hundredth of a second, so the second is heard on
-- silence rather than on the first one's tail.
synth.song(song(function(s)
  s.tracks[1] = { type = "drum", vol = 0.7, kit = 1,
                  clips = { drums(16, { [7] = { 1, 5 } }) } }
end))
synth.play()
synth.render(RATE)

local first = synth.onset(0, 0.01)
local second = synth.onset(math.floor(STEP * 3), 0.01)

check(first and first < 64, "the first rim did not start the song: " .. tostring(first))
check(second and math.abs(second - STEP * 4) < 64,
      ("the second rim is at %s, not %.1f, a beat later"):format(tostring(second), STEP * 4))

-- Swing: the second step starts late by the swing's share of the first.
synth.song(song(function(s)
  s.swing = 0.5
  s.tracks[1] = { type = "drum", vol = 0.7, kit = 1, clips = { drums(16, { [7] = { 2 } }) } }
end))
synth.play()
synth.render(RATE)

local swung = synth.onset(0, 0.01)

check(swung and math.abs(swung - STEP * 1.5) < 64,
      ("the swung step is at %s, not %.1f"):format(tostring(swung), STEP * 1.5))

-- A sine at A 440, held for a second: 440 rising crossings, give or take.
synth.song(song(function(s)
  s.tracks[3].params = { w1 = 4, mix = 0, cut = 18000, res = 0, env = 0,
                         aA = 0.001, aS = 1, lvl = 1 }
end))
synth.note_on(3, 69, 1)
synth.render(RATE * 2)

local crossings = synth.crossings(RATE, RATE * 2)

check(math.abs(crossings - 440) <= 3, "A 440 crossed zero " .. crossings .. " times in a second")

synth.note_off(3, 69)
synth.render(RATE)
check(synth.peak(RATE * 3 - 1000, RATE * 3) < 0.001, "the note rang on after its release")

-- A scene launched mid-bar starts on the next bar, not at once: scene 1 has a
-- rim on step 9 only, scene 2 on step 1 only.
synth.song(song(function(s)
  s.tracks[1] = { type = "drum", vol = 0.7, kit = 1,
                  clips = { drums(16, { [7] = { 9 } }), drums(16, { [7] = { 1 } }) } }
end))
synth.play()
synth.render(1000)
synth.launch_scene(2)
synth.render(RATE * 3)

local nine = synth.onset(1000, 0.01)
local bar = synth.onset(math.floor(STEP * 12), 0.01)

check(nine and math.abs(nine - STEP * 8) < 64,
      "scene 1 did not finish its bar: its rim is at " .. tostring(nine))
check(bar and math.abs(bar - STEP * 16) < 64,
      "scene 2 did not start on the next bar: its rim is at " .. tostring(bar))

-- A muted track is silent; the song still plays.
synth.song(song(function(s)
  s.tracks[1] = { type = "drum", vol = 0.7, kit = 1, mute = true,
                  clips = { drums(16, { { 1, 5, 9, 13 } }) } }
end))
synth.play()
synth.render(RATE)
check(synth.peak(0, RATE) == 0 and synth.state().playing, "a muted track was heard")

-- A song of one bar ends after it, and says so.
synth.song(song(function(s)
  s.chain = { { scene = 1, bars = 1 } }
  s.tracks[1] = { type = "drum", vol = 0.7, kit = 1, clips = { drums(16, { { 1 } }) } }
end))
synth.mode(true)
synth.play()
synth.render(math.floor(STEP * 20))

local st = synth.state()

check(st.finished and not st.playing, "a one-bar song did not finish after its bar")
synth.mode(false)

-- A lane that brings a track up from nothing: the first beat is quieter than
-- the last.
synth.song(song(function(s)
  s.chain = { { scene = 1, bars = 1, auto = { ["t1.m.vol"] = {} } } }
  for step = 1, 16 do s.chain[1].auto["t1.m.vol"][step] = (step - 1) / 15 end
  s.autoBase = { ["t1.m.vol"] = 0.7 }
  s.tracks[1] = { type = "drum", vol = 0.7, kit = 1,
                  clips = { drums(16, { { 1, 5, 9, 13 } }) } }
end))
synth.mode(true)
synth.play()
synth.render(math.floor(STEP * 16))

local early = synth.peak(0, math.floor(STEP * 2))
local late = synth.peak(math.floor(STEP * 12), math.floor(STEP * 14))

check(late > early * 3, ("the lane did not bring the track up: %.4f then %.4f"):format(early, late))
synth.mode(false)

-- The closed hat stops the open one: the open hat alone rings longer.
local function hats(rows)
  synth.song(song(function(s)
    s.tracks[1] = { type = "drum", vol = 0.7, kit = 1, clips = { drums(16, rows) } }
  end))
  synth.play()
  synth.render(math.floor(STEP * 4))
  return synth.peak(math.floor(STEP * 2), math.floor(STEP * 3))
end

local open = hats({ [5] = { 1 } })
local choked = hats({ [4] = { 2 }, [5] = { 1 } })

check(open > 0.001 and choked < open / 4,
      ("the closed hat did not stop the open one: %.4f then %.4f"):format(open, choked))

-- Nonsense is brought into range rather than handed to the audio thread.
synth.song(song(function(s)
  s.bpm = 0
  s.fx.duckRel = -1
  s.tracks[1] = { type = "drum", vol = 5, kit = 99, clips = { drums(16, { { 1 } }) } }
end))
synth.play()
synth.render(RATE)
check(synth.peak(0, RATE) > 0 and synth.peak(0, RATE) <= 1,
      "a song with no tempo and a kit that is not one did not play within range")

-- The same song twice is the same sound, each from a fresh engine - one
-- that has played has its delay's and reverb's memories and its noise
-- further on, as it should.
local function demo()
  synth.reset()
  synth.song(song(function(s)
    s.tracks[1] = { type = "drum", vol = 0.7, kit = 2,
                    clips = { drums(16, { { 1, 9 }, { 5, 13 }, nil, { 3, 7, 11, 15 } }) } }
    s.tracks[3].params = { w1 = 1, cut = 400, res = 0.6, noise = 0.1 }
    s.tracks[3].clips = { { len = 16, notes = { { step = 0, pitch = 36, len = 2, vel = 1 },
                                                 { step = 8, pitch = 43, len = 2, vel = 1 } } } }
  end))
  synth.play()
  synth.render(RATE * 2)
  return synth.sum()
end

check(demo() == demo(), "the same song twice was not the same sound")

if failed > 0 then
  print(("FAIL: %d of %d checks on the Synth Kit's engine"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the Synth Kit's engine (silence, a rim shot on its step, "
       .. "swing, A 440, a release, a scene on the next bar, mute, a song that "
       .. "ends, a lane, a choked hat, nonsense in range, the same twice)"):format(passed))
