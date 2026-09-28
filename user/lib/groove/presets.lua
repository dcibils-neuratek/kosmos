-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Groove's sound bank (`roadmap.md` 6zh): what every knob is - its range,
-- its default, how it is shown - and the synthesiser presets and drum kits,
-- as PulseMusic has them.
--
-- **The kit keeps its own copy of these numbers**, because the audio thread
-- plays them: `synth_dsp.c` holds the drum kits and `synth_lua.c` each
-- parameter's range and default. This file is what the window shows and
-- what a new track is filled from. The two are the same table written
-- twice, and `tools/test_groove.lua` holds them to each other.
local P = {}

local WAVES = { "SAW", "SQR", "TRI", "SIN" }
P.synthParams = {
  { k = "w1", l = "OSC 1", min = 1, max = 4, def = 1, choices = WAVES },
  { k = "w2", l = "OSC 2", min = 1, max = 4, def = 1, choices = WAVES },
  { k = "oct2", l = "OCT 2", min = -2, max = 2, def = 0, int = true },
  { k = "det", l = "DETUNE", min = 0, max = 50, def = 8, fmt = "ct" },
  { k = "mix", l = "OSC MIX", min = 0, max = 1, def = 0.5 },
  { k = "uni", l = "UNISON", min = 1, max = 3, def = 1, choices = { "1", "3", "5" } },
  { k = "sub", l = "SUB", min = 0, max = 1, def = 0 },
  { k = "noise", l = "NOISE", min = 0, max = 1, def = 0 },
  { k = "mono", l = "VOICE", min = 1, max = 2, def = 1, choices = { "POLY", "MONO" } },

  { k = "ftype", l = "FILTER", min = 1, max = 3, def = 1, choices = { "LP", "BP", "HP" } },
  { k = "cut", l = "CUTOFF", min = 30, max = 18000, def = 2000, exp = true, fmt = "hz" },
  { k = "res", l = "RESO", min = 0, max = 1, def = 0.2 },
  { k = "env", l = "ENV AMT", min = 0, max = 1, def = 0.3 },
  { k = "fA", l = "F.ATK", min = 0.001, max = 8, def = 0.001, exp = true, fmt = "s" },
  { k = "fD", l = "F.DEC", min = 0.01, max = 4, def = 0.2, exp = true, fmt = "s" },
  { k = "fS", l = "F.SUS", min = 0, max = 1, def = 0.2 },
  { k = "drive", l = "DRIVE", min = 0, max = 1, def = 0 },
  { k = "glide", l = "GLIDE", min = 0, max = 0.5, def = 0, fmt = "s" },

  { k = "aA", l = "ATTACK", min = 0.001, max = 4, def = 0.002, exp = true, fmt = "s" },
  { k = "aD", l = "DECAY", min = 0.01, max = 4, def = 0.3, exp = true, fmt = "s" },
  { k = "aS", l = "SUSTAIN", min = 0, max = 1, def = 0.7 },
  { k = "aR", l = "RELEASE", min = 0.005, max = 4, def = 0.15, exp = true, fmt = "s" },
  { k = "lfoR", l = "LFO HZ", min = 0.05, max = 20, def = 2, exp = true, fmt = "hzf" },
  { k = "lfoC", l = "LFO>CUT", min = 0, max = 1, def = 0 },
  { k = "lfoP", l = "LFO>PIT", min = 0, max = 1, def = 0 },
  { k = "lvl", l = "LEVEL", min = 0, max = 1.5, def = 0.35 },
}

P.drumParams = {
  { k = "tune", l = "TUNE", min = -12, max = 12, def = 0, fmt = "st" },
  { k = "decay", l = "DECAY", min = 0.2, max = 3, def = 1, exp = true, fmt = "x" },
  { k = "tone", l = "TONE", min = 0, max = 1, def = 0.5 },
  { k = "level", l = "LEVEL", min = 0, max = 1.5, def = 1 },
}

P.mixParams = {
  sendA = { k = "sendA", l = "DELAY", min = 0, max = 1, def = 0 },
  sendB = { k = "sendB", l = "REVERB", min = 0, max = 1, def = 0 },
  duck = { k = "duck", l = "DUCK", min = 0, max = 1, def = 0 },
  pan = { k = "pan", l = "PAN", min = -1, max = 1, def = 0, fmt = "pan" },
}

P.fxParams = {
  { k = "dTime", l = "D.TIME", min = 1, max = 3, def = 2, choices = { "1/8", "3/16", "1/4" } },
  { k = "dFb", l = "D.FDBK", min = 0, max = 0.9, def = 0.45 },
  { k = "dDamp", l = "D.DAMP", min = 0, max = 1, def = 0.4 },
  { k = "rSize", l = "R.SIZE", min = 0, max = 1, def = 0.75 },
  { k = "rDamp", l = "R.DAMP", min = 0, max = 1, def = 0.4 },
  { k = "duckRel", l = "DUCK R", min = 0.04, max = 0.6, def = 0.16, exp = true, fmt = "s" },
  { k = "dRet", l = "D.RET", min = 0, max = 1.5, def = 0.8 },
  { k = "rRet", l = "R.RET", min = 0, max = 1.5, def = 0.8 },
}

P.rowNames = { "KICK", "SNARE", "CLAP", "C-HAT", "O-HAT", "TOM", "RIM", "PERC" }

---------------------------------------------------------------- synth presets
P.synths = {
  { name = "ROLLING BASS", w1 = 1, w2 = 2, oct2 = 0, det = 4, mix = 0.4, sub = 0.5, mono = 2, cut = 220, res = 0.25, env = 0.35, fD = 0.12, fS = 0, aD = 0.18, aS = 0.3, aR = 0.05, drive = 0.3, lvl = 1.05 },
  { name = "SUB BASS", w1 = 4, w2 = 3, mix = 0.2, sub = 0.3, mono = 2, cut = 600, env = 0.1, aD = 0.5, aS = 0.8, aR = 0.08, glide = 0.04, lvl = 0.5 },
  { name = "ACID 303", w1 = 1, mix = 0, mono = 2, cut = 320, res = 0.86, env = 0.55, fD = 0.16, fS = 0, aD = 0.25, aS = 0.5, aR = 0.04, glide = 0.07, drive = 0.55, lvl = 1.3 },
  { name = "ACID SQUARE", w1 = 2, mix = 0, mono = 2, cut = 260, res = 0.8, env = 0.62, fD = 0.22, fS = 0, aD = 0.3, aS = 0.4, aR = 0.04, glide = 0.09, drive = 0.7, lvl = 1.4 },
  { name = "REESE", w1 = 1, w2 = 1, det = 28, mix = 0.5, uni = 2, mono = 2, cut = 500, res = 0.3, env = 0.15, fD = 0.6, fS = 0.5, aS = 0.9, aR = 0.1, glide = 0.05, drive = 0.4, lfoR = 0.3, lfoC = 0.15, lvl = 1.15 },
  { name = "HOUSE ORGAN", w1 = 4, w2 = 4, oct2 = 1, det = 0, mix = 0.45, sub = 0.35, mono = 2, cut = 2500, res = 0.1, env = 0.2, fD = 0.1, fS = 0.3, aA = 0.001, aD = 0.25, aS = 0.55, aR = 0.06, drive = 0.35, lvl = 1.1 },
  { name = "PLUCK BASS", w1 = 2, w2 = 1, oct2 = -1, mix = 0.5, mono = 2, cut = 300, res = 0.35, env = 0.5, fD = 0.09, fS = 0, aD = 0.2, aS = 0, aR = 0.08, drive = 0.2, lvl = 0.95 },
  { name = "TECHNO STAB", w1 = 1, w2 = 1, det = 14, mix = 0.5, uni = 2, cut = 700, res = 0.35, env = 0.5, fD = 0.14, fS = 0, aD = 0.22, aS = 0, aR = 0.12, drive = 0.2, lvl = 0.4 },
  { name = "HOUSE KEYS", w1 = 3, w2 = 2, oct2 = 0, det = 5, mix = 0.35, cut = 1100, res = 0.15, env = 0.4, fD = 0.25, fS = 0.1, aD = 0.5, aS = 0.15, aR = 0.2, lvl = 0.26 },
  { name = "PLUCK", w1 = 1, w2 = 2, oct2 = 1, det = 6, mix = 0.3, cut = 900, res = 0.3, env = 0.55, fD = 0.12, fS = 0, aD = 0.25, aS = 0, aR = 0.2, lvl = 0.3 },
  { name = "SUPERSAW LEAD", w1 = 1, w2 = 1, oct2 = 1, det = 22, mix = 0.3, uni = 3, cut = 3500, res = 0.15, env = 0.3, fD = 0.4, fS = 0.4, aA = 0.005, aS = 0.8, aR = 0.3, lvl = 0.25 },
  { name = "SQUARE LEAD", w1 = 2, w2 = 2, det = 9, mix = 0.5, cut = 1800, res = 0.3, env = 0.3, fD = 0.2, fS = 0.3, aD = 0.2, aS = 0.5, aR = 0.12, lfoR = 5.5, lfoP = 0.12, lvl = 0.3 },
  { name = "WARM PAD", w1 = 1, w2 = 3, det = 16, mix = 0.4, uni = 2, cut = 900, res = 0.1, env = 0.2, fA = 1.2, fD = 2, fS = 0.6, aA = 0.6, aD = 1, aS = 0.85, aR = 1.4, lfoR = 0.25, lfoC = 0.12, lvl = 0.25 },
  { name = "DARK PAD", w1 = 1, w2 = 2, oct2 = -1, det = 20, mix = 0.35, uni = 2, cut = 420, res = 0.35, env = 0.25, fA = 2.5, fD = 3, fS = 0.5, aA = 1.0, aD = 1, aS = 0.9, aR = 2.0, lfoR = 0.12, lfoC = 0.3, lvl = 0.22 },
  { name = "STRINGS", w1 = 1, w2 = 1, oct2 = 1, det = 18, mix = 0.35, uni = 3, cut = 2600, res = 0.05, env = 0.1, aA = 0.35, aS = 0.9, aR = 0.9, lfoR = 4.5, lfoP = 0.08, lvl = 0.2 },
  { name = "RISER", w1 = 1, mix = 0, noise = 1, ftype = 2, cut = 180, res = 0.55, env = 0.9, fA = 7, fD = 1, fS = 1, aA = 3.5, aS = 1, aR = 0.4, lfoR = 6, lfoC = 0.06, lvl = 0.32 },
  { name = "INIT", lvl = 0.35 },
}

function P.toNorm(s, v)
  if s.exp then return math.log(v / s.min) / math.log(s.max / s.min) end
  return (v - s.min) / (s.max - s.min)
end

function P.fromNorm(s, n)
  if n < 0 then n = 0 elseif n > 1 then n = 1 end
  local v
  if s.exp then v = s.min * (s.max / s.min) ^ n else v = s.min + n * (s.max - s.min) end
  if s.choices or s.int then v = math.floor(v + 0.5) end
  return v
end

P.volParam = { k = "vol", l = "VOLUME", min = 0, max = 1, def = 0.7 }

function P.applySynth(params, idx)
  local pr = P.synths[idx]
  for _, s in ipairs(P.synthParams) do
    params[s.k] = pr[s.k] ~= nil and pr[s.k] or s.def
  end
end

function P.fillDefaults(t, specs)
  for _, s in pairs(specs) do if t[s.k] == nil then t[s.k] = s.def end end
end

---------------------------------------------------------------- drum kits
P.kits = {
  { name = "909 TECHNO", rows = {
    { type = "kick", f = 50, sweep = 250, pd = 0.028, dec = 0.13, drive = 0.55, click = 0.35, g = 1.0 },
    { type = "snare", f = 190, tdec = 0.06, dec = 0.13, hp = 1800, g = 0.6 },
    { type = "clap", f = 1150, dec = 0.11, g = 0.62 },
    { type = "hat", fm = 1.35, hp = 8000, dec = 0.03, metal = 0.45, g = 0.4 },
    { type = "hat", fm = 1.35, hp = 7000, dec = 0.22, metal = 0.45, g = 0.36 },
    { type = "tom", f = 105, sweep = 90, pd = 0.05, dec = 0.18, drive = 0.2, click = 0.05, g = 0.7 },
    { type = "rim", f = 1750, dec = 0.012, g = 0.5 },
    { type = "perc", f = 410, ratio = 1.51, bp = 2100, dec = 0.09, g = 0.7 },
  } },
  { name = "808 DEEP", rows = {
    { type = "kick", f = 45, sweep = 110, pd = 0.045, dec = 0.42, drive = 0.1, click = 0.1, g = 1.0 },
    { type = "snare", f = 170, tdec = 0.09, dec = 0.17, hp = 1200, g = 0.6 },
    { type = "clap", f = 1000, dec = 0.16, g = 0.7 },
    { type = "hat", fm = 1.0, hp = 7000, dec = 0.035, metal = 0.8, g = 0.42 },
    { type = "hat", fm = 1.0, hp = 6500, dec = 0.32, metal = 0.8, g = 0.38 },
    { type = "tom", f = 90, sweep = 60, pd = 0.06, dec = 0.3, drive = 0.05, click = 0, g = 0.7 },
    { type = "rim", f = 1650, dec = 0.01, g = 0.5 },
    { type = "perc", f = 540, ratio = 1.481, bp = 2640, dec = 0.18, g = 0.8 },
  } },
  { name = "HOUSE CLASSIC", rows = {
    { type = "kick", f = 54, sweep = 190, pd = 0.035, dec = 0.17, drive = 0.3, click = 0.3, g = 1.0 },
    { type = "snare", f = 200, tdec = 0.05, dec = 0.12, hp = 2200, g = 0.55 },
    { type = "clap", f = 1250, dec = 0.14, g = 0.66 },
    { type = "hat", fm = 1.2, hp = 8500, dec = 0.028, metal = 0.35, g = 0.38 },
    { type = "hat", fm = 1.2, hp = 7500, dec = 0.2, metal = 0.35, g = 0.36 },
    { type = "tom", f = 120, sweep = 80, pd = 0.05, dec = 0.2, drive = 0.1, click = 0.05, g = 0.7 },
    { type = "rim", f = 1850, dec = 0.011, g = 0.5 },
    { type = "perc", f = 620, ratio = 1.34, bp = 3000, dec = 0.06, g = 0.7 },
  } },
  { name = "INDUSTRIAL", rows = {
    { type = "kick", f = 47, sweep = 420, pd = 0.02, dec = 0.2, drive = 1.0, click = 0.6, g = 0.95 },
    { type = "snare", f = 150, tdec = 0.05, dec = 0.22, hp = 900, g = 0.65 },
    { type = "clap", f = 850, dec = 0.2, g = 0.8 },
    { type = "hat", fm = 1.7, hp = 9000, dec = 0.02, metal = 0.7, g = 0.4 },
    { type = "hat", fm = 1.7, hp = 6000, dec = 0.28, metal = 0.7, g = 0.36 },
    { type = "tom", f = 80, sweep = 200, pd = 0.04, dec = 0.25, drive = 0.8, click = 0.2, g = 0.7 },
    { type = "rim", f = 1300, dec = 0.02, g = 0.55 },
    { type = "perc", f = 300, ratio = 1.72, bp = 1500, dec = 0.14, g = 0.8 },
  } },
}

function P.newRows()
  local rows = {}
  for r = 1, 8 do
    rows[r] = {}
    for _, s in ipairs(P.drumParams) do rows[r][s.k] = s.def end
  end
  return rows
end

return P
