-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Locale
-- kosmos: name Maps
-- kosmos: section applications
-- kosmos: needs tiles
--
-- Maps: OpenStreetMap's world as shapes, drawn by Kosmos (`docs/maps.md`,
-- the window `docs/maps.html`, agreed by Diego on 8 October 2026: "mockup
-- is great", "but i do want a close/open sidebar button always visible").
--
--   maps                       the region the image carries, Port Alder,
--                              and the world from OpenFreeMap around it
--   maps /Home/Maps/x.pmtiles  a region of one's own
--   maps --source URL          the world's tiles from somewhere else: a
--                              TileJSON, or an address with {z}, {x}, {y}
--   maps --finder URL          places looked up somewhere other than
--                              Nominatim, with "?q=" added
--
-- **The thinnest layer** (`CLAUDE.md`, kits supply and applications
-- orchestrate): the Map Kit reads a region, decodes its tiles and draws
-- them through gfx's paths; this decides which tiles go where, what the
-- sidebar says and what a press does. Pixels are never computed here.
--
-- A window that draws its own pixels - the map is redrawn on every frame of
-- a drag - with its own header (`pixelkit`): the sidebar's button always at
-- its left, the title, the dots, and the three where every header has them.

local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local keys = use("/Kosmos/Libraries/keys.lua")
local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
local map = use("/Kosmos/Kits/map")
local text_kit = use("/Kosmos/Libraries/text.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

--------------------------------------------------------------------------
-- The region: the image's own, or a file named on the command line.
--------------------------------------------------------------------------

local SHIPPED = "maps/port-alder.pmtiles"
local SOURCE = "https://tiles.openfreemap.org/planet"
local region_path, source, finder = "", SOURCE, nil

do
  local words = {}

  for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

  local i = 1

  while i <= #words do
    if words[i] == "--source" and words[i + 1] then
      source, i = words[i + 1], i + 2
    elseif words[i] == "--finder" and words[i + 1] then
      finder, i = words[i + 1], i + 2
    else
      region_path, i = words[i], i + 1
    end
  end
end
local bytes, why

if region_path ~= "" then
  bytes, why = fs.read(region_path)
else
  bytes, why = sys.asset(SHIPPED)
  region_path = SHIPPED
end

local region

if type(bytes) == "string" then
  region, why = map.open(bytes)
end

if not region then
  print("maps: " .. region_path .. ": " .. tostring(why))
  return
end

local info = region:info()
local made_up = region_path == SHIPPED

print(("maps: %s, zooms %d to %d"):format(region_path, info.min_zoom, info.max_zoom))

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local screen = gfx.screen()
local sw, sh = 1280, 800

if screen then sw, sh = screen:size() end

local W = math.min(1100, sw - 80)
local H = math.min(700, sh - 120)

local win = ui.window{ title = "Maps", w = W, h = H, direct = true, header = true,
                       resizable = true, centre = true }

if not win or not win:surface() then
  print("maps: no window")
  return
end

local SIDE_W = 300
local sidebar = true

--------------------------------------------------------------------------
-- The style: Night's colours, or a light map's - the drawing's tokens.
--------------------------------------------------------------------------

local function style_for(dark)
  local c = dark and {
    -- Land and water far enough apart that a coast is seen at a glance -
    -- the M700, 8 October: at 0x1d2433 against 0x10233f Africa was barely
    -- there at zoom 3.
    land = 0x283142, block = 0x2d3749, park = 0x24423a, water = 0x0c1828,
    building = 0x36415a, casing = 0x1a2130, minor = 0x445168, major = 0x5e6d87,
    motor = 0x8a6f3e, path = 0x4a5568, border = 0x6b5f8a,
  } or {
    land = 0xf2efe9, block = 0xe8e3da, park = 0xcfe8c8, water = 0xaad3f2,
    building = 0xdcd5c9, casing = 0xd9d2c6, minor = 0xffffff, major = 0xfde8a8,
    motor = 0xf6bb6a, path = 0xc8bfae, border = 0xb9a6c9,
  }

  return c, map.style{ background = c.land, rules = {
    { layer = "landuse", classes = "residential,commercial,industrial,retail", fill = c.block },
    { layer = "landcover", classes = "wood,forest,grass,farmland,meadow", fill = c.park },
    { layer = "park", fill = c.park },
    { layer = "water", fill = c.water },
    { layer = "waterway", line = c.water, width = { 12, 1, 18, 8 } },
    { layer = "boundary", line = c.border, width = { 2, 0.8, 10, 1.5 } },
    { layer = "building", fill = c.building, minzoom = 14 },
    { layer = "transportation", classes = "path,track", line = c.path,
      width = { 15, 0.6, 18, 2 }, minzoom = 15 },
    { layer = "transportation", classes = "minor,service", line = c.casing,
      width = { 13, 1.5, 18, 22 }, minzoom = 13 },
    { layer = "transportation", classes = "primary,secondary,tertiary,trunk", line = c.casing,
      width = { 10, 2, 18, 30 } },
    { layer = "transportation", classes = "motorway", line = c.casing, width = { 8, 2.5, 18, 36 } },
    { layer = "transportation", classes = "minor,service", line = c.minor,
      width = { 13, 0.8, 18, 18 }, minzoom = 13 },
    { layer = "transportation", classes = "primary,secondary,tertiary,trunk", line = c.major,
      width = { 10, 1.2, 18, 26 } },
    { layer = "transportation", classes = "motorway", line = c.motor, width = { 8, 1.6, 18, 32 } },
  } }
end

local function is_dark()
  local w = theme.window or 0
  local lum = ((w >> 16) & 255) * 3 + ((w >> 8) & 255) * 6 + (w & 255)

  return lum < 1280
end

local colours, style = style_for(is_dark())

print("maps: the map is " .. (is_dark() and "dark" or "light"))

--------------------------------------------------------------------------
-- Where the map is looking: its centre across the world, 0 to 1, and its
-- zoom, which need not be whole.
--------------------------------------------------------------------------

local cx, cy = map.project(info.lon, info.lat)
local zoom = math.max(info.min_zoom, math.min(info.max_zoom, (info.zoom or 14) > 0 and info.zoom or 14))
local MIN_ZOOM, MAX_ZOOM = math.max(2, info.min_zoom - 1), info.max_zoom + 3
local TILE = 512

--------------------------------------------------------------------------
-- The world, from the network (`docs/maps.md` M6d): the tiles server
-- fetches what is asked for into `/Home/Cache/Maps`, and says when each has
-- come. Nothing here waits for one - a tile that is not here yet is a
-- blank, or its parent drawn larger, until the server says it came.
--------------------------------------------------------------------------

local NET_MAX = 14                     -- OpenFreeMap's deepest; drawn larger past it
local TILES_REQUEST = "<I4I4I4I4" .. string.rep("I4", 192)
local TILES_SOURCE = "<I4I4I4I4c768"
local TILES_REPLY = "<I4I4I4I4I4I4I4I4c64c128" .. string.rep("I4", 192)
local OP_SOURCE, OP_WANT, OP_ARRIVED, OP_FIND, OP_FINDER = 1, 2, 3, 4, 5
local ZEROS = {}

for i = 1, 192 do ZEROS[i] = 0 end

local net = nil                        -- { dir, since, outstanding, failed, ... }
local on_found                         -- a search answered: the search's, below

local function zstring(s) return (s:gsub("%z.*$", "")) end

-- One exchange with `/Tiles`; the reply as a table, or nil and why.
local function tiles_ask(packed)
  local reply, why = fs.raw("/Tiles", packed, nil, "tiles")

  if type(reply) ~= "string" or #reply < 992 then return nil, why or "no answer" end

  local v = { string.unpack(TILES_REPLY, reply) }
  local out = { status = v[1], count = v[2], seq = v[3], outstanding = v[4], failed = v[5],
                found = v[6], found_status = v[7], cache = zstring(v[9]), why = zstring(v[10]),
                tiles = {} }

  for i = 1, out.count do
    local at = 10 + (i - 1) * 3

    out.tiles[i] = { z = v[at + 1], x = v[at + 2], y = v[at + 3] }
  end

  return out
end

do
  local said, why = tiles_ask(string.pack(TILES_SOURCE, OP_SOURCE, 0, 0, 0, source))

  if said and said.status == 0 then
    net = { dir = said.cache, since = 0, outstanding = 0, failed = said.failed,
            asked = {}, sent = "", polled = 0, came = 0, finding = nil }
    MIN_ZOOM, MAX_ZOOM = 1, 19
    print(("maps: tiles from %s into %s"):format(source, said.cache))

    if finder then
      local f = tiles_ask(string.pack(TILES_SOURCE, OP_FINDER, 0, 0, 0, finder))

      print("maps: places from " .. finder .. ((f and f.status == 0) and "" or ", refused"))
    end
  else
    print("maps: no tiles from the network: " .. tostring(said and said.why or why))
  end
end

if made_up then
  -- Port Alder's middle, a little south where the market is.
  cx, cy = map.project(0.0, -0.0006)
  zoom = 15
end

-- The map's own rectangle in the window.
local function map_box()
  local x = sidebar and SIDE_W or 0

  return x, L.head, W - x, H - L.head
end

--------------------------------------------------------------------------
-- Tiles, decoded once and kept: the last 96 asked for.
--------------------------------------------------------------------------

local cache, order = {}, {}

local function keep_tile(key, t)
  cache[key] = t
  order[#order + 1] = key

  if #order > 96 then
    cache[table.remove(order, 1)] = nil
  end
end

local function tile_at(z, x, y)
  local key = z .. "/" .. x .. "/" .. y
  local t = cache[key]

  if t == nil then
    t = region:tile(z, x, y) or false
    keep_tile(key, t)
  end

  return t or nil
end

-- A tile from the network's cache: the tile, false when there is none of
-- it (the sea), or nil when it is not here yet - and then it is wanted.
local wants = {}

local function net_tile(z, x, y)
  local key = "n" .. z .. "/" .. x .. "/" .. y
  local t = cache[key]

  if t ~= nil then return t or nil end

  -- Asked for already and not come: asked for again, since each list
  -- replaces the last - a frame that left it out would take it back.
  if net.asked[key] then
    wants[#wants + 1] = { z, x, y }
    return nil
  end

  local bytes = fs.read(net.dir .. "/" .. z .. "/" .. x .. "-" .. y .. ".pbf")

  if type(bytes) == "string" then
    t = #bytes > 0 and map.decode(bytes) or false
    keep_tile(key, t)
    return t or nil
  end

  net.asked[key] = true
  wants[#wants + 1] = { z, x, y }
  return nil
end

-- The nearest of its ancestors already here, and how many levels up.
local function net_parent(z, x, y)
  for up = 1, math.min(z, 4) do
    local t = cache["n" .. (z - up) .. "/" .. (x >> up) .. "/" .. (y >> up)]

    if t then return t, up end
  end
end

-- After a frame: what it found missing, asked for, the middle first.
local function send_wants(cx_tile, cy_tile)
  if not net then return end

  table.sort(wants, function(a, b)
    local da = (a[2] + 0.5 - cx_tile) ^ 2 + (a[3] + 0.5 - cy_tile) ^ 2
    local db = (b[2] + 0.5 - cx_tile) ^ 2 + (b[3] + 0.5 - cy_tile) ^ 2

    return da < db
  end)

  -- Those still missing from what was asked before are asked again with
  -- the new ones, since the list replaces the last.
  local ids, seen = {}, {}

  for _, w in ipairs(wants) do
    local k = w[1] .. "/" .. w[2] .. "/" .. w[3]

    if #ids < 64 * 3 and not seen[k] then
      seen[k] = true
      ids[#ids + 1], ids[#ids + 2], ids[#ids + 3] = w[1], w[2], w[3]
    end
  end

  local sent = table.concat(ids, ",")

  if sent == net.sent then return end

  net.sent = sent

  local count = #ids // 3

  for i = #ids + 1, 192 do ids[i] = 0 end

  local said = tiles_ask(string.pack(TILES_REQUEST, OP_WANT, count, 0, 0, table.unpack(ids)))

  if said then net.outstanding = said.outstanding end
end

-- On Maps' own clock, while anything is to come: what came. True when
-- something did, and the map is to be drawn again.
local function poll_arrived()
  if not net or (net.outstanding == 0 and net.sent == "" and not net.finding) then return false end

  local now = sys.ticks()

  if now - net.polled < counter_hz // 5 then return false end

  net.polled = now

  local said = tiles_ask(string.pack(TILES_REQUEST, OP_ARRIVED, 0, net.since, 0,
                                     table.unpack(ZEROS)))

  if not said then return false end

  net.since, net.outstanding = said.seq, said.outstanding

  -- The search asked for, answered (`on_found`, with the search, below).
  if net.finding and said.found >= net.finding then
    local number = net.finding

    net.finding = nil
    on_found(number, said.found_status)
    return true
  end

  for _, t in ipairs(said.tiles) do
    local key = "n" .. t.z .. "/" .. t.x .. "/" .. t.y

    net.asked[key] = nil
    cache[key] = nil
  end

  -- A failed fetch is tried again at the next ask.
  if said.failed ~= net.failed then
    print(("maps: %d tiles failed: %s"):format(said.failed - net.failed, said.why))
    net.failed = said.failed
    net.asked = {}
  end

  if #said.tiles > 0 then
    net.came = net.came + #said.tiles
    net.sent = ""
    print(("maps: %d tiles came, %d in all, %d to come"):format(#said.tiles, net.came,
          said.outstanding))
    return true
  end

  if said.outstanding == 0 then net.sent = "" end

  return false
end

--------------------------------------------------------------------------
-- Places: what a name is, where it is, and what a person kept.
--------------------------------------------------------------------------

local prefs = use("/Kosmos/Libraries/prefs.lua")

-- What a person reads for a class: OpenMapTiles' words, said plainly.
local KIND = {
  marketplace = "Market", ferry_terminal = "Ferry terminal", railway = "Station",
  drinking_water = "Water", townhall = "Town hall", city = "City", town = "Town",
  suburb = "District", neighbourhood = "Neighbourhood", village = "Village",
  primary = "Main road", secondary = "Road", tertiary = "Road", minor = "Street",
  motorway = "Motorway", river = "River", bay = "Bay", park = "Park",
}

local function kind_of(p)
  local k = KIND[p.class or ""]

  if not k then
    local c = tostring(p.class or p.layer or "place"):gsub("_", " ")

    k = c:sub(1, 1):upper() .. c:sub(2)
  end

  -- A place in the world says where it is too: "City - Uruguay".
  if p.where and p.where ~= "" then k = k .. " \u{b7} " .. p.where end

  return k
end

-- A point of interest's colour, by what it is for.
local function dot_of(p)
  local c = p.class or ""

  if c:match("cafe") or c:match("bakery") or c:match("restaurant") then return 0xffe8833a end
  if c:match("market") or c:match("shop") or c:match("pharmacy") then return 0xffe0573f end
  if c:match("museum") or c:match("library") or c:match("cinema") or c:match("school") then
    return 0xff8a63d2
  end
  if c:match("railway") or c:match("ferry") or c:match("station") then return 0xff3584e4 end
  if c:match("viewpoint") or c:match("lighthouse") or c:match("water") then return 0xff2f9d62 end

  return 0xff7a8599
end

-- Kept on this machine: { pinned, saved, recent }, each a list of places
-- `{ name, class, layer, x, y }` with x and y across the world.
local kept = prefs.read("maps")

for _, k in ipairs({ "pinned", "saved", "recent" }) do
  if type(kept[k]) ~= "table" then kept[k] = {} end
end

local function keep()
  local ok, why = prefs.write("maps", kept)

  if not ok then print("maps: not kept: " .. tostring(why)) end
end

local function same(a, b) return a and b and a.name == b.name and a.layer == b.layer end

local function find_in(list, p)
  for i, q in ipairs(list) do
    if same(q, p) then return i end
  end

  return nil
end

local function toggle_in(list, p, most)
  local i = find_in(list, p)

  if i then
    table.remove(list, i)
    return false
  end

  table.insert(list, 1, { name = p.name, class = p.class, layer = p.layer, x = p.x, y = p.y })

  while #list > most do table.remove(list) end

  return true
end

--
-- **The search's index**: every name in the region, once, with where it
-- is - made the first time it is wanted from the tiles at zoom 14 (or the
-- region's deepest, if that is shallower), sixty-four tiles round the
-- region's middle at most. Port Alder is four; a country's extract would be
-- searched round where it opens until search comes from the network (M6).
--
local index = nil

local function build_index()
  if index then return index end

  local z = math.min(info.max_zoom, 14)
  local n = 1 << z
  local x0, y0 = map.project(info.west, info.north)
  local x1, y1 = map.project(info.east, info.south)
  local tx0, ty0 = math.floor(x0 * n), math.floor(y0 * n)
  local tx1, ty1 = math.floor(x1 * n), math.floor(y1 * n)
  local mx_, my_ = (tx0 + tx1) // 2, (ty0 + ty1) // 2
  local seen, tiles = {}, 0

  index = {}

  for ty = math.max(ty0, my_ - 3), math.min(ty1, my_ + 4) do
    for tx = math.max(tx0, mx_ - 3), math.min(tx1, mx_ + 4) do
      local t = region:tile(z, tx, ty)

      tiles = tiles + 1

      for _, l in ipairs(t and t:labels() or {}) do
        local key = l.name .. "|" .. l.layer

        if not seen[key] then
          seen[key] = true
          index[#index + 1] = { name = l.name, class = l.class, layer = l.layer,
                                x = (tx + l.x) / n, y = (ty + l.y) / n }
        end
      end
    end
  end

  table.sort(index, function(a, b) return a.name < b.name end)
  print(("maps: %d names from %d tiles at zoom %d"):format(#index, tiles, z))
  return index
end

-- Names that start with what was typed, then names that hold it; twelve.
local function search_for(text)
  local want = text:lower()
  local starts, holds = {}, {}

  if want == "" then return {} end

  for _, p in ipairs(build_index()) do
    local at = p.name:lower():find(want, 1, true)

    if at == 1 then starts[#starts + 1] = p elseif at then holds[#holds + 1] = p end
  end

  for _, p in ipairs(holds) do starts[#starts + 1] = p end

  while #starts > 12 do table.remove(starts) end

  return starts
end

local results, chosen_result = {}, 1
local card = nil                       -- the place whose card is open

--------------------------------------------------------------------------
-- Places in the world, looked up by name (`docs/maps.md` M6e): asked of
-- the tiles server, which asks Nominatim and writes its answer into the
-- cache; read here with the system's JSON reader. Asked when Return is
-- pressed, never as each key is typed - Nominatim's policy, and a search a
-- keystroke would be a request a keystroke.
--------------------------------------------------------------------------

local json = use("/Kosmos/Libraries/json.lua")
local world = { asked = nil, results = nil, said = nil }   -- the last search's

-- How close to go, by Nominatim's `place_rank`: a country from afar, a
-- house from near.
local function zoom_for_rank(rank)
  rank = tonumber(rank) or 30

  if rank <= 4 then return 5 elseif rank <= 8 then return 7 elseif rank <= 12 then return 9
  elseif rank <= 16 then return 12 elseif rank <= 20 then return 14 elseif rank <= 26 then return 16 end

  return 17
end

local function find_in_world(text)
  if not net or text == "" then return false end

  local said = tiles_ask(string.pack(TILES_SOURCE, OP_FIND, 0, 0, 0, text))

  if not said or said.status ~= 0 then
    world.said = "the search could not be asked"
    return false
  end

  world.asked, world.results, world.said = said.seq, nil, nil
  net.finding = said.seq
  print(("maps: searching the world for %q"):format(text))
  return true
end

-- The answer, once the server says it came: places, or nil and why.
local function read_found(number, status)
  if status ~= 200 then return nil, "the search could not be answered" end

  local text = fs.read(net.dir:gsub("/[^/]*$", "") .. "/places/" .. number .. ".json")

  if type(text) ~= "string" then return nil, "the answer could not be read" end

  local list, why = json.decode(text)

  if type(list) ~= "table" then return nil, "the answer is not JSON: " .. tostring(why) end

  local out = {}

  for _, r in ipairs(list) do
    local lat, lon = tonumber(r.lat), tonumber(r.lon)
    local name = type(r.name) == "string" and r.name ~= "" and r.name
                 or tostring(r.display_name or ""):match("^[^,]+") or "?"

    if lat and lon then
      local x, y = map.project(lon, lat)
      local where = tostring(r.display_name or ""):gsub("^[^,]*,%s*", "")

      -- What a person would call it: Nominatim's `type` is "administrative"
      -- for a city drawn as a boundary, and its `addresstype` says "city".
      local class = (r.type == "administrative" or not r.type) and r.addresstype or r.type

      out[#out + 1] = { name = name, class = tostring(class or "place"),
                        layer = "world", where = where, x = x, y = y,
                        zoom = zoom_for_rank(r.place_rank) }
    end
  end

  return out
end

on_found = function(number, status)
  local list, why = read_found(number, status)

  if list then
    world.results = list
    results, chosen_result = list, 1
    world.said = #list == 0 and "Nowhere in the world is called that" or nil
    print(("maps: %d found in the world%s"):format(#list,
          list[1] and (", first " .. list[1].name .. " (" .. kind_of(list[1]) .. ")") or ""))
  else
    world.said = why
    print("maps: the world's search: " .. tostring(why))
  end
end

--------------------------------------------------------------------------
-- The header, the sidebar, the controls over the map.
--------------------------------------------------------------------------

local side_button = { icon = "sidebar" }
local dots = { icon = "more" }
local zoom_in = { w = 40, h = 40 }
local zoom_out = { w = 40, h = 40 }
local search = { text = "" }
local search_focused = false

local said_controls = nil

local function place_controls()
  local three = (win.lights and win.lights.w) or 68

  side_button.x, side_button.y = L.head_edge, pk.centre(26)
  dots.x, dots.y = W - L.lights_in - three - L.head_edge - 26, pk.centre(26)

  local mx, my, mw = map_box()

  zoom_in.x, zoom_in.y = mx + mw - 16 - 40, my + 16
  zoom_out.x, zoom_out.y = zoom_in.x, zoom_in.y + 40

  search.x, search.y, search.w, search.h = 12, L.head + 12, SIDE_W - 24, 38

  -- Said once a place, for a harness to aim at rather than work out.
  local where = ("sidebar %d,%d; zoom in %d,%d; zoom out %d,%d; search %d,%d; map %d,%d %dx%d")
    :format(side_button.x, side_button.y, zoom_in.x, zoom_in.y, zoom_out.x, zoom_out.y,
            search.x, search.y, mx, my, mw, H - my)

  if where ~= said_controls then
    said_controls = where
    print("maps: controls " .. where)
  end
end

local function draw_header(s)
  pk.header(s, 0, 0, W, "Maps", net and "The world, from OpenFreeMap"
            or made_up and "Port Alder, a made-up city" or region_path,
            dots.x - 8, side_button.x + 26 + 10)
  side_button.pressed = sidebar
  pk.iconbutton(s, side_button)
  pk.iconbutton(s, dots)
end

local function section(s, y, text)
  s:text(18, y, text:upper(), theme.text_dim, nil, "label")

  return y + gfx.height("label") + 8
end

local rows = {}                         -- the sidebar's pressable rows

local function place_row(s, y, p, lit)
  local h = 44

  if lit then s:fill_round(8, y, SIDE_W - 16, h, theme.sunken, 9) end

  s:disc(8 + 10 + 15, y + h // 2, 15, theme.raised)
  s:disc(8 + 10 + 15, y + h // 2, 6, p.layer == "poi" and dot_of(p) or theme.text_dim)
  s:text(8 + 10 + 30 + 12, y + 5, ui.fitted(p.name, SIDE_W - 90, "ui"), theme.text, nil, "ui")
  s:text(8 + 10 + 30 + 12, y + 5 + gfx.height() + 1, ui.fitted(kind_of(p), SIDE_W - 90, "ui"),
         theme.text_dim, nil, "ui")
  rows[#rows + 1] = { x = 8, y = y, w = SIDE_W - 16, h = h, place = p }

  return y + h + 2
end

local function draw_sidebar(s)
  rows = {}

  if not sidebar then return end

  s:fill(0, L.head, SIDE_W, H - L.head, theme.window)
  s:fill(SIDE_W - 1, L.head, 1, H - L.head, theme.line_soft)

  pk.field(s, search, search_focused)
  pk.icon(s, "search", search.x + 12, search.y + (search.h - 15) // 2, theme.text_dim)

  local tx = search.x + 12 + 15 + 10
  local ty = search.y + (search.h - gfx.height()) // 2

  if search.text == "" then
    s:text(tx, ty, "Search Maps", theme.text_dim, nil, "ui")
  else
    s:text(tx, ty, search.text, theme.text, nil, "ui")
  end

  if search_focused then
    local cx_ = tx + (search.text == "" and 0 or gfx.measure(search.text))

    s:fill(cx_, search.y + 9, 2, search.h - 18, theme.accent)
  end

  local y = search.y + search.h + 16

  -- While something is typed: what matches, the chosen one lit.
  if search.text ~= "" then
    y = section(s, y, ("%d found"):format(#results))

    for i, p in ipairs(results) do
      if y + 44 > H then break end
      y = place_row(s, y, p, i == chosen_result)
    end

    if #results == 0 then
      local line = not net and "Nothing here is called that"
                   or world.asked and not world.results and not world.said and "Searching the world\u{2026}"
                   or world.said or "Return searches the world"

      s:text(18, y, line, theme.text_dim, nil, "ui")
    end

    return
  end

  -- Pinned, as round marks along the top, four at most.
  y = section(s, y, "Pinned")

  if #kept.pinned == 0 then
    s:text(18, y, "A place's Pin puts it here", theme.text_dim, nil, "ui")
    y = y + gfx.height() + 18
  else
    for i, p in ipairs(kept.pinned) do
      if i > 4 then break end

      local x = 18 + (i - 1) * 68

      s:disc(x + 22, y + 22, 22, p.layer == "poi" and dot_of(p) or theme.accent)
      s:text(x + 22 - gfx.measure(p.name:sub(1, 1):upper(), "title") // 2,
             y + 22 - gfx.height("title") // 2, p.name:sub(1, 1):upper(), 0xffffffff, nil, "title")
      s:text(x, y + 48, ui.fitted(p.name, 64, "ui"), theme.text_dim, nil, "ui")
      rows[#rows + 1] = { x = x, y = y, w = 64, h = 70, place = p }
    end

    y = y + 78
  end

  y = section(s, y, "Saved")

  if #kept.saved == 0 then
    s:text(18, y, "A place's Save keeps it here", theme.text_dim, nil, "ui")
    y = y + gfx.height() + 18
  else
    for i, p in ipairs(kept.saved) do
      if i > 4 or y + 44 > H then break end
      y = place_row(s, y, p, false)
    end

    y = y + 8
  end

  y = section(s, y, "Recently viewed")

  if #kept.recent == 0 then
    s:text(18, y, "Places you look at appear here", theme.text_dim, nil, "ui")
  else
    for _, p in ipairs(kept.recent) do
      if y + 44 > H then break end
      y = place_row(s, y, p, false)
    end
  end
end

-- A glass panel over the map: the window's colour, its edge.
local function glass(s, x, y, w, h)
  s:fill_round(x, y, w, h, theme.window, 12)
  s:frame_round(x, y, w, h, theme.line_soft, 12)
end

local save_button, pin_button, close_button = {}, {}, { icon = "close" }
local said_card = nil

local function draw_card(s)
  if not card then return end

  local mx, my = map_box()
  local x, y, w = mx + 16, my + 16, 320
  local h = 136

  glass(s, x, y, w, h)
  s:text(x + 16, y + 14, ui.fitted(card.name, w - 64, "title"), theme.text, nil, "title")
  s:text(x + 16, y + 14 + gfx.height("title") + 4, ui.fitted(kind_of(card), w - 32, "ui"),
         theme.text_dim, nil, "ui")

  close_button.x, close_button.y = x + w - 12 - 26, y + 10
  pk.iconbutton(s, close_button)

  local by = y + h - 16 - 31
  local saved, pinned = find_in(kept.saved, card) ~= nil, find_in(kept.pinned, card) ~= nil

  save_button.x, save_button.y, save_button.text = x + 16, by, saved and "Saved" or "Save"
  save_button.go = not saved
  save_button.w = nil
  pk.button(s, save_button)
  pin_button.x, pin_button.y, pin_button.text = save_button.x + save_button.w + 8, by,
                                                pinned and "Pinned" or "Pin"
  pin_button.w = nil
  pk.button(s, pin_button)

  local where = ("save %d,%d; pin %d,%d; close %d,%d"):format(save_button.x, save_button.y,
                pin_button.x, pin_button.y, close_button.x, close_button.y)

  if where ~= said_card then
    said_card = where
    print("maps: card buttons " .. where)
  end
end

local function draw_controls(s)
  local mx, my, mw, mh = map_box()

  glass(s, zoom_in.x, zoom_in.y, 40, 80)
  s:fill(zoom_in.x + 1, zoom_in.y + 40, 38, 1, theme.line_soft)
  pk.icon(s, "plus", zoom_in.x + 12, zoom_in.y + 12, theme.text)
  pk.icon(s, "minus", zoom_out.x + 12, zoom_out.y + 12, theme.text)

  -- North: the map is always north up until it turns (M6).
  local nx, ny = zoom_in.x + 20, zoom_out.y + 40 + 14 + 20

  s:disc(nx, ny, 20, theme.window)
  s:triangle(nx, ny - 13, nx - 6, ny, nx + 6, ny, 0xffff5f57)
  s:triangle(nx, ny + 13, nx - 6, ny, nx + 6, ny, theme.text_dim)

  -- The scale: a round distance and the bar as long as it is.
  local metres_per_point = 40075016.7 / (TILE * 2 ^ zoom)
                           * math.cos(math.rad(select(2, map.unproject(cx, cy))))
  local want = metres_per_point * 100
  local nice = 1

  for _, v in ipairs({ 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000,
                       20000, 50000, 100000, 200000, 500000 }) do
    if v <= want then nice = v end
  end

  local len = math.floor(nice / metres_per_point + 0.5)
  local label = nice >= 1000 and ("%d km"):format(nice // 1000) or ("%d m"):format(nice)
  local bx, by = mx + 16, my + mh - 20

  s:text(bx, by - gfx.height("mono") - 4, label, theme.text, nil, "mono")
  s:fill(bx, by, len, 2, theme.text)
  s:fill(bx, by - 5, 2, 7, theme.text)
  s:fill(bx + len - 2, by - 5, 2, 7, theme.text)

  -- Whose map it is, always on it: OpenFreeMap asks for its line, and
  -- Port Alder says it is made up wherever it is drawn.
  local region_shown = math.floor(zoom) >= info.min_zoom
  local credit = made_up and "Made up for Kosmos - not a real place"
                 or "\u{a9} OpenStreetMap contributors"

  if net then
    credit = "OpenFreeMap  \u{a9} OpenMapTiles  \u{a9} OpenStreetMap"
             .. ((made_up and region_shown) and "  \u{b7}  Port Alder is made up" or "")
  end
  local cw = gfx.measure(credit) + 16
  local ch = gfx.height() + 6

  s:fill_round(mx + mw - cw - 10, my + mh - ch - 8, cw, ch, theme.window, 6)
  s:text(mx + mw - cw - 2, my + mh - ch - 5, credit, theme.text_dim, nil, "ui")
end

--------------------------------------------------------------------------
-- The map itself.
--------------------------------------------------------------------------

local drawn_tiles, drawn_ms = 0, 0

--
-- **The names on the map**, from each visible tile's labels, kept with the
-- tile: the city and its districts, then water, parks, points of interest
-- and street names - each only where nothing written before it is, so they
-- never overlap. A street's name is drawn level, so only along streets
-- that run within thirty degrees of it, until gfx can turn text.
--
local label_cache = {}
local placed_labels = {}
local draw_labels
local drawn_labels = 0

-- From which zoom each kind of place is named - OpenMapTiles' classes,
-- as a printed map names a continent's countries and keeps its provinces
-- for when there is room (`docs/maps.md`; seen on the M700, 8 October, with
-- Africa's provinces covering its cities at zoom 3).
local PLACE_FROM = { continent = 0, country = 2, state = 5, province = 5, city = 4,
                     town = 8, village = 11, hamlet = 13, suburb = 12, quarter = 13,
                     neighbourhood = 14, isolated_dwelling = 15, locality = 15 }

-- Which points of interest are named at a zoom: OpenMapTiles ranks them,
-- the lower the more notable, so at 15 only the most and from 17 all. A
-- city's centre at 15 was every shop's name at once (Montevideo, the M700).
local function POI_RANK(z)
  if z >= 17 then return math.huge elseif z >= 16 then return 20 end

  return 8
end

local ORDER = { place = 1, water_name = 2, park = 3, poi = 4, transportation_name = 5 }

local function labels_of(key, t)
  local l = label_cache[key]

  if l == nil then
    l = t:labels()
    label_cache[key] = l
  end

  return l
end

local function overlaps(b)
  for _, o in ipairs(placed_labels) do
    if b.x < o.x + o.w and o.x < b.x + b.w and b.y < o.y + o.h and o.y < b.y + b.h then
      return true
    end
  end

  return false
end

local function halo_text(s, x, y, text, colour, face)
  local back = 0xff000000 | colours.land

  for _, d in ipairs({ { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }) do
    s:text(x + d[1], y + d[2], text, back, nil, face)
  end

  s:text(x, y, text, colour, nil, face)
end

draw_labels = function(s, drawn, mx, my, mw, mh)
  local candidates = {}
  local named = {}                     -- each name's places this frame

  -- What the card and the controls cover is taken before any name is.
  placed_labels = {}
  drawn_labels = 0

  if card then placed_labels[1] = { x = mx + 16, y = my + 16, w = 320, h = 136 } end

  placed_labels[#placed_labels + 1] = { x = mx + mw - 16 - 40, y = my + 16, w = 40, h = 80 + 14 + 40 }

  for _, d in ipairs(drawn) do
    do
      local key, t, tx, ty, n, size = d.key, d.t, d.tx, d.ty, d.n, d.size

      for _, l in ipairs(labels_of(key, t)) do
        local wanted = (l.layer == "place" and zoom >= (PLACE_FROM[l.class] or 12))
          or (l.layer == "water_name")
          or (l.layer == "park" and zoom >= 14)
          or (l.layer == "poi" and zoom >= 15 and (l.rank or 0) <= POI_RANK(zoom))
          or (l.layer == "transportation_name" and zoom >= 15 and math.abs(l.angle) < 0.52)

        if wanted then
          candidates[#candidates + 1] = {
            l = l, x = d.ox + l.x * size, y = d.oy + l.y * size,
            order = (ORDER[l.layer] or 9) * 100 + (l.rank or 0),
            world_x = (tx + l.x) / n, world_y = (ty + l.y) / n,
          }
        end
      end
    end
  end

  table.sort(candidates, function(a, b)
    if a.order ~= b.order then return a.order < b.order end
    return a.l.name < b.l.name
  end)

  for _, c in ipairs(candidates) do
    local l = c.l
    local face = (l.layer == "place" and l.class == "city") and "title" or "ui"
    local text = l.name
    local colour = theme.text

    if l.layer == "place" and l.class ~= "city" then
      text, face, colour = text_kit.upper(text), "label", theme.text_dim
    elseif l.layer == "water_name" then
      colour = 0xff3a6ea8
    elseif l.layer == "park" then
      colour = 0xff3f7a43
    end

    local tw, th = gfx.measure(text, face), gfx.height(face)
    local dot = l.layer == "poi" and 6 or 0
    local box = { x = c.x - (dot > 0 and dot or tw / 2), y = c.y - th / 2,
                  w = tw + dot * 2 + 4, h = th }

    -- A street crossing two tiles has a name in each: written again only
    -- 300 points from where it was, as a map repeats a long road's name.
    local again = false

    for _, at in ipairs(named[l.name] or {}) do
      if math.abs(at[1] - c.x) < 300 and math.abs(at[2] - c.y) < 300 then again = true end
    end

    if not again and box.x >= mx and box.y >= my and box.x + box.w <= mx + mw
       and box.y + box.h <= my + mh and not overlaps(box) then
      named[l.name] = named[l.name] or {}
      table.insert(named[l.name], { c.x, c.y })
      placed_labels[#placed_labels + 1] = box
      box.place = { name = l.name, class = l.class, layer = l.layer, x = c.world_x, y = c.world_y }

      if dot > 0 then
        s:disc(math.floor(c.x), math.floor(c.y), dot, 0xffffffff)
        s:disc(math.floor(c.x), math.floor(c.y), dot - 2, dot_of(l))
        halo_text(s, math.floor(c.x + dot + 5), math.floor(box.y), text, colour, face)
      else
        halo_text(s, math.floor(box.x), math.floor(box.y), text, colour, face)
      end

      drawn_labels = drawn_labels + 1
    end
  end
end

-- One zoom level's tiles over the map's rectangle, each handed to `each`
-- with where it goes and how large.
local function over_tiles(z, mx, my, mw, mh, each)
  local size = TILE * 2 ^ (zoom - z)
  local n = 1 << z
  local left, top = cx * n * size - mw / 2, cy * n * size - mh / 2
  local x0, y0 = math.floor(left / size), math.floor(top / size)
  local x1, y1 = math.floor((left + mw) / size), math.floor((top + mh) / size)

  for ty = math.max(0, y0), math.min(n - 1, y1) do
    for tx = math.max(0, x0), math.min(n - 1, x1) do
      each(tx, ty, mx + tx * size - left, my + ty * size - top, size, n)
    end
  end

  return n
end

local function draw_map(s)
  local mx, my, mw, mh = map_box()
  local t0 = sys.ticks()
  local drawn = {}

  s:fill(mx, my, mw, mh, 0xff000000 | colours.land)
  drawn_tiles = 0

  -- The world beneath, from the network: at its own deepest past 14, and
  -- a tile not here yet as its nearest ancestor drawn larger.
  if net then
    local z = math.max(0, math.min(NET_MAX, math.floor(zoom)))

    wants = {}

    local n = over_tiles(z, mx, my, mw, mh, function(tx, ty, ox, oy, size, n)
      local t = net_tile(z, tx, ty)

      if t then
        t:draw(s, ox, oy, size, zoom, style, mx, my, mx + mw, my + mh)
        drawn[#drawn + 1] = { t = t, key = "n" .. z .. "/" .. tx .. "/" .. ty,
                              ox = ox, oy = oy, size = size, n = n, tx = tx, ty = ty }
        drawn_tiles = drawn_tiles + 1
      elseif t == nil then
        local p, up = net_parent(z, tx, ty)

        if p then
          local big = size * (1 << up)
          local mask = (1 << up) - 1

          p:draw(s, ox - (tx & mask) * size, oy - (ty & mask) * size, big, zoom, style,
                 math.max(mx, ox), math.max(my, oy),
                 math.min(mx + mw, ox + size), math.min(my + mh, oy + size))
        end
      end
    end)

    send_wants(cx * n, cy * n)
  end

  -- The region on top, where it has tiles: its own detail wins.
  local z = math.floor(zoom)

  if z >= info.min_zoom and z <= info.max_zoom + 3 then
    z = math.min(info.max_zoom, z)

    over_tiles(z, mx, my, mw, mh, function(tx, ty, ox, oy, size, n)
      local t = tile_at(z, tx, ty)

      if t then
        t:draw(s, ox, oy, size, zoom, style, mx, my, mx + mw, my + mh)
        drawn[#drawn + 1] = { t = t, key = z .. "/" .. tx .. "/" .. ty,
                              ox = ox, oy = oy, size = size, n = n, tx = tx, ty = ty }
        drawn_tiles = drawn_tiles + 1
      end
    end)
  end

  draw_labels(s, drawn, mx, my, mw, mh)

  drawn_ms = (sys.ticks() - t0) * 1000 / counter_hz
end

local function draw_all()
  local s = win:surface()

  place_controls()
  draw_map(s)
  draw_controls(s)
  draw_card(s)
  draw_sidebar(s)
  draw_header(s)

  return win:commit{ x = 0, y = 0, w = W, h = H }
end

local function say_where()
  local lon, lat = map.unproject(cx, cy)

  print(("maps: at %.5f %.5f, zoom %.2f, %d tiles in %.1f ms, %d names")
        :format(lon, lat, zoom, drawn_tiles, drawn_ms, drawn_labels))
end

--------------------------------------------------------------------------
-- Moving the map.
--------------------------------------------------------------------------

local function world_size() return TILE * 2 ^ zoom end

local function pan(dx, dy)
  local ws = world_size()

  cx = math.max(0, math.min(1, cx - dx / ws))
  cy = math.max(0, math.min(1, cy - dy / ws))
end

-- Zoomed by `by`, keeping the world under (px, py) - the window's - where
-- it was: what a wheel under the pointer does.
local function zoom_by(by, px, py)
  local mx, my, mw, mh = map_box()
  local to = math.max(MIN_ZOOM, math.min(MAX_ZOOM, zoom + by))
  local ox = (px or mx + mw / 2) - (mx + mw / 2)
  local oy = (py or my + mh / 2) - (my + mh / 2)
  local before = world_size()

  -- The world point under the pointer, then the centre that keeps it there.
  local ux, uy = cx + ox / before, cy + oy / before

  zoom = to

  local after = world_size()

  cx, cy = ux - ox / after, uy - oy / after
end

-- A place shown: the map centred on it, close enough to see it, its card
-- open, and it at the top of Recently viewed.
local function go_to(p)
  cx, cy = p.x, p.y
  zoom = p.zoom and math.max(MIN_ZOOM, math.min(MAX_ZOOM, p.zoom)) or math.max(zoom, 16)
  card = p
  toggle_in(kept.recent, p, 8)
  if find_in(kept.recent, p) == nil then toggle_in(kept.recent, p, 8) end
  keep()
  print("maps: card " .. p.name)
end

local function toggle_sidebar()
  sidebar = not sidebar
  search_focused = search_focused and sidebar
  print("maps: sidebar " .. (sidebar and "shown" or "hidden"))
end

local function dots_menu()
  local items = {
    { text = made_up and "Port Alder (made up)" or "Port Alder", on_choose = function()
      if not made_up then
        fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/maps.lua", wait = false })
      end
    end },
  }

  -- Regions of one's own, in /Home/Maps.
  for _, name in ipairs((fs.list and fs.list("/Home/Maps")) or {}) do
    local n = type(name) == "table" and name.name or name

    if tostring(n):match("%.pmtiles$") then
      items[#items + 1] = { text = tostring(n), on_choose = function()
        fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/maps.lua",
                                 args = "/Home/Maps/" .. tostring(n), wait = false })
      end }
    end
  end

  win:open_menu(win.origin_x + dots.x, win.origin_y + L.head, items)
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

local decode = keys.decoder()
local dragging = nil
local dirty = true
local last_press, last_x, last_y = 0, 0, 0

local function inside_map(x, y)
  local mx, my, mw, mh = map_box()

  return x >= mx and x < mx + mw and y >= my and y < my + mh
end

-- **A click is a press and a release** (Diego, 9 October 2026: "mouse
-- click is working in mouse down not in a real click which is mouse
-- down+mouse up"). A button is held on the press (`pk.hold`) and done on
-- the release over it; a row or a label is remembered by its place and
-- gone to on the release if the same place is under the pointer still -
-- found again then, since rows and labels are made anew on every draw and
-- a search's answer can rebuild them between the two.
local clicked_moved = false             -- a held control moved the map
local place_held = nil                  -- { from = "row" or "label", key = ... }

local function place_key(p)
  return ("%s@%s,%s"):format(p.name or "", tostring(p.x), tostring(p.y))
end

local function row_at(x, y)
  for _, r in ipairs(rows) do
    if pk.inside(r, x, y) then return r.place end
  end
end

local function label_at(x, y)
  local on_label = nil

  for _, box in ipairs(placed_labels) do
    if box.place and pk.inside(box, x, y) then on_label = box.place end
  end

  return on_label
end

-- The card opened on a place, and it at the top of Recently viewed.
local function open_card(p)
  card = p
  toggle_in(kept.recent, card, 8)
  if find_in(kept.recent, card) == nil then toggle_in(kept.recent, card, 8) end
  keep()
  print("maps: card " .. card.name)
end

-- The button come up: a held control done if it is under the pointer, a
-- held place gone to if it is the same one. True when there is anything
-- to draw again.
local function release(x, y)
  if pk.release(x, y) then return true end

  if place_held then
    local held = place_held
    local p = nil

    place_held = nil

    if held.from == "row" then
      p = sidebar and x < SIDE_W and row_at(x, y)
    else
      p = inside_map(x, y) and label_at(x, y)
    end

    if p and place_key(p) == held.key then
      if held.from == "row" then
        go_to(p)
        clicked_moved = true
      else
        open_card(p)
      end
    end

    return true
  end

  return false
end

local function key(c)
  local k, mods = keys.parts(c)
  local step = 120

  if search_focused then
    local typed = search.text

    if k == keys.ESCAPE then
      search_focused = false
    elseif k == keys.ENTER or k == 10 then
      if results[chosen_result] then
        go_to(results[chosen_result])
      else
        find_in_world(search.text)
      end
    elseif k == keys.DOWN then
      chosen_result = math.min(#results, chosen_result + 1)
    elseif k == keys.UP then
      chosen_result = math.max(1, chosen_result - 1)
    elseif k == 8 or k == 127 then
      search.text = search.text:sub(1, (utf8.offset(search.text, -1) or 1) - 1)
    elseif k >= 32 and mods == 0 then
      search.text = search.text .. utf8.char(k)
    else
      return false
    end

    if search.text ~= typed then
      world.asked, world.results, world.said = nil, nil, nil
      if net then net.finding = nil end
      results, chosen_result = search_for(search.text), 1
      print(("maps: search %q, %d found%s"):format(search.text, #results,
            results[1] and (", first " .. results[1].name) or ""))
    end

    return true
  end

  -- Ctrl+B, which arrives as the control character it is.
  if c == 2 or (k == 98 and mods & keys.CTRL ~= 0) then
    toggle_sidebar()
  elseif k == keys.LEFT then pan(step, 0)
  elseif k == keys.RIGHT then pan(-step, 0)
  elseif k == keys.UP then pan(0, step)
  elseif k == keys.DOWN then pan(0, -step)
  elseif k == 43 or k == 61 then zoom_by(1)
  elseif k == 45 then zoom_by(-1)
  else
    return false
  end

  return true
end

if not draw_all() then return end
say_where()

while win.running do
  local reply = wmproto.poll(win.handle, dirty and 0 or 25)

  if not reply then break end

  -- Tiles that came are drawn, and where the map is said again with them.
  local moved = poll_arrived()

  if moved then dirty = true end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      dirty = true
    elseif ev.type == "resize" then
      W, H = ev.w, ev.h
      print(("maps: resized to %dx%d"):format(W, H))
      dirty = true
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "theme" then
      -- The look changed (applied by `direct_event`): the dark map for a
      -- dark look, the light one for a light look.
      colours, style = style_for(is_dark())
      print("maps: the map is " .. (is_dark() and "dark" or "light"))
      dirty = true
    elseif ev.type == "key" then
      local a, b = decode(ev.code)

      for _, c in ipairs({ a, b }) do
        if key(c) then dirty, moved = true, true end
      end
    elseif ev.type == "wheel" then
      if inside_map(ev.x or 0, ev.y or 0) then
        zoom_by((ev.n or 0) * 0.5, ev.x, ev.y)
        dirty, moved = true, true
      end
    elseif ev.type == "mouse" and not ev.menu then
      local x, y = ev.x or 0, ev.y or 0

      if ev.action == "press" and ev.button ~= "right" then
        local t = sys.ticks()
        local double = (t - last_press) < counter_hz * 0.4
                       and math.abs(x - last_x) < 6 and math.abs(y - last_y) < 6

        last_press, last_x, last_y = t, x, y

        -- The buttons are held, and done on the release over them; the
        -- dots open their menu on the press, as a menu does.
        if pk.inside(side_button, x, y) then
          pk.hold(side_button, toggle_sidebar)
          dirty = true
        elseif pk.inside(dots, x, y) then
          dots_menu()
        elseif y < L.head then
          win:take_hold(x, y)
        elseif card and pk.inside(close_button, x, y) then
          pk.hold(close_button, function() card = nil end)
          dirty = true
        elseif card and pk.inside(save_button, x, y) then
          pk.hold(save_button, function()
            if not card then return end

            local now = toggle_in(kept.saved, card, 50)
            keep()
            print(("maps: %s %s"):format(now and "saved" or "unsaved", card.name))
          end)
          dirty = true
        elseif card and pk.inside(pin_button, x, y) then
          pk.hold(pin_button, function()
            if not card then return end

            local now = toggle_in(kept.pinned, card, 4)
            keep()
            print(("maps: %s %s"):format(now and "pinned" or "unpinned", card.name))
          end)
          dirty = true
        elseif pk.inside(zoom_in, x, y) then
          pk.hold(zoom_in, function() zoom_by(1) clicked_moved = true end)
        elseif pk.inside(zoom_out, x, y) then
          pk.hold(zoom_out, function() zoom_by(-1) clicked_moved = true end)
        elseif sidebar and x < SIDE_W then
          -- The field takes the caret on the press; a row goes to its
          -- place on the release.
          search_focused = pk.inside(search, x, y)

          local p = row_at(x, y)

          if p then place_held = { from = "row", key = place_key(p) } end

          dirty = true
        elseif inside_map(x, y) then
          search_focused = false

          local on_label = label_at(x, y)

          -- A double press zooms on the second press, and a press
          -- anywhere else on the map begins a pan.
          if double then
            zoom_by(1, x, y)
            dirty, moved = true, true
          elseif on_label then
            place_held = { from = "label", key = place_key(on_label) }
          else
            dragging = { x = x, y = y }
          end
        end
      elseif ev.action == "move" and dragging then
        pan(x - dragging.x, y - dragging.y)
        dragging.x, dragging.y = x, y
        dirty, moved = true, true
      elseif ev.action == "release" then
        if release(x, y) then dirty = true end

        if clicked_moved then
          clicked_moved = false
          moved = true
        end

        -- Where a drag ended is said once it has, as a zoom's is.
        if dragging then dirty, moved = true, true end
        dragging = nil
      end
    end
  end

  if not win.running then break end

  if dirty then
    dirty = false
    if not draw_all() then break end
    if moved and not dragging then say_where() end
  end
end
