-- solar/app.lua — the application. Host-agnostic: see README.md for the host contract.
--
--   local app = App.new(host)      host = { width, height, read(path), now() }
--   app:update(dt)  app:draw()  -> then blit app.g.fb (w*h numbers, 0xRRGGBB)
--   app:key(name, shift)  app:pointerDown(x,y)  app:pointerMove(x,y,dx,dy)
--   app:pointerUp(x,y)    app:wheel(dy)

local Sim  = require "solar.sim"
local Soft = require "solar.soft"
local Font = require "solar.font"

local sin, cos, sqrt, pi, tan = math.sin, math.cos, math.sqrt, math.pi, math.tan
local floor, abs, min, max, exp = math.floor, math.abs, math.min, math.max, math.exp
local atan2 = math.atan2 or math.atan
local TAU, RAD = 2 * pi, pi / 180
local NEAR = 0.05
local SEG = 80                       -- moon-orbit segments

local App = {}
App.__index = App

--------------------------------------------------------------------------------
-- Graphics levels, 1 (fastest) .. 10 (everything). Each row is plain data: edit freely.
--
--   stars, belts   fraction of the star field / belt objects drawn
--   orbitStride    draw every n-th orbit vertex (220 per orbit at 1)
--   aa             anti-aliased lines (false = one pixel per step, about half the cost)
--   moonOrbits     draw the moons' orbit circles
--   step, bigStep, bigAt
--                  spheres, rings and the sun are shaded once per step x step block;
--                  bodies whose screen radius exceeds bigAt pixels use bigStep instead.
--                  This is the big lever: cost falls with the square of the step.
--   tex            surface textures (false = flat colour: no atan2/acos/fetch per pixel)
--   aux            Earth's clouds, ocean glint and city lights
--   atm            atmosphere halo + limb haze
--   ringShadow     rings cast a shadow on Saturn;  planetShadow: Saturn shadows its rings
--   glow           sun glow extent in sun radii (1 = bare disc)
--   hudAlpha       panel opacity (1 = opaque = a plain store per pixel)
--   labelShadow    drop shadow under labels (text drawn twice)
--------------------------------------------------------------------------------

local function Q(stars, belts, orbitStride, aa, moonOrbits, step, bigStep, bigAt, tex, aux, atm, ringShadow, planetShadow, glow, hudAlpha, labelShadow)
  return { stars = stars, belts = belts, orbitStride = orbitStride, aa = aa, moonOrbits = moonOrbits,
           step = step, bigStep = bigStep, bigAt = bigAt, tex = tex, aux = aux, atm = atm,
           ringShadow = ringShadow, planetShadow = planetShadow, glow = glow, hudAlpha = hudAlpha, labelShadow = labelShadow }
end
local Y, N = true, false
App.QUALITY = {
  --  stars belts strd aa  mOrb step big  bigAt tex aux atm rSh pSh glow hud   lblSh
  Q(  0.15, 0.00,  5,  N,  N,   3,   3,    0,   N,  N,  N,  N,  N,  1.0, 1,    N ),   --  1
  Q(  0.25, 0.00,  4,  N,  N,   2,   3,  120,   N,  N,  N,  N,  N,  2.0, 1,    N ),   --  2
  Q(  0.35, 0.25,  4,  N,  N,   2,   3,  120,   Y,  N,  N,  N,  N,  2.5, 1,    N ),   --  3
  Q(  0.50, 0.40,  2,  N,  Y,   2,   2,    0,   Y,  N,  N,  N,  Y,  3.0, 1,    N ),   --  4
  Q(  0.60, 0.50,  2,  Y,  Y,   2,   2,    0,   Y,  N,  Y,  N,  Y,  3.5, 1,    N ),   --  5
  Q(  0.70, 0.60,  2,  Y,  Y,   1,   2,   60,   Y,  Y,  Y,  N,  Y,  4.0, 1,    N ),   --  6
  Q(  0.85, 0.80,  1,  Y,  Y,   1,   2,   80,   Y,  Y,  Y,  Y,  Y,  5.0, 1,    Y ),   --  7
  Q(  1.00, 1.00,  1,  Y,  Y,   1,   2,  140,   Y,  Y,  Y,  Y,  Y,  5.0, 1,    Y ),   --  8
  Q(  1.00, 1.00,  1,  Y,  Y,   1,   1,    0,   Y,  Y,  Y,  Y,  Y,  5.0, 1,    Y ),   --  9
  Q(  1.00, 1.00,  1,  Y,  Y,   1,   1,    0,   Y,  Y,  Y,  Y,  Y,  5.0, 0.74, Y ),   -- 10
}

local function rgb255(c) return { floor(c[1] * 255), floor(c[2] * 255), floor(c[3] * 255) } end

function App.new(host)
  local self = setmetatable({}, App)
  self.host = host
  self.sim = Sim.new({ now = host.now and host.now() or nil, stars = host.stars, belts = host.belts })
  self:resize(host.width, host.height, host.fb)
  self.cam = { yaw = 0.7, pitch = 0.45, dist = 95, distGoal = 95, fov = 50 * RAD,
               tx = 0, ty = 0, tz = 0, ox = 0, oy = 0, oz = 0 }
  self.showOrbits, self.showLabels, self.showHelp = true, true, true
  self.drag = { active = false, moved = 0 }
  self.mx, self.my = -1, -1
  self.focus = self.sim.sun
  self.drawList = {}
  self.quit = false
  self.auto = host.autoQuality or false     -- adapt the level to the measured frame time
  self.avgDt, self.autoWait, self.autoCeil, self.ceilWait = 1 / 30, 0, 10, 0
  self:setQuality(host.quality or 6)

  -- Textures are optional: a body without one is drawn flat-shaded in its base colour,
  -- so the port can come up before the OS has a filesystem.
  local dir = host.assets or "assets/"
  local function tex(name)
    if not host.read then return nil end
    local ok, s = pcall(host.read, dir .. name .. ".ppm")
    return ok and Soft.parsePPM(s) or nil
  end
  for _, b in ipairs(self.sim.bodies) do
    b.tex = tex(b.id)
    b.color255 = rgb255(b.c1)
    b.atm255 = rgb255(b.atmColor)
    if b.kind == 1 then b.aux = tex(b.id .. "_aux") end
  end

  self.RC, self.RS = {}, {}
  for j = 0, SEG do self.RC[j], self.RS[j] = cos(j / SEG * TAU), sin(j / SEG * TAU) end
  return self
end

--------------------------------------------------------------------------------
-- Update / input
--------------------------------------------------------------------------------

-- Call whenever the render size changes. fb: optional host pixel buffer (see Soft.new).
-- The UI scale follows the height so the HUD keeps its proportions from 540p to 4K;
-- host.uiScale (1, 2 or 3) overrides it.
function App:resize(w, h, fb)
  w, h = floor(w), floor(h)
  self.g = Soft.new(w, h, fb)
  local ui = self.host.uiScale or (h < 900 and 1 or h < 1700 and 2 or 3)
  self.ui = max(1, min(3, floor(ui)))
  self.fontS, self.fontL = Font.get(12 * self.ui), Font.get(24 * self.ui)
end

function App:setQuality(n)
  n = max(1, min(#App.QUALITY, floor(n + 0.5)))
  self.quality, self.q = n, App.QUALITY[n]
end

-- Auto level: driven by the dt the host already passes to update(), so no extra host API.
-- Drops a level above ~24 fps worth of frame time, climbs back under ~45 fps worth. After a
-- drop, the level it fell from stays off-limits for 20 s so it cannot oscillate.
function App:autoTune(dt)
  self.avgDt = self.avgDt * 0.9 + dt * 0.1
  self.autoWait = self.autoWait - dt
  self.ceilWait = self.ceilWait - dt
  if self.ceilWait <= 0 then self.autoCeil = #App.QUALITY end
  if self.autoWait > 0 then return end
  if self.avgDt > 1 / 24 and self.quality > 1 then
    self.autoCeil, self.ceilWait = self.quality - 1, 20
    self:setQuality(self.quality - 1)
    self.autoWait, self.avgDt = 1.0, 1 / 30
  elseif self.avgDt < 1 / 45 and self.quality < self.autoCeil then
    self:setQuality(self.quality + 1)
    self.autoWait, self.avgDt = 1.5, 1 / 30
  end
end

function App:setFocus(b)
  if self.focus == b then return end
  local cam, sim = self.cam, self.sim
  self.focus = b
  cam.ox, cam.oy, cam.oz = cam.tx - b.x, cam.ty - b.y, cam.tz - b.z
  cam.distGoal = b.star and (sim.scaleGoal == Sim.P_REAL and 420 or 95) or max(b.r * 7, 1.2)
  if not b.star then                       -- swing round to the lit side
    cam.yawGoal = atan2(-b.x, -b.z) + 0.65
    cam.pitchGoal = 0.22
  end
end

function App:pick(mx, my)
  local best, bestZ
  for _, b in ipairs(self.sim.bodies) do
    if b.visible then
      local dx, dy = mx - b.sx, my - b.sy
      local rr = max(b.sr, 11 * self.ui)
      if dx * dx + dy * dy < rr * rr and (not bestZ or b.vz < bestZ) then best, bestZ = b, b.vz end
    end
  end
  return best
end

function App:update(dt)
  if self.auto then self:autoTune(dt) end
  dt = min(dt, 0.1)
  local cam, f = self.cam, self.focus
  self.sim:update(dt)
  local k = exp(-dt * 5)
  cam.ox, cam.oy, cam.oz = cam.ox * k, cam.oy * k, cam.oz * k
  cam.tx, cam.ty, cam.tz = f.x + cam.ox, f.y + cam.oy, f.z + cam.oz
  cam.dist = cam.dist * (cam.distGoal / cam.dist) ^ (1 - exp(-dt * 6))
  if cam.yawGoal then
    local d = (cam.yawGoal - cam.yaw + pi) % TAU - pi
    local t = 1 - exp(-dt * 4)
    cam.yaw = cam.yaw + d * t
    cam.pitch = cam.pitch + (cam.pitchGoal - cam.pitch) * t
    if abs(d) < 0.002 then cam.yawGoal = nil end
  end
  if not self.drag.active then self.hovered = self:pick(self.mx, self.my) end
end

function App:pointerDown(x, y) self.drag.active, self.drag.moved = true, 0; self.mx, self.my = x, y end

function App:pointerMove(x, y, dx, dy)
  self.mx, self.my = x, y
  if not self.drag.active then return end
  local cam = self.cam
  self.drag.moved = self.drag.moved + abs(dx) + abs(dy)
  cam.yawGoal = nil
  local k = 0.006 * 540 / self.g.h            -- same feel at any render resolution
  cam.yaw = cam.yaw + dx * k
  cam.pitch = max(-1.55, min(1.55, cam.pitch + dy * k))
end

function App:pointerUp(x, y)
  if self.drag.active and self.drag.moved < 5 * self.ui then
    local b = self:pick(x, y)
    if b then self:setFocus(b) end
  end
  self.drag.active = false
end

function App:wheel(dy)
  local cam = self.cam
  cam.distGoal = max(self.focus.r * 1.6, min(2500, cam.distGoal * 0.86 ^ dy))
end

-- key names: "space" "left" "right" "tab" "escape" "/" "0".."9" and single letters
function App:key(key, shift)
  local sim, cam = self.sim, self.cam
  if key == "escape" then self.quit = true
  elseif key == "space" then sim.paused = not sim.paused
  elseif key == "right" or key == "." then sim.speedIdx = min(#Sim.speeds, sim.speedIdx + 1)
  elseif key == "left" or key == "," then sim.speedIdx = max(1, sim.speedIdx - 1)
  elseif key == "/" or key == "backspace" then sim.dir = -sim.dir
  elseif key == "n" then sim.days = sim.epoch
  elseif key == "r" then
    sim.scaleGoal = (sim.scaleGoal == Sim.P_REAL) and Sim.P_COMPRESS or Sim.P_REAL
    if self.focus.star then cam.distGoal = sim.scaleGoal == Sim.P_REAL and 420 or 95 end
  elseif key == "o" then self.showOrbits = not self.showOrbits
  elseif key == "l" then self.showLabels = not self.showLabels
  elseif key == "h" then self.showHelp = not self.showHelp
  elseif key == "]" then self.auto = false; self:setQuality(self.quality + 1)
  elseif key == "[" then self.auto = false; self:setQuality(self.quality - 1)
  elseif key == "a" then self.auto = not self.auto
  elseif key == "up" or key == "=" then self:wheel(1)       -- zoom without a wheel
  elseif key == "down" or key == "-" then self:wheel(-1)
  elseif key == "tab" then
    local bodies = sim.bodies
    for i, b in ipairs(bodies) do
      if b == self.focus then self:setFocus(bodies[(i - 1 + (shift and -1 or 1)) % #bodies + 1]); break end
    end
  elseif key == "0" then self:setFocus(sim.sun)
  elseif tonumber(key) and sim.planets[tonumber(key)] then self:setFocus(sim.planets[tonumber(key)])
  end
end

--------------------------------------------------------------------------------
-- Draw
--------------------------------------------------------------------------------

local function fmtPeriod(d)
  d = abs(d)
  if d < 2 then return string.format("%.1f h", d * 24) end
  if d < 1000 then return string.format("%.1f days", d) end
  return string.format("%.2f years", d / 365.25)
end

local function fmtKm(n)
  local s = string.format("%d", floor(n + 0.5))
  local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
  return (out:gsub("^,", ""))
end

function App:draw()
  local g, sim, cam = self.g, self.sim, self.cam
  local W, H = g.w, g.h
  local CX, CY = W / 2, H / 2
  local F = (H / 2) / tan(cam.fov / 2)
  local focus, hovered = self.focus, self.hovered
  local q = self.q
  local drawLine = q.aa and g.line or g.lineFast

  -- camera basis: view space is x right, y up, z forward
  local cp, sp, sy, cy = cos(cam.pitch), sin(cam.pitch), sin(cam.yaw), cos(cam.yaw)
  local ex, ey, ez = cam.tx + cam.dist * cp * sy, cam.ty + cam.dist * sp, cam.tz + cam.dist * cp * cy
  local fx, fy, fz = -cp * sy, -sp, -cp * cy
  local l = sqrt(fz * fz + fx * fx)
  local rx, ry, rz = fz / l, 0, -fx / l
  local ux, uy, uz = fy * rz - fz * ry, fz * rx - fx * rz, fx * ry - fy * rx

  local function rotView(x, y, z)
    return x * rx + y * ry + z * rz, x * ux + y * uy + z * uz, x * fx + y * fy + z * fz
  end
  local function toView(x, y, z)
    return rotView(x - ex, y - ey, z - ez)
  end
  local function line3(x1, y1, z1, x2, y2, z2, r, gg, b, a)      -- view-space segment, near-clipped
    if z1 < NEAR and z2 < NEAR then return end
    if z1 < NEAR then
      local t = (NEAR - z1) / (z2 - z1)
      x1, y1, z1 = x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, NEAR
    elseif z2 < NEAR then
      local t = (NEAR - z2) / (z1 - z2)
      x2, y2, z2 = x2 + (x1 - x2) * t, y2 + (y1 - y2) * t, NEAR
    end
    local s1, s2 = F / z1, F / z2
    drawLine(g, CX + x1 * s1, CY - y1 * s1, CX + x2 * s2, CY - y2 * s2, r, gg, b, a)
  end

  g:clear(66052)                                                 -- 0x010204

  -- stars (directions only: infinitely far)
  for _, s in ipairs(sim.stars) do
    local dir, col, size = s.dir, s.col, min(3, s.size + self.ui - 1)
    local inv = 1 / q.stars                                   -- stride sampling keeps the mix of
    for j = 0, floor(s.n * q.stars) - 1 do                    -- field and Milky Way stars uniform
      local i = floor(j * inv)
      local vx, vy, vz = rotView(dir[i * 3 + 1], dir[i * 3 + 2], dir[i * 3 + 3])
      if vz > 0.05 then
        local k = F / vz
        local x, y = floor(CX + vx * k), floor(CY - vy * k)
        if x >= 0 and y >= 0 and x < W and y < H then
          g:point(x, y, size, col[i * 4 + 1], col[i * 4 + 2], col[i * 4 + 3], col[i * 4 + 4])
        end
      end
    end
  end

  -- project bodies
  local list = self.drawList
  for i = #list, 1, -1 do list[i] = nil end
  for _, b in ipairs(sim.bodies) do
    b.vx, b.vy, b.vz = toView(b.x, b.y, b.z)
    b.visible = b.vz > NEAR + b.r
    if b.visible then
      local k = F / b.vz
      b.sx, b.sy, b.sr = CX + b.vx * k, CY - b.vy * k, b.r * k
      list[#list + 1] = b
    end
  end
  table.sort(list, function(a, b) return a.vz > b.vz end)

  -- orbits
  if self.showOrbits then
    local N = Sim.ORBIT_N
    for _, b in ipairs(sim.planets) do
      local path = b.path
      local hot = (b == focus or b == hovered or focus.parentBody == b)
      local base = hot and 0.95 or 0.55
      local c = b.c2
      local cr, cg, cb = 255 * (0.35 + 0.65 * c[1]), 255 * (0.35 + 0.65 * c[2]), 255 * (0.35 + 0.65 * c[3])
      local pvx, pvy, pvz
      local stride = q.orbitStride
      for i = 0, N, stride do
        local k = (i % N) * 3
        local vx, vy, vz = toView(sim:toDisplay(path[k + 1], path[k + 2], path[k + 3]))
        if i > 0 then
          local phase = ((b.E - (i - 0.5 * stride) / N * TAU) % TAU) / TAU   -- 0 = right behind the planet
          line3(pvx, pvy, pvz, vx, vy, vz, cr, cg, cb, base * (0.10 + 0.90 * (1 - phase) ^ 2.4))
        end
        pvx, pvy, pvz = vx, vy, vz
      end
    end
    local RC, RS = self.RC, self.RS
    for _, m in ipairs(sim.moons) do
      local p = m.parentBody
      if q.moonOrbits and p.visible and p.sr >= 5 then
        local a = (m == focus or m == hovered) and 0.5 or 0.16
        local pvx, pvy, pvz
        for j = 0, SEG do
          local c, s = RC[j] * m.dist, RS[j] * m.dist
          local x, y, z
          if m.ecl then x, y, z = p.x + c, p.y, p.z + s
          else x, y, z = p.x + c * p.e1x + s * p.e2x, p.y + c * p.e1y + s * p.e2y, p.z + c * p.e1z + s * p.e2z end
          local vx, vy, vz = toView(x, y, z)
          if j > 0 then line3(pvx, pvy, pvz, vx, vy, vz, 178, 191, 217, a) end
          pvx, pvy, pvz = vx, vy, vz
        end
      end
    end
  end

  -- asteroid + Kuiper belts
  for _, belt in ipairs(sim.belts) do
    local br, bg, bb, ba = belt.r, belt.g, belt.b, belt.a
    local items = belt.items
    local inv = q.belts > 0 and 1 / q.belts or 0
    for j = 0, floor(belt.n * q.belts) - 1 do
      local it = items[floor(j * inv) + 1]
      local th = it.phase + it.n * sim.days
      local a = it.a
      local vx, vy, vz = toView(sim:toDisplay(a * cos(th), a * it.si * sin(th + it.node), a * sin(th)))
      if vz > NEAR then
        local k = F / vz
        local x, y = floor(CX + vx * k), floor(CY - vy * k)
        if x >= 0 and y >= 0 and x < W and y < H then
          local s = it.shade
          g:blend(x, y, br * s, bg * s, bb * s, ba)
        end
      end
    end
  end

  -- bodies, far to near
  local sun = sim.sun
  for _, b in ipairs(list) do
    local sx, sy_, sr = b.sx, b.sy, b.sr
    local reach = sr * (b.star and q.glow or (b.rings and Soft.RING_OUT or 1.35))
    local step = (sr > q.bigAt) and q.bigStep or q.step
    if not (sx + reach < 0 or sx - reach > W or sy_ + reach < 0 or sy_ - reach > H) then
      if sr < 1.4 and not b.star then
        local c = b.color255
        g:point(floor(sx), floor(sy_), min(3, 1 + self.ui), 100 + 0.6 * c[1], 100 + 0.6 * c[2], 100 + 0.6 * c[3], 1)
      else
        local c, s = cos(b.spin), sin(b.spin)
        local A = { rotView(b.e1x * c + b.e2x * s, b.e1y * c + b.e2y * s, b.e1z * c + b.e2z * s) }
        local B = { rotView(b.upx, b.upy, b.upz) }
        local C = { rotView(-b.e1x * s + b.e2x * c, -b.e1y * s + b.e2y * c, -b.e1z * s + b.e2z * c) }
        if b.star then
          g:sun({ sx = sx, sy = sy_, sr = sr, ax = A, ay = B, az = C, tex = q.tex and b.tex or nil,
                  pad = q.glow, step = step })
        else
          local lx, ly, lz = sun.vx - b.vx, sun.vy - b.vy, sun.vz - b.vz
          local ll = sqrt(lx * lx + ly * ly + lz * lz)
          local L = { lx / ll, ly / ll, lz / ll }
          g:sphere({ sx = sx, sy = sy_, sr = sr, ax = A, ay = B, az = C, L = L,
                     tex = q.tex and b.tex or nil, aux = q.aux and b.aux or nil,
                     cloudU = (sim.animT * 0.004) % 1, color = b.color255,
                     atm = q.atm and b.atm or 0, atmColor = b.atm255,
                     ringShadow = q.ringShadow and b.rings, step = step })
          if b.rings and sr > 2 then
            g:ring({ sx = sx, sy = sy_, sr = sr, N = B, L = L, step = step, shadow = q.planetShadow })
          end
        end
      end
    end
  end

  -- labels + hover marker
  local fs, u = self.fontS, self.ui
  for _, b in ipairs(list) do
    local show = self.showLabels
    local p = b.parentBody
    if p then
      show = show and p.visible and p.sr > 6 * u and ((b.sx - p.sx) ^ 2 + (b.sy - p.sy) ^ 2 > (p.sr + 16 * u) ^ 2)
    end
    if b == hovered and b ~= focus then
      g:circle(b.sx, b.sy, max(b.sr, 3 * u) + 6 * u, 153, 204, 255, 0.7)
      show = true
    end
    if show then
      local a = (b == focus or b == hovered) and 1 or 0.7
      local off = max(b.sr, 2) * 0.75
      if q.labelShadow then g:text(fs, b.name, b.sx + off + 8 * u, b.sy - off - 13 * u, 0, 0, 0, 0.6 * a) end
      g:text(fs, b.name, b.sx + off + 7 * u, b.sy - off - 14 * u, 217, 230, 255, a)
    end
  end

  self:drawHUD()
end

function App:drawHUD()
  local g, sim = self.g, self.sim
  local W, H = g.w, g.h
  local fs, fl, u = self.fontS, self.fontL, self.ui
  local alpha = self.q.hudAlpha
  -- Layout is authored at 1x (540p) and multiplied by the UI scale.
  local function panel(x, y, w, h)
    g:rect(x, y, w, h, 8, 13, 23, alpha)
    g:frame(x, y, w, h, 115, 153, 217, 0.25)
  end
  local function text(font, str, x, y, r, gg, b, a, align) g:text(font, str, x, y, r, gg, b, a, align) end

  -- date / speed / graphics
  local yy, mo, dd, hh, mi = Sim.calendar(sim.days)
  panel(12 * u, 12 * u, 290 * u, 88 * u)
  text(fl, string.format("%04d-%02d-%02d", yy, mo, dd), 24 * u, 17 * u, 235, 242, 255, 1)
  text(fs, string.format("%02d:%02d UTC", hh, mi), 292 * u, 28 * u, 153, 184, 230, 1, "right")
  if sim.paused then
    text(fs, "paused", 24 * u, 56 * u, 255, 178, 89, 1)
  else
    text(fs, (sim.dir < 0 and "<< " or ">> ") .. Sim.speeds[sim.speedIdx][1], 24 * u, 56 * u, 140, 230, 178, 1)
  end
  text(fs, sim.scaleGoal == Sim.P_REAL and "true distances" or "compressed", 292 * u, 56 * u, 128, 148, 178, 1, "right")
  local gfx = string.format("gfx %d/10%s", self.quality, self.auto and string.format(" auto %.0fms", self.avgDt * 1000) or "")
  text(fs, gfx, 24 * u, 74 * u, 128, 148, 178, 1)
  text(fs, string.format("%dx%d", W, H), 292 * u, 74 * u, 128, 148, 178, 1, "right")

  -- focused body
  local b = self.focus
  local lines = { { "Radius", fmtKm(b.radiusKm) .. " km" } }
  if b.year then lines[#lines + 1] = { "Orbital period", fmtPeriod(b.year) } end
  if b.period then lines[#lines + 1] = { "Orbital period", fmtPeriod(b.period) .. (b.period < 0 and " retro" or "") } end
  if b.rot then lines[#lines + 1] = { "Rotation", fmtPeriod(b.rot) } end
  if b.tilt then lines[#lines + 1] = { "Axial tilt", string.format("%.1f deg", b.tilt) } end
  if b.au then lines[#lines + 1] = { "Sun distance", string.format("%.3f AU", b.au) } end
  if b.e then lines[#lines + 1] = { "Eccentricity", string.format("%.4f", b.e) } end
  local pw = 300 * u
  local descLines = {}
  if b.desc then                                          -- word wrap
    local cols, cur = floor((pw - 28 * u) / fs.cw), ""
    for word in b.desc:gmatch("%S+") do
      if #cur + #word + 1 > cols and #cur > 0 then descLines[#descLines + 1] = cur; cur = word
      else cur = (#cur > 0) and (cur .. " " .. word) or word end
    end
    if #cur > 0 then descLines[#descLines + 1] = cur end
  end
  local ph = (72 + #lines * 17 + (#descLines > 0 and (#descLines * 16 + 10) or 0)) * u
  local x0, y0 = W - pw - 12 * u, 12 * u
  panel(x0, y0, pw, ph)
  text(fl, b.name, x0 + 14 * u, y0 + 5 * u, 242, 247, 255, 1)
  text(fs, string.upper(b.class or ""), x0 + 14 * u, y0 + 38 * u, 140, 173, 230, 1)
  local y = y0 + 62 * u
  for _, ln in ipairs(lines) do
    text(fs, ln[1], x0 + 14 * u, y, 140, 158, 189, 1)
    text(fs, ln[2], x0 + pw - 14 * u, y, 230, 237, 255, 1, "right")
    y = y + 17 * u
  end
  y = y + 6 * u
  for _, s in ipairs(descLines) do
    text(fs, s, x0 + 14 * u, y, 184, 199, 224, 0.9)
    y = y + 16 * u
  end

  if self.showHelp then
    local help = {
      { "drag", "orbit camera" }, { "wheel / up down", "zoom" }, { "click", "focus body" },
      { "0-9 / tab", "Sun, planets / cycle" }, { "space", "pause" }, { "left right", "time speed" },
      { "/", "reverse time" }, { "N", "jump to now" }, { "R", "true / compressed scale" },
      { "O  L", "orbits / labels" }, { "[  ]", "graphics level 1-10" }, { "A", "auto graphics level" },
    }
    for _, ln in ipairs(self.host.help or {}) do help[#help + 1] = ln end      -- host-specific keys
    help[#help + 1] = { "H", "hide help" }
    local hh2 = (#help * 16 + 16) * u
    panel(12 * u, H - hh2 - 12 * u, 330 * u, hh2)
    for i, ln in ipairs(help) do
      local yy2 = H - hh2 - 12 * u + (8 + (i - 1) * 16) * u
      text(fs, ln[1], 24 * u, yy2, 153, 199, 255, 1)
      text(fs, ln[2], 150 * u, yy2, 184, 194, 214, 1)
    end
  end
end

return App
