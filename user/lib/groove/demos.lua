-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Groove's two starter songs, techno and house, as PulseMusic ships them
-- (`roadmap.md` 6zh): eight tracks, eight scenes and a chain of sections,
-- written as patterns - `X` an accent, `x` a hit, `o` a ghost - and notes.
local E = use("/Kosmos/Libraries/groove/engine.lua")
local M = {}

local VEL = { x = 0.8, X = 1.0, o = 0.42 }
local function drum(name, len, rows)
  local c = E.newClip("drum", len, name)
  for r, pat in pairs(rows) do
    for s = 1, len do
      local ch = pat:sub((s - 1) % #pat + 1, (s - 1) % #pat + 1)
      c.steps[r][s] = VEL[ch] or 0
    end
  end
  return c
end

local function synth(name, len, notes)
  local c = E.newClip("synth", len, name)
  for _, n in ipairs(notes) do
    c.notes[#c.notes + 1] = { step = n[1], pitch = n[2], len = n[3] or 1, vel = n[4] or 0.8 }
  end
  return c
end

local function chordNotes(out, step, chord, len, vel)
  for _, p in ipairs(chord) do out[#out + 1] = { step, p, len, vel } end
end

local function place(song, ti, clip, scenes)
  for _, sc in ipairs(scenes) do song.tracks[ti].clips[sc] = E.copy(clip) end
end

local function mix(t, vol, sendA, sendB, duck, pan)
  t.vol, t.sendA, t.sendB, t.duck, t.pan = vol, sendA or 0, sendB or 0, duck or 0, pan or 0
end

---------------------------------------------------------------- TECHNO
function M.techno()
  local s = E.newSong()
  s.bpm, s.swing = 132, 0
  local T = s.tracks
  T[1] = E.newTrack("KICK", "drum", 1, 1); T[2] = E.newTrack("TOPS", "drum", 2, 1)
  T[3] = E.newTrack("BASS", "synth", 3, 1); T[4] = E.newTrack("ACID", "synth", 4, 3)
  T[5] = E.newTrack("STAB", "synth", 5, 8); T[6] = E.newTrack("LEAD", "synth", 6, 10)
  T[7] = E.newTrack("PAD", "synth", 7, 14); T[8] = E.newTrack("RISER", "synth", 8, 16)
  mix(T[1], 0.7); mix(T[2], 0.6, 0, 0.08)
  mix(T[3], 0.72, 0, 0, 0.75); mix(T[4], 0.58, 0.28, 0.05, 0.3, -0.15)
  mix(T[5], 0.6, 0.5, 0.3, 0.4, 0.2); mix(T[6], 0.64, 0.45, 0.25, 0.3, 0.1)
  mix(T[7], 0.55, 0, 0.55, 0.65); mix(T[8], 0.5, 0.2, 0.45)
  s.fx.dTime, s.fx.dFb, s.fx.rSize = 2, 0.5, 0.82
  s.scenes = { "INTRO", "KICK IN", "ACID", "BREAK", "PEAK", "PEAK 2", "OUTRO", "SCENE 8" }

  local kick = drum("4x4", 16, { [1] = "X...X...X...X..." })
  local kickClap = drum("4x4+Clap", 32, { [1] = "X...X...X...X...", [3] = "....x.......x..." })
  kickClap.steps[6][31], kickClap.steps[6][32] = 0.6, 0.8
  local topsLite = drum("Hats Lite", 16, { [4] = "o.o.o.o.o.o.o.oo", [5] = "..x...x...x...x." })
  local tops = drum("Tops", 16, { [4] = "xo.oxo.oxo.oxo.o", [5] = "..x...x...x...x.", [7] = "...x..x.....x...",
    [8] = ".....o.....x..o." })

  local bn = {}
  for bar = 0, 1 do
    for b = 0, 3 do
      local p = 33
      if bar == 1 and b == 3 then p = 36 end
      local o = bar * 16 + b * 4
      bn[#bn + 1] = { o + 1, p, 1, 0.65 }; bn[#bn + 1] = { o + 2, p, 1, 0.9 }; bn[#bn + 1] = { o + 3, p, 1, 0.75 }
    end
  end
  local bass = synth("Rumble", 32, bn)

  local acid = synth("Acid A", 16, { { 0, 45, 1, 1 }, { 2, 45, 1, 0.6 }, { 3, 57, 1, 0.7 }, { 5, 45, 1, 0.6 },
    { 6, 48, 2, 0.8 }, { 7, 50, 1, 0.8 }, { 8, 45, 1, 1 }, { 10, 52, 1, 0.7 }, { 11, 45, 1, 0.6 },
    { 13, 55, 2, 1 }, { 14, 57, 1, 0.9 }, { 15, 43, 1, 0.6 } })

  local sn = {}
  chordNotes(sn, 3, { 57, 60, 64 }, 1, 0.9); chordNotes(sn, 10, { 57, 60, 64 }, 1, 0.75)
  chordNotes(sn, 19, { 57, 60, 64 }, 1, 0.9); chordNotes(sn, 26, { 57, 60, 65 }, 1, 0.8)
  local stab = synth("Stab Am", 32, sn)

  local ln = {}
  local seq = { 69, 76, 72, 76, 69, 79, 76, 72 }
  for i, p in ipairs(seq) do ln[#ln + 1] = { (i - 1) * 2, p, 1, i % 2 == 1 and 0.9 or 0.65 } end
  local lead = synth("Arp", 16, ln)

  local pn = {}
  chordNotes(pn, 0, { 45, 52, 57, 60 }, 32, 0.7); chordNotes(pn, 32, { 41, 48, 53, 57 }, 32, 0.7)
  local pad = synth("Am - F", 64, pn)
  local riser = synth("Riser", 64, { { 0, 60, 64, 0.9 } })

  place(s, 1, kick, { 2, 3 }); place(s, 1, kickClap, { 5, 6, 7 })
  place(s, 2, topsLite, { 1, 2 }); place(s, 2, tops, { 3, 5, 6, 7 })
  place(s, 3, bass, { 2, 3, 5, 6 })
  place(s, 4, acid, { 3, 4, 5, 6 })
  place(s, 5, stab, { 4, 5, 6, 7 })
  place(s, 6, lead, { 5, 6 })
  place(s, 7, pad, { 1, 4, 6 })
  place(s, 8, riser, { 4 })
  s.chain = { { scene = 1, bars = 8 }, { scene = 2, bars = 8 }, { scene = 3, bars = 16 }, { scene = 4, bars = 8 },
    { scene = 5, bars = 16 }, { scene = 6, bars = 16 }, { scene = 7, bars = 8 } }
  return s
end

---------------------------------------------------------------- HOUSE
function M.house()
  local s = E.newSong()
  s.bpm, s.swing = 124, 0.16
  local T = s.tracks
  T[1] = E.newTrack("KICK", "drum", 1, 3); T[2] = E.newTrack("TOPS", "drum", 2, 3)
  T[3] = E.newTrack("BASS", "synth", 3, 6); T[4] = E.newTrack("KEYS", "synth", 4, 9)
  T[5] = E.newTrack("PAD", "synth", 5, 13); T[6] = E.newTrack("LEAD", "synth", 6, 10)
  T[7] = E.newTrack("ARP", "synth", 7, 12); T[8] = E.newTrack("RISER", "synth", 8, 16)
  mix(T[1], 0.7); mix(T[2], 0.6, 0, 0.1)
  mix(T[3], 0.7, 0, 0, 0.7); mix(T[4], 0.62, 0.22, 0.3, 0.35, -0.1)
  mix(T[5], 0.5, 0, 0.5, 0.6); mix(T[6], 0.66, 0.4, 0.3, 0.2, 0.15)
  mix(T[7], 0.55, 0.45, 0.3, 0.3, -0.3); mix(T[8], 0.45, 0.2, 0.45)
  s.fx.dTime, s.fx.dFb, s.fx.rSize = 2, 0.42, 0.7
  s.scenes = { "INTRO", "GROOVE", "KEYS", "BREAK", "DROP", "FULL", "OUTRO", "SCENE 8" }

  local kick = drum("4 Floor", 16, { [1] = "X...X...X...X..." })
  local kickClap = drum("Kick+Clap", 16, { [1] = "X...X...X...X...", [3] = "....X.......X..." })
  local tops = drum("Shuffle", 16, { [4] = "x..ox..ox..ox..o", [5] = "..x...x...x...x.", [8] = "...o......o..o.." })
  local topsLite = drum("Offbeat", 16, { [5] = "..x...x...x...x." })

  local chords = { { 57, 60, 64, 67 }, { 57, 60, 64, 67 }, { 53, 57, 60, 64 }, { 55, 59, 62, 64 } }
  local roots = { 45, 45, 41, 43 }
  local kn, bn, pn, an = {}, {}, {}, {}
  for bar = 0, 3 do
    local o = bar * 16
    for _, st in ipairs({ 0, 3, 6, 10, 13 }) do chordNotes(kn, o + st, chords[bar + 1], 2, st == 0 and 0.95 or 0.75) end
    for _, st in ipairs({ 2, 6, 10, 14 }) do bn[#bn + 1] = { o + st, roots[bar + 1], 2, 0.9 } end
    bn[#bn + 1] = { o + 13, roots[bar + 1] + 12, 1, 0.6 }
    chordNotes(pn, o, chords[bar + 1], 16, 0.7)
    for st = 0, 15 do
      local c = chords[bar + 1]
      an[#an + 1] = { o + st, c[(st * 3) % 4 + 1] + 12, 1, st % 4 == 0 and 0.85 or 0.55 }
    end
  end
  local keys, bass, pad, arp = synth("Am7 F G", 64, kn), synth("Organ Bass", 64, bn), synth("Pad", 64, pn), synth("Arp", 64, an)
  local lead = synth("Melody", 64, { { 0, 76, 2 }, { 3, 74, 1 }, { 6, 72, 2 }, { 10, 69, 3 }, { 18, 72, 1 }, { 20, 74, 2 },
    { 24, 76, 4 }, { 32, 77, 2 }, { 35, 76, 1 }, { 38, 72, 2 }, { 42, 69, 3 }, { 50, 71, 1 }, { 52, 74, 2 }, { 56, 79, 2 }, { 60, 76, 4 } })
  local riser = synth("Riser", 64, { { 0, 60, 64, 0.9 } })

  place(s, 1, kick, { 1, 2 }); place(s, 1, kickClap, { 3, 5, 6, 7 })
  place(s, 2, topsLite, { 1 }); place(s, 2, tops, { 2, 3, 5, 6, 7 })
  place(s, 3, bass, { 2, 3, 5, 6, 7 })
  place(s, 4, keys, { 3, 4, 5, 6 })
  place(s, 5, pad, { 4, 6 })
  place(s, 6, lead, { 5, 6 })
  place(s, 7, arp, { 4, 6 })
  place(s, 8, riser, { 4 })
  s.chain = { { scene = 1, bars = 8 }, { scene = 2, bars = 8 }, { scene = 3, bars = 16 }, { scene = 4, bars = 8 },
    { scene = 5, bars = 16 }, { scene = 6, bars = 16 }, { scene = 7, bars = 8 } }
  return s
end

return M
