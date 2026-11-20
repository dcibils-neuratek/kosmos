-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Flight: a light trainer over a real city, its hills from elevation tiles
-- and its streets from satellite imagery, drawn by Vulkan (`docs/flight.md`).
--
-- kosmos: application
-- kosmos: name Flight
-- kosmos: icon App_Generic
-- kosmos: section applications
-- kosmos: needs tiles
-- kosmos: image k3dvk.elf
--
-- What is here is the application's own and nothing else (`CLAUDE.md`,
-- *Kits supply, applications orchestrate*): the flight model, the camera,
-- the keys, and which tiles to ask the `tiles` server for. The terrain, its
-- chunks, its builder thread and every pixel are the 3D Kit's, through the
-- door `k3dvk.elf` (`user/kits/3d/k3d_vk_kosmos.c`); the tiles are fetched
-- and cached by `tiles` (`tiles.lua`), as Maps' are.
--
-- **Nothing a frame allocates that it can help**: the view, the flight
-- state and the list of tiles to ask for are made once and reused, so the
-- collector stays out of the frame (`CLAUDE.md`, *when a Lua process is on
-- a deadline, ask what it allocates*).
--
-- Keys (evdev's numbers, as the window's `rawkey` events carry them):
-- arrows pitch and roll, A and Z the throttle, F the flaps, V the view,
-- P pause, R start again.

local k = use("k3dvk.elf")
local tiles = use("/Kosmos/Libraries/tiles.lua")
local prefs = use("/Kosmos/Libraries/prefs.lua")

local settings = prefs.open("flight", {
  lon = -56.1650, lat = -34.9060,                -- over Montevideo's bay
  imagery = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
  imagery_format = "jpg",
  elevation = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png",
  width = 1280, height = 720,
  model = "/Home/Apps/Flight/cessna_172.glb",    -- the aircraft (assets/models/cessna172)
  graphics = true,                               -- the graphics panel open (F3)
  frames = 0,                                    -- stop after this many, 0 never
  report = 120,                                  -- a line on the console every this many
})

local KEY = { up = 103, down = 108, left = 105, right = 106, a = 30, z = 44, f = 33,
              v = 47, p = 25, r = 19, f3 = 61 }

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local function now() return sys.ticks() / counter_hz end

local function say(s) print("flight " .. s) end

-- The aircraft: a Cessna 172's numbers, near enough to fly like one.
local PLANE = {
  mass = 1043, wing = 16.2,                      -- kg, m^2
  cl0 = 0.30, cla = 5.0, stall = math.rad(15),   -- lift: CL0 + CLa * alpha
  cd0 = 0.030, k = 0.050,                        -- drag: CD0 + k CL^2
  flaps_cl = 0.45, flaps_cd = 0.030,
  power = 96000, static = 2600,                  -- W at the propeller, N standing
  roll_rate = math.rad(70), alpha_most = math.rad(12),
  gear = 1.4,                                    -- the eye's height on its wheels
}
local G = 9.81

-- The state, one table for the whole flight.
local S = {}
local function start()
  S.x, S.y, S.z = 0, 600, 0          -- metres east, up, south of the origin
  S.v = 55                           -- airspeed, m/s
  S.gamma = 0                        -- the path's climb, radians
  S.psi = 0                          -- heading: 0 north, clockwise
  S.phi = 0                          -- roll
  S.alpha = 0.02                     -- angle of attack
  S.throttle = 0.7
  S.flaps = 0
  S.ground = false
  S.crashed = false
  S.view = 1                         -- 1 chase, 2 cockpit, 3 tower
  S.paused = false
end
start()

-- One step of the flight model, `dt` seconds: speed, climb and heading from
-- the forces in the path's frame, as a point mass with an attitude - which
-- is how a trainer feels, and a few dozen multiplications.
local function fly(dt, pitch, roll, ground_h)
  local P = PLANE
  local rho = 1.225 * math.exp(-S.y / 8500)
  local q = 0.5 * rho * S.v * S.v

  -- The yoke sets the angle of attack it asks for; the aircraft gets there
  -- quickly but not at once, as a real one's pitch does.
  local trim = 0.03 + 0.04 * S.flaps
  local want = trim + pitch * P.alpha_most
  S.alpha = S.alpha + (want - S.alpha) * math.min(1, 3 * dt)

  local cl = P.cl0 + P.cla * S.alpha + P.flaps_cl * S.flaps
  if S.alpha > P.stall then cl = cl * 0.55 end   -- stalled
  local cd = P.cd0 + P.k * cl * cl + P.flaps_cd * S.flaps
  local lift, drag = q * P.wing * cl, q * P.wing * cd
  local thrust = S.throttle * math.min(P.static, P.power / math.max(S.v, 1))
  local m = P.mass

  if S.ground then
    -- Rolling: no bank, the wheels' friction, and off when lift is enough.
    S.phi = 0
    local friction = 0.04 * math.max(0, m * G - lift)
    S.v = math.max(0, S.v + (thrust - drag - friction) / m * dt)
    S.gamma = 0
    if lift > m * G * 1.02 and pitch > 0 then S.ground = false end
    S.psi = S.psi + roll * math.rad(10) * dt * math.min(1, S.v / 10)
  else
    S.phi = S.phi + roll * P.roll_rate * dt
    -- Let go of the ailerons and a trainer rolls back towards level.
    if roll == 0 then S.phi = S.phi * (1 - 0.4 * dt) end
    S.phi = math.max(-math.rad(75), math.min(math.rad(75), S.phi))
    local v = math.max(S.v, 5)
    S.v = S.v + ((thrust * math.cos(S.alpha) - drag) / m - G * math.sin(S.gamma)) * dt
    S.v = math.max(S.v, 0)
    S.gamma = S.gamma + ((lift * math.cos(S.phi) + thrust * math.sin(S.alpha))
                         / (m * v) - G * math.cos(S.gamma) / v) * dt
    S.gamma = math.max(-1.4, math.min(1.4, S.gamma))
    S.psi = S.psi + lift * math.sin(S.phi) / (m * v * math.cos(S.gamma)) * dt
  end

  local horizontal = S.v * math.cos(S.gamma)
  S.x = S.x + horizontal * math.sin(S.psi) * dt
  S.z = S.z - horizontal * math.cos(S.psi) * dt
  S.y = S.y + S.v * math.sin(S.gamma) * dt

  -- The ground: touched gently it is a landing, hard it is the end.
  if ground_h and S.y < ground_h + P.gear then
    local sink = -S.v * math.sin(S.gamma)
    if not S.ground and (sink > 4 or math.abs(S.phi) > math.rad(20)) then
      S.crashed = true
    end
    S.y = ground_h + P.gear
    S.ground = true
  end
end

-- The camera, written into one view table a frame.
local view = {
  eye = { 0, 0, 0 }, target = { 0, 0, 0 }, up = { 0, 1, 0 },
  focal = 1.4, near = 0.5, far = 60000,
  sun = { 0.3, 1.0, 0.2 }, ambient = 0.35,
  zenith = { 0.22, 0.42, 0.80 }, horizon = { 0.78, 0.84, 0.92 },
  haze = { 0.72, 0.78, 0.86 }, haze_start = 4000, haze_end = 40000,
}

local function look()
  local ch, sh = math.cos(S.psi), math.sin(S.psi)
  local theta = S.gamma + S.alpha
  local fx, fy, fz = sh * math.cos(theta), math.sin(theta), -ch * math.cos(theta)
  local e, t, u = view.eye, view.target, view.up

  if S.view == 2 then
    -- The cockpit: the pilot's eye, rolling with the wings.
    e[1], e[2], e[3] = S.x, S.y + 1.0, S.z
    t[1], t[2], t[3] = S.x + fx * 100, S.y + 1.0 + fy * 100, S.z + fz * 100
    -- Up: the world's, leaning towards the lower wing as it banks - right
    -- for a right bank, which turns it right.
    local c, s = math.cos(S.phi), math.sin(S.phi)
    u[1], u[2], u[3] = s * ch, c, s * sh
  elseif S.view == 3 then
    -- The tower: where the flight began, following it.
    e[1], e[2], e[3] = 0, (S.ground_at_start or 0) + 30, 0
    t[1], t[2], t[3] = S.x, S.y, S.z
    u[1], u[2], u[3] = 0, 1, 0
  else
    -- Chase: behind and a little above, along the heading, level.
    e[1], e[2], e[3] = S.x - sh * 28, S.y + 7, S.z + ch * 28
    t[1], t[2], t[3] = S.x + sh * 40, S.y + fy * 40, S.z - ch * 40
    u[1], u[2], u[3] = 0, 1, 0
  end
end

-- **The aircraft where it is**: the model's frame is glTF's - its nose
-- along +X, up +Y, the right wing +Z, metres, the wheels at 0 - so the
-- world from it has the aircraft's forward, up and right as its columns,
-- and its wheels' place as the fourth. One table, refilled a frame.
local placed = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }
local SURFACE = { elevator = 1, flaps = 2, right = 3, left = 4, rudder = 5, prop = 6 }
local MOVING = { "elevator_0", "flaps_1", "aileron-right_2", "aileron-left_3", "rudder_4",
                 "prop_5" }

local function place(plane, pitch, roll, dt)
  local ch, sh = math.cos(S.psi), math.sin(S.psi)
  local theta = S.gamma + S.alpha
  local ct, st = math.cos(theta), math.sin(theta)
  local cp, sp = math.cos(S.phi), math.sin(S.phi)
  -- Forward, and up before the roll; then the roll about forward.
  local fx, fy, fz = sh * ct, st, -ch * ct
  local ux, uy, uz = -sh * st, ct, ch * st
  local rx, rz = ch, sh                                  -- right, level
  local Ux, Uy, Uz = ux * cp + rx * sp, uy * cp, uz * cp + rz * sp
  local Rx, Ry, Rz = rx * cp - ux * sp, -uy * sp, rz * cp - uz * sp
  placed[1], placed[2], placed[3] = fx, fy, fz
  placed[5], placed[6], placed[7] = Ux, Uy, Uz
  placed[9], placed[10], placed[11] = Rx, Ry, Rz
  placed[13], placed[14], placed[15] = S.x, S.y - PLANE.gear, S.z
  plane:place(placed)
  -- The surfaces follow the yoke, and the propeller the throttle.
  plane:turn(SURFACE.elevator, math.rad(20) * pitch)
  plane:turn(SURFACE.right, math.rad(15) * roll)
  plane:turn(SURFACE.left, -math.rad(15) * roll)
  plane:turn(SURFACE.flaps, -math.rad(30) * S.flaps)
  S.prop = ((S.prop or 0) + (6 + 34 * S.throttle) * 2 * math.pi * dt) % (2 * math.pi)
  plane:spin(SURFACE.prop, S.prop)
end

-- The tiles: what the terrain still wants, asked of `tiles` and handed to
-- the terrain as each arrives - from the cache at once if it is there.
local net = {}

local function sources()
  local why
  net.imagery, why = tiles.source(settings.imagery, settings.imagery_format)
  if not net.imagery then return nil, why end
  net.elevation, why = tiles.source(settings.elevation, "png")
  if not net.elevation then return nil, why end
  return true
end

local decoders = { png = gfx.png, jpg = gfx.jpeg }

-- A tile there is none of - the sea past the last tile, a server's 404 -
-- is an empty file: flat at sea level, and the sea's colour.
local SEA_LEVEL = gfx.surface{ w = 2, h = 2 }
SEA_LEVEL:fill(0, 0, 2, 2, 0xFF800000)              -- terrarium's 0 m
local SEA = gfx.surface{ w = 2, h = 2 }
SEA:fill(0, 0, 2, 2, 0xFF1E3C5A)

local handed = 0

local function hand(g, kind, z, x, y)
  local src = kind == "e" and net.elevation or net.imagery
  local bytes = fs.read(src:path(z, x, y))
  if not bytes then return false end
  local picture
  if #bytes == 0 then
    picture = kind == "e" and SEA_LEVEL or SEA
  else
    local ok, made = pcall(decoders[src.format], bytes)
    if not ok or not made then picture = kind == "e" and SEA_LEVEL or SEA else picture = made end
  end
  if kind == "e" then g:elevation(z, x, y, picture) else g:imagery(z, x, y, picture) end
  handed = handed + 1
  return true
end

local asked = { e = {}, i = {} }        -- flat lists of z, x, y, reused
local asking = { e = 0, i = 0 }
local polled = 0
-- Tiles looked for in the cache and not there: not looked for again until
-- the server says one has come, so a frame does not ask the disk for every
-- tile still on its way.
local missing = { e = {}, i = {} }
local function key(z, x, y) return (z << 50) | (x << 25) | y end

local function feed(g, eye)
  asking.e, asking.i = 0, 0
  for _, t in ipairs(g:wanted(eye)) do
    local kind = t[1]
    local id = key(t[2], t[3], t[4])
    local gone = missing[kind]
    if (gone[id] or not hand(g, kind, t[2], t[3], t[4])) and asking[kind] < tiles.MAX then
      gone[id] = true
      local list, n = asked[kind], asking[kind] * 3
      list[n + 1], list[n + 2], list[n + 3] = t[2], t[3], t[4]
      asking[kind] = asking[kind] + 1
    end
  end
  -- Ask the server on our own clock, never more than five times a second:
  -- each ask is an exchange, and a tile is a hundred milliseconds away.
  local t = now()
  if t - polled < 0.2 then return end
  polled = t
  for kind, src in pairs({ e = net.elevation, i = net.imagery }) do
    local list = asked[kind]
    for i = asking[kind] * 3 + 1, #list do list[i] = nil end
    src:want(list)
    -- What has come is read from the cache the next time it is wanted.
    local came = src:arrived()
    for _, c in ipairs(came or {}) do missing[kind][key(c.z, c.x, c.y)] = nil end
  end
end

local frame = 0
local function down(w, code) return w:held(code) end

-- **The strip** (F5e): what a pilot reads, along the foot of the chase and
-- tower views - speed, altitude, heading, climb, throttle and flaps, in the
-- units a Cessna's own instruments use. Drawn in a gfx surface with the
-- system's fonts and laid over the frame by the 3D Kit (`w:overlay`), and
-- only when what it says changed: a frame that changes nothing copies
-- nothing to the device. The cockpit will have its panel instead.
local STRIP_H = 40
local CELL = 170                     -- the least a label and its value take
local BAND, LABEL, VALUE, WARN = 0xb8101820, 0xff8fa4b8, 0xffffffff, 0xffffb020
local strip, strip_face, strip_said
-- `ground_h` is nil until the ground under the aircraft has arrived.
local function strip_words(ground_h)
  local words = {
    { "SPEED", ("%d kt"):format(math.floor(S.v * 1.94384 + 0.5)) },
    { "ALTITUDE", ("%d ft"):format(math.floor(S.y * 3.28084 / 10 + 0.5) * 10) },
    { "HEADING", ("%03d\u{b0}"):format(math.floor(math.deg(S.psi) % 360 + 0.5) % 360) },
    { "CLIMB", ("%+d fpm"):format(math.floor(S.v * math.sin(S.gamma) * 196.85 / 50 + 0.5) * 50) },
    { "THROTTLE", ("%d%%"):format(math.floor(S.throttle * 100 + 0.5)) },
    { "FLAPS", ("%d\u{b0}"):format(math.floor(S.flaps * 30 + 0.5)) },
  }
  local state = S.paused and "PAUSED" or S.alpha > PLANE.stall and "STALL"
                or ground_h and (S.y - ground_h) < 3 and "ON THE GROUND" or nil
  return words, state
end

local function draw_strip(w, ground_h)
  if S.view == 2 then strip_said = nil return end    -- the panel's overlay now
  local words, state = strip_words(ground_h)
  local parts = {}
  for i, p in ipairs(words) do parts[i] = p[1] .. " " .. p[2] end
  local key = table.concat(parts, "  ") .. (state and ("  " .. state) or "")
  if key == strip_said then return end

  if not strip then
    strip = gfx.surface{ w = settings.width, h = STRIP_H }
    strip_face = gfx.face("ibmplexsans-medium", 18) or gfx.face("ibmplexsans", 18)
  end
  -- As many cells as the width holds, the most wanted first - speed, then
  -- altitude, then heading - spread evenly; the state on the right where
  -- there is room for it.
  local width = settings.width
  local room = width - 32 - (width >= 900 and 184 or 0)
  local shown_n = math.max(1, math.min(#words, math.floor(room / CELL)))
  local cell = math.floor(room / shown_n)
  local y = math.floor((STRIP_H - (strip_face and 22 or 16)) / 2)
  strip:fill(0, 0, width, STRIP_H, BAND)
  for i = 1, shown_n do
    local x = 16 + (i - 1) * cell
    x = strip:text(x, y + 3, words[i][1], LABEL) + 8
    strip:text(x, y, words[i][2], VALUE, nil, strip_face)
  end
  if state and width >= 900 then
    strip:text(width - 184, y, state, WARN, nil, strip_face)
  end
  local ok, why = w:overlay(strip, 0, settings.height - STRIP_H)
  if not ok then say("strip failed=" .. tostring(why)) end
  if not strip_said then say("strip " .. key) end
  strip_said = key
end

-- **The panel** (F5f): the cockpit's six round instruments, in the order
-- every trainer's panel has them - airspeed, attitude, altitude above;
-- turn, heading, climb below - on a dark panel at the foot of the view.
-- Drawn as the strip is, in a gfx surface laid over the frame, and only
-- when a needle would move by a pixel's worth: a degree, ten feet, fifty
-- feet a minute. Lua says where each needle points; gfx draws it.
local PANEL_BG, RIM, FACE, TICK = 0xe8161a20, 0xff59606d, 0xff0d1015, 0xffd8dde6
local NEEDLE, AMBER, SKY, EARTH = 0xfff4f6f9, 0xfff5bd2e, 0xff3c7fd6, 0xff7a5532
local GREEN, YELLOW, RED = 0xff35b04a, 0xffe8c230, 0xffe04040
local panel, panel_face, panel_said, panel_geo
local turn_rate, last_psi = 0, nil

local function round(v) return math.floor(v + 0.5) end
-- A point `l` from the centre at `a`, clockwise from twelve o'clock.
local function at(cx, cy, a, l) return cx + math.sin(a) * l, cy - math.cos(a) * l end

-- The panel's size from the window's: two rows of three dials under the
-- lower 40% of the view, as large as that and the width allow.
local function panel_layout()
  local width, height = settings.width, settings.height
  local ph = math.min(280, math.floor(height * 0.4))
  local pad = math.max(4, math.floor(ph / 20))
  local r = math.floor((ph - 3 * pad) / 4)
  r = math.max(4, math.min(r, math.floor((width - 4 * pad) / 6)))
  local pw = 3 * (2 * r + pad) + pad
  ph = 2 * (2 * r + pad) + pad
  return { r = r, pad = pad, w = pw, h = ph, x = math.floor((width - pw) / 2),
           y = height - ph, words = r >= 30 }
end

local function dial(s, cx, cy, r)
  s:disc(cx, cy, r + math.max(1, r // 18), RIM, true)
  s:disc(cx, cy, r, FACE, true)
end

-- Ticks round a dial: `n` intervals from `from` to `to`, every `long`th longer.
local function ticks(s, cx, cy, r, from, to, n, long, colour)
  for i = 0, n do
    local a = from + (to - from) * i / n
    local x1, y1 = at(cx, cy, a, r - ((i % long == 0) and r * 0.2 or r * 0.1))
    local x2, y2 = at(cx, cy, a, r - 1)
    s:line(round(x1), round(y1), round(x2), round(y2), (i % long == 0) and 2 or 1, colour or TICK)
  end
end

local function needle(s, cx, cy, a, l, wide, colour)
  local tx, ty = at(cx, cy, a, l)
  local lx, ly = at(cx, cy, a - math.pi / 2, wide)
  local rx, ry = at(cx, cy, a + math.pi / 2, wide)
  local bx, by = at(cx, cy, a + math.pi, l * 0.2)
  s:polygon({ tx, ty, rx, ry, bx, by, lx, ly }, colour or NEEDLE)
end

local function word(s, x, y, text, colour)
  if not panel_geo.words then return end
  local wide = gfx.measure(text, panel_face)
  s:text(round(x - wide / 2), round(y - 9), text, colour or TICK, nil, panel_face)
end

local function airspeed(s, cx, cy, r, kt)
  local function a(v) return math.rad(math.min(v, 200) / 200 * 320) end
  -- The coloured arcs a Cessna's has: green to the caution speed, yellow to
  -- the never-exceed, as short strokes along the rim.
  for v = 48, 162, 3 do
    local x1, y1 = at(cx, cy, a(v), r - 3)
    local x2, y2 = at(cx, cy, a(v + 3), r - 3)
    s:line(round(x1), round(y1), round(x2), round(y2), math.max(2, r // 14), v < 129 and GREEN or YELLOW)
  end
  local x1, y1 = at(cx, cy, a(163), r - 1)
  local x2, y2 = at(cx, cy, a(163), r * 0.75)
  s:line(round(x1), round(y1), round(x2), round(y2), 2, RED)
  ticks(s, cx, cy, r, 0, a(200), 20, 2)
  for v = 40, 200, 40 do
    local x, y = at(cx, cy, a(v), r * 0.66)
    word(s, x, y, tostring(v))
  end
  word(s, cx, cy + r * 0.42, "KNOTS")
  needle(s, cx, cy, a(kt), r * 0.85, math.max(2, r / 16))
end

local function attitude(s, cx, cy, r, pitch, roll)
  local ri = r - 1
  s:disc(cx, cy, ri, SKY, true)
  -- The ground: the dial's circle cut by the horizon, which leans against
  -- the bank and drops as the nose rises - two pixels a degree of a 54.
  local d = math.max(-ri * 0.8, math.min(ri * 0.8, pitch * r / 27))
  local nx, ny = math.sin(roll), math.cos(roll)            -- towards the ground
  local px, py = cx + nx * d, cy + ny * d
  local pts, cut = {}, {}
  local function side(x, y) return (x - px) * nx + (y - py) * ny end
  for i = 0, 35 do
    local a1, a2 = math.rad(i * 10), math.rad((i + 1) * 10)
    local x1, y1 = cx + math.cos(a1) * ri, cy + math.sin(a1) * ri
    local x2, y2 = cx + math.cos(a2) * ri, cy + math.sin(a2) * ri
    local s1, s2 = side(x1, y1), side(x2, y2)
    if s1 >= 0 then pts[#pts + 1] = x1 pts[#pts + 1] = y1 end
    if (s1 >= 0) ~= (s2 >= 0) then
      local f = s1 / (s1 - s2)
      local x, y = x1 + (x2 - x1) * f, y1 + (y2 - y1) * f
      pts[#pts + 1] = x pts[#pts + 1] = y
      cut[#cut + 1] = x cut[#cut + 1] = y
    end
  end
  if #pts >= 6 then s:polygon(pts, EARTH) end
  if #cut == 4 then
    s:line(round(cut[1]), round(cut[2]), round(cut[3]), round(cut[4]), 2, NEEDLE)
  end
  -- The bank scale, fixed, and its pointer turning with the horizon.
  for _, b in ipairs({ -60, -30, -20, -10, 0, 10, 20, 30, 60 }) do
    local x1, y1 = at(cx, cy, math.rad(b), r - 1)
    local x2, y2 = at(cx, cy, math.rad(b), r * (b % 30 == 0 and 0.8 or 0.88))
    s:line(round(x1), round(y1), round(x2), round(y2), 1, NEEDLE)
  end
  local tx, ty = at(cx, cy, -roll, r * 0.78)
  local lx, ly = at(cx, cy, -roll - 0.08, r * 0.66)
  local rx, ry = at(cx, cy, -roll + 0.08, r * 0.66)
  s:polygon({ tx, ty, rx, ry, lx, ly }, AMBER)
  -- The aircraft, fixed in the middle.
  local wing, gap = r * 0.42, r * 0.14
  local t = math.max(2, r // 18)
  s:line(round(cx - wing), round(cy), round(cx - gap), round(cy), t, AMBER)
  s:line(round(cx + gap), round(cy), round(cx + wing), round(cy), t, AMBER)
  s:disc(round(cx), round(cy), math.max(1, r // 18), AMBER, true)
end

local function altimeter(s, cx, cy, r, ft)
  ticks(s, cx, cy, r, 0, 2 * math.pi, 50, 5)
  for i = 0, 9 do
    local x, y = at(cx, cy, math.rad(i * 36), r * 0.68)
    word(s, x, y, tostring(i))
  end
  word(s, cx, cy + r * 0.42, "FEET")
  needle(s, cx, cy, (ft % 10000) / 10000 * 2 * math.pi, r * 0.5, math.max(2, r / 10))
  needle(s, cx, cy, (ft % 1000) / 1000 * 2 * math.pi, r * 0.85, math.max(2, r / 18))
end

local function turn(s, cx, cy, r, rate)
  -- A standard-rate turn, three degrees a second - two minutes round -
  -- leans the little aircraft to the marks, twenty degrees.
  for _, m in ipairs({ -90, -110, 90, 110 }) do
    local x1, y1 = at(cx, cy, math.rad(m), r - 1)
    local x2, y2 = at(cx, cy, math.rad(m), r * 0.8)
    s:line(round(x1), round(y1), round(x2), round(y2), 2, NEEDLE)
  end
  word(s, cx - r * 0.62, cy + r * 0.5, "L")
  word(s, cx + r * 0.62, cy + r * 0.5, "R")
  word(s, cx, cy + r * 0.42, "2 MIN")
  local a = math.rad(math.max(-45, math.min(45, rate / 3 * 20)))
  local x1, y1 = at(cx, cy, a - math.pi / 2, r * 0.7)
  local x2, y2 = at(cx, cy, a + math.pi / 2, r * 0.7)
  s:line(round(x1), round(y1), round(x2), round(y2), math.max(2, r // 14), NEEDLE)
  local fx, fy = at(cx, cy, a, r * 0.22)
  s:line(round(cx), round(cy), round(fx), round(fy), math.max(2, r // 18), NEEDLE)
  s:disc(round(cx), round(cy), math.max(2, r // 10), NEEDLE, true)
end

local LETTERS = { [0] = "N", [9] = "E", [18] = "S", [27] = "W" }
local function heading(s, cx, cy, r, psi)
  -- The card turns under a fixed lubber line, as a heading indicator's does.
  for i = 0, 35 do
    local a = math.rad(i * 10) - psi
    local x1, y1 = at(cx, cy, a, r - 1)
    local x2, y2 = at(cx, cy, a, r * (i % 3 == 0 and 0.8 or 0.9))
    s:line(round(x1), round(y1), round(x2), round(y2), i % 3 == 0 and 2 or 1, TICK)
    if i % 3 == 0 then
      local x, y = at(cx, cy, a, r * 0.64)
      word(s, x, y, LETTERS[i] or tostring(i), LETTERS[i] and AMBER or TICK)
    end
  end
  local tx, ty = at(cx, cy, 0, r * 0.82)
  s:polygon({ tx, ty, tx + r * 0.08, ty - r * 0.14, tx - r * 0.08, ty - r * 0.14 }, AMBER)
  -- The aircraft, nose up the dial.
  s:line(round(cx), round(cy - r * 0.3), round(cx), round(cy + r * 0.25), math.max(2, r // 18), AMBER)
  s:line(round(cx - r * 0.25), round(cy - r * 0.06), round(cx + r * 0.25), round(cy - r * 0.06),
         math.max(2, r // 18), AMBER)
  s:line(round(cx - r * 0.1), round(cy + r * 0.22), round(cx + r * 0.1), round(cy + r * 0.22),
         math.max(1, r // 27), AMBER)
end

local function climb(s, cx, cy, r, fpm)
  -- Nought at nine o'clock; up is clockwise, two thousand feet a minute at
  -- 170 degrees either way.
  local function a(f) return math.rad(-90 + math.max(-1, math.min(1, f / 2000)) * 170) end
  ticks(s, cx, cy, r, a(-2000), a(2000), 20, 5)
  for _, f in ipairs({ -2000, -1000, 0, 1000, 2000 }) do
    local x, y = at(cx, cy, a(f), r * 0.66)
    word(s, x, y, tostring(math.abs(f) // 1000))
  end
  word(s, cx + r * 0.1, cy - r * 0.32, "UP")
  word(s, cx + r * 0.1, cy + r * 0.32, "DOWN")
  needle(s, cx, cy, a(fpm), r * 0.85, math.max(2, r / 16))
end

local function draw_panel(w, dt)
  -- The turn's rate, from the heading's change, smoothed over a few frames.
  if last_psi and dt > 0 then
    local d = (S.psi - last_psi + math.pi) % (2 * math.pi) - math.pi
    turn_rate = turn_rate + (math.deg(d) / dt - turn_rate) * math.min(1, 4 * dt)
  end
  last_psi = S.psi
  if S.view ~= 2 then panel_said = nil return end      -- the strip's overlay now

  local kt = S.v * 1.94384
  local ft = S.y * 3.28084
  local pitch = math.deg(S.gamma + S.alpha)
  local fpm = S.v * math.sin(S.gamma) * 196.85
  local key = ("AIRSPEED %d kt  PITCH %d  ROLL %d  ALTITUDE %d ft  HEADING %03d  TURN %.1f  CLIMB %+d fpm")
      :format(round(kt), round(pitch), round(math.deg(S.phi)), round(ft / 10) * 10,
              round(math.deg(S.psi) % 360) % 360, round(turn_rate * 2) / 2, round(fpm / 50) * 50)
  if key == panel_said then return end

  if not panel then
    panel_geo = panel_layout()
    panel = gfx.surface{ w = panel_geo.w, h = panel_geo.h }
    panel_face = gfx.face("ibmplexsans-medium", math.max(10, math.min(15, panel_geo.r // 4)))
                 or gfx.face("ibmplexsans", 13)
  end
  local P = panel_geo
  local r, pad = P.r, P.pad
  panel:fill(0, 0, P.w, P.h, PANEL_BG)
  local draw = { airspeed, attitude, altimeter, turn, heading, climb }
  local args = {
    { kt }, { pitch, S.phi }, { ft }, { turn_rate }, { S.psi }, { fpm },
  }
  for i = 1, 6 do
    local cx = pad + r + ((i - 1) % 3) * (2 * r + pad)
    local cy = pad + r + (i > 3 and (2 * r + pad) or 0)
    dial(panel, cx, cy, r)
    draw[i](panel, cx, cy, r, table.unpack(args[i]))
  end
  local ok, why = w:overlay(panel, P.x, P.y)
  if not ok then say("panel failed=" .. tostring(why)) end
  if not panel_said then say(("panel %dx%d %s"):format(P.w, P.h, key)) end
  panel_said = key
end

-- **The graphics panel** (F5g), the reason Flight exists: what the 3D API
-- did for the last frames - frames a second and a sparkline of their
-- times, the device's time and the processor's, the draw calls and
-- triangles the 3D Kit recorded, the terrain's chunks, the device and how
-- a frame reaches the screen. Top right, in the overlay's second place, so
-- it lies over the strip or the panel; redrawn four times a second, not
-- every frame. `F3` hides it, and that is remembered. In a window too
-- narrow for it, only the frames a second, in a small box.
local GLASS, DIM, BRIGHT, LINE = 0xd8141820, 0xff9aa6b8, 0xfff4f6f9, 0xff3a4250
local SPARK, BUDGET = 0xff5ab0ff, 0xff4a5262
local TIMES = 120                                  -- the sparkline's frames
local times, times_at, times_n = {}, 0, 0
local processor_ms = nil                           -- the frame less the device's, smoothed
for i = 1, TIMES do times[i] = 0 end
local spark_xy = {}
local graphics, graphics_said, graphics_next = nil, false, 0
local face_big, face_small, face_mono
local recent = { n = 0, since = 0, fps = 0 }

local function count_of(n)
  if n >= 1e6 then return ("%.2f M"):format(n / 1e6) end
  if n >= 1e4 then return ("%.1f k"):format(n / 1e3) end
  return tostring(n)
end

local function draw_graphics(w, said, took, t)
  times_at = times_at % TIMES + 1
  times[times_at] = took
  times_n = math.min(TIMES, times_n + 1)
  -- The processor's part of a frame: the frame less the device's time,
  -- which it waits for, every frame - smoothed over about a second's worth.
  local draws, triangles, gpu_ms, presented = w:stats()
  local own = math.max(0, took * 1000 - (gpu_ms or 0))
  processor_ms = processor_ms and processor_ms + (own - processor_ms) * 0.1 or own
  recent.n = recent.n + 1
  if t - recent.since >= 1 then
    recent.fps = recent.n / (t - recent.since)
    recent.n, recent.since = 0, t
  end
  if not settings.graphics then
    if graphics then w:overlay(nil, 0, 0, 2) graphics = nil end
    return
  end
  if t < graphics_next then return end
  graphics_next = t + 0.25

  local full = settings.width >= 480
  if not graphics then
    face_small = gfx.face("ibmplexsans", 13) or gfx.face("ibmplexsans", 12)
    face_big = gfx.face("ibmplexmono-medium", 26) or gfx.face("ibmplexmono", 26)
    face_mono = gfx.face("ibmplexmono", 13)
    if full then
      graphics = gfx.surface{ w = 262, h = 266 }
    else
      graphics = gfx.surface{ w = math.min(settings.width - 8, 64), h = 18 }
    end
  end
  local gw, gh = graphics:size()
  graphics:fill(0, 0, gw, gh, GLASS)
  local fps = ("%.1f"):format(recent.fps)

  if not full then
    graphics:text(4, 1, fps .. " fps", BRIGHT, nil, face_small)
  else
    local x, right = 14, gw - 14
    graphics:text(x, 10, "Graphics", BRIGHT, nil, face_small)
    local chip = gfx.measure("F3", face_mono)
    graphics:fill_round(right - chip - 10, 9, chip + 10, 20, LINE, 5)
    graphics:text(right - chip - 5, 10, "F3", DIM, nil, face_mono)
    -- "llvmpipe/1.4 1280x720": the device, the API, the picture's size.
    local api, name = w:device():match("^(%S-)/(%S+)")
    graphics:text(x, 32, ("Vulkan %s \u{b7} %s"):format(name or "?", api or w:device()), DIM, nil,
                  face_small)
    local after = graphics:text(x, 52, fps, BRIGHT, nil, face_big)
    graphics:text(after + 10, 62, "frames a second", DIM, nil, face_small)

    -- The sparkline: the last frames' times, oldest on the left, against
    -- a line at a 60th of a second - as many as have been flown, up to
    -- two minutes' at sixty.
    local sx, sy, sw, sh = x, 92, right - x, 38
    local most = 1 / 30
    for i = 1, TIMES do if times[i] > most then most = times[i] end end
    local first = times_n < TIMES and 1 or times_at + 1
    local budget = sy + sh - math.floor(sh * (1 / 60) / most + 0.5)
    graphics:fill(sx, budget, sw, 1, BUDGET)
    local n = 0
    for i = 1, times_n do
      local v = times[(first + i - 2) % TIMES + 1]
      spark_xy[n + 1] = sx + (i - 1) * sw / (TIMES - 1)
      spark_xy[n + 2] = sy + sh - sh * v / most
      n = n + 2
    end
    for i = n + 1, #spark_xy do spark_xy[i] = nil end
    if n >= 4 then graphics:polyline(spark_xy, 1.5, SPARK) end

    local drawn, chosen = said:match("^drawn=(%d+) of (%d+)")
    local rows = {
      { "GPU a frame", gpu_ms and ("%.1f ms"):format(gpu_ms) or "not kept" },
      { "Processor a frame", ("%.1f ms"):format(processor_ms) },
      { "Draw calls", tostring(draws) },
      { "Triangles", count_of(triangles) },
      { "Terrain tiles", ("%s of %s"):format(drawn or "?", chosen or "?") },
      { "Presented by", presented },
    }
    local y = 140
    for _, row in ipairs(rows) do
      graphics:text(x, y, row[1], DIM, nil, face_small)
      local wide = gfx.measure(row[2], face_mono)
      graphics:text(right - wide, y, row[2], BRIGHT, nil, face_mono)
      y = y + 20
    end
  end
  local gx, gy = settings.width - gw - (full and 16 or 4), full and 16 or 4
  local ok, why = w:overlay(graphics, gx, gy, 2)
  if not ok then say("graphics failed=" .. tostring(why)) end
  if not graphics_said and recent.fps > 0 then
    say(("graphics %dx%d at %d,%d device=%s draws=%d triangles=%d gpu_ms=%s frame_ms=%.2f "
         .. "processor_ms=%.2f presented=%s")
        :format(gw, gh, gx, gy, w:device():match("^%S+"), draws, triangles,
                gpu_ms and ("%.2f"):format(gpu_ms) or "none", took * 1000, processor_ms,
                presented))
    graphics_said = true
  end
end

-- The window and the terrain.
local w, why = k.window("Flight", settings.width, settings.height)
if not w then say("failed=" .. tostring(why)) return end
local plane
plane, why = w:model(settings.model, MOVING)
if plane then say("aircraft " .. plane:info()) else say("no aircraft: " .. tostring(why)) end
local ok
ok, why = sources()
if not ok then say("failed=no tiles: " .. tostring(why)) return end

-- Roots at zoom 11 - about 16 km a side here - one each way of the eye's:
-- 50 km across, past the haze's end, and no further; split to zoom 15 near
-- it, which is about 1 m a texel of imagery under the wheels.
--
-- **How many chunks to hold, from the machine's memory**: a chunk is about
-- half a megabyte - its mesh, and its imagery as a texture, which `lavapipe`
-- keeps in the same memory as everything else - so a quarter of what is
-- free, in chunks, between enough for one view and twice that.
local free_mb = (fs.read("/Devices/memory") or {}).free_mb or 512
local keep = math.max(120, math.min(600, math.floor(free_mb / 4 / 0.5)))
local g = k.terrain{ lon = settings.lon, lat = settings.lat, root = 11, deepest = 15,
                     radius = 1, split = 2.0, most = math.min(300, keep), elevation = 13,
                     keep = keep, uploads = 8 }
say(("device=%s keep=%d free_mb=%d"):format(w:device(), keep, free_mb))

local last = now()
local slowest, total = 0, 0
while not w:closed(0) do
  local t = now()
  local dt = math.min(0.05, t - last)
  last = t

  if w:pressed(KEY.v) then S.view = S.view % 3 + 1 end
  if w:pressed(KEY.p) then S.paused = not S.paused end
  if w:pressed(KEY.f3) then
    settings:set("graphics", not settings.graphics)
    graphics_next = 0
  end
  if w:pressed(KEY.f) then S.flaps = (S.flaps + 0.5) % 1.5 end
  if w:pressed(KEY.r) or S.crashed then
    if S.crashed then say("crashed") end
    start()
  end

  if not S.paused then
    local pitch = (down(w, KEY.down) and 1 or 0) - (down(w, KEY.up) and 1 or 0)
    local roll = (down(w, KEY.right) and 1 or 0) - (down(w, KEY.left) and 1 or 0)
    if down(w, KEY.a) then S.throttle = math.min(1, S.throttle + 0.5 * dt) end
    if down(w, KEY.z) then S.throttle = math.max(0, S.throttle - 0.5 * dt) end
    fly(dt, pitch, roll, g:height(S.x, S.z))
    if plane then place(plane, pitch, roll, dt) end
  end
  draw_strip(w, g:height(S.x, S.z))
  draw_panel(w, dt)

  look()
  feed(g, view.eye)
  -- The aircraft is not drawn from inside it.
  local said, failed = w:draw(g, view, S.view ~= 2 and plane or nil)
  if not said then say("failed=" .. tostring(failed)) break end

  frame = frame + 1
  local took = now() - t
  draw_graphics(w, said, took, t)
  total, slowest = total + took, math.max(slowest, took)
  if frame % settings.report == 0 or frame == settings.frames then
    say(("frame=%d view=%d x=%.0f y=%.0f z=%.0f v=%.1f heading=%.0f roll=%.0f ground=%s "
         .. "throttle=%.2f tiles=%d mean_ms=%.1f slowest_ms=%.1f %s")
        :format(frame, S.view, S.x, S.y, S.z, S.v, math.deg(S.psi) % 360, math.deg(S.phi),
                tostring(S.ground), S.throttle, handed, 1000 * total / frame,
                1000 * slowest, said))
  end
  if frame == settings.frames then say("done") w:closed(120) break end
end
