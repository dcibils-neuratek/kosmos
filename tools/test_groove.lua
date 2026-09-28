-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Groove's Lua, on this machine (`roadmap.md` 6zh).
--
--   build/host/test_synth tools/test_groove.lua
--
-- Run by the Synth Kit's harness, which is the kit's engine with a Lua
-- interpreter round it, so the window's half of Groove can be held to the
-- sound's half without booting anything:
--
-- - **the sound bank is one table written twice** - `groove/presets.lua` is
--   what the knobs show, the kit's C is what the audio thread plays - and
--   every knob's range, curve, steps and default, and every drum of every
--   kit, is checked to be the same number in both;
-- - the two demo songs build, and play: sound, in song mode, as long as
--   their chains say;
-- - a project saved and read back is the same song, and a file that is not
--   one - or is bytecode - is refused;
-- - a lane's resting value is kept and given back when the lane goes.

local passed, failed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local harness = synth

-- `use`, as Kosmos resolves it, for the files Groove reaches: its own, and
-- the two Kosmos ones - the synth kit, here the harness, and `ui` for a
-- face, which the engine never draws with.
local stub = {
  ["/Kosmos/Kits/synth"] = setmetatable({
    song = function(t, held) harness.song(t, held) end,
    state = function(t) return t or {} end,
  }, { __index = function() return function() return 1 end end }),
  ["/Kosmos/Libraries/ui.lua"] = { sized = function(_, px) return px end },
  ["/Kosmos/Libraries/clock.lua"] = { now = function() return nil end },
}
local loaded = {}

function use(path)
  if stub[path] then return stub[path] end
  if loaded[path] == nil then
    local name = path:match("^/Kosmos/Libraries/groove/([%w_]+%.lua)$")
    assert(name, "test_groove: nothing here answers " .. path)
    loaded[path] = dofile("user/lib/groove/" .. name)
  end
  return loaded[path]
end

local P = use("/Kosmos/Libraries/groove/presets.lua")
local E = use("/Kosmos/Libraries/groove/engine.lua")
local Demos = use("/Kosmos/Libraries/groove/demos.lua")

---------------------------------------------------------------- the sound bank
-- A song with one drum track and one synth whose tables leave everything
-- out: what the C reads for each is its default.
local blank = E.newSong()

for t = 1, 8 do
  blank.tracks[t].params, blank.tracks[t].rows = {}, {}
  for r = 1, 8 do blank.tracks[t].rows[r] = {} end
end

blank.fx = {}
blank.tracks[1].vol, blank.tracks[1].pan, blank.tracks[1].sendA = nil, nil, nil
blank.tracks[1].sendB, blank.tracks[1].duck = nil, nil
harness.song(blank)

local function near(a, b) return a and b and math.abs(a - b) <= 1e-9 * math.max(1, math.abs(b)) end

local function same_spec(target, spec, default)
  local t = harness.target(target)
  if not t then
    check(false, target .. " is not a target the kit knows")
    return
  end
  check(near(t.min, spec.min) and near(t.max, spec.max),
        ("%s ranges %g..%g in the C and %g..%g in presets.lua"):format(target, t.min, t.max, spec.min, spec.max))
  check(t.exp == (spec.exp or false),
        target .. (t.exp and " is exponential in the C and not in presets.lua" or " is exponential in presets.lua and not in the C"))
  check(t.stepped == ((spec.choices or spec.int) and true or false),
        target .. " is stepped in one and not the other")
  if default ~= false then
    local v = harness.value(target)
    check(near(v, spec.def), ("%s defaults to %s in the C and %g in presets.lua"):format(target, tostring(v), spec.def))
  end
end

for _, s in ipairs(P.synthParams) do same_spec("t3.p." .. s.k, s) end
for _, s in ipairs(P.drumParams) do same_spec("t1.r2." .. s.k, s) end
for _, s in pairs(P.mixParams) do same_spec("t1.m." .. s.k, s) end
same_spec("t1.m.vol", P.volParam)
for _, s in ipairs(P.fxParams) do same_spec("fx." .. s.k, s) end

local drums = 0

for k, kit in ipairs(P.kits) do
  for r, row in ipairs(kit.rows) do
    local c = harness.kit(k, r)
    check(c.type == row.type, ("kit %d row %d is a %s in the C and a %s in presets.lua"):format(k, r, c.type, row.type))
    for field, v in pairs(row) do
      if field ~= "type" then
        check(near(c[field], v), ("kit %d row %d: %s is %s in the C and %g in presets.lua")
                                    :format(k, r, field, tostring(c[field]), v))
      end
    end
    drums = drums + 1
  end
end

check(drums == 32, "presets.lua has " .. drums .. " drums, not four kits of eight")

---------------------------------------------------------------- the demos
local RATE = 44100

for _, name in ipairs({ "techno", "house" }) do
  local song = Demos[name]()
  E.song = song
  local secs, bars = E.songSeconds()
  local expect = 0
  for _, e in ipairs(song.chain) do expect = expect + e.bars end
  check(bars == expect and bars > 0, name .. " counts " .. bars .. " bars")
  check(math.abs(secs - bars * 4 * 60 / song.bpm) < 1e-9, name .. "'s length is not its bars at its tempo")

  harness.reset()
  harness.song(song)
  harness.mode(true)
  harness.play()
  harness.render(RATE * 20)
  local peak = harness.peak(0, RATE * 20)
  check(peak > 0.1 and peak <= 1, ("the %s demo peaks at %.3f in its first twenty seconds"):format(name, peak))
  check(harness.state().playing, "the " .. name .. " demo stopped inside its first twenty seconds")
  harness.mode(false)
end

---------------------------------------------------------------- projects
local song = Demos.house()
song.chain[2].auto = { ["t3.p.cut"] = { [1] = 0.25, [9] = 0.75 } }
song.autoBase = { ["t3.p.cut"] = 0.5 }

local text = E.serialize(song)
local back, why = E.parse("return " .. text, "house")

check(back ~= nil, "a saved project did not read back: " .. tostring(why))
check(back and E.serialize(back) == text, "a saved project read back as a different song")

check(E.parse("return 1") == nil, "a file returning a number was taken for a project")
check(E.parse("return { tracks = { {} } }") == nil, "a project with one track was taken")
check(E.parse(string.dump(function() return {} end)) == nil, "bytecode was taken for a project")
check(E.parse("return { tracks = os }") == nil, "a project reached outside its own world")

---------------------------------------------------------------- automation
E.setSong(Demos.techno())
local entry = E.song.chain[3]
local cut = E.song.tracks[3].params.cut

E.autoWrite(entry, "t3.p.cut", 1, 1)
check(E.isAutomated("t3.p.cut"), "a lane written did not make its knob automated")
check(near(E.song.autoBase["t3.p.cut"], P.toNorm(P.synthParams[11], cut)),
      "the knob's resting value was not where the knob was")
E.song.tracks[3].params.cut = 17000
E.autoClearLane(entry, "t3.p.cut")
check(not E.isAutomated("t3.p.cut"), "the last lane gone left the knob automated")
check(math.abs(E.song.tracks[3].params.cut - cut) < 1e-6,
      ("the knob was not given back its resting value: %g, not %g"):format(E.song.tracks[3].params.cut, cut))

if failed > 0 then
  print(("FAIL: %d of %d checks on Groove's Lua"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on Groove's Lua (every knob and drum the same in presets.lua and the kit, "
       .. "both demos playing their length, a project saved and read back, three files refused, "
       .. "a lane's resting value)"):format(passed))
