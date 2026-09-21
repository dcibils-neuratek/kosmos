-- solar/soft.lua — software rasterizer. Pure Lua 5.1..5.4 / LuaJIT, no dependencies.
--
-- Framebuffer: self.fb[1 .. w*h], row-major, each entry an integer-valued number
-- 0xRRGGBB (i.e. XRGB8888 without the X). A Lua table by default, or a host buffer (see S.new).
-- Any resolution: nothing here assumes a size.
-- Colours passed in are 0..255, alpha 0..1. Pixel coordinates passed to the
-- low-level calls must be integers.

local Noise = require "solar.noise"
local floor, sqrt, abs, min, max = math.floor, math.sqrt, math.abs, math.min, math.max
local acos, exp, pi = math.acos, math.exp, math.pi
local atan2 = math.atan2 or math.atan          -- 5.3+/5.4: math.atan(y, x)
local byte = string.byte
local INV_TAU, INV_PI = 1 / (2 * pi), 1 / pi
local smoothstep = Noise.smoothstep

local S = {}
S.__index = S

-- fb is optional: pass any object indexable 1 .. w*h that stores numbers (a native pixel
-- buffer, an FFI uint32 array...). The rasterizer only ever does fb[i] and fb[i] = n, so a
-- host whose framebuffer is XRGB8888 can have it written in place and skip the copy.
function S.new(w, h, fb)
  local self = setmetatable({ w = w, h = h, fb = fb or {} }, S)
  self:clear(0)
  return self
end

function S:clear(c)
  local fb = self.fb
  for i = 1, self.w * self.h do fb[i] = c end
end

function S:blend(x, y, r, g, b, a)
  if x < 0 or y < 0 or x >= self.w or y >= self.h or a <= 0 then return end
  local fb, i = self.fb, y * self.w + x + 1
  if a >= 1 then
    fb[i] = floor(r) * 65536 + floor(g) * 256 + floor(b)
    return
  end
  local d = fb[i]
  local dr, dg, db = floor(d / 65536), floor(d / 256) % 256, d % 256
  fb[i] = floor(dr + (r - dr) * a) * 65536 + floor(dg + (g - dg) * a) * 256 + floor(db + (b - db) * a)
end

-- size 1: one pixel; 2: soft plus; 3: soft 3x3
function S:point(x, y, size, r, g, b, a)
  self:blend(x, y, r, g, b, a)
  if size >= 2 then
    local e = a * (size >= 3 and 0.6 or 0.35)
    self:blend(x - 1, y, r, g, b, e); self:blend(x + 1, y, r, g, b, e)
    self:blend(x, y - 1, r, g, b, e); self:blend(x, y + 1, r, g, b, e)
    if size >= 3 then
      e = a * 0.25
      self:blend(x - 1, y - 1, r, g, b, e); self:blend(x + 1, y - 1, r, g, b, e)
      self:blend(x - 1, y + 1, r, g, b, e); self:blend(x + 1, y + 1, r, g, b, e)
    end
  end
end

-- Anti-aliased line, float endpoints, clipped to the screen (Liang-Barsky).
function S:line(x0, y0, x1, y1, r, g, b, a)
  local w, h = self.w - 1, self.h - 1
  local dx, dy = x1 - x0, y1 - y0
  local t0, t1 = 0, 1
  local function clip(p, q)
    if p == 0 then return q >= 0 end
    local t = q / p
    if p < 0 then
      if t > t1 then return false end
      if t > t0 then t0 = t end
    else
      if t < t0 then return false end
      if t < t1 then t1 = t end
    end
    return true
  end
  if not (clip(-dx, x0) and clip(dx, w - x0) and clip(-dy, y0) and clip(dy, h - y0)) then return end
  x0, y0, x1, y1 = x0 + t0 * dx, y0 + t0 * dy, x0 + t1 * dx, y0 + t1 * dy
  dx, dy = x1 - x0, y1 - y0

  if abs(dx) >= abs(dy) then
    if x0 > x1 then x0, y0, x1, y1 = x1, y1, x0, y0 end
    local grad = (dx == 0) and 0 or dy / dx
    local xs, xe = floor(x0 + 0.5), floor(x1 + 0.5)
    local y = y0 + grad * (xs - x0)
    for x = xs, xe do
      local yi = floor(y)
      local f = y - yi
      self:blend(x, yi, r, g, b, a * (1 - f))
      self:blend(x, yi + 1, r, g, b, a * f)
      y = y + grad
    end
  else
    if y0 > y1 then x0, y0, x1, y1 = x1, y1, x0, y0 end
    local grad = dx / dy
    local ys, ye = floor(y0 + 0.5), floor(y1 + 0.5)
    local x = x0 + grad * (ys - y0)
    for y = ys, ye do
      local xi = floor(x)
      local f = x - xi
      self:blend(xi, y, r, g, b, a * (1 - f))
      self:blend(xi + 1, y, r, g, b, a * f)
      x = x + grad
    end
  end
end

-- Same clipping as S:line but one pixel per step and no coverage maths: about half the cost.
function S:lineFast(x0, y0, x1, y1, r, g, b, a)
  local w, h = self.w - 1, self.h - 1
  if (x0 < 0 and x1 < 0) or (y0 < 0 and y1 < 0) or (x0 > w and x1 > w) or (y0 > h and y1 > h) then return end
  local dx, dy = x1 - x0, y1 - y0
  local n = floor(max(abs(dx), abs(dy))) + 1
  if n > 4000 then return self:line(x0, y0, x1, y1, r, g, b, a) end   -- huge: let Liang-Barsky clip it
  local sx, sy = dx / n, dy / n
  for _ = 0, n do
    self:blend(floor(x0 + 0.5), floor(y0 + 0.5), r, g, b, a)
    x0, y0 = x0 + sx, y0 + sy
  end
end

-- step x step blocks, used by sphere/ring/sun when rendering at reduced resolution
local function fillBlock(fb, W, H, px, py, step, c)
  for yy = py, min(py + step - 1, H - 1) do
    local row = yy * W + 1
    for i = row + px, row + min(px + step - 1, W - 1) do fb[i] = c end
  end
end

local function blendBlock(fb, W, H, px, py, step, r, g, b, a)
  local k = 1 - a
  local sr, sg, sb = r * a, g * a, b * a
  for yy = py, min(py + step - 1, H - 1) do
    local row = yy * W + 1
    for i = row + px, row + min(px + step - 1, W - 1) do
      local d = fb[i]
      fb[i] = floor(sr + floor(d / 65536) * k) * 65536 + floor(sg + floor(d / 256) % 256 * k) * 256 + floor(sb + d % 256 * k)
    end
  end
end

function S:rect(x, y, w, h, r, g, b, a)
  x, y, w, h = floor(x), floor(y), floor(w), floor(h)
  local x0, x1 = max(0, x), min(self.w - 1, x + w - 1)
  local y0, y1 = max(0, y), min(self.h - 1, y + h - 1)
  local fb, W = self.fb, self.w
  if a >= 1 then
    local c = floor(r) * 65536 + floor(g) * 256 + floor(b)
    for yy = y0, y1 do
      local row = yy * W + 1
      for i = row + x0, row + x1 do fb[i] = c end
    end
    return
  end
  -- Inlined blend: this fills ~140k pixels per frame for the HUD panels, so no per-pixel
  -- method call, no bounds check, and the source terms are hoisted out of the loop.
  local k = 1 - a
  local sr, sg, sb = r * a, g * a, b * a
  for yy = y0, y1 do
    local row = yy * W + 1
    for i = row + x0, row + x1 do
      local d = fb[i]
      fb[i] = floor(sr + floor(d / 65536) * k) * 65536 + floor(sg + floor(d / 256) % 256 * k) * 256 + floor(sb + d % 256 * k)
    end
  end
end

function S:frame(x, y, w, h, r, g, b, a)
  x, y, w, h = floor(x), floor(y), floor(w), floor(h)
  for xx = x, x + w - 1 do self:blend(xx, y, r, g, b, a); self:blend(xx, y + h - 1, r, g, b, a) end
  for yy = y + 1, y + h - 2 do self:blend(x, yy, r, g, b, a); self:blend(x + w - 1, yy, r, g, b, a) end
end

function S:circle(cx, cy, rad, r, g, b, a)      -- outline
  local n = max(16, floor(rad * 0.8))
  local px, py = cx + rad, cy
  for i = 1, n do
    local t = i / n * 2 * pi
    local x, y = cx + rad * math.cos(t), cy + rad * math.sin(t)
    self:line(px, py, x, y, r, g, b, a)
    px, py = x, y
  end
end

-- Monospace bitmap text. align: nil/"left" or "right" (x is then the right edge).
function S:text(font, str, x, y, r, g, b, a, align)
  local cw, ch, glyphs = font.cw, font.ch, font.glyphs
  x, y = floor(x), floor(y)
  if align == "right" then x = x - #str * cw end
  for i = 1, #str do
    local gl = glyphs[byte(str, i)]
    if gl then
      local k = 1
      for yy = 0, ch - 1 do
        for xx = 0, cw - 1 do
          local v = gl[k]
          if v > 0 then self:blend(x + xx, y + yy, r, g, b, a * v) end
          k = k + 1
        end
      end
    end
    x = x + cw
  end
end

--------------------------------------------------------------------------------
-- Textures: binary PPM (P6, maxval 255). Pixel data stays a Lua string and is
-- read with string.byte — 3 bytes per texel instead of a table slot each.
--------------------------------------------------------------------------------

function S.parsePPM(s)
  if not s then return nil end
  local w, h, mx, pos = s:match("^P6%s+(%d+)%s+(%d+)%s+(%d+)%s()")
  if not w or tonumber(mx) ~= 255 then return nil end
  return { w = tonumber(w), h = tonumber(h), data = s:sub(pos) }
end

--------------------------------------------------------------------------------
-- Saturn's rings: 1-D profile (density + colour) over rho in planet radii
--------------------------------------------------------------------------------

local RING_IN, RING_OUT, RING_N = 1.22, 2.29, 512
local ringLut

local function band(x, a, b)
  return smoothstep(a - 0.004, a + 0.004, x) * (1 - smoothstep(b - 0.004, b + 0.004, x))
end

local function buildRingLut()
  ringLut = { dens = {}, r = {}, g = {}, b = {} }
  for i = 1, RING_N do
    local rho = RING_IN + (RING_OUT - RING_IN) * (i - 0.5) / RING_N
    local dens = 0.20 * band(rho, 1.24, 1.53) + 0.90 * band(rho, 1.53, 1.95)      -- C, B
               + 0.04 * band(rho, 1.95, 2.03) + 0.58 * band(rho, 2.03, 2.27)      -- Cassini, A
    dens = dens * (1 - 0.85 * band(rho, 2.205, 2.217))                            -- Encke
    local fine = (0.62 + 0.38 * Noise.vnoise(rho * 70, 1.3, 0)) * (0.85 + 0.15 * Noise.vnoise(rho * 260, 4.1, 0))
    local t = smoothstep(1.3, 1.7, rho)
    local k = 0.8 + 0.25 * fine
    ringLut.dens[i] = dens * fine
    ringLut.r[i] = (148 + (230 - 148) * t) * k
    ringLut.g[i] = (133 + (209 - 133) * t) * k
    ringLut.b[i] = (115 + (168 - 115) * t) * k
  end
end

local RING_IN2, RING_OUT2 = RING_IN * RING_IN, RING_OUT * RING_OUT
local RING_K = RING_N / (RING_OUT - RING_IN)

--------------------------------------------------------------------------------
-- Lit, textured sphere (orthographic impostor, like the GPU version)
--
-- o.sx, o.sy, o.sr        screen centre / radius in pixels
-- o.ax, o.ay, o.az        body axes in view space ({x,y,z} each); ay = spin axis
-- o.L                     unit vector body -> sun, view space (x right, y up, z away)
-- o.tex                   equirect texture or nil (then o.color {r,g,b} 0..255 is used)
-- o.aux, o.cloudU         Earth only: R = clouds, G = ocean mask, B = city lights
-- o.atm, o.atmColor       atmosphere strength 0..1 and colour 0..255
-- o.ringShadow            true: rings (equatorial plane) cast their shadow on the globe
-- o.step                  1 = full resolution; 2, 3 = shade once per step x step block
--------------------------------------------------------------------------------

function S:sphere(o)
  local W, H, fb = self.w, self.h, self.fb
  local cx, cy, R = o.sx, o.sy, o.sr
  local atm = o.atm or 0
  local pad = (atm > 0) and 1.35 or 1
  local Ro = R * pad
  local axx, axy, axz = o.ax[1], o.ax[2], o.ax[3]
  local ayx, ayy, ayz = o.ay[1], o.ay[2], o.ay[3]
  local azx, azy, azz = o.az[1], o.az[2], o.az[3]
  local Lx, Ly, Lz = o.L[1], o.L[2], o.L[3]
  local tex, aux = o.tex, o.aux
  local tw, th, data = 0, 0, nil
  if tex then tw, th, data = tex.w, tex.h, tex.data end
  local fr, fg, fb_ = 128, 128, 128
  if o.color then fr, fg, fb_ = o.color[1], o.color[2], o.color[3] end
  local ar, ag, ab = 255, 255, 255
  if o.atmColor then ar, ag, ab = o.atmColor[1], o.atmColor[2], o.atmColor[3] end
  local cloudU = o.cloudU or 0
  local ringShadow = o.ringShadow
  if ringShadow and not ringLut then buildRingLut() end
  local LN = Lx * ayx + Ly * ayy + Lz * ayz          -- sun vs ring normal
  -- half vector for the ocean glint: normalize(L + view), view = (0,0,-1)
  local hx, hy, hz = Lx, Ly, Lz - 1
  local hl = sqrt(hx * hx + hy * hy + hz * hz)
  if hl < 1e-6 then hl = 1 end
  hx, hy, hz = hx / hl, hy / hl, hz / hl
  local edge = (1 - 1.5 / R); edge = edge > 0 and edge * edge or 0
  local step = o.step or 1
  local half = step * 0.5

  for py = max(0, floor(cy - Ro)), min(H - 1, floor(cy + Ro) + 1), step do
    local dy = py + half - cy
    local span2 = Ro * Ro - dy * dy
    if span2 > 0 then
      local span = sqrt(span2)
      local ny = -dy / R
      local row = py * W + 1
      for px = max(0, floor(cx - span)), min(W - 1, floor(cx + span)), step do
        local nx = (px + half - cx) / R
        local r2 = nx * nx + ny * ny
        if r2 < 1 then
          local nz = -sqrt(1 - r2)
          local d = nx * Lx + ny * Ly + nz * Lz
          local cr, cg, cb = fr, fg, fb_
          local u, ty
          if data then
            local lx = nx * axx + ny * axy + nz * axz
            local ly = nx * ayx + ny * ayy + nz * ayz
            local lz = nx * azx + ny * azy + nz * azz
            if ly > 1 then ly = 1 elseif ly < -1 then ly = -1 end
            u = atan2(lz, lx) * INV_TAU + 0.5
            ty = floor(acos(ly) * INV_PI * th); if ty >= th then ty = th - 1 end
            local k = (ty * tw + floor(u * tw) % tw) * 3 + 1
            cr, cg, cb = byte(data, k, k + 2)
          end

          local lit = d > 0 and d or 0
          if atm > 0 then lit = lit + (smoothstep(-0.15, 0.55, d) - lit) * atm * 0.6 end

          local er, eg, eb, spec = 0, 0, 0, 0
          if aux and u then
            local aw, ah, ad = aux.w, aux.h, aux.data
            local ay2 = floor(ty * ah / th)
            local k1 = (ay2 * aw + floor(u * aw) % aw) * 3 + 1
            local _, ocean, lights = byte(ad, k1, k1 + 2)
            local k2 = (ay2 * aw + floor((u + cloudU) * aw) % aw) * 3 + 1
            local cl = byte(ad, k2) / 255
            if d > 0 and ocean > 0 then
              local sd = nx * hx + ny * hy + nz * hz
              if sd > 0.9 then spec = sd ^ 50 * (ocean / 255) * (1 - cl) * 0.7 * 255 end
            end
            if lights > 0 and d < 0.1 then
              local e = (1 - smoothstep(-0.15, 0.1, d)) * (lights / 255) * (1 - cl * 0.7) * 0.9
              er, eg, eb = 255 * e, 191 * e, 89 * e
            end
            local c9 = cl * 0.9
            cr, cg, cb = cr + (255 - cr) * c9, cg + (255 - cg) * c9, cb + (255 - cb) * c9
          end

          if ringShadow and lit > 0 and abs(LN) > 1e-4 then
            local t = -(nx * ayx + ny * ayy + nz * ayz) / LN
            if t > 0 then
              local qx, qy, qz = nx + t * Lx, ny + t * Ly, nz + t * Lz
              local q2 = qx * qx + qy * qy + qz * qz
              if q2 > RING_IN2 and q2 < RING_OUT2 then
                lit = lit * (1 - 0.8 * ringLut.dens[floor((sqrt(q2) - RING_IN) * RING_K) + 1])
              end
            end
          end

          local I = lit * 1.15 + 0.035
          local r_, g_, b_ = cr * I + spec + er, cg * I + spec * 0.95 + eg, cb * I + spec * 0.85 + eb

          if atm > 0 then                                  -- limb haze
            local t = 1 + nz
            local rim = t * t * sqrt(t) * atm * 0.7
            local s = smoothstep(-0.2, 0.5, d)
            r_, g_, b_ = r_ + (ar * s - r_) * rim, g_ + (ag * s - g_) * rim, b_ + (ab * s - b_) * rim
          end
          if r_ > 255 then r_ = 255 end
          if g_ > 255 then g_ = 255 end
          if b_ > 255 then b_ = 255 end

          if step > 1 then
            fillBlock(fb, W, H, px, py, step, floor(r_) * 65536 + floor(g_) * 256 + floor(b_))
          elseif r2 > edge then                            -- anti-aliased silhouette
            local cov = (1 - sqrt(r2)) * R
            if cov < 1 then
              local i = row + px
              local dd = fb[i]
              local dr, dg, db = floor(dd / 65536), floor(dd / 256) % 256, dd % 256
              r_, g_, b_ = dr + (r_ - dr) * cov, dg + (g_ - dg) * cov, db + (b_ - db) * cov
            end
          end
          if step == 1 then fb[row + px] = floor(r_) * 65536 + floor(g_) * 256 + floor(b_) end

        elseif atm > 0 then                                -- halo outside the disc
          local r = sqrt(r2)
          local hh = 1 - (r - 1) / (pad - 1)
          if hh > 0 then
            local lit = smoothstep(-0.5, 0.6, (nx * Lx + ny * Ly) / r)
            local a = atm * hh * hh * hh * hh * lit * 0.65
            if a > 0.004 and step > 1 then
              blendBlock(fb, W, H, px, py, step, ar, ag, ab, a)
            elseif a > 0.004 then
              local i = row + px
              local dd = fb[i]
              local dr, dg, db = floor(dd / 65536), floor(dd / 256) % 256, dd % 256
              fb[i] = floor(dr + (ar - dr) * a) * 65536 + floor(dg + (ag - dg) * a) * 256 + floor(db + (ab - db) * a)
            end
          end
        end
      end
    end
  end
end

--------------------------------------------------------------------------------
-- Rings: per-pixel ray/plane intersection. Draw AFTER the planet's sphere; the
-- planet occludes the far side analytically, so no front/back splitting needed.
-- o.N = ring normal (the planet's spin axis) in view space. o.step as in S:sphere.
-- o.shadow = false skips the planet's shadow across the rings.
--------------------------------------------------------------------------------

function S:ring(o)
  if not ringLut then buildRingLut() end
  local W, H, fb = self.w, self.h, self.fb
  local cx, cy, R = o.sx, o.sy, o.sr
  local Nx, Ny, Nz = o.N[1], o.N[2], o.N[3]
  if abs(Nz) < 0.015 then return end                       -- edge-on: nothing to see
  local Lx, Ly, Lz = o.L[1], o.L[2], o.L[3]
  local light = 0.6 + 0.5 * abs(Nx * Lx + Ny * Ly + Nz * Lz)
  local ex = RING_OUT * R * sqrt(max(0, 1 - Nx * Nx))      -- screen extent of the tilted disc
  local ey = RING_OUT * R * sqrt(max(0, 1 - Ny * Ny))
  local dens, lr, lg, lb = ringLut.dens, ringLut.r, ringLut.g, ringLut.b
  local invR, invNz = 1 / R, 1 / Nz
  local step = o.step or 1
  local half = step * 0.5
  local castShadow = o.shadow ~= false

  for py = max(0, floor(cy - ey)), min(H - 1, floor(cy + ey) + 1), step do
    local qy = -(py + half - cy) * invR
    local row = py * W + 1
    for px = max(0, floor(cx - ex)), min(W - 1, floor(cx + ex) + 1), step do
      local qx = (px + half - cx) * invR
      local qz = -(qx * Nx + qy * Ny) * invNz
      local r2 = qx * qx + qy * qy
      local q2 = r2 + qz * qz
      if q2 > RING_IN2 and q2 < RING_OUT2 and not (r2 < 1 and qz > -sqrt(1 - r2)) then
        local k = floor((sqrt(q2) - RING_IN) * RING_K) + 1
        local a = dens[k]
        if a > 0.01 then
          local sh = 1
          local along = qx * Lx + qy * Ly + qz * Lz
          if castShadow and along < 0 then sh = 0.06 + 0.94 * smoothstep(0.96, 1.04, sqrt(max(0, q2 - along * along))) end
          local m = light * sh
          local r_, g_, b_ = lr[k] * m, lg[k] * m, lb[k] * m
          if r_ > 255 then r_ = 255 end
          if g_ > 255 then g_ = 255 end
          if b_ > 255 then b_ = 255 end
          if step > 1 then
            blendBlock(fb, W, H, px, py, step, r_, g_, b_, a)
          else
            local i = row + px
            local dd = fb[i]
            local dr, dg, db = floor(dd / 65536), floor(dd / 256) % 256, dd % 256
            fb[i] = floor(dr + (r_ - dr) * a) * 65536 + floor(dg + (g_ - dg) * a) * 256 + floor(db + (b_ - db) * a)
          end
        end
      end
    end
  end
end

--------------------------------------------------------------------------------
-- The Sun: textured emissive disc + glow
--------------------------------------------------------------------------------

local SUN_PAD, GLOW_N = 5, 256
local glowCache = {}

local function glowLut(pad)                       -- one LUT per glow extent, built on first use
  local lut = glowCache[pad]
  if lut then return lut end
  lut = { a = {}, g = {}, b = {} }
  for i = 1, GLOW_N do
    local g = (i - 0.5) / GLOW_N * (pad - 1)
    local a = (0.85 * exp(-g * 2.4) + 0.35 / (1 + g * g * 6)) * (1 - smoothstep(1, pad, 1 + g))
    lut.a[i] = a > 1 and 1 or a
    lut.g[i] = min(255, 184 + 51 * exp(-g * 6))
    lut.b[i] = min(255, 82 + 102 * exp(-g * 6))
  end
  glowCache[pad] = lut
  return lut
end

-- o.pad: glow extent in sun radii (default 5; <= 1 draws the bare disc). o.step as in S:sphere.
function S:sun(o)
  local W, H, fb = self.w, self.h, self.fb
  local cx, cy, R = o.sx, o.sy, o.sr
  local pad = o.pad or SUN_PAD
  if pad < 1 then pad = 1 end
  local Ro = R * pad
  local axx, axy, axz = o.ax[1], o.ax[2], o.ax[3]
  local ayx, ayy, ayz = o.ay[1], o.ay[2], o.ay[3]
  local azx, azy, azz = o.az[1], o.az[2], o.az[3]
  local tex = o.tex
  local tw, th, data = 0, 0, nil
  if tex then tw, th, data = tex.w, tex.h, tex.data end
  local glowA, glowG, glowB, GK
  if pad > 1 then
    local lut = glowLut(pad)
    glowA, glowG, glowB, GK = lut.a, lut.g, lut.b, GLOW_N / (pad - 1)
  end
  local step = o.step or 1
  local half = step * 0.5

  for py = max(0, floor(cy - Ro)), min(H - 1, floor(cy + Ro) + 1), step do
    local dy = py + half - cy
    local span2 = Ro * Ro - dy * dy
    if span2 > 0 then
      local span = sqrt(span2)
      local ny = -dy / R
      local row = py * W + 1
      for px = max(0, floor(cx - span)), min(W - 1, floor(cx + span)), step do
        local nx = (px + half - cx) / R
        local r2 = nx * nx + ny * ny
        if r2 < 1 then
          local z = sqrt(1 - r2)
          local cr, cg, cb = 255, 200, 90
          if data then
            local nz = -z
            local lx = nx * axx + ny * axy + nz * axz
            local ly = nx * ayx + ny * ayy + nz * ayz
            local lz = nx * azx + ny * azy + nz * azz
            if ly > 1 then ly = 1 elseif ly < -1 then ly = -1 end
            local ty = floor(acos(ly) * INV_PI * th); if ty >= th then ty = th - 1 end
            local k = (ty * tw + floor((atan2(lz, lx) * INV_TAU + 0.5) * tw) % tw) * 3 + 1
            cr, cg, cb = byte(data, k, k + 2)
          end
          local m = (0.55 + 0.45 * z ^ 0.45) * 1.35          -- limb darkening
          local r_, g_, b_ = cr * m, cg * m, cb * m
          if r_ > 255 then r_ = 255 end
          if g_ > 255 then g_ = 255 end
          if b_ > 255 then b_ = 255 end
          local c = floor(r_) * 65536 + floor(g_) * 256 + floor(b_)
          if step > 1 then fillBlock(fb, W, H, px, py, step, c) else fb[row + px] = c end
        elseif glowA then
          local k = floor((sqrt(r2) - 1) * GK) + 1
          if k <= GLOW_N then
            local a = glowA[k]
            if a > 0.004 then
              if step > 1 then
                blendBlock(fb, W, H, px, py, step, 255, glowG[k], glowB[k], a)
              else
                local i = row + px
                local dd = fb[i]
                local dr, dg, db = floor(dd / 65536), floor(dd / 256) % 256, dd % 256
                fb[i] = floor(dr + (255 - dr) * a) * 65536 + floor(dg + (glowG[k] - dg) * a) * 256 + floor(db + (glowB[k] - db) * a)
              end
            end
          end
        end
      end
    end
  end
end

S.SUN_PAD = SUN_PAD
S.RING_OUT = RING_OUT
return S
