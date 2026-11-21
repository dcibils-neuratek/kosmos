-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Flight: a light trainer over a real city, its hills from elevation tiles
-- and its streets from satellite imagery, drawn by Vulkan (`docs/flight.md`).
--
-- kosmos: application
-- kosmos: name Flight
-- kosmos: icon App_Generic
-- kosmos: section applications
-- kosmos: needs tiles audio
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
local audio = use("/Kosmos/Libraries/audio.lua")
local synth = use("/Kosmos/Kits/synth")
local airports = use("/Kosmos/Libraries/airports.lua")

local settings = prefs.open("flight", {
  city = "mvd",                                  -- which of CITIES, or none
  lon = -56.1650, lat = -34.9060,                -- over Montevideo's bay
  imagery = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
  imagery_format = "jpg",
  elevation = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png",
  width = 1280, height = 720,
  aircraft = "cessna172",                        -- which of Aircraft/<name>.lua (F12)
  graphics = true,                               -- the graphics panel open (F3)
  start = "city",                                -- "city", "runway" or "final" (F5i)
  final = 5000,                                  -- metres out a final-approach start begins
  time = "noon",                                 -- the time of day, one of HOURS (F6a)
  sound = true,                                  -- the engine and the wind heard (F6c, M)
  runways = true,                                -- the runways drawn over the imagery (F8b)
  buildings = true,                              -- the city's buildings stood up (F6, B3)
  shadows = true,                                -- the sun's shadows (F6, S2, H)
  weather = "clear",                             -- one of DECK.choices (F6, W1)
  wind = 0, wind_from = 270,                     -- knots, and the bearing it blows from (W2)
  vector = "https://tiles.openfreemap.org/planet",   -- where their outlines come from, as Maps'
  place = false,                                 -- a place searched for: { name, note, lon, lat }
  recent = false,                                -- the sheet's Recent: { key } or places
  finder = false,                                -- places looked up there, not Nominatim
  frames = 0,                                    -- stop after this many, 0 never
  report = 120,                                  -- a line on the console every this many
  anchor_after = 40000,                          -- metres from the terrain's centre to move it (F10a)
  fly_to = "",                                   -- an airport's code: Q flies there (F10c)
})

local KEY = { up = 103, down = 108, left = 105, right = 106, a = 30, z = 44, f = 33,
              v = 47, p = 25, r = 19, f3 = 61, t = 20, m = 50, b = 48, h = 35,
              -- The throttle's other keys: Page Up and Down, + and - (the
              -- main row's and the keypad's), and 1 to 9 and 0 to set it at
              -- once - 0 idle, 1 a tenth, 9 nine tenths; Shift-less = is +.
              pgup = 104, pgdn = 109, minus = 12, equal = 13, kpplus = 78, kpminus = 74,
              f1 = 59, esc = 1, space = 57, x = 45, q = 16,
              digits = { 11, 2, 3, 4, 5, 6, 7, 8, 9, 10 } }

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local function now() return sys.ticks() / counter_hz end

local function say(s) print("flight " .. s) end

-- **The aircraft, a file of its own** (F12): `Aircraft/<name>.lua` in
-- Flight's folder returns its numbers, its engine, its model and its
-- sound; `Aircraft/cessna172.lua` says what each one is. Read as data -
-- with nothing but `math` to hand - and held field by field to what the
-- flight model, the 3D Kit and the sound read, so a file that leaves one
-- out is refused by its name rather than failing in the air.
local AIRCRAFT = {
  folder = "/Home/Apps/Flight",
}
-- The arithmetic of flying - an aircraft file's fields and their checks,
-- the wind, the flight model and the autopilot (F10b) - in a library
-- beside this program with nothing of the screen in it, which is what
-- lets `tools/test_flightmodel.lua` fly it on the host.
AIRCRAFT.model = use(AIRCRAFT.folder .. "/model.lua")

function AIRCRAFT.read(key)
  local path = AIRCRAFT.folder .. "/Aircraft/" .. tostring(key) .. ".lua"
  local text, why = fs.read(path)
  if type(text) ~= "string" then return nil, "not read: " .. tostring(why) end
  return AIRCRAFT.model.aircraft(text, key, AIRCRAFT.folder)
end

-- Every file in the folder, read: the ones that fly for the sheet's row,
-- and each refused said with why.
function AIRCRAFT.list()
  local out, refused = {}, {}
  for _, f in ipairs(fs.list(AIRCRAFT.folder .. "/Aircraft") or {}) do
    local key = tostring(f):match("^(.+)%.lua$")
    if key then
      local a, why = AIRCRAFT.read(key)
      if a then out[#out + 1] = { key = key, name = a.name }
      else refused[#refused + 1] = key .. " refused: " .. why end
    end
  end
  table.sort(out, function(x, y) return x.name < y.name end)
  return out, refused
end

local PLANE, PLANE_WHY = AIRCRAFT.read(settings.aircraft)
-- An aircraft chosen once and since removed or broken: the Cessna, which
-- Flight carries, rather than no flight at all.
if not PLANE and settings.aircraft ~= "cessna172" then
  say(("aircraft %s refused: %s; the Cessna instead"):format(tostring(settings.aircraft),
      tostring(PLANE_WHY)))
  settings:set("aircraft", "cessna172")
  PLANE, PLANE_WHY = AIRCRAFT.read("cessna172")
end
if not PLANE then
  say(("no aircraft %s: %s"):format(tostring(settings.aircraft), tostring(PLANE_WHY)))
  return
end

-- **Where the terrain is centred** (F10a): the place's own longitude and
-- latitude at first, and moved under the aircraft once it has flown
-- `settings.anchor_after` metres from it - the runways levelled, drawn
-- and lit, and the cloud deck, are those near the centre, and a flight
-- from Carrasco to Ezeiza leaves Carrasco's 230 km behind. `settings`
-- keeps the place the flight began in; this is the frame's origin.
local ANCHOR = { lon = settings.lon, lat = settings.lat }

-- The state, one table for the whole flight; `start` fills it (below).
local S = { view = 1, rate = 1, ap = {} }   -- 1 chase, 2 cockpit, 3 tower; the time 1x-4x;
                                            -- the autopilot's (F10b)

-- **The wind** (F6, W2): a strength and the bearing it blows from, chosen
-- in the pause sheet, with gusts - two slow waves on the strength, a fifth
-- of it at most. The flight model's speed is the air's; the wind carries
-- the air, so it is added to the track in the air, and on the wheels the
-- headwind's share is what the wings feel on top of the ground speed.
local WIND = { strengths = { { key = 0, name = "Calm" }, { key = 10, name = "10 kt" },
                             { key = 25, name = "25 kt" } },
               froms = { { key = 0, name = "N" }, { key = 90, name = "E" },
                         { key = 180, name = "S" }, { key = 270, name = "W" } },
               t = 0, x = 0, z = 0 }


-- The camera, written into one view table a frame.
local view = {
  eye = { 0, 0, 0 }, target = { 0, 0, 0 }, up = { 0, 1, 0 },
  focal = 1.4, near = 0.5, far = 60000,
  sun = { 0.3, 1.0, 0.2 }, ambient = 0.35,
  zenith = { 0.22, 0.42, 0.80 }, horizon = { 0.78, 0.84, 0.92 },
  haze = { 0.72, 0.78, 0.86 }, haze_start = 4000, haze_end = 40000,
  light = { 1, 1, 1 }, glow = 0.3,
  shadow_centre = { 0, 0, 0 },          -- with shadow_radius, set by look() (S2)
}

-- **The time of day** (F6a; Diego, 20 November 2026: "I love playing in
-- dusk", "We neee to chose what time of day to play in"). An hour of the
-- local solar day, at the equinox, so it means the same in every city:
-- the sun's direction from the hour and the place's latitude, and the
-- sky, the haze, the sun's colour and the ambient share blended between
-- five pictures of the sky by how high the sun stands - night at twelve
-- degrees under the horizon, twilight at three under, the golden hour at
-- four over, the afternoon at fifteen and the day from thirty-five. Dusk
-- is the sun three degrees up in the west, orange along the horizon.
-- `T` steps through them while flying; the sheet chooses one.
-- **The weather** (F6, W1; `docs/flight.md`, *Weather and wind*): clear,
-- haze, or an overcast deck 900 m over the field (3,000 ft). What the time of day
-- makes of the sky is kept in DECK.sky, and the weather is laid over it every
-- frame, since under, in and over the deck are three skies.
-- One table, since Flight's main chunk is near Lua's 200 locals: the
-- choices, the sky the hour made, and the deck.
local DECK = { above = 900, thick = 60, h = nil, shape = nil, said = nil,
               choices = { { key = "clear", name = "Clear" }, { key = "haze", name = "Haze" },
                           { key = "cloud", name = "Low cloud" } },
               sky = { zenith = {}, horizon = {}, haze = {}, light = {}, ambient = 0.35, glow = 0.3 } }

-- The view's sky from DECK.sky and the weather, for an eye `eye_y` metres up
-- (nil before there is one). Whether the sun's shadows are to be drawn.
function DECK.weather_sky(eye_y)
  for _, k in ipairs({ "zenith", "horizon", "haze", "light" }) do
    for i = 1, 3 do view[k][i] = DECK.sky[k][i] end
  end
  view.ambient, view.glow = DECK.sky.ambient, DECK.sky.glow
  view.haze_start, view.haze_end = 4000, 40000
  local function grey(c, f)          -- towards its own brightness, by f
    local l = 0.3 * c[1] + 0.55 * c[2] + 0.15 * c[3]
    for i = 1, 3 do c[i] = c[i] + (l - c[i]) * f end
  end
  local key = settings.weather
  if key == "haze" then
    view.haze_start, view.haze_end = 1500, 12000
    grey(view.haze, 0.5)
    for i = 1, 3 do
      view.horizon[i] = view.horizon[i] + (view.haze[i] - view.horizon[i]) * 0.6
      view.light[i] = view.light[i] * 0.8
    end
    view.ambient = math.min(0.7, view.ambient + 0.08)
    return true
  end
  if key ~= "cloud" or not DECK.h or not eye_y then return true end
  local over = eye_y - DECK.h
  if over > DECK.thick then return true end   -- above it: the sky is clear
  -- Under it, or in it: the sun's light through a deck is a quarter of
  -- it, spread from the whole sky, so the ambient rises and no shadow is
  -- sharp enough to draw. How bright the grey is follows the hour.
  local bright = 0.3 * DECK.sky.light[1] + 0.55 * DECK.sky.light[2] + 0.15 * DECK.sky.light[3]
  local g = 0.18 + 0.5 * bright * math.max(0.2, DECK.sky.ambient / 0.35)
  g = math.min(0.78, g)
  for i = 1, 3 do
    view.light[i] = view.light[i] * 0.25
    view.zenith[i] = g * (0.92 + 0.04 * i)
    view.horizon[i] = g * (0.98 + 0.02 * i)
    view.haze[i] = g * (0.97 + 0.02 * i)
  end
  view.ambient = math.min(0.8, view.ambient + 0.3)
  view.glow = 0
  view.haze_start, view.haze_end = 3000, 20000
  if over > -DECK.thick then
    -- In it: white, eighty metres.
    for i = 1, 3 do view.haze[i] = math.min(0.92, g + 0.15) end
    view.haze_start, view.haze_end = 0, 80
  end
  return false
end

local HOURS = { { key = "dawn", name = "Dawn", hour = 6.25 },
                { key = "morning", name = "Morning", hour = 9 },
                { key = "noon", name = "Noon", hour = 12 },
                { key = "afternoon", name = "Afternoon", hour = 15.5 },
                { key = "dusk", name = "Dusk", hour = 17.75 },
                { key = "night", name = "Night", hour = 21 } }
local SKIES = {   -- the sun's height in degrees, then what the sky is at it
  { -12, zenith = { 0.02, 0.03, 0.08 }, horizon = { 0.05, 0.07, 0.14 },
    haze = { 0.05, 0.07, 0.13 }, light = { 0.35, 0.40, 0.55 }, ambient = 0.16, glow = 0 },
  { -3, zenith = { 0.08, 0.12, 0.30 }, horizon = { 0.50, 0.36, 0.42 },
    haze = { 0.36, 0.30, 0.36 }, light = { 0.85, 0.50, 0.40 }, ambient = 0.28, glow = 0.8 },
  { 4, zenith = { 0.18, 0.30, 0.58 }, horizon = { 0.84, 0.62, 0.56 },
    haze = { 0.72, 0.58, 0.55 }, light = { 1.00, 0.65, 0.42 }, ambient = 0.48, glow = 1.0 },
  { 15, zenith = { 0.20, 0.36, 0.72 }, horizon = { 0.88, 0.80, 0.74 },
    haze = { 0.80, 0.76, 0.74 }, light = { 1.00, 0.88, 0.72 }, ambient = 0.36, glow = 0.6 },
  { 35, zenith = { 0.22, 0.42, 0.80 }, horizon = { 0.78, 0.84, 0.92 },
    haze = { 0.72, 0.78, 0.86 }, light = { 1.00, 1.00, 1.00 }, ambient = 0.35, glow = 0.3 },
}

local function time_of(key)
  for i, t in ipairs(HOURS) do if t.key == key then return t, i end end
  return HOURS[3], 3
end

local function set_time(key)
  local t = time_of(key)
  local lat, h = math.rad(ANCHOR.lat), math.rad((t.hour - 12) * 15)
  -- East, up and north at the equinox, then the world's x east, y up, z south.
  local east, up, north = -math.sin(h), math.cos(lat) * math.cos(h), -math.sin(lat) * math.cos(h)
  local height = math.deg(math.asin(math.max(-1, math.min(1, up))))
  local a, b = SKIES[1], SKIES[#SKIES]
  for i = 1, #SKIES - 1 do
    if height >= SKIES[i][1] and height < SKIES[i + 1][1] then a, b = SKIES[i], SKIES[i + 1] end
  end
  local f = b[1] == a[1] and 0 or math.max(0, math.min(1, (height - a[1]) / (b[1] - a[1])))
  if height < SKIES[1][1] then f = 0 end
  local function mix(x, y) return x + (y - x) * f end
  for _, k3 in ipairs({ "zenith", "horizon", "haze", "light" }) do
    for i = 1, 3 do view[k3][i] = mix(a[k3][i], b[k3][i]) end
  end
  view.ambient, view.glow = mix(a.ambient, b.ambient), mix(a.glow, b.glow)
  for _, k3 in ipairs({ "zenith", "horizon", "haze", "light" }) do
    for i = 1, 3 do DECK.sky[k3][i] = view[k3][i] end
  end
  DECK.sky.ambient, DECK.sky.glow = view.ambient, view.glow
  -- What lights the ground: the sun held just over the horizon through the
  -- twilight, and at night the moon, opposite it.
  if height < -6 then
    view.sun[1], view.sun[2], view.sun[3] = -east, -up, north
  else
    view.sun[1], view.sun[2], view.sun[3] = east, math.max(up, 0.03), -north
  end
  settings:set("time", t.key)
  say(("time %s at %.2f h: the sun %.1f degrees up, towards %.0f"):format(t.key, t.hour, height,
      (math.deg(math.atan(east, north)) + 360) % 360))
end
set_time(settings.time)

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
local with_field                       -- below, with the airports
local function where_now()
  return CITIES[settings.city]
         or (type(settings.place) == "table" and with_field(settings.place)) or nil
end

-- A place's metres east and south of where the terrain is centred, in the
-- terrain's own projection - Web Mercator, scaled at the centre's latitude
-- (`k3d_terrain.c`) - so the tower stands where its tiles draw the field.
local function metres_at(lon, lat)
  local function mercator(lo, la)
    local r = math.rad(la)
    return (lo + 180) / 360, (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2
  end
  local ox, oy = mercator(ANCHOR.lon, ANCHOR.lat)
  local mx, my = mercator(lon, lat)
  local scale = 40075016.68557849 * math.cos(math.rad(ANCHOR.lat))
  return (mx - ox) * scale, (my - oy) * scale
end

-- And back: where a point of the frame is on the Earth.
function ANCHOR.lonlat(x, z)
  local r = math.rad(ANCHOR.lat)
  local ox = (ANCHOR.lon + 180) / 360
  local oy = (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2
  local scale = 40075016.68557849 * math.cos(r)
  local mx, my = ox + x / scale, oy + z / scale
  -- Latitude from Mercator's y: 2 atan(e^y) - pi/2, Lua having no sinh.
  return mx * 360 - 180, math.deg(2 * math.atan(math.exp(math.pi * (1 - 2 * my))) - math.pi / 2)
end

-- **The airports** (F8): every paved runway OurAirports has, beside
-- Flight's other files - asked by code or name in the sheet, the nearest
-- one's runway given to any place found, and every runway near the
-- terrain's centre levelled under it. Read the first time it is wanted.
local airports_db, airports_why
local function airports_here()
  if airports_db == nil and airports_why == nil then
    airports_db, airports_why = airports.open(AIRCRAFT.folder .. "/airports.tsv")
    if not airports_db then say("airports none: " .. tostring(airports_why)) end
  end
  return airports_db
end

-- An airport, made a place: its longest runway's first end the runway a
-- start lines up on.
local function airport_place(a)
  local e = a.runways[1].ends[1]
  return { name = a.ident .. " \u{b7} " .. a.name,
           code = (a.iata or a.ident) .. " \u{b7} " .. a.name,
           note = a.town ~= "" and (a.town .. ", " .. a.country) or a.country,
           lon = a.lon, lat = a.lat, field = { a.lon, a.lat }, airport = a.ident,
           runway = { name = e.name, lon = e.lon, lat = e.lat, heading = e.heading,
                      feet = e.feet } }
end

-- A place found without an airfield given the nearest one within 25 km,
-- so a start on the runway is there wherever a field is near.
with_field = function(p)
  if not p or p.runway or p.invented or p.key then return p end
  local db = airports_here()
  local a = db and db:near(p.lon, p.lat, 25, 1)[1]
  if a then
    local f = airport_place(a)
    p.field, p.runway, p.airport = f.field, f.runway, f.airport
    p.note = (p.note and p.note ~= "" and (p.note .. "; ") or "")
             .. ("%s %s, %.0f km"):format(a.iata or a.ident, a.name, a.km)
  end
  return p
end

-- The runways near the terrain's centre, as the 3D Kit's flats - their
-- ends in the frame, their width and their ends' heights - and kept, to
-- say which runway a landing was on.
local RUNWAYS = {}
local function flats_here()
  RUNWAYS = {}
  local db = airports_here()
  if not db then return nil end
  local list, near = {}, db:near(ANCHOR.lon, ANCHOR.lat, 30, 24)
  for _, a in ipairs(near) do
    for _, r in ipairs(a.runways) do
      local x1, z1 = metres_at(r.ends[1].lon, r.ends[1].lat)
      local x2, z2 = metres_at(r.ends[2].lon, r.ends[2].lat)
      local width = math.max(10, math.min(900, (r.width or 75) * 0.3048))
      list[#list + 1] = { x1, z1, x2, z2, width, r.ends[1].feet * 0.3048,
                          r.ends[2].feet * 0.3048 }
      RUNWAYS[#RUNWAYS + 1] = { x1 = x1, z1 = z1, x2 = x2, z2 = z2, width = width,
                                h1 = r.ends[1].feet * 0.3048, h2 = r.ends[2].feet * 0.3048,
                                name = (a.iata or a.ident) .. " " .. r.name }
    end
  end
  say(("flats %d runways of %d airports within 30 km"):format(#list, #near))
  return list
end

-- The runway under a place, if one is: within its half-width and ten
-- metres, between its ends.
local function runway_at(x, z)
  for _, r in ipairs(RUNWAYS) do
    local dx, dz = r.x2 - r.x1, r.z2 - r.z1
    local len2 = dx * dx + dz * dz
    local t = len2 > 0 and ((x - r.x1) * dx + (z - r.z1) * dz) / len2 or -1
    if t >= 0 and t <= 1 then
      local ex, ez = x - (r.x1 + t * dx), z - (r.z1 + t * dz)
      if ex * ex + ez * ez <= (r.width / 2 + 10) ^ 2 then return r.name end
    end
  end
  return nil
end

here = where_now()

-- **The runways drawn** (F8b): each near runway a strip of asphalt from
-- its two ends at its width, its paint over it - the edges, the centre
-- line's dashes, the threshold's piano keys and the aiming point's two
-- blocks, at ICAO's sizes - made once a terrain as a shape of the 3D
-- Kit's, two parts for all of them: the asphalt, and the paint pulled a
-- little nearer than it so the two never fight for a pixel. The imagery
-- under it is a satellite's, a metre a texel at best, and from the
-- cockpit a runway is narrower than that is sharp.
local frame_models
do
local ASPHALT = { 0.13, 0.13, 0.14, 1 }
local PAINT = { 0.78, 0.78, 0.76, 1 }
local function runway_parts()
  local hard = { vertices = {}, indices = {}, colour = ASPHALT, pull = 0.004 }
  local paint = { vertices = {}, indices = {}, colour = PAINT, pull = 0.006 }
  for _, r in ipairs(RUNWAYS) do
    local dx, dz = r.x2 - r.x1, r.z2 - r.z1
    local len = math.sqrt(dx * dx + dz * dz)
    if len > 100 then
      local ux, uz = dx / len, dz / len          -- along, from the first end
      local nx, nz = -uz, ux                     -- across
      -- A rectangle along the runway from `a` to `b` metres, across from
      -- `c` to `d`, at the runway's height there, facing up.
      local function quad(part, a, b, c, d)
        local v, n = part.vertices, #part.vertices / 3
        for _, at in ipairs{ { a, c }, { b, c }, { b, d }, { a, d } } do
          local t = at[1] / len
          v[#v + 1] = r.x1 + ux * at[1] + nx * at[2]
          v[#v + 1] = r.h1 + (r.h2 - r.h1) * t
          v[#v + 1] = r.z1 + uz * at[1] + nz * at[2]
        end
        -- Wound so its face is up: a cross product's y, x east and z south.
        local i = part.indices
        if uz * nx - ux * nz > 0 then
          i[#i + 1], i[#i + 2], i[#i + 3] = n + 1, n + 2, n + 3
          i[#i + 1], i[#i + 2], i[#i + 3] = n + 1, n + 3, n + 4
        else
          i[#i + 1], i[#i + 2], i[#i + 3] = n + 1, n + 3, n + 2
          i[#i + 1], i[#i + 2], i[#i + 3] = n + 1, n + 4, n + 3
        end
      end
      local half = r.width / 2
      quad(hard, 0, len, -half, half)
      -- The edges, and the centre line: 30 m of paint and 20 of none.
      quad(paint, 0, len, -half + 0.5, -half + 1.4)
      quad(paint, 0, len, half - 1.4, half - 0.5)
      for a = 80, len - 110, 50 do quad(paint, a, a + 30, -0.45, 0.45) end
      -- At each end the piano keys, 1.8 m wide and 1.8 apart, 6 m in and
      -- 30 long; the aiming point's blocks 300 m in, 45 by 8.
      local keys = math.max(2, math.floor((r.width - 6) / 3.6 / 2)) -- each side
      for _, from in ipairs{ 0, len } do
        local into = from == 0 and 1 or -1
        local function along(a, b)
          local p, q = from + into * a, from + into * b
          return math.min(p, q), math.max(p, q)
        end
        local a, b = along(6, 36)
        for k = 0, keys - 1 do
          local c = 1.8 + k * 3.6
          if c + 1.8 < half - 1.5 then
            quad(paint, a, b, c, c + 1.8)
            quad(paint, a, b, -c - 1.8, -c)
          end
        end
        if len > 1200 then
          a, b = along(300, 345)
          local c = math.min(half - 3, 9)
          quad(paint, a, b, c - 8 > 1 and c - 8 or 1.5, c)
          quad(paint, a, b, -c, -(c - 8 > 1 and c - 8 or 1.5))
        end
      end
    end
  end
  if #hard.indices == 0 then return nil end
  return { hard, paint }, #hard.indices / 6, #paint.indices / 6
end

-- **The runway's lights** (F8c; Diego, 21 November: "the runway should
-- have lights as real runways"), laid as ICAO's Annex 14 has them for a
-- precision runway: edge lights every 60 m a side, white and yellow over
-- each end's last 600 m; the centre line's every 30 m; a row of green
-- across each threshold, just outside it, and of red just inside, the
-- end seen from the other way; and an approach line 900 m out from each
-- threshold, a light every 30 m, with a crossbar 300 m out. Each is three
-- quads 1.2 m a side crossed - two upright, one flat - 0.6 m over the
-- ground, so a row reads as a line out to the far end; a part a colour,
-- each a `light`, drawn its own colour and hazed (`k3d_model.frag`).
local LAMP = 0.6
local function light_parts()
  local colours = {
    white = { 1.0, 0.93, 0.78, 1 }, yellow = { 1.0, 0.78, 0.25, 1 },
    green = { 0.25, 1.0, 0.45, 1 }, red = { 1.0, 0.18, 0.12, 1 } }
  local parts, count = {}, 0
  for name, c in pairs(colours) do
    parts[name] = { vertices = {}, indices = {}, colour = c, light = true }
  end
  local function lamp(part, x, y, z)
    local v, i = part.vertices, part.indices
    local n = #v / 3
    y = y + LAMP
    for _, q in ipairs{ { LAMP, 0, 0, 0, LAMP, 0 }, { 0, 0, LAMP, 0, LAMP, 0 },
                        { LAMP, 0, 0, 0, 0, LAMP } } do
      local ax, ay, az, bx, by, bz = q[1], q[2], q[3], q[4], q[5], q[6]
      for _, k in ipairs{ { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } } do
        v[#v + 1] = x + k[1] * ax + k[2] * bx
        v[#v + 1] = y + k[1] * ay + k[2] * by
        v[#v + 1] = z + k[1] * az + k[2] * bz
      end
      local b = n
      i[#i + 1], i[#i + 2], i[#i + 3] = b + 1, b + 2, b + 3
      i[#i + 1], i[#i + 2], i[#i + 3] = b + 1, b + 3, b + 4
      n = n + 4
    end
    count = count + 1
  end
  for _, r in ipairs(RUNWAYS) do
    local dx, dz = r.x2 - r.x1, r.z2 - r.z1
    local len = math.sqrt(dx * dx + dz * dz)
    if len > 100 then
      local ux, uz = dx / len, dz / len
      local nx, nz = -uz, ux
      local half = r.width / 2
      -- `a` metres along from the first end (negative before it), `c`
      -- across; the height is the runway's, held at its ends beyond them.
      local function at(part, a, c)
        local t = math.max(0, math.min(1, a / len))
        lamp(part, r.x1 + ux * a + nx * c, r.h1 + (r.h2 - r.h1) * t, r.z1 + uz * a + nz * c)
      end
      for a = 0, len, 60 do
        local part = (a < 600 or a > len - 600) and parts.yellow or parts.white
        at(part, a, -half - 1.5)
        at(part, a, half + 1.5)
      end
      for a = 15, len - 15, 30 do at(parts.white, a, 0) end
      for _, from in ipairs{ 0, len } do
        local out = from == 0 and -1 or 1
        for c = -half, half + 0.1, 3 do
          at(parts.green, from + out * 1.5, c)
          at(parts.red, from - out * 1.5, c)
        end
        for d = 30, 900, 30 do at(parts.white, from + out * d, 0) end
        for c = -15, 15.1, 1.5 do at(parts.white, from + out * 300, c) end
      end
    end
  end
  local out = {}
  for _, name in ipairs{ "white", "yellow", "green", "red" } do
    if #parts[name].indices > 0 then out[#out + 1] = parts[name] end
  end
  if count == 0 then return nil end
  return out, count
end

-- The lights are lit from dusk to dawn, and under the cloud deck.
local function lights_lit()
  return settings.time == "dusk" or settings.time == "night" or settings.time == "dawn"
      or settings.weather == "cloud"
end

-- **The cloud deck** (W1): one grid 60 km a side over the terrain's centre,
-- a point every 500 m, its height lumped by a few long waves of where it
-- stands so the light on it has patches, as an overcast's base does. The
-- haze hides its edge. A shape of the 3D Kit's, made once and placed at
-- the deck's height.
local DECK_CELLS, DECK_SIDE = 120, 60000
local function deck_parts()
  local v, idx = {}, {}
  local n, step, half = DECK_CELLS, DECK_SIDE / DECK_CELLS, DECK_SIDE / 2
  for j = 0, n do
    for i = 0, n do
      local x, z = i * step - half, j * step - half
      local y = 28 * math.sin(x / 1700 + 0.7) * math.sin(z / 2300 + 1.9)
              + 16 * math.sin((x + 1.3 * z) / 830) + 9 * math.sin((1.7 * x - z) / 470 + 0.4)
      v[#v + 1], v[#v + 2], v[#v + 3] = x, y, z
    end
  end
  for j = 0, n - 1 do
    for i = 0, n - 1 do
      local a = j * (n + 1) + i + 1
      local b, c, d = a + 1, a + n + 1, a + n + 2
      -- Wound to face up: x east, z south.
      idx[#idx + 1], idx[#idx + 2], idx[#idx + 3] = a, c, b
      idx[#idx + 1], idx[#idx + 2], idx[#idx + 3] = b, c, d
    end
  end
  return { { vertices = v, indices = idx, colour = { 0.86, 0.87, 0.90, 1 } } }
end
local deck_at = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }

-- The shape for the runways `flats_here` found, made again when they are
-- other runways: the frame's models are the aircraft and it.
local strips, strips_of, drawn = nil, nil, {}
local lamps = { shape = nil, lit = nil }
function frame_models(w, with_plane)
  if strips_of ~= RUNWAYS then
    strips_of, strips = RUNWAYS, nil
    local parts, hard, paint = runway_parts()
    if parts then
      local why
      strips, why = w:shape(parts)
      say(strips and ("runways drawn %d strips %d marks"):format(hard, paint)
          or ("runways not drawn: " .. tostring(why)))
    end
    lamps.shape, lamps.lit = nil, nil
    local lparts, n = light_parts()
    if lparts then
      local why
      lamps.shape, why = w:shape(lparts)
      say(lamps.shape and ("runway lights made %d lights in %d colours"):format(n, #lparts)
          or ("runway lights not made: " .. tostring(why)))
    end
  end
  local lit = lights_lit() and settings.runways and lamps.shape ~= nil
  if lit ~= lamps.lit then
    lamps.lit = lit
    say("runway lights " .. (lit and "on" or "off"))
  end
  for i = #drawn, 1, -1 do drawn[i] = nil end
  if with_plane then drawn[#drawn + 1] = with_plane end
  if strips and settings.runways then drawn[#drawn + 1] = strips end
  if lamps.lit then drawn[#drawn + 1] = lamps.shape end
  if settings.weather == "cloud" and DECK.h then
    if not DECK.shape then
      local why
      DECK.shape, why = w:shape(deck_parts())
      if not DECK.shape then say("cloud deck not made: " .. tostring(why)) DECK.h = nil
      else say(("cloud deck made %d triangles at %.0f m"):format(2 * DECK_CELLS * DECK_CELLS, DECK.h)) end
    end
    if DECK.shape then
      deck_at[14] = DECK.h
      DECK.shape:place(deck_at)
      drawn[#drawn + 1] = DECK.shape
    end
  end
  return drawn
end
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
  S.v = PLANE.speeds.cruise          -- airspeed, m/s
  S.gamma = 0                        -- the path's climb, radians
  S.psi = 0                          -- heading: 0 north, clockwise
  S.phi = 0                          -- roll
  S.alpha = 0.02                     -- angle of attack
  S.throttle = PLANE.speeds.cruise_throttle
  S.flaps = 0
  S.ground = false
  S.crashed = false
  S.paused = false
  S.spool = nil                      -- a jet's spool, taken from the throttle at once
  S.spool_q, S.simt, S.rolled = 0, 0, nil
  S.ap = {}                          -- the autopilot off
  local rw = here and here.runway
  local how = rw and settings.start or "city"
  if how ~= "city" then
    local tx, tz = metres_at(rw.lon, rw.lat)
    local psi, elevation = math.rad(rw.heading), rw.feet * 0.3048
    S.psi = psi
    if how == "runway" then
      S.x, S.z, S.y = tx, tz, elevation + PLANE.gear
      S.v, S.throttle, S.alpha, S.ground = 0, 0, 0, true
      S.rolled = { x = S.x, z = S.z, t = 0 }    -- where a take-off's roll begins
    else
      -- The path aims at the touchdown zone, 300 m past the threshold, as
      -- a runway's glide path does (F8): aimed at the threshold itself the
      -- wheels met the ground short of the runway.
      local back = settings.final
      local ax, az = tx + math.sin(psi) * 300, tz - math.cos(psi) * 300
      S.x, S.z = ax - math.sin(psi) * back, az + math.cos(psi) * back
      S.y = elevation + back * math.tan(math.rad(3)) + PLANE.gear
      local sp = PLANE.speeds
      S.v, S.gamma, S.flaps, S.throttle = sp.approach, -math.rad(3), sp.approach_flaps,
                                          sp.approach_throttle
    end
  end
  S.started, S.ground_said, S.landed, S.rollout = how, nil, nil, nil
  say(("wind from %03d at %d kt"):format(settings.wind_from or 270, settings.wind or 0))
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
    local ahead, high = PLANE.eye.ahead, PLANE.eye.high
    e[1], e[2], e[3] = S.x + fx * ahead, S.y + high + fy * ahead, S.z + fz * ahead
    t[1], t[2], t[3] = e[1] + fx * 100, e[2] + fy * 100, e[3] + fz * 100
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
    -- Above whatever is built there (B3): at Heathrow the field's point is
    -- among the terminals, and an eye 30 m up stood inside one.
    local roof = g:top(tower.x, tower.z, 15)
    e[1], e[2], e[3] = tower.x, math.max((tower.ground or 0) + 30, (roof or -1e9) + 8), tower.z
    t[1], t[2], t[3] = S.x, S.y, S.z
    u[1], u[2], u[3] = 0, 1, 0
    local dx, dy, dz = S.x - e[1], S.y - e[2], S.z - e[3]
    tower.distance = math.sqrt(dx * dx + dy * dy + dz * dz)
    tower.focal = math.max(FOCAL, math.min(400, tower.distance / 60))
    view.focal = tower.focal
  else
    -- Chase: behind along the nose - its pitch too - and a little above,
    -- looking at a point just ahead of the aircraft, with the horizon
    -- level. It looked 40 m ahead from a fixed 7 m above, so in a steep
    -- climb the aim rose while the eye did not and the aircraft left the
    -- bottom of the picture (Diego, 21 November: "when flying a lot of
    -- times the plane is out of frame", "in chase view").
    -- How far behind and above is the aircraft's own (`chase`): the
    -- Cessna's 28 m and 7 m, and as much further for a bigger one.
    local back, high = PLANE.chase.back, PLANE.chase.high
    local ahead = back * 0.43
    e[1], e[2], e[3] = S.x - fx * back, S.y - fy * back + high, S.z - fz * back
    t[1], t[2], t[3] = S.x + fx * ahead, S.y + fy * ahead + high * 0.29, S.z + fz * ahead
    u[1], u[2], u[3] = 0, 1, 0
    -- Climbing away from a runway the eye behind is lower than the
    -- aircraft: never under the ground there.
    e[2] = math.max(e[2], (g:height(e[1], e[3]) or -1e9) + 2)
  end
  -- **The sun's shadows** (S2): the 3D Kit's map over a box on the ground
  -- under the aircraft, reaching further ahead than behind since that is
  -- where the eye looks, and wider the higher it flies - 400 m a side on a
  -- runway, 0.4 m a texel, to 2 km at 1,600 m above the ground. The
  -- tower's box is the aircraft's own, since that is what it watches.
  -- The weather's sky, for where the eye is; the deck over the field
  -- once the field's height is known.
  if settings.weather == "cloud" and not DECK.h then
    local field = g:height(tower.x or 0, tower.z or 0)
    if field then
      DECK.h = field + DECK.above
      say(("weather cloud deck=%.0f m"):format(DECK.h))
    end
  end
  local sunny = DECK.weather_sky(e[2])
  local state = settings.weather
  if state == "cloud" then
    state = not DECK.h and "cloud waiting" or e[2] > DECK.h + DECK.thick and "cloud over"
            or e[2] > DECK.h - DECK.thick and "cloud in" or "cloud under"
  end
  if state ~= DECK.said then
    DECK.said = state
    say(("weather %s haze=%.0f-%.0f sun=%.2f ambient=%.2f"):format(state, view.haze_start,
        view.haze_end, view.light[2], view.ambient))
  end
  if settings.shadows and sunny then
    local ground = g:height(S.x, S.z) or 0
    local radius = math.max(400, math.min(2000, 400 + (S.y - ground)))
    local ahead = S.view == 3 and 0 or radius * 0.5
    local c = view.shadow_centre
    c[1], c[2], c[3] = S.x + sh * ahead, ground, S.z - ch * ahead
    view.shadow_radius = radius
  else
    view.shadow_radius = nil
  end
end

-- **The aircraft where it is**: the model's frame is glTF's - its nose
-- along +X, up +Y, the right wing +Z, metres, the wheels at 0 - so the
-- world from it has the aircraft's forward, up and right as its columns,
-- and its wheels' place as the fourth. One table, refilled a frame.
local placed = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }

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
  local tx, ty, tz = S.x, S.y - PLANE.gear, S.z
  local axes = PLANE.axes
  if axes then
    -- The model's own frame into Flight's: each of its axes is where its
    -- forward, up and right parts take it, scaled to metres, and the
    -- origin moved to the point it stands on.
    local sc, o = PLANE.pose.scale, PLANE.pose.origin
    local f, u, r = axes[1], axes[2], axes[3]
    for k = 1, 3 do
      local i = (k - 1) * 4
      placed[i + 1] = sc * (fx * f[k] + Ux * u[k] + Rx * r[k])
      placed[i + 2] = sc * (fy * f[k] + Uy * u[k] + Ry * r[k])
      placed[i + 3] = sc * (fz * f[k] + Uz * u[k] + Rz * r[k])
    end
    local of = o[1] * f[1] + o[2] * f[2] + o[3] * f[3]
    local ou = o[1] * u[1] + o[2] * u[2] + o[3] * u[3]
    local orr = o[1] * r[1] + o[2] * r[2] + o[3] * r[3]
    tx = tx - sc * (fx * of + Ux * ou + Rx * orr)
    ty = ty - sc * (fy * of + Uy * ou + Ry * orr)
    tz = tz - sc * (fz * of + Uz * ou + Rz * orr)
  else
    placed[1], placed[2], placed[3] = fx, fy, fz
    placed[5], placed[6], placed[7] = Ux, Uy, Uz
    placed[9], placed[10], placed[11] = Rx, Ry, Rz
  end
  placed[13], placed[14], placed[15] = tx, ty, tz
  plane:place(placed)
  -- The surfaces follow the yoke, and the propeller the throttle - those
  -- of them the model has.
  local sf = PLANE.surface
  if sf.elevator then plane:turn(sf.elevator, math.rad(20) * pitch) end
  if sf.right then plane:turn(sf.right, math.rad(15) * roll) end
  if sf.left then plane:turn(sf.left, -math.rad(15) * roll) end
  if sf.flaps then plane:turn(sf.flaps, -math.rad(30) * S.flaps) end
  -- **The propeller as it is seen** (F6c). Turned at its true rate - 40
  -- turns a second at full power - two blades come round to where they
  -- were between one frame and the next, and it stood still or crawled
  -- backwards. A camera shows a propeller turning slower than it does, and
  -- this does too: from three turns a second at idle to twelve at full
  -- power, and never more than a sixth of a turn a frame, which is a third
  -- of the way from one blade to the next and turns forwards at any rate.
  local turns = math.min(3 + 9 * S.throttle, (1 / 6) / math.max(dt, 1e-3))
  S.prop_step = turns * dt
  S.prop = ((S.prop or 0) + turns * 2 * math.pi * dt) % (2 * math.pi)
  if sf.prop then plane:spin(sf.prop, S.prop) end
end

-- **The engine and the wind, heard** (F6c), on the Synth Kit - the
-- engine Groove plays, in the audio band, into a stream of Flight's own
-- in the Mixer. Two tracks hold a note each for as long as Flight flies:
-- the engine, a saw and a square an octave down with a sine under them,
-- through a low-pass that opens with the throttle and a wobble at the
-- blades' rate; and the wind, noise through a band-pass that rises and
-- grows with the airspeed. The engine's pitch is its firing rate - a
-- four-cylinder fires twice a turn, so 700 turns a minute at idle are 23
-- a second and 2700 at full power 90 - bent from E2, 82 Hz, which is as
-- far as the kit's bend reaches either side. What changes is said to the
-- kit as a song, at most ten times a second and only when it moved: the
-- kit reads a track's parameters every block, so the held notes go on.
-- The note and the rates are the aircraft's (`sound`, `engine`): a
-- Cessna's are E2 and 700 to 2700 turns a minute.
local engine = { on = false, sent = 0, said = false }
local engine_song = {
  bpm = 120, master = 0.8,
  tracks = {
    { type = "synth", vol = 0.55, params = {
        w1 = 1, w2 = 2, oct2 = -1, mix = 0.35, sub = 0.5, noise = 0.04, mono = 2,
        ftype = 1, cut = 400, res = 0.25, env = 0, aA = 0.4, aD = 1, aS = 1, aR = 0.6,
        lfoR = 10, lfoC = 0.12, lfoP = 0, lvl = 0.9, uni = 1, det = 0, glide = 0 } },
    { type = "synth", vol = 0, params = {
        w1 = 4, w2 = 4, oct2 = -2, mix = 1, sub = 0, noise = 1, mono = 2,
        -- No sweep: band-passed noise whose cutoff a slow LFO swings is
        -- the sea on a beach, which is what Diego heard (21 November: "some
        -- weird sea waves sound beyond the propeller sound"). The wind is a
        -- steady rush that follows the airspeed alone.
        ftype = 2, cut = 600, res = 0.1, env = 0, aA = 0.8, aD = 1, aS = 1, aR = 0.8,
        lfoR = 0.3, lfoC = 0, lfoP = 0, lvl = 0.6, uni = 1, det = 0, glide = 0 } },
    -- The tyres meeting the runway (Diego, 21 November: "we need a way to
    -- hear the landing sound like wheels squeaking"): a short high chirp,
    -- a square and a little noise through a band-pass around 1.8 kHz,
    -- struck at touchdown as hard as the landing was and gone in a third
    -- of a second; a second, softer, for the other wheel.
    { type = "synth", vol = 0.5, params = {
        w1 = 3, w2 = 1, oct2 = 0, mix = 0.3, sub = 0, noise = 0.35, mono = 1,
        ftype = 2, cut = 1800, res = 0.55, env = 0, aA = 0.004, aD = 0.22, aS = 0, aR = 0.08,
        lfoR = 18, lfoC = 0.25, lfoP = 0, lvl = 0.9, uni = 1, det = 0, glide = 0 } },
  },
}

local function engine_start()
  if engine.on or not settings.sound then return end
  local out, why = audio.open("Flight", 8)
  if out then
    local ok, err = pcall(synth.start, out.ring, out.rate)
    if not ok then out:close() out, why = nil, err end
  end
  if not out then
    say("sound none: " .. tostring(why))
    settings.sound = false                   -- not asked again this flight
    return
  end
  engine.out, engine.on, engine.sent = out, true, 0
  -- The engine's voice is the aircraft's (`sound.voice`): what it names
  -- over the piston's, the piston's kept aside to start from each time.
  local e = engine_song.tracks[1]
  if not engine.base then
    engine.base = {}
    for key, v in pairs(e.params) do engine.base[key] = v end
  end
  for key, v in pairs(engine.base) do e.params[key] = v end
  for key, v in pairs(type(PLANE.sound.voice) == "table" and PLANE.sound.voice or {}) do
    if type(v) == "number" and e.params[key] ~= nil then e.params[key] = v end
  end
  synth.song(engine_song)
  synth.note_on(1, PLANE.sound.note, 0.8)
  synth.note_on(2, 24, 0.8)
  say("sound on: the engine and the wind, into the stream Flight")
end

local function engine_stop()
  if not engine.on then return end
  synth.close()
  engine.out:close()
  engine.on, engine.out = false, nil
end

local engine_state = {}
local function engine_sound(t)
  if not settings.sound then engine_stop() return end
  engine_start()
  local sq = engine.squeak
  if engine.on and sq then
    if not sq.second and t - sq.t > 0.12 then
      synth.note_off(3, 90)
      synth.note_on(3, 88, sq.hard * 0.6)
      sq.second = true
    elseif t - sq.t > 0.5 then
      synth.note_off(3, 88)
      engine.squeak = nil
    end
  end
  if not engine.on or t - engine.sent < 0.1 then return end
  local en = PLANE.engine
  local rpm = math.min(en.full_rpm + 50, en.idle_rpm + (en.full_rpm - en.idle_rpm) * (S.spool or S.throttle)
                                         + (en.kind == "jet" and 0 or 8 * S.v))
  local hz = rpm / 30
  local e, wind = engine_song.tracks[1], engine_song.tracks[2]
  local cut = math.floor(250 + 1400 * (S.spool or S.throttle) + 4 * S.v)
  local wcut = math.floor(300 + 30 * S.v)
  local wvol = math.min(0.2, S.v / 70 * 0.2)
  e.params.lfoR = math.floor(hz / 6 * 10 + 0.5) / 10
  if math.abs(e.params.cut - cut) > 15 or math.abs(wind.vol - wvol) > 0.01
     or math.abs(wind.params.cut - wcut) > 15 or engine.sent == 0 then
    e.params.cut, wind.params.cut, wind.vol = cut, wcut, wvol
    synth.song(engine_song)
  end
  synth.bend(1, 12 * math.log(hz / PLANE.sound.hz, 2))
  engine.sent = t
  engine.rpm = rpm
  if not engine.said then
    local st = synth.state(engine_state)
    -- Said once the engine is past its attack: the first period alone
    -- is the envelope's rise, and its peak says nothing of the sound.
    if (st.rendered or 0) >= 4096 then
      say(("sound engine rpm=%.0f hz=%.1f cut=%d wind=%.2f rendered=%d peak=%.3f")
          :format(rpm, hz, cut, wvol, st.rendered, math.max(st.peak_l or 0, st.peak_r or 0)))
      engine.said = true
    end
  end
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
  -- The buildings' outlines, the map's own vector tiles, shared with Maps'
  -- cache; a flight without them is still a flight.
  net.vector = nil
  if settings.buildings then
    net.vector, why = tiles.source(settings.vector, "pbf")
    if not net.vector then say("buildings none: " .. tostring(why)) end
  end
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

-- **The minimap** (F5k; Diego, 21 November: "We should add a small map
-- that shows where we are and heading so the user can orientate itself"):
-- a square of map in the lower left corner, north up, the aircraft at its
-- middle as an arrow along its heading, the runways known as bars, and
-- the scale under it. Drawn by the Map Kit in Maps' own style
-- (`mapstyle.lua`) from the vector tiles the buildings already come from -
-- zoom 13 below 600 m over the ground, 12 above - asked in the same list,
-- each decoded once and kept while it is under the map. Drawn again twice
-- a second, or when the aircraft has moved five points on it.
local MINI = { side = 220, tile = 256, kept = {}, wanted = {}, z = 12, map = use("/Kosmos/Kits/map") }
MINI.colours, MINI.style = use("/Kosmos/Libraries/mapstyle.lua").style_for(true)

-- The aircraft's place across the world, 0 to 1, as the Map Kit's.
function MINI.world(x, z)
  local ox, oy = MINI.map.project(ANCHOR.lon, ANCHOR.lat)
  local scale = 40075016.68557849 * math.cos(math.rad(ANCHOR.lat))
  return ox + x / scale, oy + z / scale, scale
end

-- The tiles under the map now, `MINI.wanted` as { z, x, y }; and where
-- the map's top left is, in pixels of zoom `z`.
function MINI.under(ground_h)
  local above = S.y - (ground_h or 0)
  local z = above < 600 and 13 or 12
  local wx, wy = MINI.world(S.x, S.z)
  local px = MINI.tile * (1 << z)
  local left, top = wx * px - MINI.side / 2, wy * px - MINI.side / 2
  local want = MINI.wanted
  for i = #want, 1, -1 do want[i] = nil end
  for ty = math.floor(top / MINI.tile), math.floor((top + MINI.side) / MINI.tile) do
    for tx = math.floor(left / MINI.tile), math.floor((left + MINI.side) / MINI.tile) do
      want[#want + 1] = { z, tx, ty }
    end
  end
  MINI.z, MINI.left, MINI.top, MINI.px = z, left, top, px
  -- Kept while under the map: past two dozen, those not under it go.
  MINI.count = (MINI.count or 0)
  if MINI.count > 24 then
    local under = {}
    for _, tw in ipairs(want) do under[(tw[1] << 50) | (tw[2] << 25) | tw[3]] = true end
    MINI.count = 0
    for id in pairs(MINI.kept) do
      if under[id] then MINI.count = MINI.count + 1 else MINI.kept[id] = nil end
    end
  end
  return want
end

-- One tile of the map: kept, read from the cache and decoded, or false
-- while it has not come (asked for in `feed`).
function MINI.get(z, x, y)
  local id = (z << 50) | (x << 25) | y
  local t = MINI.kept[id]
  if t ~= nil then return t end
  if not net.vector then return false end
  local bytes = fs.read(net.vector:path(z, x, y))
  if type(bytes) ~= "string" then return false end
  t = #bytes > 0 and MINI.map.decode(bytes) or false
  MINI.kept[id] = t
  MINI.count = (MINI.count or 0) + 1
  MINI.decoded = (MINI.decoded or 0) + 1
  say(("minimap tile %d/%d/%d decoded%s, %d kept"):format(z, x, y, t and "" or " (empty)", MINI.count))
  return t
end

local function hand(g, kind, z, x, y)
  -- A vector tile is read by the terrain itself, never a Lua string: a
  -- city's is most of a megabyte.
  if kind == "v" then
    if not net.vector or not g:buildings(z, x, y, net.vector:path(z, x, y)) then return false end
    handed = handed + 1
    return true
  end
  local src = kind == "e" and net.elevation or net.imagery      -- "r", a roof's: the imagery
  local bytes = fs.read(src:path(z, x, y))
  if not bytes then return false end
  -- What did not come is the sea - but a roof's piece is left to the
  -- terrain, which lays the walls' concrete there instead (nil).
  local missing_picture = kind == "e" and SEA_LEVEL or kind == "i" and SEA or nil
  local picture
  if #bytes == 0 then
    picture = missing_picture
  else
    local ok, made = pcall(decoders[src.format], bytes)
    if not ok or not made then picture = missing_picture else picture = made end
  end
  if kind == "e" then g:elevation(z, x, y, picture)
  elseif kind == "r" then g:roof(z, x, y, picture)
  else g:imagery(z, x, y, picture) end
  handed = handed + 1
  return true
end

local asked = { e = {}, i = {}, v = {} }   -- flat lists of z, x, y, reused
local asking = { e = 0, i = 0, v = 0, budget = {}, polled = 0 }
-- Tiles looked for in the cache and not there: not looked for again until
-- the server says one has come, so a frame does not ask the disk for every
-- tile still on its way.
local missing = { e = {}, i = {}, v = {} }
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

-- **The finest zooms only near the ground**: zoom 17 under 150 m above
-- it, 16 under 400, and 15 above - the eye's height over the terrain under
-- it, so a high field is low as a low one is. At cruise a zoom-17 chunk is
-- 190 m and changes under the eye every few seconds, each one a picture to
-- decode; on the M700 that was 19.6 ms of processor a frame at 600 m,
-- against 3.5 with 15 the deepest.
local function feed(g, eye)
  do
    local above = eye[2] - (g:height(eye[1], eye[3]) or 0)
    g:deepest(above < 150 and 17 or above < 400 and 16 or 15)
  end
  if offline.on then return feed_invented(g, eye) end
  asking.e, asking.i, asking.v = 0, 0, 0
  -- **A budget of pictures a frame**: each tile handed is read from the
  -- cache and decoded here, on the frame's thread, about a millisecond
  -- apiece on the M700. Without a limit a turn low over a city handed
  -- dozens in one frame - 21.8 ms of processor at 100 m with zoom 17 near
  -- the eye. What is over the budget is handed in the frames after, and
  -- draws its parent meanwhile. A budget for each kind, so the imagery
  -- of a wide view does not starve the buildings and their roofs.
  -- Elevation has none: the ground under the wheels and the runways
  -- levelled wait on it, and it stops at zoom 13, so a view wants few - a
  -- budget of four held the height under Rio's 02R back past the suite's
  -- wait under TCG, where a frame is four seconds.
  local budget = asking.budget
  budget.e, budget.i, budget.r, budget.v = math.huge, 8, 8, 2
  for _, t in ipairs(g:wanted(eye)) do
    -- A roof's piece ("r") is the imagery source's, asked in its one list:
    -- a source's `want` replaces the list before it.
    local kind = t[1] == "r" and "i" or t[1]
    local id = key(t[2], t[3], t[4])
    local gone = missing[kind]
    local tried = not gone[id] and budget[t[1]] > 0
    if tried then budget[t[1]] = budget[t[1]] - 1 end
    if (gone[id] or (tried and not hand(g, t[1], t[2], t[3], t[4]))) and asking[kind] < tiles.MAX then
      gone[id] = true
      local list, n = asked[kind], asking[kind] * 3
      list[n + 1], list[n + 2], list[n + 3] = t[2], t[3], t[4]
      asking[kind] = asking[kind] + 1
    end
  end
  -- The minimap's tiles (F5k), in the buildings' list: each not yet here
  -- is asked for, as the terrain's are.
  if net.vector then
    for _, tw in ipairs(MINI.wanted) do
      local id = key(tw[1], tw[2], tw[3])
      if MINI.kept[id] == nil and (missing.v[id] or not MINI.get(tw[1], tw[2], tw[3]))
         and asking.v < tiles.MAX then
        missing.v[id] = true
        local list, n = asked.v, asking.v * 3
        list[n + 1], list[n + 2], list[n + 3] = tw[1], tw[2], tw[3]
        asking.v = asking.v + 1
      end
    end
  end
  -- Ask the server on our own clock, never more than five times a second:
  -- each ask is an exchange, and a tile is a hundred milliseconds away.
  local t = now()
  if t - asking.polled < 0.2 then return end
  asking.polled = t
  for kind, src in pairs({ e = net.elevation, i = net.imagery, v = net.vector or false }) do
    if not src then goto next_source end
    local list = asked[kind]
    for i = asking[kind] * 3 + 1, #list do list[i] = nil end
    src:want(list)
    -- What has come is read from the cache the next time it is wanted.
    local came = src:arrived()
    for _, c in ipairs(came or {}) do missing[kind][key(c.z, c.x, c.y)] = nil end
    ::next_source::
  end
end

local frame = 0
-- **A game controller** (F7): the left stick is the yoke, the right trigger
-- opens the throttle and the left one closes it, A holds the brakes and B
-- steps the flaps - added to the keys, so either flies. `w:pad` answers only
-- while this window is in front; the stick's middle tenth is nothing, so a
-- pad at rest does not fly the aeroplane.
local PAD = { dead = 0.1, was = 0, said = false }

function PAD.read(w)
  local lx, ly, _, _, lt, rt, buttons = w:pad(1)

  if not lx then return 0, 0, 0, 0 end
  if not PAD.said then
    say("pad in front")
    PAD.said = true
  end

  local function stick(v)
    if math.abs(v) < PAD.dead then return 0 end
    return (v - (v > 0 and PAD.dead or -PAD.dead)) / (1 - PAD.dead)
  end

  local pressed = buttons & ~PAD.was
  PAD.was = buttons
  if pressed & 2 ~= 0 then S.flaps = (S.flaps + 0.5) % 1.5 end

  return -stick(ly), stick(lx), rt - lt, (buttons & 1 ~= 0) and 1 or 0
end

-- The keys and the pad together: the yoke, the throttle moved, the brakes.
function PAD.controls(w, dt)
  local pp, pr, pt, pb = PAD.read(w)
  local function key(code) return w:held(code) and 1 or 0 end
  local function clamp(v) return math.max(-1, math.min(1, v)) end

  local pitch = clamp(key(KEY.down) - key(KEY.up) + pp)
  local roll = clamp(key(KEY.right) - key(KEY.left) + pr)
  pt = clamp(pt + key(KEY.a) - key(KEY.z) + key(KEY.pgup) - key(KEY.pgdn)
             + key(KEY.equal) + key(KEY.kpplus) - key(KEY.minus) - key(KEY.kpminus))
  -- The autopilot on (F10b): what the yoke and the throttle would have
  -- done they do to what it holds instead - the arrows turn its heading
  -- 20 degrees a second and its height 300 ft, A and Z its speed 5 kt -
  -- and it flies; a number key is the pilot taking the power, and lets go.
  local ap = S.ap
  if ap.on then
    for _, code in ipairs(KEY.digits) do
      if w:pressed(code) then ap.on = false say("autopilot off: the throttle taken") end
    end
  end
  if ap.on and ap.plan and (roll ~= 0 or pitch ~= 0 or pt ~= 0) then
    ap.plan, ap.vs_path = nil, nil
    say("route left: the autopilot holds what the keys set")
  end
  if ap.on then
    ap.heading = ap.heading + roll * math.rad(20) * dt
    ap.alt = math.max(150, ap.alt + pitch * 91.44 * dt)   -- down, nose up: higher
    ap.speed = math.max(PLANE.speeds.approach, ap.speed + pt * 2.572 * dt)
    S.braking = 0
    return 0, 0, 0
  end
  for i, code in ipairs(KEY.digits) do
    if w:pressed(code) then S.throttle = (i - 1) / 10 end
  end
  S.throttle = math.max(0, math.min(1, S.throttle + 0.5 * pt * dt))
  S.braking = math.max(pb, key(KEY.b), key(KEY.space))
  return pitch, roll, S.braking
end

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
  -- With the autopilot on (F10b), each label carries what it holds, in
  -- the amber AUTOPILOT is said in: "AP" as well did not fit a cell.
  local ap = S.ap.on and S.ap
  local function held(label, v) return ap and ("%s %s"):format(label, v) or label end
  local words = {
    { held("SPEED", ap and math.floor(ap.speed * 1.94384 + 0.5)),
      ("%d kt"):format(math.floor(S.v * 1.94384 + 0.5)) },
    { held("ALTITUDE", ap and math.floor(ap.alt * 3.28084 / 10 + 0.5) * 10),
      ("%d ft"):format(math.floor(S.y * 3.28084 / 10 + 0.5) * 10) },
    { held("HEADING", ap and ("%03d"):format(math.floor(math.deg(ap.heading) % 360 + 0.5) % 360)),
      ("%03d\u{b0}"):format(math.floor(math.deg(S.psi) % 360 + 0.5) % 360) },
    { "CLIMB", ("%+d fpm"):format(math.floor(S.v * math.sin(S.gamma) * 196.85 / 50 + 0.5) * 50) },
    -- The keys beside it, since a landing is flown on it (Diego, 21
    -- November: "the aircraft needs a throttle"): it had A and Z, said
    -- only in the sheet.
    { "THROTTLE A/Z", ("%d%%"):format(math.floor(S.throttle * 100 + 0.5)) },
    { "FLAPS", ("%d\u{b0}"):format(math.floor(S.flaps * 30 + 0.5)) },
  }
  local state = S.paused and "PAUSED" or S.alpha > PLANE.stall and "STALL"
    or S.ground and (S.braking or 0) > 0 and "BRAKES"
                or ground_h and (S.y - ground_h) < 3 and "ON THE GROUND" or nil
  -- The time faster (F11), said only while it is, in the state's place on
  -- the right: as a seventh cell it was the one a 1280-point window had
  -- no room for, and went unseen at the window's own size.
  if S.rate > 1 then
    state = state and ("%dx  %s"):format(S.rate, state) or ("TIME %dx"):format(S.rate)
  end
  state = state or ap and (ap.plan and ("NAV %s %d nm"):format(ap.plan.to.iata or ap.plan.to.ident,
                                          math.floor((ap.plan.left or 0) + 0.5))
                           or "AUTOPILOT") or nil
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
    x = strip:text(x, y + 3, words[i][1], S.ap.on and i <= 3 and WARN or LABEL) + 8
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
local times = { at = 0, n = 0 }                   -- and where the next goes, and how many
local processor_ms = nil                           -- the frame less the device's, smoothed
for i = 1, TIMES do times[i] = 0 end
local spark_xy = {}
local graphics, graphics_said, graphics_next = nil, false, 0
local shadows_said = false
local face_big, face_small, face_mono
local rate = { n = 0, since = 0, fps = 0 }

local function count_of(n)
  if n >= 1e6 then return ("%.2f M"):format(n / 1e6) end
  if n >= 1e4 then return ("%.1f k"):format(n / 1e3) end
  return tostring(n)
end

local function draw_graphics(w, said, took, t)
  times.at = times.at % TIMES + 1
  times[times.at] = took
  times.n = math.min(TIMES, times.n + 1)
  -- The processor's part of a frame: the frame less the device's time,
  -- which it waits for, every frame - smoothed over about a second's worth.
  local draws, triangles, gpu_ms, presented, shadowed = w:stats()
  local own = math.max(0, took * 1000 - (gpu_ms or 0))
  processor_ms = processor_ms and processor_ms + (own - processor_ms) * 0.1 or own
  rate.n = rate.n + 1
  if t - rate.since >= 1 then
    rate.fps = rate.n / (t - rate.since)
    rate.n, rate.since = 0, t
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
      graphics = gfx.surface{ w = 262, h = 286 }
    else
      graphics = gfx.surface{ w = math.min(settings.width - 8, 64), h = 18 }
    end
  end
  local gw, gh = graphics:size()
  graphics:fill(0, 0, gw, gh, GLASS)
  local fps = ("%.1f"):format(rate.fps)

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
    local first = times.n < TIMES and 1 or times.at + 1
    local budget = sy + sh - math.floor(sh * (1 / 60) / most + 0.5)
    graphics:fill(sx, budget, sw, 1, BUDGET)
    local n = 0
    for i = 1, times.n do
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
      { "Shadows  H", view.shadow_radius
          and ("%s, %.1f m"):format(count_of(shadowed or 0), view.shadow_radius / 1024) or "off" },
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
  if not graphics_said and rate.fps > 0 then
    say(("graphics %dx%d at %d,%d device=%s draws=%d triangles=%d gpu_ms=%s frame_ms=%.2f "
         .. "processor_ms=%.2f presented=%s")
        :format(gw, gh, gx, gy, w:device():match("^%S+"), draws, triangles,
                gpu_ms and ("%.2f"):format(gpu_ms) or "none", took * 1000, processor_ms,
                presented))
    graphics_said = true
  end
  -- The shadows, once each time they are turned on, for the suite.
  if view.shadow_radius and not shadows_said and (shadowed or 0) > 0 then
    say(("shadows radius=%.0f texel=%.2f triangles=%d"):format(view.shadow_radius,
        view.shadow_radius / 1024, shadowed))
    shadows_said = true
  elseif not view.shadow_radius then
    shadows_said = false
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

-- **The view, said** (F6b; Diego, 20 November 2026: "don't know how to
-- change cockpit to chase view"). `V` stepped the views and nothing on the
-- screen said so. A chip in the top right corner of every view names it
-- and its key - under the graphics panel when that is open - and a click
-- on it is `V`. Redrawn only when the view or its place changes.
local VIEWS = { "Chase", "Cockpit", "Tower" }
local view_chip, view_chip_face, view_chip_said, view_chip_box

local function draw_view_chip(w)
  local y = 16
  if graphics then
    local _, gh = graphics:size()
    y = 16 + gh + 8
  end
  local key = S.view .. ":" .. y
  if key == view_chip_said then return end
  if settings.width < 120 then return end
  view_chip_face = view_chip_face or gfx.face("ibmplexsans-medium", 13) or gfx.face("ibmplexsans", 13)
  local words = (settings.width >= 480 and "View: " or "") .. VIEWS[S.view]
  local cw = gfx.measure(words, view_chip_face) + gfx.measure("V", view_chip_face) + 44
  view_chip = gfx.surface{ w = cw, h = 30 }
  view_chip:fill(0, 0, cw, 30, 0x00000000)
  view_chip:fill_round(0, 0, cw, 30, 0xe0161a20, 9)
  local after = view_chip:text(12, 6, words, 0xfff4f6f9, nil, view_chip_face)
  view_chip:fill_round(after + 10, 5, cw - after - 18, 20, 0xff4c8dff, 5)
  view_chip:text(after + 16, 6, "V", 0xffffffff, nil, view_chip_face)
  -- A narrow window has no place box, and its top left is free.
  local x = settings.width >= 480 and settings.width - cw - 16 or 4
  if settings.width < 480 then y = graphics and 26 or 4 end
  local ok, why = w:overlay(view_chip, x, y, 6)
  if not ok then say("view chip failed=" .. tostring(why)) return end
  view_chip_box = { x, y, cw, 30 }
  say(("view %s laid at %d,%d %dx30"):format(VIEWS[S.view]:lower(), x, y, cw))
  view_chip_said = key
end

local function on_view_chip(cx, cy)
  local b = view_chip_box
  return b and cx and cx >= b[1] and cx < b[1] + b[3] and cy >= b[2] and cy < b[2] + b[4]
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

-- The minimap drawn (F5k), here where the sheet's colours and the
-- strip's height are known.
function MINI.draw(w, ground_h, t)
  local side = MINI.side
  local width, height = settings.width, settings.height
  if width < 600 or height < 420 then
    if not MINI.small_said then say(("minimap not laid: %dx%d"):format(width, height)) end
    MINI.small_said = true
    MINI.under(ground_h)
    return
  end
  local want = MINI.under(ground_h)
  local moved = MINI.drawn_left and (math.abs(MINI.left - MINI.drawn_left)
                + math.abs(MINI.top - MINI.drawn_top) >= 5 or MINI.z ~= MINI.drawn_z)
  local came = 0
  for _, tw in ipairs(want) do if MINI.kept[(tw[1] << 50) | (tw[2] << 25) | tw[3]] then came = came + 1 end end
  if MINI.drawn_left and not moved and came == MINI.drawn_came and t - MINI.drawn_t < 0.5 then
    return
  end
  MINI.drawn_left, MINI.drawn_top, MINI.drawn_z, MINI.drawn_t, MINI.drawn_came =
    MINI.left, MINI.top, MINI.z, t, came
  local hgt = side + 22
  if not MINI.surface then
    MINI.surface = gfx.surface{ w = side, h = hgt }
    MINI.face = gfx.face("ibmplexsans-medium", 12)
  end
  local s, c = MINI.surface, MINI.colours
  s:fill(0, 0, side, hgt, GLASS)
  s:fill(0, 0, side, side, 0xff000000 | c.land)
  for _, tw in ipairs(want) do
    local tile = MINI.get(tw[1], tw[2], tw[3])
    if tile then
      tile:draw(s, tw[2] * MINI.tile - MINI.left, tw[3] * MINI.tile - MINI.top, MINI.tile,
                tw[1], MINI.style, 0, 0, side, side)
    end
  end
  -- The runways, at their width where that is more than two pixels.
  local _, _, scale = MINI.world(0, 0)
  local per_m = MINI.px / scale
  local function at(x, z)
    local wx, wy = MINI.world(x, z)
    return math.floor(wx * MINI.px - MINI.left + 0.5), math.floor(wy * MINI.px - MINI.top + 0.5)
  end
  for _, r in ipairs(RUNWAYS) do
    local x0, y0 = at(r.x1, r.z1)
    local x1, y1 = at(r.x2, r.z2)
    s:line(x0, y0, x1, y1, math.max(3, math.floor(r.width * per_m + 0.5)), 0xffd8dce4)
  end
  -- The aircraft: an arrow along its heading, x east and the map's y south.
  local fx, fy = math.sin(S.psi), -math.cos(S.psi)
  local rx, ry = -fy, fx
  local cx, cy = side / 2, side / 2
  s:polygon({ cx + 12 * fx, cy + 12 * fy,
              cx - 8 * fx + 7 * rx, cy - 8 * fy + 7 * ry,
              cx - 3 * fx, cy - 3 * fy,
              cx - 8 * fx - 7 * rx, cy - 8 * fy - 7 * ry }, 0xffffc23d)
  -- North, and a frame.
  s:text(side - 18, 4, "N", BRIGHT, nil, MINI.face)
  s:fill(0, 0, side, 1, LINE) s:fill(0, side - 1, side, 1, LINE)
  s:fill(0, 0, 1, side, LINE) s:fill(side - 1, 0, 1, side, LINE)
  -- The scale: a bar a round distance long, near a quarter of the map.
  local metres = side / 4 / per_m
  local nice = 100
  for _, m in ipairs{ 100, 200, 500, 1000, 2000, 5000, 10000, 20000 } do
    if m <= metres then nice = m end
  end
  local bar = math.floor(nice * per_m + 0.5)
  s:fill(8, side + 10, bar, 2, BRIGHT)
  s:text(14 + bar, side + 3, nice >= 1000 and ("%d km"):format(nice // 1000) or ("%d m"):format(nice),
         BRIGHT, nil, MINI.face)
  local ok, why = w:overlay(s, 16, height - STRIP_H - hgt - 12, 7)
  if not MINI.said or MINI.said ~= MINI.z then
    MINI.said = MINI.z
    say(("minimap zoom %d, %d tiles under it, %d of them here%s"):format(MINI.z, #want, came,
        ok and "" or (" failed=" .. tostring(why))))
  end
end

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
  rows[#rows + 1] = { kind = "aircraft", label = "Aircraft", items = AIRCRAFT.items or {} }
  rows[#rows + 1] = { kind = "times", label = "Time", items = HOURS }
  rows[#rows + 1] = { kind = "weathers", label = "Weather", items = DECK.choices }
  rows[#rows + 1] = { kind = "winds", label = "Wind", items = WIND.strengths }
  rows[#rows + 1] = { kind = "froms", label = "From", items = WIND.froms }
  rows[#rows + 1] = { kind = "buttons", items = { { name = "Keys" }, { name = "Restart" }, { name = "Fly on" } } }
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
  if row.kind == "times" then return item.key == settings.time end
  if row.kind == "aircraft" then return item.key == settings.aircraft end
  if row.kind == "weathers" then return item.key == settings.weather end
  if row.kind == "winds" then return item.key == settings.wind end
  if row.kind == "froms" then return item.key == settings.wind_from end
  return item.name == "Fly on"
end

local function sheet_open()
  SHEET.open, SHEET.focus, SHEET.cols, SHEET.text = true, 1, {}, ""
  SHEET.place, SHEET.start = here, settings.start
  SHEET.found, SHEET.asked, SHEET.said, SHEET.drawn = nil, nil, nil, nil
  -- The aircraft files read once a sheet, not every frame it is up.
  AIRCRAFT.items = AIRCRAFT.list()
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
  -- The airports first (F8), from the file at once - a code or a name -
  -- and the finder's places after them when they come.
  local db = airports_here()
  local fields = db and db:search(text, 3) or {}
  SHEET.airports = {}
  for i, a in ipairs(fields) do SHEET.airports[i] = airport_place(a) end
  say(("sheet airports %d%s"):format(#fields, fields[1] and (" first " .. fields[1].ident
      .. " runway " .. fields[1].runways[1].ends[1].name) or ""))
  SHEET.found = #SHEET.airports > 0 and { table.unpack(SHEET.airports) } or nil
  local n, why = tiles.find(text)
  if not n then
    SHEET.said = #SHEET.airports == 0 and ("the search could not be asked: " .. tostring(why)) or nil
    return
  end
  SHEET.asked, SHEET.said = n, "Searching\u{2026}"
  say(("sheet searching %q"):format(text))
end

local function sheet_found()
  if not SHEET.asked then return end
  local done, status = tiles.found()
  if done < SHEET.asked then return end
  local list, why = tiles.places(SHEET.asked, status)
  SHEET.asked = nil
  if not list then SHEET.said = why return end
  SHEET.found = { table.unpack(SHEET.airports or {}) }
  for i = 1, math.min(4, #list) do
    local r = list[i]
    SHEET.found[#SHEET.found + 1] = with_field{ name = r.name, note = r.where, lon = r.lon,
                                                lat = r.lat }
  end
  SHEET.said = #SHEET.found == 0 and "Nowhere is called that" or nil
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
  elseif row.kind == "times" then
    set_time(item.key)                 -- at once, under the sheet
  elseif row.kind == "aircraft" then
    settings:set("aircraft", item.key) -- flown from the next frame, started again
    say("aircraft chosen " .. item.key)
  elseif row.kind == "weathers" then
    settings:set("weather", item.key)  -- at once too, from the next frame
    DECK.h = nil
    say("weather " .. item.key)
  elseif row.kind == "winds" or row.kind == "froms" then
    settings:set(row.kind == "winds" and "wind" or "wind_from", item.key)
    say(("wind from %03d at %d kt"):format(settings.wind_from, settings.wind))
  elseif item.name == "Restart" then
    restart(SHEET.place, SHEET.start)
    sheet_close(w)
  elseif item.name == "Keys" then
    sheet_close(w)
    SHEET.keys.show()
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
  local parts = { SHEET.focus, SHEET.text, SHEET.said or "", settings.time, settings.weather,
                  settings.wind, settings.wind_from, settings.aircraft,
                  SHEET.place and SHEET.place.name or "-", SHEET.start or "-" }
  for _, r in ipairs(rows) do
    parts[#parts + 1] = (r.label or r.kind) .. ":" .. tostring(SHEET.cols[r.label or r.kind])
    for _, item in ipairs(r.items or {}) do parts[#parts + 1] = item.name end
  end
  local key = table.concat(parts, "|")
  if key == SHEET.drawn then return end
  SHEET.drawn = key
  local focus = rows[SHEET.focus]
  say(("sheet focus=%s:%s place=%s start=%s aircraft=%s text=%q found=%d%s"):format(
      focus.label or focus.kind, tostring(SHEET.cols[focus.label or focus.kind] or "-"),
      SHEET.place and SHEET.place.name or "none", SHEET.start, settings.aircraft, SHEET.text,
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
  local sh = 70 + 26                  -- and the keys' line, above the buttons
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
      -- The keys, said once where a person looks for them (F6b).
      s:fill(18, y - 4, sw - 36, 1, LINE)
      s:text(18, y + 4, "Arrows pitch and roll \u{b7} A Z or 0-9 throttle \u{b7} F flaps \u{b7} F1 all the keys",
             DIM, nil, sheet_face)
      y = y + 26
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

-- **The keys, all of them** (F6h; Diego, 21 November: "we need a help
-- screen to show all the commands"): F1 opens them where the pause sheet
-- sits, paused under it, and F1 or Esc - or a click - closes them. The
-- sheet's Keys button opens them too. What a key steps through is said
-- beside it, since the flaps and the views step and do not say so.
SHEET.keys = { open = false, groups = {
  { "Flying", {
    { "\u{2191} \u{2193}", "pitch: nose down, nose up" },
    { "\u{2190} \u{2192}", "roll left, roll right" },
    { "A  Z", "throttle up, down - also Page Up and Down, + and -" },
    { "0 to 9", "throttle at once: 0 idle, 5 half, 9 nine tenths" },
    { "F", "flaps a step: 0\u{b0}, 15\u{b0}, 30\u{b0}, then up again" },
    { "B  Space", "brakes, while held, on the ground" },
    { "Q", "autopilot: holds the heading, height and speed; the arrows, A and Z change them" } } },
  { "The view and the world", {
    { "V", "view: chase, cockpit, tower - or click the view's chip" },
    { "T", "time of day: dawn, morning, noon, afternoon, dusk, night" },
    { "H", "shadows on and off" },
    { "M", "sound on and off" },
    { "F3", "the graphics panel: frames, GPU and processor times" } } },
  { "The flight", {
    { "P", "pause: the place, the start, the time, the weather, the wind" },
    { "R", "start again, from the same place and start" },
    { "X", "the time faster: 1x, 2x, 3x, 4x, and back to 1x" },
    { "F1", "these keys; F1 or Esc closes them" } } },
  { "A gamepad", {
    { "left stick", "the yoke: pitch and roll, as far as it is pushed" },
    { "triggers", "right opens the throttle, left closes it" },
    { "A  B", "A brakes, B a step of flaps" } } } } }

function SHEET.keys.show()
  SHEET.keys.open, SHEET.keys.drawn, S.paused = true, false, true
  local n = 0
  for _, gr in ipairs(SHEET.keys.groups) do n = n + #gr[2] end
  say(("keys shown: %d keys in %d groups"):format(n, #SHEET.keys.groups))
end

function SHEET.keys.close(w)
  SHEET.keys.open, S.paused = false, false
  w:overlay(nil, 0, 0, 5)
  say("keys closed")
end

function SHEET.keys.draw(w)
  if not SHEET.keys.open or SHEET.keys.drawn then return end
  SHEET.keys.drawn = true
  if not sheet_face then
    sheet_face = gfx.face("ibmplexsans", 13)
    sheet_bold = gfx.face("ibmplexsans-medium", 13) or sheet_face
    sheet_title = gfx.face("ibmplexsans-medium", 18) or sheet_bold
  end
  local width, height = settings.width, settings.height
  local sw, rows = math.min(560, width - 32), 0
  for _, gr in ipairs(SHEET.keys.groups) do rows = rows + #gr[2] end
  local sh = math.min(height - 16, 70 + #SHEET.keys.groups * 34 + rows * 22 + 12)
  if sw < 300 or sh < 200 then say(("keys not laid: %dx%d"):format(width, height)) return end
  if not SHEET.keys.surface or SHEET.keys.surface:size() ~= sw or select(2, SHEET.keys.surface:size()) ~= sh then
    SHEET.keys.surface = gfx.surface{ w = sw, h = sh }
  end
  local s = SHEET.keys.surface
  s:fill(0, 0, sw, sh, GLASS)
  s:text(18, 14, "Keys", BRIGHT, nil, sheet_title)
  s:text(18, 42, "Paused while these are shown. F1 or Esc goes back to flying.", DIM, nil, sheet_face)
  local y, cap = 70, 120
  for _, gr in ipairs(SHEET.keys.groups) do
    s:fill(18, y, sw - 36, 1, LINE)
    s:text(18, y + 8, gr[1], ACCENT, nil, sheet_bold)
    y = y + 34
    for _, it in ipairs(gr[2]) do
      s:text(30, y, it[1], BRIGHT, nil, sheet_bold)
      s:text(18 + cap, y, it[2], DIM, nil, sheet_face)
      y = y + 22
    end
  end
  local ok, why = w:overlay(s, (width - sw) // 2, (height - sh) // 2, 5)
  say(("keys laid %dx%d %d keys%s"):format(sw, sh, rows, ok and "" or (" failed=" .. tostring(why))))
end

-- The window and the terrain.
do
  local list, refused = AIRCRAFT.list()
  local names = {}
  for i, a in ipairs(list) do names[i] = ("%s (%s)"):format(a.name, a.key) end
  say(("aircraft files: %s%s"):format(table.concat(names, ", "),
      #refused > 0 and ("; " .. table.concat(refused, "; ")) or ""))
end
say(("aircraft %s from Aircraft/%s.lua, %s engine"):format(PLANE.name, PLANE.key, PLANE.engine.kind))
local w, why = k.window("Flight", settings.width, settings.height)
if not w then say("failed=" .. tostring(why)) return end
local plane
plane, why = w:model(PLANE.path, PLANE.parts)
if plane then say("aircraft " .. plane:info()) else say("no aircraft: " .. tostring(why)) end

-- Another aircraft chosen in the sheet (F12): read, its model put in the
-- old one's place, and the flight started again in it. A file that will
-- not fly leaves the one flying.
-- **The route** (F10c): with `fly_to` set, engaging the autopilot makes
-- a plan from the nearest airport - or, with none within 50 km, from
-- where the aircraft is - to that one, and each frame the plan sets what
-- the autopilot holds. Both are `model.lua`'s, handed the Airports library
-- for its great circles.
function AIRCRAFT.route(ap)
  if (settings.fly_to or "") == "" then return end
  local db = airports_here()
  local to = db and db:find(settings.fly_to)
  if not to then say("route: no airport " .. tostring(settings.fly_to)) return end
  local lon, lat = ANCHOR.lonlat(S.x, S.z)
  local from = db:near(lon, lat, 50, 1)[1] or { lon = lon, lat = lat, feet = 0, runways = {} }
  local plan, why = AIRCRAFT.model.route(from, to, PLANE,
                                         (settings.wind or 0) > 0 and settings.wind_from or nil, airports)
  if not plan then say("route: " .. tostring(why)) return end
  ap.plan = plan
  AIRCRAFT.guide(ap)
  say(("route %s to %s %s, %.0f nm, cruise %d ft, bearing %03d"):format(from.iata or from.ident or "here",
      to.iata or to.ident, plan.arr.name, plan.nm, plan.cruise_ft,
      math.floor(math.deg(ap.heading) % 360 + 0.5) % 360))
end

function AIRCRAFT.guide(ap)
  local lon, lat = ANCHOR.lonlat(S.x, S.z)
  AIRCRAFT.model.guide(ap.plan, ap, S, PLANE, lon, lat, airports, say)
end

function AIRCRAFT.switch(g)
  local a, refused = AIRCRAFT.read(settings.aircraft)
  if not a then
    say(("aircraft %s refused: %s"):format(tostring(settings.aircraft), tostring(refused)))
    settings:set("aircraft", PLANE.key)
    return
  end
  local m, failed = w:model(a.path, a.parts)
  if not m then
    say(("aircraft %s's model: %s"):format(a.key, tostring(failed)))
    settings:set("aircraft", PLANE.key)
    return
  end
  PLANE, plane = a, m
  engine_stop()                      -- heard again in the new one's voice
  say(("aircraft %s from Aircraft/%s.lua, %s engine"):format(a.name, a.key, a.engine.kind))
  -- Started again where it is, and still paused if the sheet is up.
  local paused = S.paused
  start(g)
  S.paused = paused
end
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
-- **Deeper near the eye** (F6, the imagery deeper): chunks go to zoom 17,
-- about 0.8 m a texel at London's latitude against 4.5 m at 15. A chunk is
-- split only when the eye is within twice its side, its height counted, so
-- the deeper zooms are under the wheels on a runway and on a low final,
-- and at cruise they are not asked for at all.
local function terrain()
  return k.terrain{ lon = ANCHOR.lon, lat = ANCHOR.lat, root = 11, deepest = 17,
                    radius = 1, split = 2.0, most = math.min(300, keep), elevation = 13,
                    keep = keep, uploads = 8, flats = not offline.on and flats_here() or nil,
                    buildings = settings.buildings and not offline.on and 14 or nil,
                    buildings_radius = 1, roofs = settings.buildings and not offline.on and 16 or nil }
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
  -- The island has no runways: a terrain without the flats the real
  -- place's would have levelled in its sea.
  g = nil
  collectgarbage()
  g = terrain()
  RUNWAYS = {}
  here = { name = "an invented island", code = "Offline: an invented island",
           note = "the imagery and terrain could not be fetched", invented = true,
           lon = ANCHOR.lon, lat = ANCHOR.lat }
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
  -- A flight that moved the terrain starts again over its own place.
  local moved = ANCHOR.lon ~= settings.lon or ANCHOR.lat ~= settings.lat
  if place and not same_place(place, here) then
    if place.key then
      settings:set{ city = place.key, lon = place.lon, lat = place.lat }
      settings:reset("place")
    else
      settings:set{ city = "", lon = place.lon, lat = place.lat,
                    place = { name = place.name, note = place.note, lon = place.lon,
                              lat = place.lat, code = place.code, field = place.field,
                              runway = place.runway, airport = place.airport } }
    end
    local kept = { place.key and { key = place.key } or settings.place }
    for _, p in ipairs(recent()) do
      if #kept < 8 and not same_place(p, place) then
        kept[#kept + 1] = p.key and { key = p.key } or { name = p.name, note = p.note,
                                                          lon = p.lon, lat = p.lat,
                                                          code = p.code, field = p.field,
                                                          runway = p.runway,
                                                          airport = p.airport }
      end
    end
    settings:set("recent", kept)
    here = where_now()
    ANCHOR.lon, ANCHOR.lat, moved = settings.lon, settings.lat, false
    g = nil
    collectgarbage()
    g = terrain()
    missing.e, missing.i = {}, {}
    place_tower()
    place_said = nil
    set_time(settings.time)            -- the sun stands elsewhere at another latitude
    DECK.h = nil                       -- and the deck over another field
    say(("restarted in %s at %.4f %.4f"):format(here and here.name or "?", settings.lat,
        settings.lon))
  end
  if moved then
    ANCHOR.lon, ANCHOR.lat = settings.lon, settings.lat
    g = nil
    collectgarbage()
    g = terrain()
    missing.e, missing.i = {}, {}
    place_tower()
    DECK.h = nil
    say(("anchored back at %.5f %.5f"):format(ANCHOR.lat, ANCHOR.lon))
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

-- **The terrain moved under the aircraft** (F10a): centred again where it
-- is, and everything placed in the frame moved by the same metres, so the
-- aircraft is where it was on the Earth. The tiles are the Earth's and do
-- not move; the chunks are made again around the new centre, from tiles
-- the cache already holds. The runways, their lights and the deck are
-- the new centre's; the tower stays at its field.
function ANCHOR.move()
  -- Every kept point to where it is on the Earth, under the old centre...
  local lon, lat = ANCHOR.lonlat(S.x, S.z)
  local far = math.sqrt(S.x * S.x + S.z * S.z)
  local on = runway_at(S.x, S.z)
  local kept = {}
  for _, p in ipairs{ S.rollout or false, S.rolled or false } do
    if p then kept[#kept + 1] = { p, ANCHOR.lonlat(p.x, p.z) } end
  end
  -- ... and back into the frame under the new one, the aircraft at nought.
  ANCHOR.lon, ANCHOR.lat = lon, lat
  S.x, S.z = metres_at(lon, lat)
  for _, k in ipairs(kept) do k[1].x, k[1].z = metres_at(k[2], k[3]) end
  g = nil
  collectgarbage()
  g = terrain()
  missing.e, missing.i = {}, {}
  place_tower()
  DECK.h = nil
  -- What says the frame moved and not the aircraft: the runway under it
  -- is the same one, found among the new centre's runways.
  say(("anchored again at %.6f %.6f, %.0f m from the last; on %s before, %s after")
      :format(lat, lon, far, on or "no runway", runway_at(S.x, S.z) or "no runway"))
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
  -- F1 kept in the keys' own table: the main chunk is at Lua's 200 locals.
  SHEET.keys.f1 = w:pressed(KEY.f1)
  if SHEET.keys.open then
    if SHEET.keys.f1 or w:pressed(KEY.esc) or p or w:clicked() then SHEET.keys.close(w) end
    w:typed()
  elseif SHEET.open and SHEET.keys.f1 then
    sheet_close(w)
    SHEET.keys.show()
  elseif SHEET.open then
    sheet_input(w)
  elseif SHEET.keys.f1 then
    SHEET.keys.show()
    w:typed()
  else
    local cx, cy = w:clicked()
    if v or on_view_chip(cx, cy) then S.view = S.view % 3 + 1 end
    if w:pressed(KEY.t) then
      local _, i = time_of(settings.time)
      set_time(HOURS[i % #HOURS + 1].key)
    end
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
    -- The time faster (F11): X steps 1x, 2x, 3x, 4x and round again.
    if w:pressed(KEY.x) then
      S.rate = S.rate % 4 + 1
      say(("time %dx"):format(S.rate))
    end
    -- The autopilot (F10b): Q holds what the aircraft is doing, and Q again
    -- lets go. In the air only: on the wheels there is nothing to hold.
    if w:pressed(KEY.q) then
      local ap = S.ap
      local function said(how)
        say(("autopilot %s: heading %03d, %d ft, %d kt"):format(how,
            math.floor(math.deg(ap.heading) % 360 + 0.5) % 360,
            math.floor(ap.alt * 3.28084 + 0.5), math.floor(ap.speed * 1.94384 + 0.5)))
      end
      if ap.on then ap.on = false said("off")
      elseif S.ground then say("autopilot: not on the ground")
      else AIRCRAFT.model.engage(ap, S, PLANE) AIRCRAFT.route(ap) said("on") end
    end
    if w:pressed(KEY.h) then
      settings:set("shadows", not settings.shadows)
      say("shadows " .. (settings.shadows and "on" or "off"))
    end
    if w:pressed(KEY.m) then
      settings:set("sound", not settings.sound)
      say("sound " .. (settings.sound and "wanted" or "off"))
    end
    if r or S.crashed then
      if S.crashed then say("crashed: " .. tostring(S.crashed)) end
      start(g)
    end
    w:typed()
  end

  if settings.aircraft ~= PLANE.key then AIRCRAFT.switch(g) end
  if not offline.on and (math.abs(S.x) > settings.anchor_after
                         or math.abs(S.z) > settings.anchor_after) then
    ANCHOR.move()
  end
  if not S.paused then
    local pitch, roll, brake = PAD.controls(w, dt)
    -- The time faster is the model stepped again with the same frame's
    -- time (F11), never one step of two or four times the length: a step
    -- of 0.2 s is where the flare and the wheels stop being stable. The
    -- ground is asked for under each step, since it moves under the last.
    -- (A count in S, not a for's: the main chunk is at Lua's 200 locals.)
    S.steps = S.rate
    WIND.kt, WIND.from = settings.wind, settings.wind_from
    if S.ap.on and S.ap.plan then AIRCRAFT.guide(S.ap) end
    repeat
      if S.ap.on then pitch, roll = AIRCRAFT.model.autopilot(S.ap, S, PLANE, dt) end
      AIRCRAFT.model.fly(S, PLANE, WIND, dt, pitch, roll, g:height(S.x, S.z), brake, say)
      if S.ap.on and (S.ground or S.crashed) then S.ap.on = false say("autopilot off: on the ground") end
      S.steps = S.steps - 1
    until S.steps == 0 or S.crashed
    -- The engine's spool said at each quarter it passes, in the flight's
    -- own seconds: a jet's lags the throttle, a propeller's is it.
    if S.spool and math.floor(S.spool * 4 + 1e-6) ~= S.spool_q then
      S.spool_q = math.floor(S.spool * 4 + 1e-6)
      say(("spool %.2f at %.1f s, throttle %.2f, %s"):format(S.spool, S.simt, S.throttle,
          PLANE.engine.kind))
    end
    if S.landed then
      say(("landed at %.0f fpm %s"):format(S.landed,
          runway_at(S.x, S.z) and ("on " .. runway_at(S.x, S.z)) or "off any runway"))
      say(("touched down at %.0f kt through the air, %.0f kt over the ground"):format(
          (S.touch_air or 0) * 1.944, (S.touch_ground or 0) * 1.944))
      -- The squeak, as hard as the sink rate: a greaser at 100 fpm is a
      -- whisper, 500 fpm a bark.
      if engine.on then
        local hard = math.max(0.25, math.min(1, S.landed / 500))
        synth.note_on(3, 90, hard)
        engine.squeak = { t = t, hard = hard }
        say(("squeak at %.2f"):format(hard))
      end
      S.landed = nil
      S.rollout = { x = S.x, z = S.z, t = t }
    end
    -- Said once it has stopped: how far the landing rolled, and how long.
    if S.rollout and S.ground and S.v == 0 then
      say(("rolled out %.0f m in %.1f s"):format(
          math.sqrt((S.x - S.rollout.x) ^ 2 + (S.z - S.rollout.z) ^ 2), t - S.rollout.t))
      S.rollout = nil
    end
    -- On a runway's start, the ground under the wheels once its tiles are
    -- in: the runway's own height, levelled (F8).
    if S.started == "runway" and not S.ground_said then
      local h = g:height(S.x, S.z)
      local rw = here and here.runway
      if h and rw then
        say(("runway ground %.2f m under the wheels, the runway %.2f m, %s")
            :format(h, rw.feet * 0.3048, runway_at(S.x, S.z) or "no runway known here"))
        S.ground_said = true
      end
    end
    if plane then place(plane, pitch, roll, dt) end
  end
  engine_sound(t)
  draw_strip(w, g:height(S.x, S.z))
  draw_panel(w, dt)
  MINI.draw(w, g:height(S.x, S.z), t)
  draw_sheet(w)
  SHEET.keys.draw(w)

  look(g)
  -- Faster, the ground goes by faster: the tiles are asked for as far
  -- ahead as the extra steps will carry it in two seconds, so they are in
  -- when it arrives (F11). At 1x this is the eye itself.
  if S.rate > 1 then
    local ahead = S.v * math.cos(S.gamma) * (S.rate - 1) * 2
    local e = view.eye
    feed(g, { e[1] + math.sin(S.psi) * ahead, e[2], e[3] - math.cos(S.psi) * ahead })
  else
    feed(g, view.eye)
  end
  if not offline.on and net.imagery and net.elevation and handed == offline.handed
     and t - offline.since > 6
     and ((net.imagery.failed or 0) > 0 or (net.elevation.failed or 0) > 0) then
    go_offline(("nothing came, %d imagery and %d elevation fetches failed%s"):format(
               net.imagery.failed or 0, net.elevation.failed or 0,
               net.imagery.why and net.imagery.why ~= "" and (" (" .. net.imagery.why .. ")") or ""))
  end
  draw_place(w)
  draw_view_chip(w)
  if S.view == 3 and not tower.said then
    say(("tower at x=%.0f z=%.0f ground=%s distance=%.0f focal=%.1f")
        :format(tower.x, tower.z, tower.ground and ("%.0f"):format(tower.ground) or "none",
                tower.distance, tower.focal))
    tower.said = true
  elseif S.view ~= 3 then
    tower.said = nil
  end
  -- The aircraft is not drawn from inside it.
  local said, failed = w:draw(g, view, frame_models(w, S.view ~= 2 and plane or nil))
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
    say("buildings " .. g:built())
    say(("propeller %.3f turns a frame at throttle %.2f, engine %s")
        :format(S.prop_step or 0, S.throttle,
                engine.on and ("%.0f rpm"):format(engine.rpm or 0) or "silent"))
  end
  if frame == settings.frames then say("done") w:closed(120) break end
end
engine_stop()
