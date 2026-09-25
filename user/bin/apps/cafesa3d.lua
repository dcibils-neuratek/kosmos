-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Teapot
-- kosmos: section applications
-- Cafesa3D: scenes of spheres, cubes and cylinders, arranged in a 3D view,
-- after Blender (`roadmap.md` 4l), as `docs/cafesa3d.html` draws it.
--
-- Diego, 25 September 2026: "A simple 3d modeling and animation tool like
-- blender3d", "I want to be able to design and render 3d scenes with 3d
-- rendering algorithms like ray tracing", and on the drawing: "3d tool is
-- perfect", "Let's call it Cafesa3D", "Let's build it".
--
-- **Step one of six**: the window, the Solid and Wireframe views, and
-- selection - in the view, in the Outliner, and shown in Properties. Adding
-- and moving things is step two, the ray tracer step three; their controls
-- are here already, drawn as the drawing has them, and wait for their step.
--
-- **A window that draws its own pixels**, as Camera and Video are: the 3D
-- view is a surface's worth of pixels on every turn of it, which a widget
-- cannot carry. So the header comes from `pixelkit` and the rest is drawn
-- here with a surface's primitives - and **no pixel of the 3D view is
-- computed in this file**: the 3D Kit (`/kits/3d`) holds the scene and
-- draws it, and this file decides what is in it and where the eye is.
--
--   cafesa3d              the still life
--   cafesa3d --wire       starting in Wireframe

local ui = use("/lib/ui.lua")
local theme = ui.theme
local pk = use("/lib/pixelkit.lua").new(ui)
local wmproto = use("/lib/wmproto.lua")
local k3 = use("/kits/3d")
local game = use("/kits/game")
local L = ui.layout

--------------------------------------------------------------------------
-- The window, at the drawing's 1400 by 820, or smaller on a smaller screen.
--------------------------------------------------------------------------

local screen = fs.read("/dev/screen") or {}
local W = math.min(1400, (screen.width or 1920) - 40)
local H = math.min(820, (screen.height or 1080) - 110)

local HEAD, FOOT = L.head, 30
local TOOLS, SIDE = 46, 340
local VX, VY = TOOLS, HEAD
local VW, VH = W - TOOLS - SIDE, H - HEAD - FOOT
local SX = W - SIDE                         -- the side panel's left edge
local PANEL_T = 32                          -- a panel's title strip
local OUT_Y = HEAD + PANEL_T                -- the Outliner's first row
local OUT_H = 236
local PROPS_Y = OUT_Y + OUT_H               -- Properties, under it
local TABS_W = 38
local ROW = 27

local win = ui.window{ title = "Cafesa3D", w = W, h = H, centre = true,
                       direct = true }

if not win or not win:surface() then
  print("cafesa3d: no window")
  return
end

local scene = k3.scene()
local view, why = k3.view(VW, VH)

if not view then
  print("cafesa3d: " .. tostring(why))
  return
end

local small = ui.sized("ui", 15)            -- the view's overlay, 11.5 in CSS
local tiny = ui.sized("label", 13)          -- a panel's title, 11 in CSS

--------------------------------------------------------------------------
-- The scene: every object with its name, and for a shape its id in the
-- kit. Materials are Blender's Principled names (`docs/cafesa3d.html`);
-- the Solid view shows each object in its display colour, which is its
-- base colour unless it says otherwise, as Blender's viewport colour is.
--------------------------------------------------------------------------

local PRESETS = {
  Plastic = { metallic = 0, rough = 0.35, trans = 0, ior = 1.5, emit = 0 },
  Metal   = { metallic = 1, rough = 0.22, trans = 0, ior = 1.5, emit = 0 },
  Mirror  = { metallic = 1, rough = 0,    trans = 0, ior = 1.5, emit = 0 },
  Glass   = { metallic = 0, rough = 0,    trans = 1, ior = 1.5, emit = 0 },
  Light   = { metallic = 0, rough = 1,    trans = 0, ior = 1.5, emit = 5 },
}
local PRESET_ORDER = { "Plastic", "Metal", "Mirror", "Glass", "Light" }

local function material(base, preset, extra)
  local m = { base = base, preset = preset }

  for k, v in pairs(PRESETS[preset]) do m[k] = v end
  for k, v in pairs(extra or {}) do m[k] = v end

  return m
end

local things = {}                           -- in the order they were added
local SHAPES = { plane = true, box = true, sphere = true, cylinder = true,
                 ico = true, cone = true, torus = true, grid = true }

-- What the kit is told about a shape: where it is, and how the Solid view
-- shows it. Glass is drawn through, as the drawing has it.
local function shown(t)
  local m = t.mat
  local glass = m and m.trans >= 0.5

  return {
    size = t.size, radius = t.radius, radius2 = t.radius2, depth = t.depth,
    segments = t.segments, rings = t.rings, subdivisions = t.subdivisions,
    loc = t.loc, rot = t.rot or { 0, 0, 0 }, scale = t.scale or { 1, 1, 1 },
    colour = t.display or (glass and 0xd1e0ed) or (m and m.base) or 0xcccccc,
    alpha = glass and 0.45 or 1,
    smooth = t.smooth or false,
    hidden = t.hidden or false,
  }
end

local function add(t)
  if SHAPES[t.kind] then
    t.rot = t.rot or { 0, 0, 0 }
    t.scale = t.scale or { 1, 1, 1 }

    local fields = shown(t)

    fields.kind = t.kind
    t.id = assert(scene:add(fields))
  end

  things[#things + 1] = t
  return t
end

local function sync(t)
  if t.id then scene:set(t.id, shown(t)) end
end

local function by_id(id)
  for _, t in ipairs(things) do
    if t.id == id then return t end
  end
end

-- A deep copy, for undo and for Shift D: an object is tables of numbers.
local function copy(v)
  if type(v) ~= "table" then return v end

  local out = {}

  for k, x in pairs(v) do out[k] = copy(x) end

  return out
end

-- A name nothing else has, as Blender makes them: Cube, then Cube.001.
local function unique(name)
  local base = name:gsub("%.%d%d%d$", "")
  local taken = {}

  for _, t in ipairs(things) do taken[t.name] = true end

  if not taken[base] then return base end

  for n = 1, 999 do
    local try = ("%s.%03d"):format(base, n)

    if not taken[try] then return try end
  end

  return base
end

local function forget(t)
  if t.id then scene:remove(t.id) end

  for i, x in ipairs(things) do
    if x == t then table.remove(things, i) break end
  end
end

-- The still life the drawing opens on: a ground, a red cube, a glass ball,
-- a gold one, a blue cylinder, one light and a camera.
add{ name = "Ground", kind = "plane", size = 100, loc = { 0, 0, 0 },
     display = 0x6b6d72, mat = material(0xc9c6bf, "Plastic", { rough = 1 }) }
local selected = add{ name = "Cube", kind = "box", size = { 1.5, 1.5, 1.5 },
     loc = { -1.75, 0.45, 0.75 }, rot = { 0, 0, 24 },
     mat = material(0xc33b2c, "Plastic") }
add{ name = "Glass", kind = "sphere", radius = 0.9, segments = 32, rings = 16,
     loc = { 0.75, -0.8, 0.9 }, smooth = true, mat = material(0xffffff, "Glass") }
add{ name = "Gold", kind = "sphere", radius = 0.6, segments = 32, rings = 16,
     loc = { 2.3, 1.05, 0.6 }, smooth = true, mat = material(0xe0a84a, "Metal") }
add{ name = "Cylinder", kind = "cylinder", radius = 0.55, depth = 1.6,
     segments = 32, loc = { 0.05, 2.15, 0.8 }, smooth = true,
     mat = material(0x3d6fc4, "Plastic") }
add{ name = "Light", kind = "light", loc = { -2.4, -3.1, 5.4 }, radius = 0.5,
     power = 1000, colour = 0xfff2e2 }
add{ name = "Camera", kind = "camera", loc = { 7.3, -6.7, 3.9 },
     target = { 0.15, 0.25, 0.7 }, focal = 50 }

--------------------------------------------------------------------------
-- The eye: turned about a point, as Blender's view is. Elevation is held
-- short of straight up and down, where "up" would stop meaning anything.
--------------------------------------------------------------------------

local orbit = { az = -0.74, el = 0.40, dist = 12.5, target = { 0.15, 0.35, 0.6 } }
local FOV = 2 * math.atan(18 / 50) * 1.25   -- the drawing's: 50 mm, a little wide

local function eye()
  local ce = math.cos(orbit.el)
  local t = orbit.target

  return { t[1] + orbit.dist * ce * math.cos(orbit.az),
           t[2] + orbit.dist * ce * math.sin(orbit.az),
           t[3] + orbit.dist * math.sin(orbit.el) }
end

-- The camera's own frame of reference: right and up, for panning.
local function axes()
  local e, t = eye(), orbit.target
  local f = { t[1] - e[1], t[2] - e[2], t[3] - e[3] }
  local n = math.sqrt(f[1] ^ 2 + f[2] ^ 2 + f[3] ^ 2)

  f = { f[1] / n, f[2] / n, f[3] / n }

  local r = { f[2], -f[1], 0 }
  local rn = math.sqrt(r[1] ^ 2 + r[2] ^ 2)

  r = { r[1] / rn, r[2] / rn, 0 }

  local u = { r[2] * f[3] - r[3] * f[2], r[3] * f[1] - r[1] * f[3],
              r[1] * f[2] - r[2] * f[1] }

  return r, u, f
end

local function look()
  local e, t = eye(), orbit.target

  view:look(e[1], e[2], e[3], t[1], t[2], t[3], FOV)
end

--------------------------------------------------------------------------
-- What is showing.
--------------------------------------------------------------------------

local shading = (tostring(args or "")):find("%-%-wire") and "wire" or "solid"
local tab = "object"
local drawn_triangles = 0
local VIEW_NAME = "User Perspective"
local view_name = VIEW_NAME

local VP_INK, VP_DIM, SEL = 0xffd9dde4, 0xff9aa1ad, 0xffffa53d

-- G, R and S (below), declared here because the view draws them.
local modal = nil
local modal_line
local AXES = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }
local AXIS_NAME = { "X", "Y", "Z" }
local AXIS_COLOUR = { 0xe0524b, 0x7cbb3a, 0x4a86e0 }

-- 0xRRGGBB to a surface's colour, and a colour's three channels for a line.
local function opaque(c) return 0xff000000 | (c & 0xffffff) end

local function rgb(c)
  return (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff
end

local function line(s, x0, y0, x1, y1, colour, alpha)
  local r, g, b = rgb(colour)

  game.line(s, 0, 0, x0, y0, x1, y1, r, g, b, alpha or 1)
end

local function polyline(s, pts, colour, alpha)
  for i = 1, #pts - 1 do
    line(s, pts[i][1], pts[i][2], pts[i + 1][1], pts[i + 1][2], colour, alpha)
  end
end

local function ring(s, cx, cy, r, colour, from, to)
  local pts = {}

  from, to = from or 0, to or 2 * math.pi

  for i = 0, 24 do
    local a = from + (to - from) * i / 24

    pts[#pts + 1] = { cx + r * math.cos(a), cy + r * math.sin(a) }
  end

  polyline(s, pts, colour)
end

local function round(v) return math.floor(v + 0.5) end

-- A point in the world, in the window's pixels; nil behind the eye.
local function on_screen(p)
  local x, y = view:project(p[1], p[2], p[3])

  if not x then return nil end

  return VX + x, VY + y
end

--------------------------------------------------------------------------
-- Line pictures for the tools and the Outliner, drawn with the same strokes
-- as the drawing's: 18 across, for a tool; 14, for a row.
--------------------------------------------------------------------------

local GLYPH = {}

function GLYPH.select(s, x, y, c)
  s:triangle(x + 4, y + 2, x + 14, y + 9, x + 5, y + 14, c)
  s:triangle(x + 9, y + 10, x + 12, y + 16, x + 10, y + 16, c)
end

function GLYPH.cursor(s, x, y, c)
  local cx, cy = x + 9, y + 9

  for i = 0, 7, 2 do
    ring(s, cx, cy, 5, c & 0xffffff, i * math.pi / 4, (i + 1) * math.pi / 4)
  end

  line(s, cx, y + 1, cx, y + 5, c) line(s, cx, y + 13, cx, y + 17, c)
  line(s, x + 1, cy, x + 5, cy, c) line(s, x + 13, cy, x + 17, cy, c)
end

function GLYPH.move(s, x, y, c)
  local cx, cy = x + 9, y + 9

  line(s, cx, y + 2, cx, y + 16, c) line(s, x + 2, cy, x + 16, cy, c)
  polyline(s, { { cx - 2.5, y + 4.5 }, { cx, y + 2 }, { cx + 2.5, y + 4.5 } }, c)
  polyline(s, { { cx - 2.5, y + 13.5 }, { cx, y + 16 }, { cx + 2.5, y + 13.5 } }, c)
  polyline(s, { { x + 4.5, cy - 2.5 }, { x + 2, cy }, { x + 4.5, cy + 2.5 } }, c)
  polyline(s, { { x + 13.5, cy - 2.5 }, { x + 16, cy }, { x + 13.5, cy + 2.5 } }, c)
end

function GLYPH.rotate(s, x, y, c)
  ring(s, x + 9, y + 9, 5.5, c, -math.pi * 0.25, math.pi * 1.45)
  polyline(s, { { x + 13.4, y + 2.6 }, { x + 13.4, y + 5.6 }, { x + 10.4, y + 5.6 } }, c)
end

function GLYPH.scale(s, x, y, c)
  polyline(s, { { x + 2.5, y + 7.5 }, { x + 10.5, y + 7.5 }, { x + 10.5, y + 15.5 },
                { x + 2.5, y + 15.5 }, { x + 2.5, y + 7.5 } }, c)
  line(s, x + 9, y + 9, x + 15.5, y + 2.5, c)
  polyline(s, { { x + 11, y + 2.5 }, { x + 15.5, y + 2.5 }, { x + 15.5, y + 7 } }, c)
end

function GLYPH.measure(s, x, y, c)
  polyline(s, { { x + 2.5, y + 12.5 }, { x + 12.5, y + 2.5 }, { x + 15.5, y + 5.5 },
                { x + 5.5, y + 15.5 }, { x + 2.5, y + 12.5 } }, c)
  line(s, x + 6, y + 9, x + 7.5, y + 10.5, c)
  line(s, x + 8.5, y + 6.5, x + 10, y + 8, c)
  line(s, x + 11, y + 4, x + 12.5, y + 5.5, c)
end

function GLYPH.mesh(s, x, y, c)
  polyline(s, { { x + 7, y + 1.5 }, { x + 13, y + 11.5 }, { x + 1, y + 11.5 },
                { x + 7, y + 1.5 } }, c)
end

function GLYPH.light(s, x, y, c)
  ring(s, x + 7, y + 6, 3.5, c)
  line(s, x + 5.5, y + 10.5, x + 8.5, y + 10.5, c)
  line(s, x + 6, y + 12.5, x + 8, y + 12.5, c)
end

function GLYPH.camera(s, x, y, c)
  polyline(s, { { x + 1.5, y + 4 }, { x + 9.5, y + 4 }, { x + 9.5, y + 11 },
                { x + 1.5, y + 11 }, { x + 1.5, y + 4 } }, c)
  polyline(s, { { x + 9.5, y + 6.5 }, { x + 13.5, y + 4.5 }, { x + 13.5, y + 10.5 },
                { x + 9.5, y + 8.5 } }, c)
end

function GLYPH.collection(s, x, y, c)
  polyline(s, { { x + 1.5, y + 3 }, { x + 12.5, y + 3 }, { x + 12.5, y + 12 },
                { x + 1.5, y + 12 }, { x + 1.5, y + 3 } }, c)
  line(s, x + 1.5, y + 5.5, x + 12.5, y + 5.5, c)
end

-- An eye: an almond and its pupil; shut, the lower lid and three lashes.
function GLYPH.eye(s, x, y, c, shut)
  local pts = {}

  for i = 0, 24 do
    local a = math.pi * i / 12

    pts[#pts + 1] = { x + 7 + 6.5 * math.cos(a), y + 7 + 3.8 * math.sin(a) }
  end

  if shut then
    local lid = {}

    for i = 0, 12 do lid[#lid + 1] = pts[i + 1] end
    polyline(s, lid, c)
    line(s, x + 3, y + 10, x + 2, y + 12.5, c)
    line(s, x + 7, y + 11, x + 7, y + 13.5, c)
    line(s, x + 11, y + 10, x + 12, y + 12.5, c)
    return
  end

  polyline(s, pts, c)
  s:disc(x + 7, y + 7, 2, c, true)
end

-- The five tabs of Properties.
function GLYPH.render(s, x, y, c)
  polyline(s, { { x + 1.5, y + 3.5 }, { x + 14.5, y + 3.5 }, { x + 14.5, y + 12.5 },
                { x + 1.5, y + 12.5 }, { x + 1.5, y + 3.5 } }, c)
  ring(s, x + 8, y + 8, 2, c)
end

function GLYPH.world(s, x, y, c)
  ring(s, x + 8, y + 8, 6, c)
  line(s, x + 2, y + 8, x + 14, y + 8, c)
  ring(s, x + 8, y + 8, 6, c, -math.pi / 2, math.pi / 2)
end

function GLYPH.object(s, x, y, c)
  s:triangle(x + 8, y + 1, x + 14, y + 5, x + 8, y + 8, c)
  s:triangle(x + 8, y + 1, x + 2, y + 5, x + 8, y + 8, c)
  polyline(s, { { x + 2, y + 5 }, { x + 2, y + 11 }, { x + 8, y + 15 }, { x + 14, y + 11 },
                { x + 14, y + 5 } }, c)
  line(s, x + 8, y + 8, x + 8, y + 15, c)
end

function GLYPH.data(s, x, y, c)
  polyline(s, { { x + 8, y + 2 }, { x + 14, y + 12 }, { x + 2, y + 12 }, { x + 8, y + 2 } }, c)
  s:disc(x + 8, y + 2, 1, c) s:disc(x + 14, y + 12, 1, c) s:disc(x + 2, y + 12, 1, c)
end

function GLYPH.material(s, x, y, c, _, colour)
  s:disc(x + 8, y + 8, 6, opaque(colour or 0xc33b2c), true)
  s:disc(x + 6, y + 6, 2, 0xffffffff, true)
end

local function kind_glyph(t)
  return t.kind == "light" and "light" or t.kind == "camera" and "camera" or "mesh"
end

--------------------------------------------------------------------------
-- The header: the title and the file, Object and Edit, Add; then the three
-- ways to see it and Render. Rendered, Render and Add wait for their steps
-- and are drawn at a third, as a control that cannot be used is.
--------------------------------------------------------------------------

local controls = {}                         -- name -> { x, y, w, h }, as drawn

local function control(name, x, y, w, h)
  controls[name] = { x = x, y = y, w = w, h = h }
  return controls[name]
end

local DIM = theme.mix(theme.sunken, theme.text_dim, 350)

-- A segmented control: `parts` of { name, text, on, disabled }.
local function segmented(s, x, y, parts)
  local h, total = 29, 0
  local widths = {}

  for i, p in ipairs(parts) do
    widths[i] = gfx.measure(p.text) + 22 + (p.dot and 19 or 0)
    total = total + widths[i]
  end

  s:fill_round(x, y, total + 2, h + 2, theme.line_soft, 7)
  s:fill_round(x + 1, y + 1, total, h, theme.sunken, 6)

  local px = x + 1

  for i, p in ipairs(parts) do
    local w = widths[i]

    if p.on then
      s:fill(px, y + 1, w, h, theme.mix(theme.sunken, theme.accent, 110))
    end

    if i > 1 then s:fill(px, y + 1, 1, h, theme.line_soft) end

    local tx = px + 11

    if p.dot then
      p.dot(s, tx, y + 1 + (h - 13) // 2)
      tx = tx + 19
    end

    s:text(tx, y + 1 + (h - gfx.height()) // 2, p.text,
           p.disabled and DIM or (p.on and theme.accent or theme.text), nil, "ui")
    control(p.name, px, y, w, h + 2)
    px = px + w
  end

  return total + 2
end

local function draw_header(s)
  local right = W - L.head_edge
  local cy = (HEAD - 1 - 31) // 2

  -- From the right: the dots, Render, the shading.
  local more = control("more", right - 26, (HEAD - 1 - 26) // 2, 26, 26)
  local render_w = pk.button_width("Render F12") + 4
  local render = control("render", more.x - L.head_gap - render_w, cy, render_w, 31)

  local shade_parts = {
    { name = "wire", text = "Wireframe", on = shading == "wire",
      dot = function(s2, x, y)
        ring(s2, x + 6.5, y + 6.5, 5.5, theme.text_dim)
        ring(s2, x + 6.5, y + 6.5, 5.5, theme.text_dim, -math.pi / 2, math.pi / 2)
        line(s2, x + 1, y + 6.5, x + 12, y + 6.5, theme.text_dim)
      end },
    { name = "solid", text = "Solid", on = shading == "solid",
      dot = function(s2, x, y) s2:disc(x + 6, y + 6, 6, theme.text_dim, true) end },
    { name = "rendered", text = "Rendered", disabled = true,
      dot = function(s2, x, y) s2:disc(x + 6, y + 6, 6, 0xffe0a84a, true) end },
  }
  local shade_w = 0

  for _, p in ipairs(shade_parts) do
    shade_w = shade_w + gfx.measure(p.text) + 22 + 19
  end

  local shade_x = render.x - 8 - shade_w - 2
  local title_end = pk.header(s, 0, 0, W, "Cafesa3D", "still-life.scene", shade_x)

  local x = title_end + 18
  x = x + segmented(s, x, (HEAD - 1 - 31) // 2,
                    { { name = "object", text = "Object", on = true },
                      { name = "edit", text = "Edit", disabled = true } }) + 6

  -- Add, with its key.
  local add_w = pk.button_width("Add") + 22 + gfx.measure("Shift A", small) + 8
  local addb = control("add", x, cy, add_w, 31)

  pk.button(s, { x = addb.x, y = addb.y, w = addb.w, text = "" })
  line(s, addb.x + 13, addb.y + 15.5, addb.x + 22, addb.y + 15.5, theme.text)
  line(s, addb.x + 17.5, addb.y + 11, addb.x + 17.5, addb.y + 20, theme.text)
  s:text(addb.x + 29, addb.y + (31 - gfx.height()) // 2, "Add", theme.text, nil, "ui")
  s:text(addb.x + 29 + gfx.measure("Add") + 8, addb.y + (31 - gfx.height(small)) // 2,
         "Shift A", theme.text_dim, nil, small)

  segmented(s, shade_x, (HEAD - 1 - 31) // 2, shade_parts)

  -- Render, the verb, filled - and at a third until step four.
  s:fill_round(render.x, render.y, render.w, 31,
               theme.mix(theme.sunken, theme.accent, 380), 7)
  s:text(render.x + 12, render.y + (31 - gfx.height()) // 2, "Render",
         theme.text_on, nil, "ui")
  s:text(render.x + 12 + gfx.measure("Render") + 7,
         render.y + (31 - gfx.height(small)) // 2, "F12",
         theme.mix(theme.accent, theme.text_on, 600), nil, small)

  pk.iconbutton(s, { x = more.x, y = more.y, icon = "more" })
end

--------------------------------------------------------------------------
-- The tools down the left: Select and the 3D cursor; Move, Rotate and
-- Scale, which step two brings; Measure.
--------------------------------------------------------------------------

local TOOL_LIST = {
  { name = "select", glyph = "select" },
  { name = "cursor", glyph = "cursor" },
  { gap = true },
  { name = "move", glyph = "move" },
  { name = "rotate", glyph = "rotate" },
  { name = "scale", glyph = "scale" },
  { gap = true },
  { name = "measure", glyph = "measure", later = true },
}
local tool = "select"

local function draw_tools(s)
  local bg = theme.mix(theme.window, theme.line_soft, 300)

  s:fill(0, HEAD, TOOLS, H - HEAD - FOOT, bg)
  s:fill(TOOLS - 1, HEAD, 1, H - HEAD - FOOT, theme.line_soft)

  local y = HEAD + 10

  for _, t in ipairs(TOOL_LIST) do
    if t.gap then
      s:fill(11, y + 4, 24, 1, theme.line_soft)
      y = y + 9
    else
      local on = tool == t.name
      local c = t.later and DIM or (on and theme.text_on or theme.text)

      if on then s:fill_round(6, y, 34, 34, theme.accent, 8) end

      GLYPH[t.glyph](s, 6 + 8, y + 8, c)
      control("tool:" .. t.name, 6, y, 34, 34)
      y = y + 38
    end
  end
end

--------------------------------------------------------------------------
-- The 3D view and what is drawn over it: the lamp, the camera, the 3D
-- cursor, the ball of axes in the corner, and where the eye is.
--------------------------------------------------------------------------

local function draw_lamp(s, t)
  local x, y = on_screen(t.loc)

  if not x then return end

  -- Its post to the ground, dashed, in the world.
  local z = t.loc[3]
  local n = math.max(1, math.floor(z / 0.25))

  for i = 0, n - 1, 2 do
    local z0, z1 = z - i * z / n, z - (i + 1) * z / n

    view:line(s, VX, VY, t.loc[1], t.loc[2], z0, t.loc[1], t.loc[2], z1,
              0x000000, 150, false)
  end

  local on = selected == t

  s:disc(round(x), round(y), 6, on and SEL or 0xfff3d36b, true)
  ring(s, x, y, 6, on and 0xffffff or 0x1f2126)
  ring(s, x, y, 11, on and SEL or 0x1f2126)
end

local function draw_camera(s, t)
  -- Its frame, a metre ahead of it, sixteen by nine, as Blender draws one.
  local p, q = t.loc, t.target
  local f = { q[1] - p[1], q[2] - p[2], q[3] - p[3] }
  local n = math.sqrt(f[1] ^ 2 + f[2] ^ 2 + f[3] ^ 2)

  f = { f[1] / n, f[2] / n, f[3] / n }

  local r = { f[2], -f[1], 0 }
  local rn = math.sqrt(r[1] ^ 2 + r[2] ^ 2)

  r = { r[1] / rn, r[2] / rn, 0 }

  local u = { r[2] * f[3] - r[3] * f[2], r[3] * f[1] - r[1] * f[3],
              r[1] * f[2] - r[2] * f[1] }
  local half = math.tan(math.atan(18 / t.focal)) * 0.9
  local fw, fh = half, half * 9 / 16
  local c = { p[1] + f[1] * 0.9, p[2] + f[2] * 0.9, p[3] + f[3] * 0.9 }
  local corners = {}

  for i, sgn in ipairs({ { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } }) do
    corners[i] = { c[1] + r[1] * sgn[1] * fw + u[1] * sgn[2] * fh,
                   c[2] + r[2] * sgn[1] * fw + u[2] * sgn[2] * fh,
                   c[3] + r[3] * sgn[1] * fw + u[3] * sgn[2] * fh }
  end

  local colour = selected == t and 0xffa53d or 0x1f2126

  for i = 1, 4 do
    local a, b = corners[i], corners[i % 4 + 1]

    view:line(s, VX, VY, p[1], p[2], p[3], a[1], a[2], a[3], colour, 255, true)
    view:line(s, VX, VY, a[1], a[2], a[3], b[1], b[2], b[3], colour, 255, true)
  end

  -- The triangle that says which way is up.
  local top1 = { c[1] + u[1] * fh * 1.15 - r[1] * fw * 0.5, c[2] + u[2] * fh * 1.15 - r[2] * fw * 0.5,
                 c[3] + u[3] * fh * 1.15 }
  local top2 = { c[1] + u[1] * fh * 1.15 + r[1] * fw * 0.5, c[2] + u[2] * fh * 1.15 + r[2] * fw * 0.5,
                 c[3] + u[3] * fh * 1.15 }
  local tip = { c[1] + u[1] * fh * 1.7, c[2] + u[2] * fh * 1.7, c[3] + u[3] * fh * 1.7 }

  view:line(s, VX, VY, top1[1], top1[2], top1[3], tip[1], tip[2], tip[3], colour, 255, true)
  view:line(s, VX, VY, tip[1], tip[2], tip[3], top2[1], top2[2], top2[3], colour, 255, true)
  view:line(s, VX, VY, top2[1], top2[2], top2[3], top1[1], top1[2], top1[3], colour, 255, true)
end

local cursor3d = { 0, 0, 0 }

local function draw_cursor(s)
  local x, y = on_screen(cursor3d)

  if not x then return end

  for i = 0, 7 do
    ring(s, x, y, 9, i % 2 == 0 and 0xe0524b or 0xffffff,
         i * math.pi / 4, (i + 1) * math.pi / 4)
  end

  line(s, x - 15, y, x - 5, y, 0x000000, 0.7) line(s, x + 5, y, x + 15, y, 0x000000, 0.7)
  line(s, x, y - 15, x, y - 5, 0x000000, 0.7) line(s, x, y + 5, x, y + 15, 0x000000, 0.7)
end

-- The ball of axes: X, Y and Z as the eye sees them, and the other three
-- as rings - which Blender's is, and which a click on turns the view to.
local gizmo = {}

local function draw_gizmo(s)
  local cx, cy, R = VX + VW - 58, VY + 62, 38
  local r, u, f = axes()
  local list = {
    { v = { 1, 0, 0 }, c = 0xe0524b, t = "X" }, { v = { 0, 1, 0 }, c = 0x7cbb3a, t = "Y" },
    { v = { 0, 0, 1 }, c = 0x4a86e0, t = "Z" }, { v = { -1, 0, 0 }, c = 0xe0524b },
    { v = { 0, -1, 0 }, c = 0x7cbb3a }, { v = { 0, 0, -1 }, c = 0x4a86e0 },
  }

  s:disc(cx, cy, R + 12, 0xff3e4249, true)

  for _, a in ipairs(list) do
    a.x = cx + (a.v[1] * r[1] + a.v[2] * r[2] + a.v[3] * r[3]) * R
    a.y = cy - (a.v[1] * u[1] + a.v[2] * u[2] + a.v[3] * u[3]) * R
    a.z = a.v[1] * f[1] + a.v[2] * f[2] + a.v[3] * f[3]
  end

  table.sort(list, function(a, b) return a.z > b.z end)
  gizmo = list

  for _, a in ipairs(list) do
    if a.t then
      line(s, cx, cy, a.x, a.y, a.c)
      line(s, cx + 0.5, cy, a.x + 0.5, a.y, a.c)
      s:disc(round(a.x), round(a.y), 9, opaque(a.c), true)
      s:text(round(a.x) - gfx.measure(a.t, tiny) // 2,
             round(a.y) - gfx.height(tiny) // 2, a.t, 0xff16181c, nil, tiny)
    else
      ring(s, a.x, a.y, 7, a.c)
    end
  end
end

--------------------------------------------------------------------------
-- The tools' handles: Move's three arrows, Rotate's three rings, Scale's
-- three axes ending in boxes, and in the middle the handle that is held to
-- no axis. The same size on the screen however far the object is, as
-- Blender's are; a drag on one is G, R or S held to its axis.
--------------------------------------------------------------------------

local HANDLE_PX = 95
local handles = {}

local function make_handles()
  handles = {}

  local t = selected

  if not t or t.hidden or modal or not (tool == "move" or tool == "rotate" or tool == "scale") then
    return
  end

  if tool ~= "move" and not t.id then return end

  local r, u, f = axes()
  local e, c = eye(), t.loc
  local depth = (c[1] - e[1]) * f[1] + (c[2] - e[2]) * f[2] + (c[3] - e[3]) * f[3]

  if depth < 0.2 then return end

  local L = HANDLE_PX * depth / ((VW / 2) / math.tan(FOV / 2))
  local cx, cy = on_screen(c)

  if not cx then return end

  local function at(p) local x, y = on_screen(p) return x and { x, y } end

  for i = 1, 3 do
    local a = AXES[i]
    local h = { op = ({ move = "grab", rotate = "rotate", scale = "scale" })[tool], axis = i,
                colour = AXIS_COLOUR[i], pts = {} }

    if tool == "rotate" then
      -- A ring round the axis: two directions square to it and each other.
      local p1 = AXES[i % 3 + 1]
      local p2 = AXES[(i + 1) % 3 + 1]

      for k = 0, 48 do
        local ang = 2 * math.pi * k / 48
        local q = at({ c[1] + (p1[1] * math.cos(ang) + p2[1] * math.sin(ang)) * L * 0.8,
                       c[2] + (p1[2] * math.cos(ang) + p2[2] * math.sin(ang)) * L * 0.8,
                       c[3] + (p1[3] * math.cos(ang) + p2[3] * math.sin(ang)) * L * 0.8 })

        if q then h.pts[#h.pts + 1] = q end
      end

      h.grip = h.pts[7]
    else
      local from = tool == "move" and 0.2 or 0
      local to = tool == "move" and 1 or 0.85
      local p0 = at({ c[1] + a[1] * L * from, c[2] + a[2] * L * from, c[3] + a[3] * L * from })
      local p1 = at({ c[1] + a[1] * L * to, c[2] + a[2] * L * to, c[3] + a[3] * L * to })

      if p0 and p1 then
        h.pts = { p0, p1 }
        h.tip = p1
        h.grip = { (p0[1] + p1[1]) / 2, (p0[2] + p1[2]) / 2 }
      end
    end

    if #h.pts >= 2 then handles[#handles + 1] = h end
  end

  -- The middle: free, in the view's plane; or for Scale, all three at once.
  if tool ~= "rotate" then
    handles[#handles + 1] = { op = tool == "move" and "grab" or "scale", centre = { cx, cy },
                              colour = 0xffffff, pts = {}, grip = { cx, cy } }
  end
end

local function draw_handles(s)
  for _, h in ipairs(handles) do
    if h.centre then
      ring(s, h.centre[1], h.centre[2], 9, h.colour)
      ring(s, h.centre[1], h.centre[2], 10, h.colour)
    else
      for k = 1, #h.pts - 1 do
        local a, b = h.pts[k], h.pts[k + 1]

        line(s, a[1], a[2], b[1], b[2], h.colour)
        line(s, a[1] + 0.7, a[2] + 0.7, b[1] + 0.7, b[2] + 0.7, h.colour)
      end

      if h.tip and h.op == "grab" then
        local a, b = h.pts[1], h.tip
        local dx, dy = b[1] - a[1], b[2] - a[2]
        local n = math.sqrt(dx * dx + dy * dy)

        if n > 1 then
          dx, dy = dx / n, dy / n
          s:triangle(b[1] + dx * 10, b[2] + dy * 10, b[1] - dy * 5, b[2] + dx * 5,
                     b[1] + dy * 5, b[2] - dx * 5, opaque(h.colour))
        end
      elseif h.tip then
        s:fill(round(h.tip[1]) - 4, round(h.tip[2]) - 4, 9, 9, opaque(h.colour))
      end
    end
  end
end

-- The handle a press at `x, y` is on: within eight pixels of its line.
local function handle_at(x, y)
  local best, best_d = nil, 8

  for _, h in ipairs(handles) do
    if h.centre then
      local d = math.sqrt((x - h.centre[1]) ^ 2 + (y - h.centre[2]) ^ 2)

      if d < 12 and d < best_d + 4 then best, best_d = h, d end
    end

    for k = 1, #h.pts - 1 do
      local a, b = h.pts[k], h.pts[k + 1]
      local dx, dy = b[1] - a[1], b[2] - a[2]
      local n2 = dx * dx + dy * dy
      local t = n2 > 0 and math.max(0, math.min(1, ((x - a[1]) * dx + (y - a[2]) * dy) / n2)) or 0
      local d = math.sqrt((x - a[1] - dx * t) ^ 2 + (y - a[2] - dy * t) ^ 2)

      if d < best_d then best, best_d = h, d end
    end

    if h.tip then
      local d = math.sqrt((x - h.tip[1]) ^ 2 + (y - h.tip[2]) ^ 2)

      if d < best_d then best, best_d = h, d end
    end
  end

  return best
end

local function draw_view(s)
  look()
  drawn_triangles = view:draw(scene, s, VX, VY, {
    mode = shading == "wire" and "wire" or "solid",
    selected = selected and selected.id or 0,
    grid = true,
  })

  for _, t in ipairs(things) do
    if not t.hidden then
      if t.kind == "light" then draw_lamp(s, t) end
      if t.kind == "camera" then draw_camera(s, t) end
    end
  end

  draw_cursor(s)
  make_handles()
  draw_handles(s)
  draw_gizmo(s)

  if modal and modal.axis then
    local a, p = AXES[modal.axis], modal.loc0
    local planes = modal.plane and { modal.axis % 3 + 1, (modal.axis + 1) % 3 + 1 }
                   or { modal.axis }

    for _, i in ipairs(planes) do
      local d = AXES[i]

      view:line(s, VX, VY, p[1] - d[1] * 60, p[2] - d[2] * 60, p[3] - d[3] * 60,
                p[1] + d[1] * 60, p[2] + d[2] * 60, p[3] + d[3] * 60,
                AXIS_COLOUR[i], 220, false)
    end
  end

  local ty = VY + 10

  s:text(VX + 14, ty, modal and modal_line() or view_name, VP_INK, nil, small)
  s:text(VX + 14, ty + gfx.height(small) + 2,
         "Collection | " .. (selected and selected.name or "nothing selected"),
         VP_DIM, nil, small)
  s:text(VX + 14, VY + VH - 12 - gfx.height(small),
         "Drag to turn \u{b7} Shift-drag to move \u{b7} scroll to come closer",
         VP_DIM, nil, small)
end

--------------------------------------------------------------------------
-- The Outliner: the collection and everything in it, by name, each with
-- its eye.
--------------------------------------------------------------------------

local rows = {}                              -- as drawn: { y, thing }

local function panel_title(s, y, text)
  s:fill(SX, y, SIDE, PANEL_T, theme.mix(theme.sunken, theme.window, 400))
  s:fill(SX, y + PANEL_T - 1, SIDE, 1, theme.line_soft)
  s:text(SX + 12, y + (PANEL_T - gfx.height(tiny)) // 2, text:upper(),
         theme.text_dim, nil, tiny)
end

local function draw_outliner(s)
  s:fill(SX, HEAD, SIDE, H - HEAD - FOOT, theme.sunken)
  s:fill(SX, HEAD, 1, H - HEAD - FOOT, theme.line_soft)
  panel_title(s, HEAD, "Outliner")

  local sorted = {}

  for _, t in ipairs(things) do sorted[#sorted + 1] = t end
  table.sort(sorted, function(a, b) return a.name < b.name end)

  rows = {}

  local y = OUT_Y + 4
  local ty = (ROW - gfx.height()) // 2

  GLYPH.collection(s, SX + 30, y + (ROW - 14) // 2, theme.text_dim)
  s:text(SX + 52, y + ty, "Collection", theme.text_dim, nil, "ui")
  y = y + ROW

  for _, t in ipairs(sorted) do
    if y + ROW > PROPS_Y then break end

    local on = t == selected

    if on then s:fill(SX + 1, y, SIDE - 1, ROW, theme.mix(theme.sunken, theme.accent, 110)) end

    GLYPH[kind_glyph(t)](s, SX + 48, y + (ROW - 14) // 2,
                         t.hidden and DIM or theme.text_dim)
    s:text(SX + 70, y + ty, t.name,
           t.hidden and DIM or (on and theme.accent or theme.text), nil, "ui")
    GLYPH.eye(s, SX + SIDE - 30, y + (ROW - 14) // 2, t.hidden and DIM or theme.text_dim,
              t.hidden)
    rows[#rows + 1] = { y = y, thing = t }
    y = y + ROW
  end

  s:fill(SX, PROPS_Y, SIDE, 1, theme.line_soft)
end

--------------------------------------------------------------------------
-- Properties: five tabs down the side, and what the chosen one says of the
-- selection. Values only in this step; step two makes them editable.
--------------------------------------------------------------------------

local TABS = { "render", "world", "object", "data", "material" }

local function fmt(v, places)
  if math.abs(v) < 0.0005 then v = 0 end
  return ("%." .. (places or 3) .. "f"):format(v)
end

-- A label on the right of a 96-wide column, and a box of words after it.
--------------------------------------------------------------------------
-- A number Properties can change: click it to type one, drag across it to
-- scrub it, as Blender's fields are. What each is - its unit, its places,
-- how far a pixel of dragging moves it, and the least and most the kit
-- will take - is said once here, and a number is held to sense before the
-- kit is told, since the kit refuses nonsense by raising.
--------------------------------------------------------------------------

local FIELD = {
  loc      = { unit = " m", places = 2, step = 0.01 },
  rot      = { unit = "\u{b0}", places = 1, step = 0.5 },
  scale    = { places = 3, step = 0.01, lo = 0.001, hi = 1000 },
  size     = { unit = " m", places = 2, step = 0.01, lo = 0.001, hi = 1000 },
  radius   = { unit = " m", places = 3, step = 0.01, lo = 0.001, hi = 1000 },
  radius2  = { unit = " m", places = 3, step = 0.01, lo = 0, hi = 1000 },
  depth    = { unit = " m", places = 3, step = 0.01, lo = 0.001, hi = 1000 },
  segments = { int = true, step = 0.2, lo = 3, hi = 256 },
  rings    = { int = true, step = 0.2, lo = 2, hi = 128 },
  subdivisions = { int = true, step = 0.05, lo = 1, hi = 6 },
  power    = { unit = " W", places = 0, step = 5, lo = 0, hi = 100000 },
  focal    = { unit = " mm", places = 0, step = 0.5, lo = 1, hi = 500 },
}

local fields_drawn = {}                     -- { x, y, w, h, f }, as drawn
local editing = nil                         -- { f, text, fresh } while typing
local field_drag = nil

local function F(t, name, index, label)
  return { t = t, name = name, index = index, spec = FIELD[name],
           label = label .. (index and (" " .. AXIS_NAME[index]) or "") }
end

local function field_value(f)
  local v = f.t[f.name]

  if f.index then v = v[f.index] end

  return v
end

local function field_text(f, unit)
  local v, sp = field_value(f), f.spec
  local text = sp.int and tostring(math.floor(v + 0.5)) or fmt(v, sp.places)

  return unit and (text .. (sp.unit or "")) or text
end

-- The least and most, which for a few depend on the shape: a grid may be
-- one square across, and a torus's tube needs three sides.
local function field_limits(f)
  local sp, kind = f.spec, f.t.kind
  local lo, hi = sp.lo, sp.hi

  if kind == "grid" and (f.name == "segments" or f.name == "rings") then lo = 1 end
  if kind == "torus" and f.name == "rings" then lo = 3 end

  return lo, hi
end

local function field_set(f, v)
  local lo, hi = field_limits(f)

  if f.spec.int then v = math.floor(v + 0.5) end
  if lo then v = math.max(lo, v) end
  if hi then v = math.min(hi, v) end

  if f.index then f.t[f.name][f.index] = v else f.t[f.name] = v end

  sync(f.t)
end

local function same_field(a, b)
  return a and b and a.t == b.t and a.name == b.name and a.index == b.index
end

local function field_row(s, x, y, w, label, values)
  s:text(x + 96 - gfx.measure(label, small), y + (26 - gfx.height(small)) // 2, label,
         theme.text_dim, nil, small)

  local fx, n = x + 104, #values
  local gap = 3
  local each = (w - 104 - gap * (n - 1)) // n

  for i, v in ipairs(values) do
    local bx = fx + (i - 1) * (each + gap)
    local f = type(v) == "table" and v or nil
    local typing = f and editing and same_field(editing.f, f)
    local text = f and (typing and (editing.text .. "|") or field_text(f, true)) or v

    s:fill_round(bx, y, each, 26, typing and theme.sunken
                 or theme.mix(theme.sunken, theme.window, 500), 6)
    s:frame_round(bx, y, each, 26, typing and theme.ring or theme.line_soft, 6)

    if typing then
      s:frame_round(bx + 1, y + 1, each - 2, 24, theme.ring, 5)
    end

    s:text(bx + (each - gfx.measure(text, small)) // 2, y + (26 - gfx.height(small)) // 2, text,
           f and theme.text or theme.text_dim, nil, small)

    if f then fields_drawn[#fields_drawn + 1] = { x = bx, y = y, w = each, h = 26, f = f } end
  end

  return y + 31
end

local AXIS = { 0xffd4453b, 0xff4f9a2c, 0xff2f68c8 }

local function axis_letters(s, x, y, w)
  local fx, each = x + 104, (w - 104 - 6) // 3

  for i, a in ipairs({ "X", "Y", "Z" }) do
    local bx = fx + (i - 1) * (each + 3)

    s:text(bx + (each - gfx.measure(a, tiny)) // 2, y, a, AXIS[i], nil, tiny)
  end

  return y + gfx.height(tiny) + 1
end

local function heading(s, x, y, text, glyph, colour)
  if glyph then
    GLYPH[glyph](s, x, y + (gfx.height("title") - 16) // 2, theme.text_dim, nil, colour)
    x = x + 24
  end

  s:text(x, y, text, theme.text, nil, "title")
  return y + gfx.height("title") + 10
end

local function note(s, x, y, w, text)
  local words, lineb = {}, ""

  for word in text:gmatch("%S+") do words[#words + 1] = word end

  for _, word in ipairs(words) do
    local try = lineb == "" and word or (lineb .. " " .. word)

    if gfx.measure(try, small) > w then
      s:text(x, y, lineb, theme.text_dim, nil, small)
      y = y + gfx.height(small) + 3
      lineb = word
    else
      lineb = try
    end
  end

  if lineb ~= "" then
    s:text(x, y, lineb, theme.text_dim, nil, small)
    y = y + gfx.height(small) + 3
  end

  return y
end

local KIND_NAME = { box = "Cube", sphere = "UV Sphere", cylinder = "Cylinder",
                    plane = "Plane", light = "Point light", camera = "Camera",
                    ico = "Ico Sphere", cone = "Cone", torus = "Torus", grid = "Grid" }

local function hexcolour(c) return ("#%06x"):format(c & 0xffffff) end

local function draw_props(s)
  local x0 = SX + TABS_W

  fields_drawn = {}
  local top = PROPS_Y + 1

  s:fill(SX + 1, top, TABS_W - 1, H - FOOT - top, theme.mix(theme.window, theme.line_soft, 300))
  s:fill(SX + TABS_W - 1, top, 1, H - FOOT - top, theme.line_soft)

  for i, name in ipairs(TABS) do
    local ty = top + 8 + (i - 1) * 33
    local on = name == tab

    if on then
      s:fill_round(SX + 4, ty, 30, 30, theme.sunken, 7)
      s:frame_round(SX + 4, ty, 30, 30, theme.line_soft, 7)
    end

    GLYPH[name](s, SX + 11, ty + 7, on and theme.accent or theme.text_dim, nil,
                selected and selected.mat and selected.mat.base)
    control("tab:" .. name, SX + 4, ty, 30, 30)
  end

  local x, w, y = x0 + 14, SIDE - TABS_W - 28, top + 12
  local t = selected

  if tab == "render" then
    y = heading(s, x, y, "Render")
    y = field_row(s, x, y, w, "Integrator", { "Final" })
    y = field_row(s, x, y, w, "Samples", { "256" })
    y = field_row(s, x, y, w, "Bounces", { "6" })
    y = field_row(s, x, y, w, "Resolution", { "1920 \u{d7} 1080" })
    y = field_row(s, x, y, w, "Saved to", { "/home/renders" })
    note(s, x, y + 4, w, "The ray tracer is step three: Preview traces as Whitted did, Final as Cycles does.")
  elseif tab == "world" then
    y = heading(s, x, y, "World")
    y = field_row(s, x, y, w, "Zenith", { "#6d90c6" })
    y = field_row(s, x, y, w, "Horizon", { "#dfe6ef" })
    y = field_row(s, x, y, w, "Strength", { "0.90" })
    note(s, x, y + 4, w, "The light from everywhere that is not a lamp: a sky.")
  elseif not t then
    note(s, x, y, w, "Nothing is selected. Click an object in the view, or its name above.")
  elseif tab == "object" then
    y = heading(s, x, y, t.name, kind_glyph(t))
    y = axis_letters(s, x, y, w)
    y = field_row(s, x, y, w, "Location", { F(t, "loc", 1, "Location"), F(t, "loc", 2, "Location"),
                                            F(t, "loc", 3, "Location") })
    y = axis_letters(s, x, y, w)

    if t.id then
      y = field_row(s, x, y, w, "Rotation", { F(t, "rot", 1, "Rotation"), F(t, "rot", 2, "Rotation"),
                                              F(t, "rot", 3, "Rotation") })
      y = axis_letters(s, x, y, w)
      y = field_row(s, x, y, w, "Scale", { F(t, "scale", 1, "Scale"), F(t, "scale", 2, "Scale"),
                                           F(t, "scale", 3, "Scale") })
    else
      y = field_row(s, x, y, w, "Rotation", { "0.0\u{b0}", "0.0\u{b0}", "0.0\u{b0}" })
      y = axis_letters(s, x, y, w)
      y = field_row(s, x, y, w, "Scale", { "1.000", "1.000", "1.000" })
    end
    note(s, x, y + 6, w, "Where the object is, how it is turned and how large - and nothing about its shape, which is in the Data tab.")
  elseif tab == "data" then
    y = heading(s, x, y, KIND_NAME[t.kind], kind_glyph(t))

    if t.kind == "box" then
      y = axis_letters(s, x, y, w)
      y = field_row(s, x, y, w, "Size", { F(t, "size", 1, "Size"), F(t, "size", 2, "Size"),
                                          F(t, "size", 3, "Size") })
    elseif t.kind == "sphere" then
      y = field_row(s, x, y, w, "Segments", { F(t, "segments", nil, "Segments") })
      y = field_row(s, x, y, w, "Rings", { F(t, "rings", nil, "Rings") })
      y = field_row(s, x, y, w, "Radius", { F(t, "radius", nil, "Radius") })
    elseif t.kind == "cylinder" then
      y = field_row(s, x, y, w, "Vertices", { F(t, "segments", nil, "Vertices") })
      y = field_row(s, x, y, w, "Radius", { F(t, "radius", nil, "Radius") })
      y = field_row(s, x, y, w, "Depth", { F(t, "depth", nil, "Depth") })
    elseif t.kind == "plane" then
      y = field_row(s, x, y, w, "Size", { F(t, "size", nil, "Size") })
    elseif t.kind == "ico" then
      y = field_row(s, x, y, w, "Subdivisions", { F(t, "subdivisions", nil, "Subdivisions") })
      y = field_row(s, x, y, w, "Radius", { F(t, "radius", nil, "Radius") })
    elseif t.kind == "cone" then
      y = field_row(s, x, y, w, "Vertices", { F(t, "segments", nil, "Vertices") })
      y = field_row(s, x, y, w, "Radius 1", { F(t, "radius", nil, "Radius 1") })
      y = field_row(s, x, y, w, "Radius 2", { F(t, "radius2", nil, "Radius 2") })
      y = field_row(s, x, y, w, "Depth", { F(t, "depth", nil, "Depth") })
    elseif t.kind == "torus" then
      y = field_row(s, x, y, w, "Major segments", { F(t, "segments", nil, "Major segments") })
      y = field_row(s, x, y, w, "Minor segments", { F(t, "rings", nil, "Minor segments") })
      y = field_row(s, x, y, w, "Major radius", { F(t, "radius", nil, "Major radius") })
      y = field_row(s, x, y, w, "Minor radius", { F(t, "radius2", nil, "Minor radius") })
    elseif t.kind == "grid" then
      y = field_row(s, x, y, w, "X subdivisions", { F(t, "segments", nil, "X subdivisions") })
      y = field_row(s, x, y, w, "Y subdivisions", { F(t, "rings", nil, "Y subdivisions") })
      y = field_row(s, x, y, w, "Size", { F(t, "size", nil, "Size") })
    elseif t.kind == "light" then
      y = field_row(s, x, y, w, "Type", { "Point" })
      y = field_row(s, x, y, w, "Colour", { hexcolour(t.colour) })
      y = field_row(s, x, y, w, "Power", { F(t, "power", nil, "Power") })
      y = field_row(s, x, y, w, "Radius", { F(t, "radius", nil, "Radius") })
    elseif t.kind == "camera" then
      y = field_row(s, x, y, w, "Focal length", { F(t, "focal", nil, "Focal length") })
      y = field_row(s, x, y, w, "Sensor", { "36 mm" })
    end

    if t.id then
      y = field_row(s, x, y, w, "Shading", { t.smooth and "Smooth" or "Flat" })
      note(s, x, y + 6, w, ("What the shape was made with stays editable here until it is edited as a mesh. The view draws its %d triangles."):format(scene:triangles(t.id)))
    end
  elseif tab == "material" then
    if not t.mat then
      heading(s, x, y, t.name, kind_glyph(t))
      note(s, x, y + gfx.height("title") + 10, w,
           t.kind == "light" and "A light has no material: its colour and power are in the Data tab."
           or "A camera has no material.")
    else
      local m = t.mat

      y = heading(s, x, y, t.name, "material", m.base)

      -- The five starting points, as chips; the one in force marked.
      local cx = x

      for _, p in ipairs(PRESET_ORDER) do
        local cw = gfx.measure(p, small) + 30

        if cx + cw > x + w then cx, y = x, y + 32 end

        local on = m.preset == p

        s:fill_round(cx, y, cw, 26, on and theme.mix(theme.sunken, theme.accent, 110) or theme.sunken, 13)
        s:frame_round(cx, y, cw, 26, on and theme.accent or theme.line_soft, 13)
        s:disc(cx + 12, y + 13, 5, ({ Plastic = 0xffc33b2c, Metal = 0xffe0a84a, Mirror = 0xffc9ced6,
                                     Glass = 0xffbfe0f0, Light = 0xffffe28a })[p], true)
        s:text(cx + 22, y + (26 - gfx.height(small)) // 2, p, on and theme.accent or theme.text, nil, small)
        cx = cx + cw + 5
      end

      y = y + 36
      y = field_row(s, x, y, w, "Base colour", { hexcolour(m.base) })
      y = field_row(s, x, y, w, "Metallic", { fmt(m.metallic, 2) })
      y = field_row(s, x, y, w, "Roughness", { fmt(m.rough, 2) })
      y = field_row(s, x, y, w, "IOR", { fmt(m.ior, 2) })
      y = field_row(s, x, y, w, "Transmission", { fmt(m.trans, 2) })
      field_row(s, x, y, w, "Emission", { fmt(m.emit, 1) })
    end
  end
end

--------------------------------------------------------------------------
-- The foot: which keys do what, and what the scene is.
--------------------------------------------------------------------------

local function draw_foot(s)
  local y = H - FOOT

  s:fill(0, y, W, FOOT, theme.window)
  s:fill(0, y, W, 1, theme.line_soft)

  local x = 14
  local ty = y + (FOOT - gfx.height(small)) // 2
  local keys = modal and { { "Click", "place" }, { "Esc", "cancel" }, { "X Y Z", "hold to an axis" },
                           { "Shift X", "all but" }, { "0-9", "exact" } }

  for _, k in ipairs(keys or { { "Drag", "turn" }, { "Shift Drag", "move" }, { "Wheel", "closer" },
                       { "Shift A", "add" }, { "X", "delete" }, { "Ctrl Z", "undo" },
                       { "Z", "shading" }, { "Home", "all" } }) do
    local kw = gfx.measure(k[1], tiny) + 10

    s:fill_round(x, y + 6, kw, FOOT - 12, theme.sunken, 4)
    s:frame_round(x, y + 6, kw, FOOT - 12, theme.line_soft, 4)
    s:text(x + 5, y + (FOOT - gfx.height(tiny)) // 2, k[1], theme.text, nil, tiny)
    x = x + kw + 5
    s:text(x, ty, k[2], theme.text_dim, nil, small)
    x = x + gfx.measure(k[2], small) + 14
  end

  local right = ("%d objects   %s triangles   Selected %s"):format(
    #things, tostring(scene:triangles()), selected and selected.name or "none")

  s:text(W - 14 - gfx.measure(right, small), ty, right, theme.text_dim, nil, small)
end

local function draw_all()
  local s = win:surface()

  draw_header(s)
  draw_tools(s)
  draw_view(s)
  draw_outliner(s)
  draw_props(s)
  draw_foot(s)
  return win:commit{ x = 0, y = 0, w = W, h = H }
end

--------------------------------------------------------------------------
-- Choosing, and turning the eye.
--------------------------------------------------------------------------

local function say_selected()
  print("cafesa3d: selected " .. (selected and selected.name or "nothing"))
end

local function select(t)
  if t ~= selected then
    selected = t
    say_selected()
  end
end

-- Where each object's middle is in the window, for whoever is watching.
local function say_where()
  look()

  local out = {}

  for _, t in ipairs(things) do
    local x, y = on_screen(t.loc)

    if x then out[#out + 1] = ("%s %d,%d"):format(t.name, round(x), round(y)) end
  end

  print("cafesa3d: at " .. table.concat(out, "; "))

  make_handles()

  if #handles > 0 then
    local hs = {}

    for _, h in ipairs(handles) do
      hs[#hs + 1] = ("%s %d,%d"):format(h.axis and AXIS_NAME[h.axis] or "free",
                                        round(h.grip[1]), round(h.grip[2]))
    end

    print(("cafesa3d: handles %s %s"):format(tool, table.concat(hs, "; ")))
  end

  if #fields_drawn > 0 then
    local fs_ = {}

    for _, d in ipairs(fields_drawn) do
      fs_[#fs_ + 1] = ("%s%s %d,%d"):format(d.f.name, d.f.index or "", d.x + d.w // 2,
                                            d.y + d.h // 2)
    end

    print("cafesa3d: fields " .. table.concat(fs_, "; "))
  end
end

-- A click in the view: the lamp or the camera if it is on one, then the
-- object the kit drew on that pixel - or nothing, which is Blender's too.
local function pick(x, y)
  for _, t in ipairs(things) do
    if not t.hidden and (t.kind == "light" or t.kind == "camera") then
      local px, py = on_screen(t.loc)

      if px and (px - x) ^ 2 + (py - y) ^ 2 < 13 ^ 2 then return t end
    end
  end

  local id = view:pick(x - VX, y - VY)

  return id and by_id(id) or nil
end

-- The ball of axes: a click on an axis looks down it.
local function gizmo_hit(x, y)
  for i = #gizmo, 1, -1 do
    local a = gizmo[i]

    if (a.x - x) ^ 2 + (a.y - y) ^ 2 < 10 ^ 2 then return a end
  end
end

local VIEWS = {
  front = { az = -math.pi / 2, el = 0, name = "Front" },
  back = { az = math.pi / 2, el = 0, name = "Back" },
  right = { az = 0, el = 0, name = "Right" },
  left = { az = math.pi, el = 0, name = "Left" },
  top = { el = 1.5, name = "Top" },
  bottom = { el = -1.5, name = "Bottom" },
}

local function set_view(which)
  local v = VIEWS[which]

  if v.az then orbit.az = v.az end
  orbit.el = v.el
  view_name = v.name .. " Perspective"
  print("cafesa3d: view " .. v.name:lower())
end

local function axis_view(a)
  if a.v[3] ~= 0 then
    set_view(a.v[3] > 0 and "top" or "bottom")
  elseif a.v[1] ~= 0 then
    set_view(a.v[1] > 0 and "right" or "left")
  else
    set_view(a.v[2] > 0 and "back" or "front")
  end
end

-- Everything shown in the view, from far enough to see it all.
local function frame_all()
  local lo, hi = { math.huge, math.huge, math.huge }, { -math.huge, -math.huge, -math.huge }

  for _, t in ipairs(things) do
    if not t.hidden and t.kind ~= "plane" and t.kind ~= "camera" and t.kind ~= "light" then
      local r = t.radius and (t.radius + (t.kind == "torus" and t.radius2 or 0))
                or (type(t.size) == "table" and math.max(t.size[1], t.size[2], t.size[3]) / 2)
                or (type(t.size) == "number" and t.size / 2) or 1

      for k = 1, 3 do
        lo[k] = math.min(lo[k], t.loc[k] - r)
        hi[k] = math.max(hi[k], t.loc[k] + r)
      end
    end
  end

  if lo[1] == math.huge then return end

  orbit.target = { (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2, (lo[3] + hi[3]) / 2 }

  local span = math.sqrt((hi[1] - lo[1]) ^ 2 + (hi[2] - lo[2]) ^ 2 + (hi[3] - lo[3]) ^ 2)

  orbit.dist = math.max(3, span / 2 / math.tan(FOV / 2) * 1.15)
  print("cafesa3d: framed everything")
end

local function set_shading(which)
  if which ~= shading then
    shading = which
    print("cafesa3d: shading " .. which)
  end
end

--------------------------------------------------------------------------
-- Undo: the whole scene as it was, before each change - a handful of
-- tables of numbers, so a snapshot is cheaper than knowing how to reverse
-- every kind of change. Ctrl Z and Ctrl Shift Z, as Blender's.
--------------------------------------------------------------------------

local undo, redo = {}, {}

local function snapshot(label)
  local list = {}

  for _, t in ipairs(things) do
    local c = copy(t)

    c.id = nil
    list[#list + 1] = c
  end

  return { label = label, things = list, cursor = copy(cursor3d),
           selected = selected and selected.name }
end

local function rebuild(snap)
  for _, t in ipairs(things) do
    if t.id then scene:remove(t.id) end
  end

  things = {}

  for _, t in ipairs(snap.things) do add(copy(t)) end

  cursor3d = copy(snap.cursor)
  selected = nil

  for _, t in ipairs(things) do
    if t.name == snap.selected then selected = t end
  end
end

-- Called before a change, with what it is called.
local function will(label)
  undo[#undo + 1] = snapshot(label)

  if #undo > 64 then table.remove(undo, 1) end

  redo = {}
end

local function undo_last()
  local s = table.remove(undo)

  if not s then return false end

  redo[#redo + 1] = snapshot(s.label)
  rebuild(s)
  print("cafesa3d: undid " .. s.label)
  return true
end

local function redo_last()
  local s = table.remove(redo)

  if not s then return false end

  undo[#undo + 1] = snapshot(s.label)
  rebuild(s)
  print("cafesa3d: redid " .. s.label)
  return true
end

--------------------------------------------------------------------------
-- Adding, at the 3D cursor, with Blender's defaults and names; deleting;
-- and Shift D.
--------------------------------------------------------------------------

local ADDABLE = {
  { kind = "plane", text = "Plane", name = "Plane", fields = { size = 2 } },
  { kind = "box", text = "Cube", name = "Cube", fields = { size = { 2, 2, 2 } } },
  { text = "Circle" },
  { kind = "sphere", text = "UV Sphere", name = "Sphere",
    fields = { radius = 1, segments = 32, rings = 16 } },
  { kind = "ico", text = "Ico Sphere", name = "Icosphere",
    fields = { radius = 1, subdivisions = 2 } },
  { kind = "cylinder", text = "Cylinder", name = "Cylinder",
    fields = { radius = 1, depth = 2, segments = 32 } },
  { kind = "cone", text = "Cone", name = "Cone",
    fields = { radius = 1, radius2 = 0, depth = 2, segments = 32 } },
  { kind = "torus", text = "Torus", name = "Torus",
    fields = { radius = 1, radius2 = 0.25, segments = 48, rings = 12 } },
  { kind = "grid", text = "Grid", name = "Grid",
    fields = { size = 2, segments = 10, rings = 10 } },
  { text = "Monkey" },
}

local function add_primitive(a)
  local name = unique(a.name)

  will("added " .. name)

  local t = { name = name, kind = a.kind, loc = copy(cursor3d),
              rot = { 0, 0, 0 }, scale = { 1, 1, 1 },
              mat = material(0xcccccc, "Plastic") }

  for k, v in pairs(a.fields) do t[k] = copy(v) end

  add(t)
  selected = t
  tab = "data"
  print(("cafesa3d: added %s, a %s, at %.2f %.2f %.2f"):format(name, a.kind,
        t.loc[1], t.loc[2], t.loc[3]))
end

local function add_light()
  local name = unique("Point")

  will("added " .. name)
  selected = add{ name = name, kind = "light", loc = copy(cursor3d), radius = 0.1,
                  power = 1000, colour = 0xffffff }
  tab = "data"
  print(("cafesa3d: added %s, a light"):format(name))
end

local function delete_selected()
  local t = selected

  if not t then return false end

  will("deleted " .. t.name)
  forget(t)
  selected = nil
  print("cafesa3d: deleted " .. t.name)
  return true
end

local function duplicate_selected()
  local t = selected

  if not t then return false end

  local c = copy(t)

  c.id = nil
  c.name = unique(t.name)
  will("duplicated " .. t.name)
  add(c)
  selected = c
  print(("cafesa3d: duplicated %s as %s"):format(t.name, c.name))
  return true
end

-- The Add menu, Blender's: what the kit can make, and the rest greyed until
-- their step. `x, y` are the window's; the menu opens there on the screen.
local function add_menu(x, y)
  local mesh = {}

  for _, a in ipairs(ADDABLE) do
    mesh[#mesh + 1] = { text = a.text, disabled = not a.kind or nil,
                        on_choose = a.kind and function() add_primitive(a) end or nil }
  end

  local m = win:open_menu((win.origin_x or 0) + x, (win.origin_y or 0) + y, {
    { text = "Mesh", submenu = mesh },
    { text = "Light", submenu = {
        { text = "Point", on_choose = add_light },
        { text = "Sun", disabled = true },
        { text = "Spot", disabled = true },
        { text = "Area", disabled = true } } },
    { text = "Camera", disabled = true },
    { text = "Empty", disabled = true },
    { separator = true },
    { text = "Import OBJ...", disabled = true },
  })

  if m then
    print(("cafesa3d: add menu at %d,%d, %d wide, rows of %d"):format(m.x, m.y, m.w, m.row))
  end
end

-- X asks first, as Blender does; Delete does not.
local function delete_menu(x, y)
  if not selected then return end

  local m = win:open_menu((win.origin_x or 0) + x, (win.origin_y or 0) + y, {
    { text = "Delete " .. selected.name, on_choose = delete_selected },
  })

  if m then
    print(("cafesa3d: delete menu at %d,%d, rows of %d"):format(m.x, m.y, m.row))
  end
end

--------------------------------------------------------------------------
-- G, R and S, as Blender's: the selection follows the pointer with no
-- button held - the window manager tells a window where the pointer goes
-- while it asks (`wmproto.track`) - until a click or Return puts it down,
-- and Esc or a right click puts it back. X, Y or Z holds it to that axis,
-- Shift with one holds it to the other two, and a number typed is the
-- amount: metres, degrees, or a factor. One undo step each.
--------------------------------------------------------------------------

local OPS = { grab = "Move", rotate = "Rotate", scale = "Scale" }

local function vadd(a, b) return { a[1] + b[1], a[2] + b[2], a[3] + b[3] } end
local function vsub(a, b) return { a[1] - b[1], a[2] - b[2], a[3] - b[3] } end
local function vmul(a, k) return { a[1] * k, a[2] * k, a[3] * k } end
local function vdot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end

-- The ray from the eye through a pixel of the window.
local function ray(x, y)
  local r, u, f = axes()
  local F = (VW / 2) / math.tan(FOV / 2)
  local d = {}

  for i = 1, 3 do
    d[i] = f[i] * F + r[i] * (x - VX - VW / 2) - u[i] * (y - VY - VH / 2)
  end

  return eye(), vmul(d, 1 / math.sqrt(vdot(d, d)))
end

-- Where that ray meets the plane through `p0` square to `n`.
local function hit_plane(x, y, p0, n)
  local o, d = ray(x, y)
  local den = vdot(d, n)

  if math.abs(den) < 1e-6 then return nil end

  return vadd(o, vmul(d, vdot(vsub(p0, o), n) / den))
end

-- How far along the unit axis `a` through `p0` the ray passes nearest it.
local function along(x, y, p0, a)
  local o, d = ray(x, y)
  local w = vsub(p0, o)
  local b, c = vdot(a, d), vdot(d, d)
  local den = c - b * b

  if math.abs(den) < 1e-6 then return nil end

  return (b * vdot(d, w) - c * vdot(a, w)) / den
end

-- Turning, as the kit does it: X, then Y, then Z - Rz Ry Rx, row by row.
local function euler_matrix(rot)
  local ax, ay, az = math.rad(rot[1]), math.rad(rot[2]), math.rad(rot[3])
  local cx, sx, cy, sy = math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay)
  local cz, sz = math.cos(az), math.sin(az)

  return { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
           sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
           -sy,     cy * sx,                cy * cx }
end

local function matrix_euler(R)
  local y = math.asin(math.max(-1, math.min(1, -R[7])))
  local x, z

  if math.abs(math.cos(y)) > 1e-6 then
    x, z = math.atan(R[8], R[9]), math.atan(R[4], R[1])
  else
    x, z = 0, math.atan(-R[2], R[5])
  end

  return { math.deg(x), math.deg(y), math.deg(z) }
end

-- A turn of `ang` about the unit axis `a`, by the right hand.
local function axis_angle(a, ang)
  local c, s_, t = math.cos(ang), math.sin(ang), 1 - math.cos(ang)
  local x, y, z = a[1], a[2], a[3]

  return { t * x * x + c,      t * x * y - s_ * z, t * x * z + s_ * y,
           t * x * y + s_ * z, t * y * y + c,      t * y * z - s_ * x,
           t * x * z - s_ * y, t * y * z + s_ * x, t * z * z + c }
end

local function mat_mul(A, B)
  local C = {}

  for i = 0, 2 do
    for j = 0, 2 do
      C[i * 3 + j + 1] = A[i * 3 + 1] * B[j + 1] + A[i * 3 + 2] * B[3 + j + 1]
                         + A[i * 3 + 3] * B[6 + j + 1]
    end
  end

  return C
end

local function begin(op)
  local t = selected

  if not t or modal or (op ~= "grab" and not t.id) then return false end

  modal = { op = op, t = t, loc0 = copy(t.loc), rot0 = copy(t.rot or { 0, 0, 0 }),
            scale0 = copy(t.scale or { 1, 1, 1 }), target0 = copy(t.target),
            snap = snapshot(({ grab = "moved ", rotate = "rotated ", scale = "scaled " })[op]
                            .. t.name),
            axis = nil, plane = false, typed = "" }
  wmproto.track(win.handle, true)
  print(("cafesa3d: %s %s"):format(OPS[op]:lower(), t.name))
  return true
end

-- The selection as the pointer at `x, y` (or the number typed) says.
local function apply_modal(x, y)
  local m = modal
  local t = m.t
  local typed = tonumber(m.typed)
  local a = m.axis and AXES[m.axis]

  t.loc, t.rot, t.scale = copy(m.loc0), copy(m.rot0), copy(m.scale0)
  t.target = copy(m.target0)

  if not typed and not (m.ref and x) then sync(t) return end

  if m.op == "grab" then
    local delta

    if typed then
      delta = vmul((a and not m.plane) and a or AXES[1], typed)
    elseif a and not m.plane then
      local t0, t1 = along(m.ref[1], m.ref[2], m.loc0, a), along(x, y, m.loc0, a)

      if t0 and t1 then delta = vmul(a, t1 - t0) end
    else
      -- The view's own plane when nothing holds it: square to the eye.
      -- (Not `select(3, axes())`: this file's `select` is choosing an
      -- object, and it chose the number 3 and took the app down with it.)
      local _, _, f = axes()
      local n = a or f
      local p0, p1 = hit_plane(m.ref[1], m.ref[2], m.loc0, n), hit_plane(x, y, m.loc0, n)

      if p0 and p1 then delta = vsub(p1, p0) end
    end

    if delta then
      t.loc = vadd(m.loc0, delta)
      if m.target0 then t.target = vadd(m.target0, delta) end
    end
  elseif m.op == "rotate" then
    local _, _, f = axes()
    local k, ang

    if typed then
      k, ang = a or vmul(f, -1), math.rad(typed)
    else
      local cx, cy = on_screen(m.loc0)

      if cx then
        ang = math.atan(y - cy, x - cx) - math.atan(m.ref[2] - cy, m.ref[1] - cx)
        k = f

        -- Seen along an axis that points at the eye, the turn runs the
        -- other way round.
        if a then
          k = a
          if vdot(a, f) < 0 then ang = -ang end
        end
      end
    end

    if k then t.rot = matrix_euler(mat_mul(axis_angle(k, ang), euler_matrix(m.rot0))) end
  else
    local k = typed

    if not k then
      local cx, cy = on_screen(m.loc0)

      if cx then
        local d0 = math.max(1, math.sqrt((m.ref[1] - cx) ^ 2 + (m.ref[2] - cy) ^ 2))

        k = math.sqrt((x - cx) ^ 2 + (y - cy) ^ 2) / d0
      end
    end

    if k then
      for i = 1, 3 do
        if not a or (m.plane and i ~= m.axis) or (not m.plane and i == m.axis) then
          t.scale[i] = m.scale0[i] * k
        end
      end
    end
  end

  sync(t)
end

local function finish(keep)
  local m = modal
  local t = m.t

  modal = nil
  wmproto.track(win.handle, false)

  if not keep then
    t.loc, t.rot, t.scale, t.target = m.loc0, m.rot0, m.scale0, m.target0
    sync(t)
    print("cafesa3d: cancelled " .. OPS[m.op]:lower())
    return
  end

  undo[#undo + 1] = m.snap
  if #undo > 64 then table.remove(undo, 1) end
  redo = {}

  if m.op == "grab" then
    print(("cafesa3d: moved %s to %s %s %s"):format(t.name, fmt(t.loc[1], 2), fmt(t.loc[2], 2),
                                                   fmt(t.loc[3], 2)))
  elseif m.op == "rotate" then
    print(("cafesa3d: rotated %s to %s %s %s"):format(t.name, fmt(t.rot[1], 1),
                                                     fmt(t.rot[2], 1), fmt(t.rot[3], 1)))
  else
    print(("cafesa3d: scaled %s to %s %s %s"):format(t.name, fmt(t.scale[1]),
                                                    fmt(t.scale[2]), fmt(t.scale[3])))
  end
end

-- What the operation says of itself, where the view's name was.
function modal_line()
  local m = modal
  local t = m.t
  local what

  if m.op == "grab" then
    local d = vsub(t.loc, m.loc0)

    what = ("D: %s  %s  %s m"):format(fmt(d[1], 3), fmt(d[2], 3), fmt(d[3], 3))
  elseif m.op == "rotate" then
    what = ("%s\u{b0} %s\u{b0} %s\u{b0}"):format(fmt(t.rot[1], 1), fmt(t.rot[2], 1),
                                                  fmt(t.rot[3], 1))
  else
    what = ("%s  %s  %s"):format(fmt(t.scale[1]), fmt(t.scale[2]), fmt(t.scale[3]))
  end

  local held = ""

  if m.axis then
    held = m.plane and ("  held off " .. AXIS_NAME[m.axis])
           or ("  along " .. AXIS_NAME[m.axis])
  end

  return OPS[m.op] .. held .. "   " .. what .. (m.typed ~= "" and ("   typed " .. m.typed) or "")
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

local shift, ctrl = false, false
local drag = nil

-- The field under a point, as last drawn.
local function field_at(x, y)
  for _, d in ipairs(fields_drawn) do
    if x >= d.x and x < d.x + d.w and y >= d.y and y < d.y + d.h then return d end
  end
end

local function say_set(f)
  local extra = f.t.id and (" - the scene is %d triangles"):format(scene:triangles()) or ""

  print(("cafesa3d: set %s of %s to %s%s"):format(f.label, f.t.name, field_text(f, false), extra))
end

local function start_edit(f)
  editing = { f = f, text = field_text(f, false), fresh = true }
  print(("cafesa3d: editing %s of %s"):format(f.label, f.t.name))
end

-- What was typed, kept - if it is a number, and a different one.
local function commit_edit()
  local e = editing

  editing = nil

  if not e then return false end

  local v = tonumber(e.text)

  if v and v ~= field_value(e.f) then
    will(("set %s of %s"):format(e.f.label, e.f.t.name))
    field_set(e.f, v)
    say_set(e.f)
  end

  return true
end
local pointer = { VX + VW // 2, VY + VH // 2 }  -- where it was last seen

local function inside(c, x, y)
  return c and x >= c.x and x < c.x + c.w and y >= c.y and y < c.y + c.h
end

local function in_view(x, y)
  return x >= VX and x < VX + VW and y >= VY and y < VY + VH
end

local function press(x, y)
  pointer = { x, y }

  if modal then finish(true) return true end

  -- A field of Properties: a drag scrubs it, a click types into it; a
  -- press anywhere else keeps what was being typed, as Blender's does.
  local fd = field_at(x, y)

  if editing and not (fd and same_field(fd.f, editing.f)) then commit_edit() end

  if fd then
    if not (editing and same_field(fd.f, editing.f)) then
      field_drag = { f = fd.f, x = x, v0 = field_value(fd.f), moved = false }
    end

    return true
  end

  for _, name in ipairs(TABS) do
    if inside(controls["tab:" .. name], x, y) then
      tab = name
      print("cafesa3d: tab " .. name)
      return true
    end
  end

  if inside(controls.add, x, y) then add_menu(controls.add.x, HEAD) return true end
  if inside(controls.wire, x, y) then set_shading("wire") return true end
  if inside(controls.solid, x, y) then set_shading("solid") return true end

  for _, t in ipairs(TOOL_LIST) do
    if t.name and not t.later and inside(controls["tool:" .. t.name], x, y) then
      if tool ~= t.name then
        tool = t.name
        print("cafesa3d: tool " .. tool)
      end

      return true
    end
  end

  -- A row of the Outliner: its eye shows or hides it; anywhere else on it
  -- selects it.
  if x >= SX and y >= OUT_Y and y < PROPS_Y then
    for _, r in ipairs(rows) do
      if y >= r.y and y < r.y + ROW then
        if x >= SX + SIDE - 36 then
          will((r.thing.hidden and "showed " or "hid ") .. r.thing.name)
          r.thing.hidden = not r.thing.hidden
          sync(r.thing)
          print(("cafesa3d: %s %s"):format(r.thing.hidden and "hid" or "showed", r.thing.name))
        else
          select(r.thing)
        end

        return true
      end
    end
  end

  if in_view(x, y) then
    local a = gizmo_hit(x, y)

    if a then axis_view(a) return true end

    local h = handle_at(x, y)

    if h and begin(h.op) then
      modal.axis = h.axis
      modal.ref = { x, y }
      modal.by_drag = true
      return true
    end

    drag = { x = x, y = y, az = orbit.az, el = orbit.el,
             target = { orbit.target[1], orbit.target[2], orbit.target[3] },
             pan = shift, moved = false }
  end

  return false
end

local function move(x, y)
  pointer = { x, y }

  if field_drag then
    local fdr = field_drag
    local dx = x - fdr.x

    if not fdr.moved and math.abs(dx) < 4 then return false end

    if not fdr.moved then
      fdr.moved = true
      will(("set %s of %s"):format(fdr.f.label, fdr.f.t.name))
    end

    field_set(fdr.f, fdr.v0 + dx * fdr.f.spec.step)
    return true
  end

  if modal then
    if not modal.ref then
      modal.ref = { x, y }
    else
      modal.last = { x, y }
      apply_modal(x, y)
    end

    return true
  end

  if not drag then return false end

  local dx, dy = x - drag.x, y - drag.y

  if not drag.moved and dx * dx + dy * dy < 16 then return false end

  drag.moved = true
  view_name = VIEW_NAME

  if drag.pan then
    local r, u = axes()
    local k = orbit.dist * 2 * math.tan(FOV / 2) / VW

    for i = 1, 3 do
      orbit.target[i] = drag.target[i] - r[i] * dx * k + u[i] * dy * k
    end
  else
    orbit.az = drag.az - dx * 0.008
    orbit.el = math.max(-1.5, math.min(1.5, drag.el + dy * 0.008))
  end

  return true
end

local function release(x, y)
  if field_drag then
    local fdr = field_drag

    field_drag = nil

    if fdr.moved then say_set(fdr.f) else start_edit(fdr.f) end

    return true
  end

  if modal and modal.by_drag then
    finish(true)
    return true
  end

  if modal then return false end

  local d = drag

  drag = nil

  if not d then return false end

  if not d.moved then
    if tool == "cursor" then
      -- Where the 3D cursor goes: the ground under the pointer.
      local r, u, f = axes()
      local e = eye()
      local F = (VW / 2) / math.tan(FOV / 2)
      local dir = {}

      for i = 1, 3 do
        dir[i] = f[i] * F + r[i] * (x - VX - VW / 2) - u[i] * (y - VY - VH / 2)
      end

      if dir[3] < 0 then
        local s = -e[3] / dir[3]

        cursor3d = { e[1] + dir[1] * s, e[2] + dir[2] * s, 0 }
        print(("cafesa3d: cursor at %.2f %.2f"):format(cursor3d[1], cursor3d[2]))
      end
    else
      select(pick(x, y))
    end

    return true
  end

  print(("cafesa3d: view turned to %.2f %.2f at %.1f"):format(orbit.az, orbit.el, orbit.dist))
  return true
end

-- Raw keys: 42 and 54 are the shifts, 29 and 97 the controls, 2..11 the
-- number row, 30 A, 32 D, 44 Z, 45 X, 102 Home, 111 Delete.
local function rawkey(ev)
  if ev.code == 42 or ev.code == 54 then
    shift = ev.down
    return false
  end

  if ev.code == 29 or ev.code == 97 then
    ctrl = ev.down
    return false
  end

  if not ev.down then return false end

  -- While a field is being typed in: its keys, and nothing else's.
  if editing then
    local e = editing
    local ch = ({ [2] = "1", [3] = "2", [4] = "3", [5] = "4", [6] = "5", [7] = "6",
                  [8] = "7", [9] = "8", [10] = "9", [11] = "0", [52] = ".", [12] = "-" })[ev.code]

    if ch then
      if e.fresh then e.text, e.fresh = "", false end
      e.text = e.text .. ch
      return true
    end

    if ev.code == 14 then
      e.text = e.fresh and "" or e.text:sub(1, -2)
      e.fresh = false
      return true
    end

    if ev.code == 28 or ev.code == 96 then commit_edit() return true end

    if ev.code == 1 then
      editing = nil
      print("cafesa3d: left the field as it was")
      return true
    end

    if ev.code == 15 then
      -- Tab: kept, and on to the next field, round to the first.
      local at = 1

      for i, d in ipairs(fields_drawn) do
        if same_field(d.f, e.f) then at = i % #fields_drawn + 1 end
      end

      commit_edit()

      if fields_drawn[at] then start_edit(fields_drawn[at].f) end

      return true
    end

    return false
  end

  -- While G, R or S is going: its keys, and nothing else's.
  if modal then
    local m = modal
    local digit = ({ [2] = "1", [3] = "2", [4] = "3", [5] = "4", [6] = "5", [7] = "6",
                     [8] = "7", [9] = "8", [10] = "9", [11] = "0", [52] = "." })[ev.code]

    if ev.code == 1 then finish(false) return true end
    if ev.code == 28 or ev.code == 96 then finish(true) return true end

    local axis = ({ [45] = 1, [21] = 2, [44] = 3 })[ev.code]

    if axis then
      if m.axis == axis and m.plane == shift then
        m.axis = nil
      else
        m.axis, m.plane = axis, shift
      end
    elseif digit then
      m.typed = m.typed .. digit
    elseif ev.code == 12 then
      m.typed = m.typed:sub(1, 1) == "-" and m.typed:sub(2) or ("-" .. m.typed)
    elseif ev.code == 14 then
      m.typed = m.typed:sub(1, -2)
    else
      return false
    end

    local at = m.last or m.ref

    apply_modal(at and at[1], at and at[2])
    return true
  end

  if not ctrl and not shift then
    if ev.code == 34 then return begin("grab") end
    if ev.code == 19 then return begin("rotate") end
    if ev.code == 31 then return begin("scale") end
  end

  if ctrl and ev.code == 44 then
    if shift then return redo_last() end
    return undo_last()
  end

  if shift and ev.code == 30 then add_menu(pointer[1], pointer[2]) return true end
  if shift and ev.code == 32 then
    -- As Blender's: the copy follows the pointer at once.
    if duplicate_selected() then begin("grab") end
    return true
  end
  if ev.code == 45 then delete_menu(pointer[1], pointer[2]) return true end
  if ev.code == 111 then return delete_selected() end

  if ev.code == 2 then set_view(shift and "back" or "front") return true end
  if ev.code == 4 then set_view(shift and "left" or "right") return true end
  if ev.code == 8 then set_view(shift and "bottom" or "top") return true end
  if ev.code == 44 and not ctrl then
    set_shading(shading == "solid" and "wire" or "solid")
    return true
  end
  if ev.code == 102 then frame_all() return true end

  return false
end

say_selected()

if not draw_all() then return end

-- Where the window is and where its rows are, for whoever drives it from
-- outside - `tools/run_cafesa3d.py` clicks where these say.
print(("cafesa3d: window at %d,%d"):format(win.origin_x or 0, win.origin_y or 0))

do
  local out = {}

  for _, r in ipairs(rows) do
    out[#out + 1] = ("%s %d,%d eye %d"):format(r.thing.name, SX + 90, r.y + ROW // 2,
                                              SX + SIDE - 23)
  end

  local tabs = {}

  for _, name in ipairs(TABS) do
    local c = controls["tab:" .. name]

    tabs[#tabs + 1] = ("%s %d,%d"):format(name, c.x + c.w // 2, c.y + c.h // 2)
  end

  print("cafesa3d: rows " .. table.concat(out, "; "))
  print("cafesa3d: tabs " .. table.concat(tabs, "; "))

  local header = {}

  for _, name in ipairs({ "add", "wire", "solid", "tool:select", "tool:move", "tool:rotate",
                          "tool:scale" }) do
    local c = controls[name]

    header[#header + 1] = ("%s %d,%d"):format(name, c.x + c.w // 2, c.y + c.h // 2)
  end

  print("cafesa3d: controls " .. table.concat(header, "; "))
end

say_where()
print(("cafesa3d: %d objects, %d triangles, the view %d by %d, %s"):format(
  #things, scene:triangles(), VW, VH, shading))

local dirty = false

while win.running do
  if dirty then
    if not draw_all() then break end
    dirty = false
  end

  local reply = wmproto.poll(win.handle, 25)

  if not reply then break end

  local said_where = false

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      dirty = true
      said_where = true
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "mouse" and not ev.menu and ev.button == "right" then
      if modal and ev.action == "press" then
        finish(false)
        dirty = true
        said_where = true
      end
    elseif ev.type == "mouse" and not ev.menu then
      -- A press or a release that changed something says where things are
      -- afterwards; they may come in two polls, so each says it for itself.
      if ev.action == "press" then
        if press(ev.x, ev.y) then dirty, said_where = true, true end
      elseif ev.action == "move" then
        dirty = move(ev.x, ev.y) or dirty
      elseif ev.action == "release" then
        if release(ev.x, ev.y) then dirty, said_where = true, true end
      end
    elseif ev.type == "wheel" and in_view(ev.x, ev.y) then
      pointer = { ev.x, ev.y }
      orbit.dist = math.max(2, math.min(60, orbit.dist * (0.9 ^ (ev.n or 0))))
      view_name = VIEW_NAME
      print(("cafesa3d: %s, at %.1f"):format((ev.n or 0) > 0 and "closer" or "further",
                                             orbit.dist))
      dirty = true
    elseif ev.type == "rawkey" then
      if rawkey(ev) then
        dirty = true
        said_where = true
      end
    end
  end

  -- Where things are after a key or a menu changed them, as a release does
  -- - drawn first, so the fields said are the ones on the screen.
  if said_where then
    if dirty then
      if not draw_all() then break end
      dirty = false
    end

    say_where()
  end
end
