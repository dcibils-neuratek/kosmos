-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **How a map is drawn in Kosmos** (`docs/flight.md` F5k): the Map Kit's
-- style for OpenMapTiles' vector tiles, in Night's colours or a light
-- map's, and the colours themselves for what a program draws over it.
-- Maps' own until Flight's minimap wanted the same map; a second copy of
-- the rules would have been two maps that disagree.
--
--     local mapstyle = use("/Kosmos/Libraries/mapstyle.lua")
--     local colours, style = mapstyle.style_for(dark)
--     tile:draw(surface, x, y, size, zoom, style)

local map = use("/Kosmos/Kits/map")

local mapstyle = {}

function mapstyle.style_for(dark)
  local c = dark and {
    -- Land and water far enough apart that a coast is seen at a glance -
    -- the M700, 8 October: at 0x1d2433 against 0x10233f Africa was barely
    -- there at zoom 3.
    land = 0x283142, block = 0x2d3749, park = 0x24423a, water = 0x0c1828,
    building = 0x36415a, casing = 0x1a2130, minor = 0x445168, major = 0x5e6d87,
    motor = 0x8a6f3e, path = 0x4a5568, border = 0x8a7bb0, border_minor = 0x4f4868,
  } or {
    land = 0xf2efe9, block = 0xe8e3da, park = 0xcfe8c8, water = 0xaad3f2,
    building = 0xdcd5c9, casing = 0xd9d2c6, minor = 0xffffff, major = 0xfde8a8,
    motor = 0xf6bb6a, path = 0xc8bfae, border = 0x9c84b3, border_minor = 0xcfc3da,
  }

  return c, map.style{ background = c.land, rules = {
    { layer = "landuse", classes = "residential,commercial,industrial,retail", fill = c.block },
    { layer = "landcover", classes = "wood,forest,grass,farmland,meadow", fill = c.park },
    { layer = "park", fill = c.park },
    { layer = "water", fill = c.water },
    { layer = "waterway", line = c.water, width = { 12, 1, 18, 8 } },
    -- Borders by level (OpenMapTiles' `admin_level`): a country's wider and
    -- stronger than a province's, which waits for zoom 4 to be drawn at all.
    { layer = "boundary", admin = { 3, 8 }, line = c.border_minor, width = { 4, 0.5, 10, 1 },
      minzoom = 4 },
    { layer = "boundary", admin = { 1, 2 }, line = c.border, width = { 2, 1, 10, 2.5 } },
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

return mapstyle
