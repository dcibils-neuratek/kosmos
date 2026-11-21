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
-- kosmos: image on intel-gpu k3dvk-anv.elf
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
local keys = use("/Kosmos/Libraries/keys.lua")

local settings = prefs.open("flight", {
  city = "mvd",                                  -- which of CITIES, or none
  lon = -56.1650, lat = -34.9060,                -- over Montevideo's bay
  imagery = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
  imagery_format = "jpg",
  elevation = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png",
  width = 1280, height = 720,
  model = "/Home/Apps/Flight/cessna_172.glb",    -- the aircraft (assets/models/cessna172)
  graphics = true,                               -- the graphics panel open (F3)
  start = "city",                                -- "city", "runway" or "final" (F5i)
  place = false,                                 -- a place searched for: { name, note, lon, lat }
  recent = false,                                -- the sheet's Recent: { key } or places
  finder = false,                                -- places looked up there, not Nominatim
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

-- The state, one table for the whole flight; `start` fills it (below).
local S = { view = 1 }                -- 1 chase, 2 cockpit, 3 tower

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
    -- On its wheels it follows the ground, which a start on a runway
    -- reckons from the airfield's elevation before the tiles are in.
    if ground_h then S.y = ground_h + P.gear end
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

-- **The cities** (F5h): where each is flown from, its airfield and a line
-- about it - the place box says them, and the tower stands on the
-- airfield. The five the pause sheet offers on a first start (F5i), each
-- with its longest-used runway's threshold, true heading and elevation in
-- feet, from OurAirports' `runways.csv` (public domain), for the sheet's
-- runway and final-approach starts.
local CITIES = {
  rio = { name = "Rio de Janeiro", code = "SDU \u{b7} Santos Dumont",
          note = "Sugarloaf and Corcovado, Guanabara Bay",
          lon = -43.205, lat = -22.955, field = { -43.1631, -22.9105 },
          runway = { name = "02R", lon = -43.1625, lat = -22.916389, heading = 357, feet = 9 } },
  sf  = { name = "San Francisco", code = "SFO \u{b7} San Francisco Intl",
          note = "the Golden Gate, Twin Peaks, the bay",
          lon = -122.44, lat = 37.785, field = { -122.3790, 37.6213 },
          runway = { name = "28L", lon = -122.358367, lat = 37.61172, heading = 298, feet = 13 } },
  cpt = { name = "Cape Town", code = "CPT \u{b7} Cape Town Intl",
          note = "Table Mountain, Lion's Head, the bay",
          lon = 18.43, lat = -33.945, field = { 18.6017, -33.9648 },
          runway = { name = "01", lon = 18.608900, lat = -33.987701, heading = 346, feet = 144 } },
  hkg = { name = "Hong Kong", code = "HKG \u{b7} Chek Lap Kok",
          note = "Victoria Peak and the harbour",
          lon = 114.165, lat = 22.29, field = { 113.9185, 22.3080 },
          runway = { name = "07R", lon = 113.898003, lat = 22.2962, heading = 71, feet = 28 } },
  mvd = { name = "Montevideo", code = "MVD \u{b7} Carrasco",
          note = "the Rambla, the Cerro, the River Plate",
          lon = -56.1650, lat = -34.9060, field = { -56.0308, -34.8384 },
          runway = { name = "07", lon = -56.040001, lat = -34.844002, heading = 53, feet = 49 } },
}
for key, c in pairs(CITIES) do c.key = key end
local ORDER = { "rio", "sf", "cpt", "hkg", "mvd" }    -- the mockup's, and Recent's first

-- Where the flight is: one of the cities, a place searched for - a name, a
-- line, a longitude and latitude, and no airfield - or nowhere known.
local here
local function where_now()
  return CITIES[settings.city] or (type(settings.place) == "table" and settings.place) or nil
end
here = where_now()

-- A place's metres east and south of where the terrain is centred, in the
-- terrain's own projection - Web Mercator, scaled at the centre's latitude
-- (`k3d_terrain.c`) - so the tower stands where its tiles draw the field.
local function metres_at(lon, lat)
  local function mercator(lo, la)
    local r = math.rad(la)
    return (lo + 180) / 360, (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2
  end
  local ox, oy = mercator(settings.lon, settings.lat)
  local mx, my = mercator(lon, lat)
  local scale = 40075016.68557849 * math.cos(math.rad(settings.lat))
  return (mx - ox) * scale, (my - oy) * scale
end

-- The tower: on the city's airfield, or where the flight began when the
-- place has none; its foot from the terrain's heights once they are in.
local tower = { x = 0, z = 0, ground = nil, focal = 1.4 }
local function place_tower()
  tower.x, tower.z, tower.ground, tower.said = 0, 0, nil, nil
  if here and here.field then tower.x, tower.z = metres_at(here.field[1], here.field[2]) end
end
place_tower()
local FOCAL = 1.4                                -- the chase's and the cockpit's

-- **The start** (F5i): over the city at 1,500 feet, heading north; or on
-- the runway's threshold, stopped, along it; or five kilometres out on a
-- three-degree path to it, slowed and with half flaps - the last two only
-- where the place has a runway. The view is kept.
local function start(g)
  local h = g and g:height(0, 0)
  S.x, S.y, S.z = 0, math.max(h or 0, 0) + 457, 0   -- metres east, up, south
  S.v = 55                           -- airspeed, m/s
  S.gamma = 0                        -- the path's climb, radians
  S.psi = 0                          -- heading: 0 north, clockwise
  S.phi = 0                          -- roll
  S.alpha = 0.02                     -- angle of attack
  S.throttle = 0.7
  S.flaps = 0
  S.ground = false
  S.crashed = false
  S.paused = false
  local rw = here and here.runway
  local how = rw and settings.start or "city"
  if how ~= "city" then
    local tx, tz = metres_at(rw.lon, rw.lat)
    local psi, elevation = math.rad(rw.heading), rw.feet * 0.3048
    S.psi = psi
    if how == "runway" then
      S.x, S.z, S.y = tx, tz, elevation + PLANE.gear
      S.v, S.throttle, S.alpha, S.ground = 0, 0, 0, true
    else
      local back = 5000
      S.x, S.z = tx - math.sin(psi) * back, tz + math.cos(psi) * back
      S.y = elevation + back * math.tan(math.rad(3)) + PLANE.gear
      S.v, S.gamma, S.flaps, S.throttle = 36, -math.rad(3), 0.5, 0.35
    end
  end
  S.started = how
  say(("started %s at x=%.0f y=%.0f z=%.0f heading=%.0f in %s"):format(how, S.x, S.y, S.z,
      math.deg(S.psi) % 360, here and here.name or "no place known"))
end
start()

local function look(g)
  local ch, sh = math.cos(S.psi), math.sin(S.psi)
  local theta = S.gamma + S.alpha
  local fx, fy, fz = sh * math.cos(theta), math.sin(theta), -ch * math.cos(theta)
  local e, t, u = view.eye, view.target, view.up

  view.focal = FOCAL
  if S.view == 2 then
    -- The cockpit: the pilot's eye, rolling with the wings.
    e[1], e[2], e[3] = S.x, S.y + 1.0, S.z
    t[1], t[2], t[3] = S.x + fx * 100, S.y + 1.0 + fy * 100, S.z + fz * 100
    -- Up: the world's, leaning towards the lower wing as it banks - right
    -- for a right bank, which turns it right.
    local c, s = math.cos(S.phi), math.sin(S.phi)
    u[1], u[2], u[3] = s * ch, c, s * sh
  elseif S.view == 3 then
    -- The tower: the airfield's camera, thirty metres up, following the
    -- aircraft with a longer lens the further it is - a sixtieth of the
    -- distance, so the Cessna stays about a sixth of the picture's height
    -- out to 24 km. A cap of 50 left it 14 pixels tall 13 km from
    -- Carrasco, on the M700 (20 November).
    if not tower.ground then tower.ground = g:height(tower.x, tower.z) end
    e[1], e[2], e[3] = tower.x, (tower.ground or 0) + 30, tower.z
    t[1], t[2], t[3] = S.x, S.y, S.z
    u[1], u[2], u[3] = 0, 1, 0
    local dx, dy, dz = S.x - e[1], S.y - e[2], S.z - e[3]
    tower.distance = math.sqrt(dx * dx + dy * dy + dz * dz)
    tower.focal = math.max(FOCAL, math.min(400, tower.distance / 60))
    view.focal = tower.focal
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

-- **Offline, an invented island** (F5j): with no `/Tiles`, or nothing
-- handed six seconds after a start and the sources saying their fetches
-- failed, the terrain is handed the 3D Kit's island instead (`g:invent`,
-- `k3d_island.c`) - made in C a tile at a time, a few a frame - and the
-- place box says so. Restart tries the network again.
local offline = { on = false, since = 0, handed = 0, made = 0 }
local INVENT_A_FRAME = 6

local function feed_invented(g, eye)
  local made = 0
  for _, t in ipairs(g:wanted(eye)) do
    if made >= INVENT_A_FRAME then break end
    g:invent(t[1], t[2], t[3], t[4])
    made, handed = made + 1, handed + 1
  end
  offline.made = offline.made + made
end

local function feed(g, eye)
  if offline.on then return feed_invented(g, eye) end
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
local strip_redrawn = 0               -- times laid again: the report says it
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
  strip_said, strip_redrawn = key, strip_redrawn + 1
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

-- **The place box and the credit** (F5h). Top left, where and from which
-- airfield, as the mockup has it; bottom right, whose imagery and whose
-- elevation, as their terms ask - over the strip in the chase and the
-- tower, in the corner beside the panel in the cockpit. Each is drawn once
-- and laid in the overlay's third and fourth places; the credit is laid
-- again only when the view moves it. A window too small for one goes
-- without it, and says so.
local WHO = {
  ["server.arcgisonline.com"] = "Esri, Maxar, Earthstar Geographics",
  ["s3.amazonaws.com"] = "Mapzen terrain tiles, AWS",
}
local function whose(url)
  local host = url:match("^%a+://([^/:]+)") or url
  return WHO[host] or host
end
local CREDIT_BAND, CREDIT_WORDS = 0x90101820, 0xffe0e6ee
local place_said, credit, credit_view

local function draw_place(w)
  local width, height = settings.width, settings.height
  if not place_said then
    local name = here and (here.code or here.name) or ("%.3f, %.3f"):format(settings.lat, settings.lon)
    local note = here and (here.field or here.invented) and here.note
                 or (here and here.note and here.note ~= "" and here.note .. "; " or "")
                    .. "no airfield here; the tower is where the flight began"
    local bold = gfx.face("ibmplexsans-medium", 15) or gfx.face("ibmplexsans", 15)
    local small = gfx.face("ibmplexsans", 13)
    local bw = math.max(gfx.measure(name, bold), gfx.measure(note, small)) + 28
    local bh = 52
    if width >= 480 and bw + 32 <= width then
      local box = gfx.surface{ w = bw, h = bh }
      box:fill(0, 0, bw, bh, GLASS)
      box:text(14, 8, name, BRIGHT, nil, bold)
      box:text(14, 28, note, DIM, nil, small)
      local ok, why = w:overlay(box, 16, 16, 3)
      if not ok then say("place failed=" .. tostring(why)) end
      say(("place %s - %s laid at 16,16 %dx%d"):format(name, note, bw, bh))
    else
      say(("place %s - %s not laid: %d wide"):format(name, note, width))
    end
    place_said = true
  end

  if offline.on then
    if credit_view ~= "off" then w:overlay(nil, 0, 0, 4) credit_view = "off" end
    return
  end
  if credit_view == S.view then return end
  local words = ("Imagery %s \u{b7} Elevation %s"):format(whose(settings.imagery),
                                                          whose(settings.elevation))
  local face = gfx.face("ibmplexsans", 11) or gfx.face("ibmplexsans", 12)
  local cw, ch = gfx.measure(words, face) + 12, 17
  local foot = S.view == 2 and 0 or STRIP_H
  if cw + 16 > width or ch + foot + 8 > height then
    if not credit_view then say(("credit %s not laid: %d wide"):format(words, width)) end
    credit_view = S.view
    return
  end
  if not credit then
    credit = gfx.surface{ w = cw, h = ch }
    credit:fill(0, 0, cw, ch, CREDIT_BAND)
    credit:text(6, 1, words, CREDIT_WORDS, nil, face)
  end
  local cx, cy = width - cw - 8, height - foot - ch - 4
  local ok, why = w:overlay(credit, cx, cy, 4)
  if not ok then say("credit failed=" .. tostring(why)) end
  if not credit_view then say(("credit %s laid at %d,%d"):format(words, cx, cy)) end
  credit_view = S.view
end

-- **The pause sheet** (F5i): where in the world and from where. `P`
-- opens it and pauses; `Escape` or Fly on closes it and flies on from
-- where it stopped; Restart starts again with what it says - another city,
-- a place searched for, another start. Its rows, as the mockup draws
-- them: the search field, what the search found, Recent - the five cities
-- first, then each place flown - and Start. Weather, wind and the
-- controls join it with what they change (F6, F7).
--
-- Typed characters go to the field (`w:typed`), the arrows move between
-- rows and choices, Return searches, chooses or presses; a click does the
-- same where it lands (`w:clicked`). A search is asked when Return is
-- pressed, never a keystroke at a time - Nominatim's policy, as Maps' -
-- and its places are read by `tiles.places`, the reader Maps uses.
--
-- Drawn in a gfx surface in the overlay's fifth place, centred, and
-- redrawn only when what it says changed.
local STARTS = { { key = "city", name = "Over the city, 1,500 ft" },
                 { key = "runway", name = "On the runway" },
                 { key = "final", name = "Final approach" } }
local ACCENT, ON, RAISED, SUNKEN = 0xff4c8dff, 0xffffffff, 0xff2a313c, 0xff0c0f14
local SHEET = { open = false, focus = 1, cols = {}, text = "", hits = {} }
local decode = keys.decoder()
local sheet_face, sheet_bold, sheet_title, sheet_surface

local function same_place(a, b)
  return a and b and math.abs(a.lon - b.lon) < 1e-4 and math.abs(a.lat - b.lat) < 1e-4
end

-- Recent: what `settings.recent` keeps - { key = "rio" } for a city, the
-- place itself otherwise - each made a place again.
local function recent()
  local out = {}
  for _, e in ipairs(type(settings.recent) == "table" and settings.recent or {}) do
    local p = e.key and CITIES[e.key] or (e.lon and e.lat and e) or nil
    if p then out[#out + 1] = p end
  end
  if #out == 0 then for _, k in ipairs(ORDER) do out[#out + 1] = CITIES[k] end end
  return out
end

local function sheet_rows()
  local rows = { { kind = "field", label = "City" } }
  if SHEET.found and #SHEET.found > 0 then
    rows[#rows + 1] = { kind = "places", label = "Found", items = SHEET.found }
  end
  rows[#rows + 1] = { kind = "places", label = "Recent", items = recent() }
  rows[#rows + 1] = { kind = "starts", label = "Start", items = STARTS }
  rows[#rows + 1] = { kind = "buttons", items = { { name = "Restart" }, { name = "Fly on" } } }
  return rows
end

-- Whether a choice can be taken: a runway's starts need a runway.
local function usable(row, item)
  if row.kind ~= "starts" or item.key == "city" then return true end
  return SHEET.place ~= nil and SHEET.place.runway ~= nil
end

local function chosen(row, item)
  if row.kind == "places" then return same_place(item, SHEET.place) end
  if row.kind == "starts" then
    return item.key == (usable(row, item) and SHEET.start or "city")
  end
  return item.name == "Fly on"
end

local function sheet_open()
  SHEET.open, SHEET.focus, SHEET.cols, SHEET.text = true, 1, {}, ""
  SHEET.place, SHEET.start = here, settings.start
  SHEET.found, SHEET.asked, SHEET.said, SHEET.drawn = nil, nil, nil, nil
  S.paused = true
end

local function sheet_close(w)
  SHEET.open, S.paused = false, false
  w:overlay(nil, 0, 0, 5)
  say("sheet closed")
end

local restart                          -- the terrain's, below

local function search(text)
  if text == "" then return end
  if settings.finder and not SHEET.finder_told then
    SHEET.finder_told = true
    if not tiles.finder(settings.finder) then say("sheet finder refused " .. settings.finder) end
  end
  local n, why = tiles.find(text)
  if not n then SHEET.said = "the search could not be asked: " .. tostring(why) return end
  SHEET.asked, SHEET.found, SHEET.said = n, nil, "Searching\u{2026}"
  say(("sheet searching %q"):format(text))
end

local function sheet_found()
  if not SHEET.asked then return end
  local done, status = tiles.found()
  if done < SHEET.asked then return end
  local list, why = tiles.places(SHEET.asked, status)
  SHEET.asked = nil
  if not list then SHEET.said = why return end
  SHEET.found = {}
  for i = 1, math.min(4, #list) do
    local r = list[i]
    SHEET.found[i] = { name = r.name, note = r.where, lon = r.lon, lat = r.lat }
  end
  SHEET.said = #list == 0 and "Nowhere is called that" or nil
  say(("sheet found %d%s"):format(#list, list[1] and (" first " .. list[1].name .. (" at %.3f %.3f")
      :format(list[1].lat, list[1].lon)) or ""))
end

-- What a row's choice does: a place or a start chosen, a button pressed.
local function choose(w, row, i)
  local item = row.items and row.items[i]
  if row.kind == "field" then search(SHEET.text) return end
  if not item then return end
  if row.kind == "places" then
    SHEET.place = item
  elseif row.kind == "starts" then
    if usable(row, item) then SHEET.start = item.key end
  elseif item.name == "Restart" then
    restart(SHEET.place, SHEET.start)
    sheet_close(w)
  else
    sheet_close(w)
  end
end

local function sheet_input(w)
  local rows = sheet_rows()
  SHEET.focus = math.max(1, math.min(#rows, SHEET.focus))
  local row = rows[SHEET.focus]
  local function col(r) return SHEET.cols[r.label or r.kind] end
  local function default(r)
    for i, item in ipairs(r.items or {}) do if chosen(r, item) then return i end end
    return 1
  end
  if row.items and not col(row) then SHEET.cols[row.label or row.kind] = default(row) end

  if w:pressed(KEY.up) then SHEET.focus = math.max(1, SHEET.focus - 1) end
  if w:pressed(KEY.down) then SHEET.focus = math.min(#rows, SHEET.focus + 1) end
  row = rows[SHEET.focus]
  if row.items and not col(row) then SHEET.cols[row.label or row.kind] = default(row) end
  if row.items then
    local c = col(row)
    if w:pressed(KEY.left) then c = math.max(1, c - 1) end
    if w:pressed(KEY.right) then c = math.min(#row.items, c + 1) end
    SHEET.cols[row.label or row.kind] = c
  else
    w:pressed(KEY.left) w:pressed(KEY.right)
  end

  -- What was typed is the window manager's bytes, a terminal's: the
  -- arrows arrive as escape sequences, which `keys.lua`'s decoder turns
  -- into keys - so an arrow's Escape is not taken for Escape. The arrows
  -- themselves are read as held keys, above.
  local typed = w:typed()
  if typed then
    local function key(c)
      if c == keys.ENTER or c == 10 then
        choose(w, row, col(row))
      elseif c == keys.ESCAPE then
        sheet_close(w)
      elseif c == 8 or c == 127 then
        SHEET.text = SHEET.text:gsub("[%z\1-\127\194-\244][\128-\191]*$", "")
        SHEET.focus = 1
      elseif c >= 32 and c < 256 and #SHEET.text < 60 then
        SHEET.text, SHEET.focus = SHEET.text .. string.char(c), 1
      end
    end
    for i = 1, #typed + 1 do
      local a, b = decode(i <= #typed and typed:byte(i) or nil)
      if a then key(a) end
      if b and SHEET.open then key(b) end
      if not SHEET.open then return end
    end
  end

  local cx, cy = w:clicked()
  if cx and SHEET.at then
    local x, y = cx - SHEET.at[1], cy - SHEET.at[2]
    for _, h in ipairs(SHEET.hits) do
      if x >= h[1] and x < h[1] + h[3] and y >= h[2] and y < h[2] + h[4] then
        SHEET.focus = h[5]
        local r = rows[h[5]]
        if r and r.items then SHEET.cols[r.label or r.kind] = h[6] end
        if r then choose(w, r, h[6]) end
        if not SHEET.open then return end
        break
      end
    end
  end
  sheet_found()
end

local function chip(s, x, y, text, on, focused, dim, face)
  local wide = gfx.measure(text, face) + 20
  if focused then s:fill_round(x - 2, y - 2, wide + 4, 28, ACCENT, 14) end
  s:fill_round(x, y, wide, 24, on and ACCENT or (focused and RAISED or LINE), 12)
  if not on then s:fill_round(x + 1, y + 1, wide - 2, 22, focused and RAISED or SUNKEN, 11) end
  s:text(x + 10, y + 3, text, dim and LINE or (on and ON or BRIGHT), nil, face)
  return wide
end

local function draw_sheet(w)
  if not SHEET.open then return end
  local rows = sheet_rows()
  local width, height = settings.width, settings.height
  local sw = math.min(460, width - 32)
  local parts = { SHEET.focus, SHEET.text, SHEET.said or "",
                  SHEET.place and SHEET.place.name or "-", SHEET.start or "-" }
  for _, r in ipairs(rows) do
    parts[#parts + 1] = (r.label or r.kind) .. ":" .. tostring(SHEET.cols[r.label or r.kind])
    for _, item in ipairs(r.items or {}) do parts[#parts + 1] = item.name end
  end
  local key = table.concat(parts, "|")
  if key == SHEET.drawn then return end
  SHEET.drawn = key
  local focus = rows[SHEET.focus]
  say(("sheet focus=%s:%s place=%s start=%s text=%q found=%d%s"):format(
      focus.label or focus.kind, tostring(SHEET.cols[focus.label or focus.kind] or "-"),
      SHEET.place and SHEET.place.name or "none", SHEET.start, SHEET.text,
      SHEET.found and #SHEET.found or 0, SHEET.said and (" said=" .. SHEET.said) or ""))
  if sw < 300 or height < 300 then
    if not SHEET.small_said then say(("sheet not laid: %dx%d"):format(width, height)) end
    SHEET.small_said = true
    return
  end

  if not sheet_face then
    sheet_face = gfx.face("ibmplexsans", 13)
    sheet_bold = gfx.face("ibmplexsans-medium", 13) or sheet_face
    sheet_title = gfx.face("ibmplexsans-medium", 18) or sheet_bold
  end
  -- The rows' heights first: chips flow onto as many lines as they need.
  local left, inner = 110, sw - 110 - 18
  local function lines(r)
    if not r.items or r.kind == "buttons" then return 1 end
    local n, x = 1, 0
    for _, item in ipairs(r.items) do
      local wide = gfx.measure(item.name, sheet_face) + 20
      if x > 0 and x + wide > inner then n, x = n + 1, 0 end
      x = x + wide + 6
    end
    return n
  end
  local sh = 70
  for _, r in ipairs(rows) do sh = sh + (r.kind == "buttons" and 48 or lines(r) * 30 + 10) end
  sh = math.min(sh, height - 16)
  if not sheet_surface or select(2, sheet_surface:size()) ~= sh or sheet_surface:size() ~= sw then
    sheet_surface = gfx.surface{ w = sw, h = sh }
  end
  local s = sheet_surface
  s:fill(0, 0, sw, sh, GLASS)
  s:text(18, 14, "Paused", BRIGHT, nil, sheet_title)
  s:text(18, 42, "Where in the world, and from where. Fly on goes on from here.", DIM, nil,
         sheet_face)
  local hits, y = {}, 70
  for ri, r in ipairs(rows) do
    local focused_row = ri == SHEET.focus
    if r.kind ~= "buttons" then
      s:fill(18, y - 4, sw - 36, 1, LINE)
      s:text(18, y + 4, r.label, DIM, nil, sheet_face)
    end
    if r.kind == "field" then
      local fw = inner
      s:fill_round(left, y, fw, 26, focused_row and ACCENT or LINE, 8)
      s:fill_round(left + 1, y + 1, fw - 2, 24, SUNKEN, 7)
      local shown = SHEET.text ~= "" and SHEET.text or (SHEET.said or "Search a city or an airport")
      local after = s:text(left + 10, y + 4, shown, SHEET.text ~= "" and BRIGHT or DIM, nil, sheet_face)
      if focused_row and SHEET.text ~= "" then s:fill(after + 1, y + 6, 1, 15, BRIGHT) end
      hits[#hits + 1] = { left, y, fw, 26, ri, 1 }
      y = y + 40
    elseif r.kind == "buttons" then
      local x = sw - 18
      for i = #r.items, 1, -1 do
        local item = r.items[i]
        local wide = gfx.measure(item.name, sheet_bold) + 32
        x = x - wide
        local focused = focused_row and SHEET.cols[r.kind] == i
        if focused then s:fill_round(x - 2, y + 4, wide + 4, 36, BRIGHT, 11) end
        s:fill_round(x, y + 6, wide, 32, item.name == "Fly on" and ACCENT or RAISED, 9)
        s:text(x + 16, y + 13, item.name, ON, nil, sheet_bold)
        hits[#hits + 1] = { x, y + 6, wide, 32, ri, i }
        x = x - 8
      end
      y = y + 48
    else
      local x, line = 0, 0
      for i, item in ipairs(r.items) do
        local wide = gfx.measure(item.name, sheet_face) + 20
        if x > 0 and x + wide > inner then x, line = 0, line + 1 end
        local cx, cy = left + x, y + line * 30
        chip(s, cx, cy, item.name, chosen(r, item), focused_row and SHEET.cols[r.label] == i,
             not usable(r, item), sheet_face)
        hits[#hits + 1] = { cx, cy, wide, 24, ri, i }
        x = x + wide + 6
      end
      y = y + (line + 1) * 30 + 10
    end
  end
  SHEET.hits = hits
  SHEET.at = { (width - sw) // 2, (height - sh) // 2 }
  local ok, why = w:overlay(s, SHEET.at[1], SHEET.at[2], 5)
  if not ok then say("sheet failed=" .. tostring(why)) end
end

-- The window and the terrain.
local w, why = k.window("Flight", settings.width, settings.height)
if not w then say("failed=" .. tostring(why)) return end
local plane
plane, why = w:model(settings.model, MOVING)
if plane then say("aircraft " .. plane:info()) else say("no aircraft: " .. tostring(why)) end
local ok
ok, why = sources()
local go_offline                       -- below, with the place box
if not ok then say("no tiles: " .. tostring(why)) end

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
local function terrain()
  return k.terrain{ lon = settings.lon, lat = settings.lat, root = 11, deepest = 15,
                    radius = 1, split = 2.0, most = math.min(300, keep), elevation = 13,
                    keep = keep, uploads = 8 }
end
local g = terrain()
say(("device=%s keep=%d free_mb=%d"):format(w:device(), keep, free_mb))

-- The flight starts again over the island's middle, at the city's height:
-- the runway or the approach it was on belonged to a field that is not
-- there, and the island is in the terrain's centre, not under the field -
-- on the M700 a Cessna left on Rio's runway sat stopped on open sea.
-- Restart starts the flight itself, so it says so and this does not.
go_offline = function(reason, restarting)
  offline.on, offline.made = true, 0
  here = { name = "an invented island", code = "Offline: an invented island",
           note = "the imagery and terrain could not be fetched", invented = true,
           lon = settings.lon, lat = settings.lat }
  place_tower()
  place_said = nil
  say("offline: " .. reason .. "; an invented island instead")
  if not restarting then start(g) end
end
if not ok then go_offline("no /Tiles") end
offline.since = now()

-- Restart (the sheet's): somewhere else is a terrain centred there - the
-- old one let go, its chunks with it - and the place box, the tower and
-- Recent follow; the same place starts again on the terrain it has.
restart = function(place, how)
  settings:set("start", how or "city")
  if place and not same_place(place, here) then
    if place.key then
      settings:set{ city = place.key, lon = place.lon, lat = place.lat }
      settings:reset("place")
    else
      settings:set{ city = "", lon = place.lon, lat = place.lat,
                    place = { name = place.name, note = place.note, lon = place.lon,
                              lat = place.lat } }
    end
    local kept = { place.key and { key = place.key } or settings.place }
    for _, p in ipairs(recent()) do
      if #kept < 8 and not same_place(p, place) then
        kept[#kept + 1] = p.key and { key = p.key } or { name = p.name, note = p.note,
                                                          lon = p.lon, lat = p.lat }
      end
    end
    settings:set("recent", kept)
    here = where_now()
    g = nil
    collectgarbage()
    g = terrain()
    missing.e, missing.i = {}, {}
    place_tower()
    place_said = nil
    say(("restarted in %s at %.4f %.4f"):format(here and here.name or "?", settings.lat,
        settings.lon))
  end
  if offline.on and not (net.imagery and net.elevation) then
    go_offline("still no /Tiles", true)      -- an island here, then
  elseif offline.on then                     -- the network asked again
    offline.on = false
    here = where_now()
    place_tower()
    place_said, credit_view = nil, nil
    g = nil
    collectgarbage()
    g = terrain()
  end
  offline.since, offline.handed = now(), handed
  start(g)
end

local last = now()
local slowest, total = 0, 0
while not w:closed(0) do
  local t = now()
  local dt = math.min(0.05, t - last)
  last = t

  -- With the sheet open, the keys are its own: the letters a search.
  local v, p, f3, f, r = w:pressed(KEY.v), w:pressed(KEY.p), w:pressed(KEY.f3),
                         w:pressed(KEY.f), w:pressed(KEY.r)
  if SHEET.open then
    sheet_input(w)
  else
    if v then S.view = S.view % 3 + 1 end
    if p then
      sheet_open()
      w:typed()                            -- the P that opened it
      w:clicked()
      -- The arrows flown with went down long ago, and are not the sheet's.
      w:pressed(KEY.up) w:pressed(KEY.down) w:pressed(KEY.left) w:pressed(KEY.right)
    end
    if f3 then
      settings:set("graphics", not settings.graphics)
      graphics_next = 0
    end
    if f then S.flaps = (S.flaps + 0.5) % 1.5 end
    if r or S.crashed then
      if S.crashed then say("crashed") end
      start(g)
    end
    w:typed()
    w:clicked()
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
  draw_sheet(w)

  look(g)
  feed(g, view.eye)
  if not offline.on and net.imagery and net.elevation and handed == offline.handed
     and t - offline.since > 6
     and ((net.imagery.failed or 0) > 0 or (net.elevation.failed or 0) > 0) then
    go_offline(("nothing came, %d imagery and %d elevation fetches failed%s"):format(
               net.imagery.failed or 0, net.elevation.failed or 0,
               net.imagery.why and net.imagery.why ~= "" and (" (" .. net.imagery.why .. ")") or ""))
  end
  draw_place(w)
  if S.view == 3 and not tower.said then
    say(("tower at x=%.0f z=%.0f ground=%s distance=%.0f focal=%.1f")
        :format(tower.x, tower.z, tower.ground and ("%.0f"):format(tower.ground) or "none",
                tower.distance, tower.focal))
    tower.said = true
  elseif S.view ~= 3 then
    tower.said = nil
  end
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
    -- What the strip shows now and how often it was laid again, so a
    -- strip that stopped following the flight is seen in the log.
    say(("strip redrawn=%d shows %s"):format(strip_redrawn, strip_said or "nothing"))
  end
  if frame == settings.frames then say("done") w:closed(120) break end
end
