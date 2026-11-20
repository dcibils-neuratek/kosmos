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
  frames = 0,                                    -- stop after this many, 0 never
  report = 120,                                  -- a line on the console every this many
})

local KEY = { up = 103, down = 108, left = 105, right = 106, a = 30, z = 44, f = 33,
              v = 47, p = 25, r = 19 }

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

  look()
  feed(g, view.eye)
  -- The aircraft is not drawn from inside it.
  local said, failed = w:draw(g, view, S.view ~= 2 and plane or nil)
  if not said then say("failed=" .. tostring(failed)) break end

  frame = frame + 1
  local took = now() - t
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
