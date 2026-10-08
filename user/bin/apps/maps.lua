-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Locale
-- kosmos: name Maps
-- kosmos: section applications
--
-- Maps: OpenStreetMap's world as shapes, drawn by Kosmos (`docs/maps.md`,
-- the window `docs/maps.html`, agreed by Diego on 8 October 2026: "mockup
-- is great", "but i do want a close/open sidebar button always visible").
--
--   maps                       the region the image carries, Port Alder
--   maps /Home/Maps/x.pmtiles  a region of one's own
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
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

--------------------------------------------------------------------------
-- The region: the image's own, or a file named on the command line.
--------------------------------------------------------------------------

local SHIPPED = "maps/port-alder.pmtiles"
local region_path = tostring(args or ""):match("^%s*(%S+)") or ""
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
    land = 0x1d2433, block = 0x222b3c, park = 0x1f3a32, water = 0x10233f,
    building = 0x2b3547, casing = 0x151b27, minor = 0x3a4659, major = 0x56647c,
    motor = 0x8a6f3e, path = 0x4a5568,
  } or {
    land = 0xf2efe9, block = 0xe8e3da, park = 0xcfe8c8, water = 0xaad3f2,
    building = 0xdcd5c9, casing = 0xd9d2c6, minor = 0xffffff, major = 0xfde8a8,
    motor = 0xf6bb6a, path = 0xc8bfae,
  }

  return c, map.style{ background = c.land, rules = {
    { layer = "landuse", classes = "residential,commercial,industrial,retail", fill = c.block },
    { layer = "landcover", classes = "wood,forest,grass,farmland,meadow", fill = c.park },
    { layer = "park", fill = c.park },
    { layer = "water", fill = c.water },
    { layer = "waterway", line = c.water, width = { 12, 1, 18, 8 } },
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

--------------------------------------------------------------------------
-- Where the map is looking: its centre across the world, 0 to 1, and its
-- zoom, which need not be whole.
--------------------------------------------------------------------------

local cx, cy = map.project(info.lon, info.lat)
local zoom = math.max(info.min_zoom, math.min(info.max_zoom, (info.zoom or 14) > 0 and info.zoom or 14))
local MIN_ZOOM, MAX_ZOOM = math.max(2, info.min_zoom - 1), info.max_zoom + 3
local TILE = 512

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

local function tile_at(z, x, y)
  local key = z .. "/" .. x .. "/" .. y
  local t = cache[key]

  if t == nil then
    t = region:tile(z, x, y) or false
    cache[key] = t
    order[#order + 1] = key

    if #order > 96 then
      cache[table.remove(order, 1)] = nil
    end
  end

  return t or nil
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
  pk.header(s, 0, 0, W, "Maps", made_up and "Port Alder, a made-up city" or region_path,
            dots.x - 8, side_button.x + 26 + 10)
  side_button.pressed = sidebar
  pk.iconbutton(s, side_button)
  pk.iconbutton(s, dots)
end

local function section(s, y, text)
  s:text(18, y, text:upper(), theme.text_dim, nil, "label")

  return y + gfx.height("label") + 8
end

local function draw_sidebar(s)
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

  local y = search.y + search.h + 22

  y = section(s, y, "Pinned")
  s:text(18, y, "Nothing pinned yet", theme.text_dim, nil, "ui")
  y = y + gfx.height() + 22
  y = section(s, y, "Saved")
  s:text(18, y, "Nothing saved yet", theme.text_dim, nil, "ui")
  y = y + gfx.height() + 22
  y = section(s, y, "Recently viewed")
  s:text(18, y, "Places you look at appear here", theme.text_dim, nil, "ui")
end

-- A glass panel over the map: the window's colour, its edge.
local function glass(s, x, y, w, h)
  s:fill_round(x, y, w, h, theme.window, 12)
  s:frame_round(x, y, w, h, theme.line_soft, 12)
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

  -- Whose map it is, always on it.
  local credit = made_up and "Made up for Kosmos - not a real place"
                 or "\u{a9} OpenStreetMap contributors"
  local cw = gfx.measure(credit) + 16
  local ch = gfx.height() + 6

  s:fill_round(mx + mw - cw - 10, my + mh - ch - 8, cw, ch, theme.window, 6)
  s:text(mx + mw - cw - 2, my + mh - ch - 5, credit, theme.text_dim, nil, "ui")
end

--------------------------------------------------------------------------
-- The map itself.
--------------------------------------------------------------------------

local drawn_tiles, drawn_ms = 0, 0

local function draw_map(s)
  local mx, my, mw, mh = map_box()
  local z = math.max(info.min_zoom, math.min(info.max_zoom, math.floor(zoom)))
  local size = TILE * 2 ^ (zoom - z)
  local n = 1 << z
  local t0 = sys.ticks()

  s:fill(mx, my, mw, mh, 0xff000000 | colours.land)

  -- The world's pixel at the window's middle of the map is the centre.
  local wx, wy = cx * n * size, cy * n * size
  local left, top = wx - mw / 2, wy - mh / 2
  local x0, y0 = math.floor(left / size), math.floor(top / size)
  local x1, y1 = math.floor((left + mw) / size), math.floor((top + mh) / size)

  drawn_tiles = 0

  for ty = math.max(0, y0), math.min(n - 1, y1) do
    for tx = math.max(0, x0), math.min(n - 1, x1) do
      local t = tile_at(z, tx, ty)

      if t then
        t:draw(s, mx + tx * size - left, my + ty * size - top, size, zoom, style,
               mx, my, mx + mw, my + mh)
        drawn_tiles = drawn_tiles + 1
      end
    end
  end

  drawn_ms = (sys.ticks() - t0) * 1000 / counter_hz
end

local function draw_all()
  local s = win:surface()

  place_controls()
  draw_map(s)
  draw_controls(s)
  draw_sidebar(s)
  draw_header(s)

  return win:commit{ x = 0, y = 0, w = W, h = H }
end

local function say_where()
  local lon, lat = map.unproject(cx, cy)

  print(("maps: at %.5f %.5f, zoom %.2f, %d tiles in %.1f ms")
        :format(lon, lat, zoom, drawn_tiles, drawn_ms))
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

local function key(c)
  local k, mods = keys.parts(c)
  local step = 120

  if search_focused then
    if k == keys.ESCAPE or k == keys.ENTER then
      search_focused = false
    elseif k == 8 or k == 127 then
      search.text = search.text:sub(1, (utf8.offset(search.text, -1) or 1) - 1)
    elseif k >= 32 and mods == 0 then
      search.text = search.text .. utf8.char(k)
    else
      return false
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

  local moved = false

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
      colours, style = style_for(is_dark())
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

        if pk.inside(side_button, x, y) then
          toggle_sidebar()
          dirty = true
        elseif pk.inside(dots, x, y) then
          dots_menu()
        elseif y < L.head then
          win:take_hold(x, y)
        elseif pk.inside(zoom_in, x, y) then
          zoom_by(1)
          dirty, moved = true, true
        elseif pk.inside(zoom_out, x, y) then
          zoom_by(-1)
          dirty, moved = true, true
        elseif sidebar and x < SIDE_W then
          search_focused = pk.inside(search, x, y)
          dirty = true
        elseif inside_map(x, y) then
          search_focused = false

          if double then
            zoom_by(1, x, y)
            dirty, moved = true, true
          else
            dragging = { x = x, y = y }
          end
        end
      elseif ev.action == "move" and dragging then
        pan(x - dragging.x, y - dragging.y)
        dragging.x, dragging.y = x, y
        dirty, moved = true, true
      elseif ev.action == "release" then
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
