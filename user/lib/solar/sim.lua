-- solar/sim.lua — simulation core. Pure Lua 5.1 / 5.2 / 5.3 / 5.4 / LuaJIT.
-- No graphics, no I/O, no globals. Keplerian orbits from JPL J2000 elements.

local sin, cos, sqrt, pi = math.sin, math.cos, math.sqrt, math.pi
local floor, abs, min, max, exp, log = math.floor, math.abs, math.min, math.max, math.exp, math.log
local TAU, RAD = 2 * pi, pi / 180

local Sim = {}
Sim.__index = Sim

Sim.DIST       = 10      -- display units per AU (before compression)
Sim.P_COMPRESS = 0.55    -- r -> r^p squeezes the outer system into view
Sim.P_REAL     = 1.0
Sim.ORBIT_N    = 220

Sim.speeds = {
  { "real time", 1 / 86400 }, { "1 min/s", 1 / 1440 }, { "1 hour/s", 1 / 24 }, { "1 day/s", 1 },
  { "1 week/s", 7 }, { "1 month/s", 30.44 }, { "1 year/s", 365.25 }, { "10 years/s", 3652.5 },
}

--------------------------------------------------------------------------------
-- Deterministic RNG (Park-Miller). Same sky on every Lua version.
--------------------------------------------------------------------------------

local function newRng(seed)
  local s = seed % 2147483647
  if s <= 0 then s = s + 2147483646 end
  local function rnd()
    s = (s * 16807) % 2147483647
    return (s - 1) / 2147483646
  end
  local function normal(sigma)
    local u1, u2 = max(rnd(), 1e-12), rnd()
    return sigma * sqrt(-2 * log(u1)) * cos(TAU * u2)
  end
  return rnd, normal
end

--------------------------------------------------------------------------------
-- Bodies. kind: 0 rocky, 1 earth, 2 banded gas, 3 ice giant, 4 cloud deck, 5 rocky + caps
-- a [AU], e, inc, L, lp (long. of perihelion), node [deg], rate [deg/century]
--------------------------------------------------------------------------------

local function bodyDefs()
  local sun = {
    name = "Sun", class = "G2V star", star = true, r = 1.8,
    radiusKm = 695700, rot = 25.38, tilt = 7.25, c1 = { 1.0, 0.8, 0.4 },
    desc = "99.86% of the system's mass.",
  }

  local planets = {
    { name = "Mercury", class = "Terrestrial planet",
      a = 0.38709927, e = 0.20563593, inc = 7.00497902, L = 252.25032350, lp = 77.45779628, node = 48.33076593, rate = 149472.67411175,
      radiusKm = 2439.7, rot = 58.646, tilt = 0.03, year = 87.969, kind = 0,
      c1 = {0.45,0.42,0.40}, c2 = {0.62,0.59,0.56}, c3 = {0.30,0.28,0.27},
      desc = "3:2 spin-orbit resonance." },
    { name = "Venus", class = "Terrestrial planet",
      a = 0.72333566, e = 0.00677672, inc = 3.39467605, L = 181.97909950, lp = 131.60246718, node = 76.67984255, rate = 58517.81538729,
      radiusKm = 6051.8, rot = 243.025, tilt = 177.36, year = 224.701, kind = 4,
      c1 = {0.80,0.62,0.35}, c2 = {0.97,0.90,0.70}, c3 = {1,1,1}, atm = 0.8, atmColor = {1.0,0.85,0.55},
      desc = "Retrograde rotation. A day outlasts its year." },
    { name = "Earth", class = "Terrestrial planet",
      a = 1.00000261, e = 0.01671123, inc = -0.00001531, L = 100.46457166, lp = 102.93768193, node = 0.0, rate = 35999.37244981,
      radiusKm = 6371.0, rot = 0.99727, tilt = 23.44, year = 365.256, kind = 1,
      c1 = {0.02,0.10,0.30}, c2 = {0.13,0.35,0.12}, c3 = {0.55,0.47,0.30}, atm = 1.0, atmColor = {0.35,0.60,1.0},
      desc = "You are here." },
    { name = "Mars", class = "Terrestrial planet",
      a = 1.52371034, e = 0.09339410, inc = 1.84969142, L = -4.55343205, lp = -23.94362959, node = 49.55953891, rate = 19140.30268499,
      radiusKm = 3389.5, rot = 1.02596, tilt = 25.19, year = 686.980, kind = 5,
      c1 = {0.70,0.33,0.16}, c2 = {0.48,0.21,0.11}, c3 = {0.85,0.55,0.35}, atm = 0.3, atmColor = {0.9,0.55,0.35},
      desc = "Olympus Mons: 21.9 km tall." },
    { name = "Jupiter", class = "Gas giant",
      a = 5.20288700, e = 0.04838624, inc = 1.30439695, L = 34.39644051, lp = 14.72847983, node = 100.47390909, rate = 3034.74612775,
      radiusKm = 69911, rot = 0.41354, tilt = 3.13, year = 4332.59, kind = 2, bands = 17, spot = 1,
      c1 = {0.86,0.76,0.60}, c2 = {0.60,0.40,0.27}, c3 = {0.94,0.89,0.80}, atm = 0.25, atmColor = {0.9,0.8,0.65},
      desc = "2.5x the mass of all other planets combined." },
    { name = "Saturn", class = "Gas giant", rings = true,
      a = 9.53667594, e = 0.05386179, inc = 2.48599187, L = 49.95424423, lp = 92.59887831, node = 113.66242448, rate = 1222.49362201,
      radiusKm = 58232, rot = 0.44401, tilt = 26.73, year = 10759.22, kind = 2, bands = 11, spot = 0,
      c1 = {0.89,0.81,0.60}, c2 = {0.74,0.63,0.42}, c3 = {0.94,0.89,0.72}, atm = 0.25, atmColor = {0.95,0.88,0.65},
      desc = "Less dense than water. Rings ~10 m thick." },
    { name = "Uranus", class = "Ice giant",
      a = 19.18916464, e = 0.04725744, inc = 0.77263783, L = 313.23810451, lp = 170.95427630, node = 74.01692503, rate = 428.48202785,
      radiusKm = 25362, rot = 0.71833, tilt = 97.77, year = 30688.5, kind = 3,
      c1 = {0.62,0.85,0.88}, c2 = {0.54,0.79,0.85}, c3 = {1,1,1}, atm = 0.5, atmColor = {0.6,0.9,0.95},
      desc = "Rolls around the Sun on its side." },
    { name = "Neptune", class = "Ice giant",
      a = 30.06992276, e = 0.00859048, inc = 1.77004347, L = -55.12002969, lp = 44.96476227, node = 131.78422574, rate = 218.45945325,
      radiusKm = 24622, rot = 0.67125, tilt = 28.32, year = 60182, kind = 3,
      c1 = {0.18,0.32,0.85}, c2 = {0.30,0.50,0.95}, c3 = {1,1,1}, atm = 0.5, atmColor = {0.3,0.5,1.0},
      desc = "Found by math before by telescope." },
    { name = "Pluto", class = "Dwarf planet",
      a = 39.48211675, e = 0.24882730, inc = 17.14001206, L = 238.92903833, lp = 224.06891629, node = 110.30393684, rate = 145.20780515,
      radiusKm = 1188.3, rot = 6.387, tilt = 122.53, year = 90560, kind = 0,
      c1 = {0.76,0.66,0.56}, c2 = {0.52,0.40,0.33}, c3 = {0.92,0.88,0.82},
      desc = "Not one full orbit since discovery (1930)." },
  }

  -- dist: display units from parent centre (not to scale); period in days
  local moons = {
    { name = "Moon", parent = "Earth", dist = 0.95, period = 27.3217, radiusKm = 1737.4, ecl = true,
      c1 = {0.55,0.54,0.52}, c2 = {0.36,0.35,0.34}, c3 = {0.70,0.69,0.67} },
    { name = "Io", parent = "Jupiter", dist = 1.65, period = 1.769, radiusKm = 1821.6,
      c1 = {0.88,0.78,0.32}, c2 = {0.90,0.55,0.20}, c3 = {0.96,0.92,0.62} },
    { name = "Europa", parent = "Jupiter", dist = 2.10, period = 3.551, radiusKm = 1560.8,
      c1 = {0.86,0.82,0.72}, c2 = {0.62,0.47,0.36}, c3 = {0.95,0.94,0.90} },
    { name = "Ganymede", parent = "Jupiter", dist = 2.65, period = 7.155, radiusKm = 2634.1,
      c1 = {0.55,0.50,0.45}, c2 = {0.36,0.32,0.29}, c3 = {0.75,0.73,0.70} },
    { name = "Callisto", parent = "Jupiter", dist = 3.35, period = 16.689, radiusKm = 2410.3,
      c1 = {0.34,0.30,0.27}, c2 = {0.22,0.20,0.18}, c3 = {0.55,0.52,0.48} },
    { name = "Titan", parent = "Saturn", dist = 3.0, period = 15.945, radiusKm = 2574.7, kind = 4,
      c1 = {0.78,0.55,0.22}, c2 = {0.90,0.70,0.35}, c3 = {1,1,1}, atm = 0.9, atmColor = {0.95,0.65,0.25} },
    { name = "Triton", parent = "Neptune", dist = 1.5, period = -5.877, radiusKm = 1353.4,
      c1 = {0.78,0.70,0.68}, c2 = {0.60,0.55,0.58}, c3 = {0.92,0.90,0.90} },
  }
  return sun, planets, moons
end

--------------------------------------------------------------------------------
-- Orbital mechanics
--------------------------------------------------------------------------------

local function orbitPoint(b, E)        -- eccentric anomaly -> AU, world axes (Y up)
  local xp, yp = b.a * (cos(E) - b.e), b.a * b.bq * sin(E)
  return b.m11 * xp + b.m12 * yp, b.m31 * xp + b.m32 * yp, b.m21 * xp + b.m22 * yp
end

local function solveKepler(b, days)
  local Ldeg = b.L + b.rate * days / 36525
  local M = ((Ldeg - b.lp) % 360) * RAD
  local e = b.e
  local E = M + e * sin(M)
  for _ = 1, 6 do E = E - (E - e * sin(E) - M) / (1 - e * cos(E)) end
  return E
end

function Sim.calendar(days)            -- days since J2000 -> y, m, d, h, min (UTC)
  local JD = 2451545.0 + days
  local Z = floor(JD + 0.5)
  local Fr = JD + 0.5 - Z
  local A = Z
  if Z >= 2299161 then
    local al = floor((Z - 1867216.25) / 36524.25)
    A = Z + 1 + al - floor(al / 4)
  end
  local B = A + 1524
  local C = floor((B - 122.1) / 365.25)
  local D = floor(365.25 * C)
  local E = floor((B - D) / 30.6001)
  local day = B - D - floor(30.6001 * E)
  local month = E < 14 and E - 1 or E - 13
  local year = month > 2 and C - 4716 or C - 4715
  local hrs = Fr * 24
  return year, month, day, floor(hrs), floor((hrs % 1) * 60)
end

--------------------------------------------------------------------------------
-- Construction
--------------------------------------------------------------------------------

local function initBody(self, b, i)
  b.r = b.r or 0.35 * (b.radiusKm / 6371) ^ 0.45 * (b.parent and 0.6 or 1)
  local t = (b.tilt or 0) * RAD
  b.upx, b.upy, b.upz = 0, cos(t), sin(t)          -- spin axis
  b.e1x, b.e1y, b.e1z = 1, 0, 0                    -- equatorial basis
  b.e2x, b.e2y, b.e2z = 0, -sin(t), cos(t)
  b.spin = (i * 2.399) % TAU
  b.seed = i * 13.37
  b.kind = b.kind or 0
  b.atm = b.atm or 0
  b.atmColor = b.atmColor or { 1, 1, 1 }
  b.x, b.y, b.z = 0, 0, 0
  b.id = string.lower(b.name)
  self.bodies[#self.bodies + 1] = b
  self.byName[b.name] = b
end

local function initOrbit(b)
  local w, O, i = (b.lp - b.node) * RAD, b.node * RAD, b.inc * RAD
  local cw, sw, cO, sO, ci, si = cos(w), sin(w), cos(O), sin(O), cos(i), sin(i)
  b.m11, b.m12 = cw * cO - sw * sO * ci, -sw * cO - cw * sO * ci
  b.m21, b.m22 = cw * sO + sw * cO * ci, -sw * sO + cw * cO * ci
  b.m31, b.m32 = sw * si, cw * si
  b.bq = sqrt(1 - b.e * b.e)
  b.path = {}
  for k = 0, Sim.ORBIT_N - 1 do
    local x, y, z = orbitPoint(b, k / Sim.ORBIT_N * TAU)
    b.path[k * 3 + 1], b.path[k * 3 + 2], b.path[k * 3 + 3] = x, y, z
  end
end

-- Stars: three size classes; flat arrays (dir = x,y,z,...  col = r,g,b,a,...), colours 0..255.
local function buildStars(nField, nBand)
  local rnd, normal = newRng(42)
  local tints = { { 191, 209, 255 }, { 255, 255, 255 }, { 255, 242, 204 }, { 255, 209, 158 }, { 217, 230, 255 } }
  local sets = { { size = 1, dir = {}, col = {}, n = 0 }, { size = 2, dir = {}, col = {}, n = 0 }, { size = 3, dir = {}, col = {}, n = 0 } }
  local function add(x, y, z, bright)
    local s = bright > 0.75 and sets[3] or bright > 0.4 and sets[2] or sets[1]
    local t = tints[floor(rnd() * #tints) + 1]
    local n = s.n
    s.dir[n * 3 + 1], s.dir[n * 3 + 2], s.dir[n * 3 + 3] = x, y, z
    s.col[n * 4 + 1], s.col[n * 4 + 2], s.col[n * 4 + 3], s.col[n * 4 + 4] = t[1], t[2], t[3], 0.25 + 0.75 * bright
    s.n = n + 1
  end
  for _ = 1, nField do
    local z = rnd() * 2 - 1
    local a = rnd() * TAU
    local r = sqrt(1 - z * z)
    add(r * cos(a), z, r * sin(a), rnd() ^ 3)
  end
  local ti = 60 * RAD                                 -- Milky Way band
  for _ = 1, nBand do
    local lon = rnd() * TAU
    local lat = normal(0.10 + 0.08 * (0.5 + 0.5 * cos(lon)))
    local x, y, z = cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon)
    y, z = y * cos(ti) - z * sin(ti), y * sin(ti) + z * cos(ti)
    add(x, y, z, rnd() ^ 4 * 0.5)
  end
  return sets
end

local function buildBelt(n, aMin, aMax, incSigma, color, seed)
  local rnd, normal = newRng(seed)
  local belt = { n = n, items = {}, r = color[1], g = color[2], b = color[3], a = color[4] }
  for k = 1, n do
    local a = aMin + (aMax - aMin) * (0.5 + 0.5 * max(-1, min(1, normal(0.45))))
    belt.items[k] = { a = a, phase = rnd() * TAU, n = TAU / (a ^ 1.5 * 365.25),
                      si = sin(normal(incSigma)), node = rnd() * TAU, shade = 0.6 + 0.4 * rnd() }
  end
  return belt
end

-- opts: now (unix seconds, optional), stars = {field, band}, belts = {main, kuiper}
function Sim.new(opts)
  opts = opts or {}
  local self = setmetatable({}, Sim)
  self.bodies, self.byName = {}, {}
  self.sun, self.planets, self.moons = bodyDefs()
  initBody(self, self.sun, 0)
  for i, p in ipairs(self.planets) do initBody(self, p, i); initOrbit(p); p.moonCount = 0 end
  for i, m in ipairs(self.moons) do
    initBody(self, m, 20 + i)
    m.parentBody = self.byName[m.parent]
    m.parentBody.moonCount = m.parentBody.moonCount + 1
    m.phase = (i * 1.7) % TAU
    m.class = "Moon of " .. m.parent
  end
  local st = opts.stars or { 2000, 2600 }
  local bl = opts.belts or { 1400, 1000 }
  self.stars = buildStars(st[1], st[2])
  self.belts = {
    buildBelt(bl[1], 2.1, 3.3, 0.09, { 184, 173, 158, 0.65 }, 11),   -- main belt
    buildBelt(bl[2], 36, 50, 0.13, { 153, 173, 204, 0.45 }, 12),     -- Kuiper belt
  }
  self.epoch = opts.now and (opts.now - 946728000) / 86400 or 9759.0   -- days since J2000
  self.days = self.epoch
  self.speedIdx, self.dir, self.paused, self.animT = 5, 1, false, 0
  self.scaleP, self.scaleGoal = Sim.P_COMPRESS, Sim.P_COMPRESS
  self:update(0)
  return self
end

function Sim:toDisplay(x, y, z)        -- AU (heliocentric) -> display units
  local r = sqrt(x * x + y * y + z * z)
  if r < 1e-9 then return 0, 0, 0 end
  local s = Sim.DIST * r ^ (self.scaleP - 1)
  return x * s, y * s, z * s
end

function Sim:update(dt)
  local ddays = self.paused and 0 or dt * Sim.speeds[self.speedIdx][2] * self.dir
  self.days = self.days + ddays
  self.animT = self.animT + dt
  self.scaleP = self.scaleP + (self.scaleGoal - self.scaleP) * (1 - exp(-dt * 3))

  for _, b in ipairs(self.planets) do
    local E = solveKepler(b, self.days)
    local x, y, z = orbitPoint(b, E)
    b.E, b.au = E, sqrt(x * x + y * y + z * z)
    b.x, b.y, b.z = self:toDisplay(x, y, z)
  end
  for _, m in ipairs(self.moons) do
    local p = m.parentBody
    local th = m.phase + TAU * self.days / m.period
    local c, s = cos(th) * m.dist, sin(th) * m.dist
    if m.ecl then
      m.x, m.y, m.z = p.x + c, p.y, p.z + s
    else
      m.x, m.y, m.z = p.x + c * p.e1x + s * p.e2x, p.y + c * p.e1y + s * p.e2y, p.z + c * p.e1z + s * p.e2z
    end
    m.au = p.au
  end
  local cap = 1.6 * dt                       -- cap visual spin so it never strobes
  for _, b in ipairs(self.bodies) do
    local period = b.rot or b.period
    b.spin = (b.spin + max(-cap, min(cap, TAU * ddays / period))) % TAU
  end
end

return Sim
