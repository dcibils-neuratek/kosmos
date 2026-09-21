-- Value noise + fbm. Pure Lua port of the GLSL used by the GPU version.
-- Used by tools/bake.lua (textures) and solar/soft.lua (ring profile).

local floor = math.floor
local N = {}

local function fract(x) return x - floor(x) end

local function hash(x, y, z)
  x, y, z = fract(x * 0.1031), fract(y * 0.1030), fract(z * 0.0973)
  local d = x * (y + 33.33) + y * (x + 33.33) + z * (z + 33.33)
  x, y, z = x + d, y + d, z + d
  return fract((x + y) * z)
end

local function vnoise(x, y, z)
  local ix, iy, iz = floor(x), floor(y), floor(z)
  local fx, fy, fz = x - ix, y - iy, z - iz
  fx, fy, fz = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy), fz * fz * (3 - 2 * fz)
  local a = hash(ix, iy, iz);         a = a + (hash(ix + 1, iy, iz) - a) * fx
  local b = hash(ix, iy + 1, iz);     b = b + (hash(ix + 1, iy + 1, iz) - b) * fx
  local c = hash(ix, iy, iz + 1);     c = c + (hash(ix + 1, iy, iz + 1) - c) * fx
  local d = hash(ix, iy + 1, iz + 1); d = d + (hash(ix + 1, iy + 1, iz + 1) - d) * fx
  a = a + (b - a) * fy
  c = c + (d - c) * fy
  return a + (c - a) * fz
end

local function fbm(x, y, z)
  local amp, s = 0.5, 0
  for _ = 1, 5 do
    s = s + amp * vnoise(x, y, z)
    x, y, z = x * 2.03 + 11.7, y * 2.03 + 3.1, z * 2.03 + 7.3
    amp = amp * 0.5
  end
  return s
end

local function smoothstep(a, b, x)
  local t = (x - a) / (b - a)
  if t <= 0 then return 0 elseif t >= 1 then return 1 end
  return t * t * (3 - 2 * t)
end

N.hash, N.vnoise, N.fbm, N.smoothstep = hash, vnoise, fbm, smoothstep
return N
