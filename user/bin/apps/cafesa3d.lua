-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Teapot
-- kosmos: name Cafesa3D
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
-- computed in this file**: the 3D Kit (`/Kosmos/Kits/3d`) holds the scene and
-- draws it, and this file decides what is in it and where the eye is.
--
--   cafesa3d              the still life
--   cafesa3d --wire       starting in Wireframe

local ui = use("/Kosmos/Libraries/ui.lua")
local theme = ui.theme
local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local k3 = use("/Kosmos/Kits/3d")
local compress = use("/Kosmos/Kits/compress")   -- base64, for a glTF's buffers
local json = use("/Kosmos/Libraries/json.lua")
local scenefile = use("/Kosmos/Libraries/scenefile.lua")
local game = use("/Kosmos/Kits/game")
local L = ui.layout

--------------------------------------------------------------------------
-- The window, maximised: Diego, 26 September, "3d tools are mostly used
-- maximized", "take the available space in the screen and open it
-- maximised always". The rectangle the window manager's own maximise would
-- give, asked for first, since a window that draws its own pixels is
-- opened at its size and never resized; the drawing's 1400 by 820 when
-- there is nobody to ask. `screen.area` rather than a local of its own:
-- this chunk is at Lua's limit of two hundred.
--------------------------------------------------------------------------

local screen = fs.read("/Devices/screen") or {}

do
  -- As a window whose header is its title bar: the room is the screen's,
  -- no tab taken off it (one window chrome, 7 October).
  local ok, got = pcall(fs.send, "/Running/wm", { type = "workarea", header = true })

  if ok and type(got) == "table" and got.ok and tonumber(got.w) and tonumber(got.h) then
    screen.area = got
  end
end

local W = screen.area and screen.area.w or math.min(1400, (screen.width or 1920) - 40)
local H = screen.area and screen.area.h or math.min(820, (screen.height or 1080) - 110)

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

-- `header = true`: it draws its own header (`pk.header`), which is the
-- title bar; the kit adds no band of its own above it.
local win = ui.window{ title = "Cafesa3D", w = W, h = H, direct = true, header = true,
                       maximised = screen.area and true or nil,
                       centre = not screen.area or nil }

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

-- How a material looks in the Material tab: the dots on the presets'
-- chips, the colours a click picks, and the kit's patterns by the names the
-- tab gives them, each with the numbers it starts from - the samples' own,
-- which look like the thing.
local LOOK = {
  dot = { Plastic = 0xffc33b2c, Metal = 0xffe0a84a, Mirror = 0xffc9ced6,
          Glass = 0xffbfe0f0, Light = 0xffffe28a },
  swatches = { 0xc81d25, 0xe8772e, 0xf2c230, 0x3f8f3a, 0x2f6fc4, 0x7a4bc2,
               0xf2f2f2, 0x8a8d92, 0x1c1c1e, 0x7a4a2a },
  texture_order = { "None", "Checker", "Brick", "Tiles", "Noise", "Wood", "Marble" },
  texture_name = { plain = "None", checker = "Checker", brick = "Brick", shingles = "Tiles",
                   noise = "Noise", wood = "Wood", marble = "Marble" },
}

LOOK.textures = {
  Checker = { pattern = "checker", colour2 = 0x1c1c1e, scale = 4, bump = 0 },
  Brick   = { pattern = "brick", colour2 = 0xd8d0c2, scale = 13, ratio = 2.9, mortar = 0.14,
              offset = 0.5, bump = 0.004 },
  Tiles   = { pattern = "shingles", colour2 = 0x4a1d14, scale = 4.5, ratio = 1.3,
              mortar = 0.05, offset = 0.5, bump = 0.018 },
  Noise   = { pattern = "noise", colour2 = 0x2a2c30, scale = 3, detail = 4, distortion = 0.6,
              bump = 0.01 },
  Wood    = { pattern = "wood", colour2 = 0x5c3a20, scale = 9, detail = 3, distortion = 2.2,
              bump = 0.002 },
  Marble  = { pattern = "marble", colour2 = 0x3a3d42, scale = 2, detail = 4, distortion = 1.5,
              bump = 0 },
}

local things = {}                           -- in the order they were added
local SHAPES = { plane = true, box = true, sphere = true, cylinder = true,
                 ico = true, cone = true, torus = true, grid = true, mesh = true }

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
    mat = m,
  }
end

-- Bumped by every change the kit is told of, so a render can tell it is
-- looking at a scene that has moved on.
local scene_version = 0

local function add(t)
  if SHAPES[t.kind] then
    t.rot = t.rot or { 0, 0, 0 }
    t.scale = t.scale or { 1, 1, 1 }

    local fields = shown(t)

    fields.kind = t.kind

    -- A mesh's triangles go to the kit when it is added and never again:
    -- `sync` sends what moved, and sending them with it would work out
    -- every corner's normal afresh on every step of a drag.
    if t.kind == "mesh" then
      fields.vertices, fields.triangles = t.vertices, t.triangles
      fields.index_bytes, fields.yup, fields.smooth_angle = t.index_bytes, true, t.smooth_angle
    end

    t.id = assert(scene:add(fields))
  end

  scene_version = scene_version + 1

  things[#things + 1] = t
  return t
end

local function sync(t)
  if t.id then scene:set(t.id, shown(t)) end

  scene_version = scene_version + 1
end

-- How pictures are made, as the Render tab shows it and the scene's file
-- keeps it: F12's size, samples and bounces, the Rendered view's samples,
-- and Final (path tracing) or Preview (Whitted's, quicker and harder).
local RENDER = { name = "the render", w = 640, h = 360, samples = 256, view_samples = 64,
                 bounces = 6, preview = false }

-- `will` is further down with undo; these are only called once it exists.
local will

local choose = {}

function choose.preset(t, p)
  will(("set the preset of %s"):format(t.name))

  for k, v in pairs(PRESETS[p]) do t.mat[k] = v end

  t.mat.preset = p
  sync(t)
  print(("cafesa3d: set preset of %s to %s"):format(t.name, p))
end

function choose.base(t, c)
  will(("set Base colour of %s"):format(t.name))
  t.mat.base = c
  sync(t)
  print(("cafesa3d: set Base colour of %s to #%06x"):format(t.name, c))
end

function choose.integrator(preview)
  will("set the integrator of the render")
  RENDER.preview = preview
  scene_version = scene_version + 1
  print(("cafesa3d: set integrator of the render to %s"):format(preview and "Preview" or "Final"))
end

function choose.size(w, h)
  will("set the size of the render")
  RENDER.w, RENDER.h = w, h
  print(("cafesa3d: set size of the render to %d by %d"):format(w, h))
end

function choose.shading(t, smooth)
  will(("set the shading of %s"):format(t.name))
  t.smooth = smooth
  sync(t)
  print(("cafesa3d: set shading of %s to %s"):format(t.name, smooth and "Smooth" or "Flat"))
end

-- A texture chosen by name: the pattern's own starting numbers, or none -
-- `false` rather than nil, which the kit takes as "no texture" where nil
-- would leave the last one on (`k3d_kosmos.c`).
function choose.texture(t, name)
  will(("set the texture of %s"):format(t.name))

  if name == "None" then
    t.mat.texture = false
  else
    local tx = {}

    for k, v in pairs(LOOK.textures[name]) do tx[k] = v end

    t.mat.texture = tx
  end

  sync(t)
  print(("cafesa3d: set texture of %s to %s"):format(t.name, name))
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

  scene_version = scene_version + 1

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

-- `--wire` and `--rendered` open on that shading: the second is how the
-- gallery `make shot` takes has the ray tracer in it.
local shading = (tostring(args or "")):find("%-%-wire") and "wire"
                or (tostring(args or "")):find("%-%-rendered") and "rendered" or "solid"
-- The file the scene is in: its name in the header, where it was saved or
-- opened from - none for the still life or a sample, which Save asks about
-- - the scene's own name, and whether it has changed since. `said` is the
-- last save's or open's outcome, on the view until the next change.
local FILE = { name = "still-life.scene", title = "Still life", path = nil, changed = false }

-- Full screen (`FULL.toggle`, near the end): whether it is on, the
-- window's size to come back to, and the view's vertical angle, which is
-- what stays when the view's shape changes.
local FULL = { on = false, was = { W, H }, tv = math.tan(FOV / 2) * 744 / 1014 }

--
-- **The Script panel** (`roadmap.md` 4l, 6n step 6; `docs/cafesa3d-scripting.html`):
-- beside the view, 470 wide, opened and closed with Shift F4. Its editor is
-- the IDE's - `ui.editor` in the code look, drawn into this window's own
-- pixels by `ui.paint_view` - and under it a strip for what a run printed
-- and how it went. While it holds the keyboard, keys are words in it rather
-- than commands here; a press anywhere else, or Escape, gives them back.
--
-- One table rather than several locals, because this file is near Lua's
-- two hundred.
--
local SCRIPT = {
  W = 470, OUT = 78, open = false, focused = false, name = "staircase",
  out = {}, decode = ui.key_decoder(),
  -- The drawing's own script (`docs/cafesa3d-scripting.html`): a spiral
  -- staircase, twenty-four steps round a column, and a lamp. What the panel
  -- offers a scene that came with no script of its own.
  SAMPLE = table.concat({
    "-- A spiral staircase: a step every fifteen degrees,",
    "-- each a little higher, round a steel column.",
    "local steps, rise = 24, 0.18",
    "",
    "for i = 0, steps - 1 do",
    "  local a = math.rad(i * 15)",
    "",
    "  scene.box{",
    "    name = \"Step\", size = { 1.4, 0.34, 0.07 },",
    "    loc = { math.cos(a) * 0.8, math.sin(a) * 0.8, 0.2 + i * rise },",
    "    rot = { 0, 0, i * 15 },",
    "    material = { preset = \"Plastic\", base = 0x7a4a2a,",
    "                 texture = \"Wood\", rough = 0.45 },",
    "  }",
    "end",
    "",
    "scene.cylinder{",
    "  name = \"Column\", radius = 0.1, depth = 0.4 + steps * rise,",
    "  loc = { 0, 0, (0.4 + steps * rise) / 2 },",
    "  material = { preset = \"Metal\", base = 0x8a8d92 },",
    "}",
    "",
    "scene.light{ loc = { -3, -4, 7 }, power = 2500, radius = 1 }",
    "print((\"%d steps, %.1f m up\"):format(steps, steps * rise))",
  }, "\n") .. "\n",
}

--
-- **Every key, in one table** (`roadmap.md` 4l, 5h). Diego, 27 September: "I
-- want a button that pops up all key commands and shortcuts". The keys the
-- view answers are handled from `KEYS.list` (filled beside `rawkey`, where
-- what they call exists) and the Keys sheet is drawn from it, so the sheet
-- cannot name a key the handler has not got or miss one it has. A row with
-- no `code` is shown and handled somewhere else: the pointer, the keys
-- inside G, R and S, a field in Properties, the Script panel.
--
local KEYS = { open = false,
  GROUPS = { "The view", "Objects", "Move, turn, size", "Files and the window",
             "The script", "A number in Properties" },
}

-- The panel's text before its editor is made; `kept` is whether the scene
-- has a script to save - once the panel has been open over it, or it came
-- with one (6d).
SCRIPT.text, SCRIPT.kept = SCRIPT.SAMPLE, false

-- The light from everywhere that is not a lamp: a sky, from the zenith to
-- the horizon. What the ray tracer (step three) lights a scene with.
local world = { zenith = 0x6d90c6, horizon = 0xdfe6ef, strength = 0.9 }
local WORLD = { name = "the world" }        -- what a sky's fields say they are of

local tab = "object"
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
-- The Rendered view: the scene ray traced as it is looked at, by the 3D
-- Kit's tracer on every processor (`k3d_trace.c`). A render of its own,
-- started again whenever the eye, the scene, a lamp or the sky moves;
-- until its tiles arrive, the Solid view shows underneath them.
--------------------------------------------------------------------------

-- The view's render: `job`, what it saw (`key`), whether its last pass has
-- been said, and `surf`, its pixels as they arrive.
local shade = {}
local HZ = fs.read("/Devices/cpu").counter_hz

local function lamps()
  local out = {}

  for _, t in ipairs(things) do
    if t.kind == "light" and not t.hidden then
      out[#out + 1] = { loc = t.loc, radius = t.radius or 0.1, power = t.power or 1000,
                        colour = t.colour or 0xffffff }
    end
  end

  return out
end

-- Everything a render looks at, as one string: when it differs from the
-- running render's, that render is of something that is no longer there.
local function render_key()
  local e, t = eye(), orbit.target
  local parts = { ("%.4f %.4f %.4f %.4f %.4f %.4f %d %x %x %.3f"):format(
    e[1], e[2], e[3], t[1], t[2], t[3], scene_version, world.zenith, world.horizon,
    world.strength) }

  for _, l in ipairs(lamps()) do
    parts[#parts + 1] = ("%.3f %.3f %.3f %.3f %.1f %x"):format(l.loc[1], l.loc[2], l.loc[3],
                                                              l.radius, l.power, l.colour)
  end

  return table.concat(parts, "|")
end

function shade.stop()
  if shade.job then
    shade.job:stop()
    shade.job = nil
  end
end

-- `s` holds the Solid view just drawn, which is what shows until the
-- render's own tiles cover it.
function shade.start(s)
  shade.stop()

  local e, t = eye(), orbit.target
  local job, why

  shade.surf = shade.surf or gfx.surface{ w = VW, h = VH }
  job, why = k3.render(scene, { w = VW, h = VH, eye = e, target = t, fov = FOV,
                                passes = RENDER.view_samples, bounces = RENDER.bounces,
                                preview = RENDER.preview, world = world,
                                lights = lamps() })

  if not job then
    print("cafesa3d: " .. tostring(why))
    return
  end

  shade.job, shade.key, shade.said, shade.first = job, render_key(), false, false
  shade.t0 = sys.ticks()
  shade.surf:blit(s, VX, VY, VW, VH, 0, 0)
  print(("cafesa3d: rendering the view, %d by %d, on %d thread%s"):format(VW, VH, job:workers(),
                                                                         job:workers() == 1 and "" or "s"))
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

-- A script: the two angle brackets code is written between.
function GLYPH.script(s, x, y, c)
  polyline(s, { { x + 5, y + 3 }, { x + 1.5, y + 7.5 }, { x + 5, y + 12 } }, c)
  polyline(s, { { x + 9, y + 3 }, { x + 12.5, y + 7.5 }, { x + 9, y + 12 } }, c)
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
  -- Left of the three: the header is the title bar (one chrome, 7 October).
  local right = W - L.lights_in - ((win and win.lights and win.lights.w) or 62) - L.head_edge
  local cy = (HEAD - 1 - 31) // 2

  -- From the right: the dots, Render, the shading.
  local more = control("more", right - 26, (HEAD - 1 - 26) // 2, 26, 26)
  local keys_w = 12 + gfx.measure("Keys") + 8 + gfx.measure("?", small) + 12
  local keysb = control("keys", more.x - L.head_gap - keys_w, cy, keys_w, 31)
  local render_w = pk.button_width("Render F12") + 4
  local render = control("render", keysb.x - L.head_gap - render_w, cy, render_w, 31)

  local shade_parts = {
    { name = "wire", text = "Wireframe", on = shading == "wire",
      dot = function(s2, x, y)
        ring(s2, x + 6.5, y + 6.5, 5.5, theme.text_dim)
        ring(s2, x + 6.5, y + 6.5, 5.5, theme.text_dim, -math.pi / 2, math.pi / 2)
        line(s2, x + 1, y + 6.5, x + 12, y + 6.5, theme.text_dim)
      end },
    { name = "solid", text = "Solid", on = shading == "solid",
      dot = function(s2, x, y) s2:disc(x + 6, y + 6, 6, theme.text_dim, true) end },
    { name = "rendered", text = "Rendered", on = shading == "rendered",
      dot = function(s2, x, y) s2:disc(x + 6, y + 6, 6, 0xffe0a84a, true) end },
  }
  local shade_w = 0

  for _, p in ipairs(shade_parts) do
    shade_w = shade_w + gfx.measure(p.text) + 22 + 19
  end

  local shade_x = render.x - 8 - shade_w - 2
  local title_end = pk.header(s, 0, 0, W, "Cafesa3D", FILE.name, shade_x)

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

  -- Duplicate, with its key (`roadmap.md` 4l, 5h): Diego, "both" - here, and
  -- in the selection's menu. Dim while nothing is selected.
  local dup_w = 12 + gfx.measure("Duplicate") + 8 + gfx.measure("Shift D", small) + 12
  local db = control("duplicate", addb.x + addb.w + 6, cy, dup_w, 31)

  pk.button(s, { x = db.x, y = db.y, w = db.w, text = "" })
  s:text(db.x + 12, db.y + (31 - gfx.height()) // 2, "Duplicate",
         selected and theme.text or theme.text_dim, nil, "ui")
  s:text(db.x + 12 + gfx.measure("Duplicate") + 8, db.y + (31 - gfx.height(small)) // 2,
         "Shift D", theme.text_dim, nil, small)

  -- Script, with its key, lit while the panel is open.
  local script_w = 12 + gfx.measure("Script") + 8 + gfx.measure("Shift F4", small) + 12
  local sb = control("script", db.x + db.w + 6, cy, script_w, 31)

  if SCRIPT.open then
    s:fill_round(sb.x, sb.y, sb.w, 31, theme.mix(theme.sunken, theme.accent, 120), 7)
    s:frame_round(sb.x, sb.y, sb.w, 31, theme.mix(theme.line_soft, theme.accent, 300), 7)
  else
    pk.button(s, { x = sb.x, y = sb.y, w = sb.w, text = "" })
  end

  s:text(sb.x + 12, sb.y + (31 - gfx.height()) // 2, "Script",
         SCRIPT.open and theme.accent or theme.text, nil, "ui")
  s:text(sb.x + 12 + gfx.measure("Script") + 8, sb.y + (31 - gfx.height(small)) // 2,
         "Shift F4", theme.text_dim, nil, small)

  segmented(s, shade_x, (HEAD - 1 - 31) // 2, shade_parts)

  -- Render, the verb, filled.
  s:fill_round(render.x, render.y, render.w, 31, theme.accent, 7)
  s:text(render.x + 12, render.y + (31 - gfx.height()) // 2, "Render",
         theme.text_on, nil, "ui")
  s:text(render.x + 12 + gfx.measure("Render") + 7,
         render.y + (31 - gfx.height(small)) // 2, "F12",
         theme.mix(theme.accent, theme.text_on, 600), nil, small)

  -- Keys, lit while the sheet is open.
  if KEYS.open then
    s:fill_round(keysb.x, keysb.y, keysb.w, 31, theme.mix(theme.sunken, theme.accent, 120), 7)
  else
    pk.button(s, { x = keysb.x, y = keysb.y, w = keysb.w, text = "" })
  end

  s:text(keysb.x + 12, keysb.y + (31 - gfx.height()) // 2, "Keys",
         KEYS.open and theme.accent or theme.text, nil, "ui")
  s:text(keysb.x + 12 + gfx.measure("Keys") + 8, keysb.y + (31 - gfx.height(small)) // 2,
         "?", theme.text_dim, nil, small)

  pk.iconbutton(s, { x = more.x, y = more.y, icon = "more" })
end

--
-- **The Keys sheet**, over the view: the groups in three columns, each key
-- on the right of its column and what it does beside it. Opened by the
-- button or `?`, closed by either, Escape, or a click anywhere.
--
function KEYS.toggle()
  KEYS.open = not KEYS.open

  if KEYS.open then
    local groups = {}

    for _, k in ipairs(KEYS.list) do groups[k.group] = true end

    local n = 0

    for _ in pairs(groups) do n = n + 1 end

    print(("cafesa3d: keys shown, %d in %d groups"):format(#KEYS.list, n))
  else
    print("cafesa3d: keys hidden")
  end

  return true
end

function KEYS.draw(s)
  if not KEYS.open then return end

  local ROW, HEAD_H, GAP, PAD = 22, 26, 34, 24
  local place = { { 1, 5 }, { 2, 6 }, { 3, 4 } }
  local rows, kw, sw = {}, 0, 0

  for _, k in ipairs(KEYS.list) do
    rows[k.group] = rows[k.group] or {}
    table.insert(rows[k.group], k)
    kw = math.max(kw, gfx.measure(k.keys, small))
    sw = math.max(sw, gfx.measure(k.says))
  end

  local colw = kw + 12 + sw
  local tall = 0

  for _, col in ipairs(place) do
    local h = 0

    for _, g in ipairs(col) do h = h + HEAD_H + #(rows[g] or {}) * ROW + 12 end

    tall = math.max(tall, h)
  end

  local w = #place * colw + (#place - 1) * GAP + 2 * PAD
  local h = tall + 2 * PAD + 30
  local x0 = VX + math.max(8, (VW - w) // 2)
  local y0 = VY + math.max(8, (VH - h) // 2)

  s:fill_round(x0, y0, w, h, theme.window, 10)
  s:frame_round(x0, y0, w, h, theme.line_soft, 10)
  s:text(x0 + PAD, y0 + PAD - 4, "Keys", theme.text, nil, "ui")
  s:text(x0 + PAD + gfx.measure("Keys") + 12, y0 + PAD - 2,
         "? or Esc closes these, or a click anywhere", theme.text_dim, nil, small)

  for c, col in ipairs(place) do
    local x = x0 + PAD + (c - 1) * (colw + GAP)
    local y = y0 + PAD + 30

    for _, g in ipairs(col) do
      s:text(x, y + 6, KEYS.GROUPS[g]:upper(), theme.text_dim, nil, tiny)
      y = y + HEAD_H

      for _, k in ipairs(rows[g] or {}) do
        s:text(x + kw - gfx.measure(k.keys, small), y + (ROW - gfx.height(small)) // 2,
               k.keys, theme.accent, nil, small)
        s:text(x + kw + 12, y + (ROW - gfx.height()) // 2, k.says, theme.text, nil, "ui")
        y = y + ROW
      end

      y = y + 12
    end
  end
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
  -- Its frame, a metre ahead of it, sixteen by nine, as Blender draws one -
  -- unless the view is looking through it, as it is when a scene opens.
  local p, q = t.loc, t.target
  local e = eye()

  if (e[1] - p[1]) ^ 2 + (e[2] - p[2]) ^ 2 + (e[3] - p[3]) ^ 2 < 1 then return end
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

  local _, _, f = axes()
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

  if shading == "rendered" then
    -- The Solid view is drawn only when the render starts again: it is
    -- what shows under tiles not yet traced, and what a click picks from,
    -- and neither changes until the eye or the scene does.
    if not shade.job or render_key() ~= shade.key then
      view:draw(scene, s, VX, VY, { mode = "solid", selected = 0, grid = false })
      shade.start(s)
    end

    if shade.job then
      shade.job:paint(shade.surf, 0, 0)
      s:blit(shade.surf, 0, 0, VW, VH, VX, VY)
    end
  else
    shade.stop()
    view:draw(scene, s, VX, VY, {
      mode = shading == "wire" and "wire" or "solid",
      selected = selected and selected.id or 0,
      grid = true,
    })
  end

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
    local p = modal.loc0
    local planes = modal.plane and { modal.axis % 3 + 1, (modal.axis + 1) % 3 + 1 }
                   or { modal.axis }

    for _, i in ipairs(planes) do
      local d = AXES[i]

      view:line(s, VX, VY, p[1] - d[1] * 60, p[2] - d[2] * 60, p[3] - d[3] * 60,
                p[1] + d[1] * 60, p[2] + d[2] * 60, p[3] + d[3] * 60,
                AXIS_COLOUR[i], 220, false)
    end
  end

  -- Light words on the Solid view's dark grey; on a render, which may be a
  -- white sky, each has a shadow under it, as Blender's overlay text does.
  local lit = shading == "rendered"
  local ty, step = VY + 10, gfx.height(small) + 2

  local function say(y, text, colour)
    if lit then s:text(VX + 15, y + 1, text, 0xff15171b, nil, small) end
    s:text(VX + 14, y, text, lit and VP_INK or colour, nil, small)
  end

  say(ty, modal and modal_line() or view_name, VP_INK)
  say(ty + step, "Collection | " .. (selected and selected.name or "nothing selected"), VP_DIM)

  if lit and shade.job then
    say(ty + 2 * step, ("Sample %d/%d"):format(shade.job:passes(), RENDER.view_samples), VP_DIM)
  end

  if FILE.said then say(ty + 3 * step, FILE.said, VP_INK) end

  say(VY + VH - 12 - gfx.height(small),
      "Drag to turn \u{b7} Shift-drag to move \u{b7} scroll to come closer", VP_DIM)
end

--------------------------------------------------------------------------
-- The Outliner: the collection and everything in it, by name, each with
-- its eye - what a script made under the script's name, as the drawing
-- has it (6d) - and the wheel to reach what does not fit: a script makes
-- more objects than there are rows.
--------------------------------------------------------------------------

-- As drawn: `rows`, each { y, thing } or { y, script }; `top`, how many
-- rows the wheel has taken off the top.
local OUTLINER = { rows = {}, top = 0 }

-- What it lists, in order: each script's objects under its name, then
-- what was made by hand, each by name.
function OUTLINER.list()
  local by, names, loose, list = {}, {}, {}, {}
  local function by_name(a, b) return a.name < b.name end

  for _, t in ipairs(things) do
    if t.by then
      if not by[t.by] then by[t.by], names[#names + 1] = {}, t.by end
      table.insert(by[t.by], t)
    else
      loose[#loose + 1] = t
    end
  end

  table.sort(names)

  for _, name in ipairs(names) do
    list[#list + 1] = { script = name }
    table.sort(by[name], by_name)
    for _, t in ipairs(by[name]) do list[#list + 1] = { thing = t, inside = true } end
  end

  table.sort(loose, by_name)
  for _, t in ipairs(loose) do list[#list + 1] = { thing = t } end

  return list
end

-- Three rows a notch; true when it moved.
function OUTLINER.wheel(n)
  local top = math.max(0, math.min(OUTLINER.top - n * 3,
                                   (OUTLINER.count or 0) - (OUTLINER.room or 0)))

  if top == OUTLINER.top then return false end

  OUTLINER.top = top
  return true
end

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

  local list = OUTLINER.list()
  local y = OUT_Y + 4
  local ty = (ROW - gfx.height()) // 2
  local first = y + ROW
  local room = (PROPS_Y - first) // ROW

  OUTLINER.top = math.max(0, math.min(OUTLINER.top, #list - room))
  OUTLINER.count, OUTLINER.room, OUTLINER.rows = #list, room, {}

  GLYPH.collection(s, SX + 30, y + (ROW - 14) // 2, theme.text_dim)
  s:text(SX + 52, y + ty, "Collection", theme.text_dim, nil, "ui")
  y = first

  for i = OUTLINER.top + 1, #list do
    if y + ROW > PROPS_Y then break end

    local r = list[i]

    if r.script then
      -- The script's own row: its name and a badge saying what it is.
      local bx = SX + 52 + gfx.measure(r.script) + 8

      GLYPH.script(s, SX + 30, y + (ROW - 14) // 2, theme.accent)
      s:text(SX + 52, y + ty, r.script, theme.text, nil, "ui")
      s:frame_round(bx, y + 5, gfx.measure("script", tiny) + 10, ROW - 10, theme.line_soft, 4)
      s:text(bx + 5, y + (ROW - gfx.height(tiny)) // 2, "script", theme.text_dim, nil, tiny)
    else
      local t, dx = r.thing, r.inside and 14 or 0
      local on = t == selected

      if on then s:fill(SX + 1, y, SIDE - 1, ROW, theme.mix(theme.sunken, theme.accent, 110)) end

      GLYPH[kind_glyph(t)](s, SX + 48 + dx, y + (ROW - 14) // 2,
                           t.hidden and DIM or theme.text_dim)
      s:text(SX + 70 + dx, y + ty, t.name,
             t.hidden and DIM or (on and theme.accent or theme.text), nil, "ui")
      GLYPH.eye(s, SX + SIDE - 30, y + (ROW - 14) // 2, t.hidden and DIM or theme.text_dim,
                t.hidden)
    end

    r.y = y
    OUTLINER.rows[#OUTLINER.rows + 1] = r
    y = y + ROW
  end

  -- Where the rows shown are among all of them, when not all fit.
  if #list > room then
    local track = PROPS_Y - first - 4
    local bh = math.max(16, track * room // #list)

    s:fill(SX + SIDE - 7, first + (track - bh) * OUTLINER.top // (#list - room), 3, bh,
           theme.line)
  end

  s:fill(SX, PROPS_Y, SIDE, 1, theme.line_soft)

  -- What it shows, said when that changes - for whoever drives Cafesa3D
  -- from outside (`tools/run_script.py`).
  local shown = {}

  for _, r in ipairs(OUTLINER.rows) do
    shown[#shown + 1] = r.script and ("script " .. r.script) or r.thing.name
  end

  local said = ("rows %d to %d of %d: %s"):format(OUTLINER.top + 1,
                OUTLINER.top + #OUTLINER.rows, #list, table.concat(shown, ", "))

  if said ~= OUTLINER.said then
    OUTLINER.said = said
    print("cafesa3d: outliner " .. said)
  end
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

  -- A material's, Blender's Principled numbers in Blender's ranges.
  metallic = { places = 2, step = 0.01, lo = 0, hi = 1 },
  rough    = { places = 2, step = 0.01, lo = 0, hi = 1 },
  ior      = { places = 2, step = 0.01, lo = 1, hi = 3 },
  trans    = { places = 2, step = 0.01, lo = 0, hi = 1 },
  emit     = { places = 1, step = 0.1, lo = 0, hi = 100 },

  -- A texture's (`k3d_texture.c`): so many a metre, and so deep.
  tscale   = { places = 2, step = 0.05, lo = 0.01, hi = 1000, key = "scale" },
  bump     = { unit = " m", places = 3, step = 0.0005, lo = 0, hi = 0.2 },

  -- The sky's.
  strength = { places = 2, step = 0.01, lo = 0, hi = 10 },

  -- The render's (`RENDER`): whole numbers, in the ranges the kit takes.
  samples      = { int = true, step = 1, lo = 1, hi = 4096 },
  view_samples = { int = true, step = 1, lo = 1, hi = 1024 },
  bounces      = { int = true, step = 0.2, lo = 1, hi = 32 },
  w            = { int = true, step = 4, lo = 16, hi = 8192 },
  h            = { int = true, step = 4, lo = 16, hi = 8192 },

  -- A colour, typed as #rrggbb; nothing to scrub.
  colour   = { colour = true },
}

local fields_drawn = {}                     -- { x, y, w, h, f }, as drawn
local editing = nil                         -- { f, text, fresh } while typing
local field_drag = nil

--
-- A field of `t`, whose value is `holder[name]` - `t` itself unless it
-- says otherwise, as a material's numbers are its object's `mat`, a
-- texture's its `mat.texture` and the sky's the world's. `kind` names the
-- spec when it is not the name: a texture's scale is not an object's.
--
local function F(t, name, index, label, holder, kind)
  local spec = FIELD[kind or name]

  return { t = t, name = spec.key or name, index = index, spec = spec,
           holder = holder or t,
           label = label .. (index and (" " .. AXIS_NAME[index]) or "") }
end

local function field_value(f)
  local v = f.holder[f.name]

  if f.index then v = v[f.index] end

  return v
end

local function field_text(f, unit)
  local v, sp = field_value(f), f.spec

  if sp.colour then return ("#%06x"):format((v or 0) & 0xffffff) end

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

  if f.index then f.holder[f.name][f.index] = v else f.holder[f.name] = v end

  sync(f.t)
end

local function same_field(a, b)
  return a and b and a.holder == b.holder and a.name == b.name and a.index == b.index
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

    -- A colour shows itself beside its number.
    local tx = bx + (each - gfx.measure(text, small)) // 2

    if f and f.spec.colour then
      s:fill_round(bx + 7, y + 6, 14, 14, 0xff000000 | (field_value(f) or 0), 3)
      s:frame_round(bx + 7, y + 6, 14, 14, theme.line_soft, 3)
      tx = math.max(tx, bx + 26)
    end

    s:text(tx, y + (26 - gfx.height(small)) // 2, text,
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

--------------------------------------------------------------------------
-- Chips: a row of choices with the one in force marked - a material's
-- preset, a swatch, a texture's pattern - each saying what a click on it
-- does. Found again from `chip.actions`, which is only what is drawn now.
--------------------------------------------------------------------------

local chip = { actions = {} }               -- actions: control name -> function, as drawn

function chip.row(s, x, y, w, items)
  local cx = x

  for _, it in ipairs(items) do
    local cw = it.swatch and 22 or gfx.measure(it.text, small) + (it.dot and 30 or 20)

    if cx + cw > x + w then cx, y = x, y + 30 end

    if it.swatch then
      if it.on then s:frame_round(cx - 2, y, 26, 26, theme.accent, 7) end

      s:fill_round(cx, y + 2, 22, 22, 0xff000000 | it.swatch, 5)
      s:frame_round(cx, y + 2, 22, 22, theme.line_soft, 5)
    else
      local tx = cx + 10

      s:fill_round(cx, y, cw, 26, it.on and theme.mix(theme.sunken, theme.accent, 110)
                   or theme.sunken, 13)
      s:frame_round(cx, y, cw, 26, it.on and theme.accent or theme.line_soft, 13)

      if it.dot then
        s:disc(cx + 12, y + 13, 5, it.dot, true)
        tx = cx + 22
      end

      s:text(tx, y + (26 - gfx.height(small)) // 2, it.text, it.on and theme.accent or theme.text,
             nil, small)
    end

    control(it.key, cx, y, cw, 26)
    chip.actions[it.key] = it.go
    cx = cx + cw + (it.swatch and 4 or 5)
  end

  return y + 34
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
  for _, line in ipairs(ui.wrapped(text, w, small)) do
    s:text(x, y, line, theme.text_dim, nil, small)
    y = y + gfx.height(small) + 3
  end

  return y
end

local KIND_NAME = { box = "Cube", sphere = "UV Sphere", cylinder = "Cylinder",
                    plane = "Plane", light = "Point light", camera = "Camera",
                    ico = "Ico Sphere", cone = "Cone", torus = "Torus", grid = "Grid",
                    mesh = "Mesh" }

local function draw_props(s)
  local x0 = SX + TABS_W

  fields_drawn = {}
  chip.actions = {}
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
    y = chip.row(s, x, y, w, {
      { text = "Final", key = "integrator:Final", on = not RENDER.preview,
        go = function() choose.integrator(false) end },
      { text = "Preview", key = "integrator:Preview", on = RENDER.preview,
        go = function() choose.integrator(true) end },
    })
    y = field_row(s, x, y, w, "Samples", { F(RENDER, "samples", nil, "Samples") })
    y = field_row(s, x, y, w, "View samples", { F(RENDER, "view_samples", nil, "View samples") })
    y = field_row(s, x, y, w, "Bounces", { F(RENDER, "bounces", nil, "Bounces") })

    s:text(x, y + 4, "SIZE", theme.text_dim, nil, tiny)
    y = y + gfx.height(tiny) + 10

    local sizes = {}

    for _, wh in ipairs({ { 640, 360 }, { 1280, 720 }, { 1920, 1080 }, { 3440, 1440 } }) do
      sizes[#sizes + 1] = { text = ("%d \u{d7} %d"):format(wh[1], wh[2]),
                            key = ("size:%dx%d"):format(wh[1], wh[2]),
                            on = RENDER.w == wh[1] and RENDER.h == wh[2],
                            go = function() choose.size(wh[1], wh[2]) end }
    end

    y = chip.row(s, x, y, w, sizes)
    y = field_row(s, x, y, w, "Width, height", { F(RENDER, "w", nil, "Width"),
                                                  F(RENDER, "h", nil, "Height") })
    note(s, x, y + 4, w, "Final traces as Cycles does, on every processor; Preview is Whitted's, quicker and harder. Rendered shows the view that way; F12 renders through the camera at this size.")
  elseif tab == "world" then
    y = heading(s, x, y, "World")
    y = field_row(s, x, y, w, "Zenith", { F(WORLD, "zenith", nil, "Zenith", world, "colour") })
    y = field_row(s, x, y, w, "Horizon", { F(WORLD, "horizon", nil, "Horizon", world, "colour") })
    y = field_row(s, x, y, w, "Strength", { F(WORLD, "strength", nil, "Strength", world) })
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
      y = field_row(s, x, y, w, "Colour", { F(t, "colour", nil, "Colour") })
      y = field_row(s, x, y, w, "Power", { F(t, "power", nil, "Power") })
      y = field_row(s, x, y, w, "Radius", { F(t, "radius", nil, "Radius") })
    elseif t.kind == "camera" then
      y = field_row(s, x, y, w, "Focal length", { F(t, "focal", nil, "Focal length") })
      y = field_row(s, x, y, w, "Sensor", { "36 mm" })
    elseif t.kind == "mesh" then
      y = field_row(s, x, y, w, "Points", { tostring(#t.vertices // 12) })
      y = field_row(s, x, y, w, "Auto smooth", { fmt(t.smooth_angle or 30, 0) .. "\u{b0}" })
    end

    if t.kind == "mesh" then
      note(s, x, y + 6, w, ("Its own triangles, %d of them, read from the file: the edges between faces that meet at less than the auto smooth angle are drawn round."):format(scene:triangles(t.id)))
    elseif t.id then
      -- Blender's Shade Flat and Shade Smooth: the facets, or the shape
      -- they stand for - which the ray tracer then traces exactly for a
      -- sphere or a cylinder.
      s:text(x + 96 - gfx.measure("Shading", small), y + (26 - gfx.height(small)) // 2,
             "Shading", theme.text_dim, nil, small)
      y = chip.row(s, x + 104, y, w - 104, {
        { text = "Flat", key = "shade:flat", on = not t.smooth,
          go = function() choose.shading(t, false) end },
        { text = "Smooth", key = "shade:smooth", on = t.smooth,
          go = function() choose.shading(t, true) end },
      }) - 3
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

      -- The five starting points, as chips; the one in force marked. A
      -- click sets all its numbers and keeps the colour and the texture.
      local presets = {}

      for _, p in ipairs(PRESET_ORDER) do
        presets[#presets + 1] = { text = p, key = "preset:" .. p, on = m.preset == p,
                                  dot = LOOK.dot[p], go = function() choose.preset(t, p) end }
      end

      y = chip.row(s, x, y, w, presets)
      y = field_row(s, x, y, w, "Base colour", { F(t, "base", nil, "Base colour", m, "colour") })

      local swatches = {}

      for i, c in ipairs(LOOK.swatches) do
        swatches[i] = { swatch = c, key = "swatch:" .. i, on = m.base == c,
                        go = function() choose.base(t, c) end }
      end

      y = chip.row(s, x, y - 2, w, swatches)
      y = field_row(s, x, y, w, "Metallic", { F(t, "metallic", nil, "Metallic", m) })
      y = field_row(s, x, y, w, "Roughness", { F(t, "rough", nil, "Roughness", m) })
      y = field_row(s, x, y, w, "Transmission", { F(t, "trans", nil, "Transmission", m) })

      if (m.trans or 0) > 0 then
        y = field_row(s, x, y, w, "IOR", { F(t, "ior", nil, "IOR", m) })
      end

      y = field_row(s, x, y, w, "Emission", { F(t, "emit", nil, "Emission", m) })

      -- The texture: a pattern worked out from where a point is, mixing the
      -- base colour towards a second one and standing the surface up by
      -- its height (`k3d_texture.c`). The Rendered view shows it.
      local tx = m.texture or nil
      local now = tx and LOOK.texture_name[tx.pattern] or "None"

      s:text(x, y + 4, "TEXTURE", theme.text_dim, nil, tiny)
      y = y + gfx.height(tiny) + 10

      local patterns = {}

      for _, name in ipairs(LOOK.texture_order) do
        patterns[#patterns + 1] = { text = name, key = "texture:" .. name, on = now == name,
                                    go = function() choose.texture(t, name) end }
      end

      y = chip.row(s, x, y, w, patterns)

      if tx then
        y = field_row(s, x, y, w, "Second colour",
                      { F(t, "colour2", nil, "Second colour", tx, "colour") })
        field_row(s, x, y, w, "Scale, bump", { F(t, "tscale", nil, "Texture scale", tx, "tscale"),
                                               F(t, "bump", nil, "Bump", tx) })
      end
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
                       { "Z", "shading" }, { "F", "frame" }, { "Home", "all" },
                       { "F11", FULL.on and "window" or "full screen" } }) do
    local kw = gfx.measure(k[1], tiny) + 10

    s:fill_round(x, y + 6, kw, FOOT - 12, theme.sunken, 4)
    s:frame_round(x, y + 6, kw, FOOT - 12, theme.line_soft, 4)
    s:text(x + 5, y + (FOOT - gfx.height(tiny)) // 2, k[1], theme.text, nil, tiny)
    x = x + kw + 5
    s:text(x, ty, k[2], theme.text_dim, nil, small)
    x = x + gfx.measure(k[2], small) + 14
  end

  -- Unsaved changes are said here, at the right, and not beside the name in
  -- the header: there it lengthened the title and moved every control after
  -- it the moment anything was changed, so Add was not where it had been.
  local right = ("%s%d objects   %s triangles   Selected %s"):format(
    FILE.changed and "Unsaved changes   " or "", #things, tostring(scene:triangles()),
    selected and selected.name or "none")

  s:text(W - 14 - gfx.measure(right, small), ty, right, theme.text_dim, nil, small)
end

--------------------------------------------------------------------------
-- The Script panel: drawn, opened, closed and run.
--------------------------------------------------------------------------

function SCRIPT.draw(s)
  if not SCRIPT.open then return end

  local x0, y0, w, h = VX + VW, HEAD, SCRIPT.W, H - HEAD - FOOT
  local strip = theme.mix(theme.window, theme.sunken, 500)

  s:fill(x0, y0, w, h, theme.sunken)
  s:fill(x0, y0, 1, h, theme.line_soft)

  -- The title strip: SCRIPT and its name, Open .lua... and Run.
  s:fill(x0 + 1, y0, w - 1, PANEL_T, strip)
  s:fill(x0 + 1, y0 + PANEL_T - 1, w - 1, 1, theme.line_soft)
  s:text(x0 + 12, y0 + (PANEL_T - 1 - gfx.height(tiny)) // 2, "SCRIPT",
         theme.text_dim, nil, tiny)

  local run_w = 10 + gfx.measure("Run") + 7 + gfx.measure("Ctrl Enter", small) + 10
  local run = control("script:run", x0 + w - 8 - run_w, y0 + (PANEL_T - 1 - 24) // 2, run_w, 24)

  s:fill_round(run.x, run.y, run.w, 24, theme.accent, 6)
  s:text(run.x + 10, run.y + (24 - gfx.height()) // 2, "Run", theme.text_on, nil, "ui")
  s:text(run.x + 10 + gfx.measure("Run") + 7, run.y + (24 - gfx.height(small)) // 2,
         "Ctrl Enter", theme.mix(theme.accent, theme.text_on, 600), nil, small)

  -- A script on its own, to share or keep (6d): Save .lua... beside Run,
  -- and Open .lua... before it.
  local bx = run.x - 6

  for _, b in ipairs({ { "save", "Save .lua..." }, { "open", "Open .lua..." } }) do
    local bw = gfx.measure(b[2]) + 20
    local c = control("script:" .. b[1], bx - bw, run.y, bw, 24)

    s:fill_round(c.x, c.y, bw, 24, theme.window, 6)
    s:frame_round(c.x, c.y, bw, 24, theme.line_soft, 6)
    s:text(c.x + 10, c.y + (24 - gfx.height()) // 2, b[2], theme.text, nil, "ui")
    bx = c.x - 6
  end

  -- The name in what is left, cut to fit it.
  local nx = x0 + 12 + gfx.measure("SCRIPT", tiny) + 8
  local name = ui.fitted(SCRIPT.name, bx - 8 - nx, "ui")

  s:text(nx, y0 + (PANEL_T - 1 - gfx.height()) // 2, name, theme.text, nil, "ui")

  -- The code: the IDE's editor, in this window's pixels.
  local lines = math.max(1, #SCRIPT.out)
  local oh = math.max(SCRIPT.OUT, 16 + lines * (gfx.height() + 3))
  local e = SCRIPT.editor

  e.w, e.h = w - 1, h - PANEL_T - oh
  e.focused = SCRIPT.focused
  control("script:code", x0 + 1, y0 + PANEL_T, e.w, e.h)
  ui.paint_view(e, s, x0 + 1, y0 + PANEL_T)

  -- Where the code and Run are, the first time it is drawn open, for
  -- whoever drives Cafesa3D from outside (`tools/run_script.py`).
  if not SCRIPT.told then
    SCRIPT.told = true
    local op, sv = controls["script:open"], controls["script:save"]

    print(("cafesa3d: script code %d,%d %dx%d; run %d,%d; open %d,%d; save %d,%d"):format(
          x0 + 1, y0 + PANEL_T, e.w, e.h, run.x + run.w // 2, run.y + run.h // 2,
          op.x + op.w // 2, op.y + op.h // 2, sv.x + sv.w // 2, sv.y + sv.h // 2))
  end

  -- What the last run printed, and how it went.
  local oy = y0 + PANEL_T + e.h

  s:fill(x0 + 1, oy, w - 1, oh, strip)
  s:fill(x0 + 1, oy, w - 1, 1, theme.line_soft)

  if #SCRIPT.out == 0 then
    s:text(x0 + 12, oy + 8, "Ctrl Enter runs it; what it prints is shown here.",
           theme.text_dim, nil, "ui")
  end

  for i, line in ipairs(SCRIPT.out) do
    s:text(x0 + 12, oy + 8 + (i - 1) * (gfx.height() + 3), line.text,
           line.colour or theme.text, nil, "ui")
  end
end

-- Opened or closed: the view made again at its new width - if the machine
-- will not give it, the panel stays as it was - and the Rendered view
-- started again at that size, as full screen does.
function SCRIPT.toggle()
  local open = not SCRIPT.open
  local v, why = k3.view(W - TOOLS - SIDE - (open and SCRIPT.W or 0), H - HEAD - FOOT)

  if not v then
    FILE.said = "No script panel: " .. tostring(why)
    print("cafesa3d: no script panel: " .. tostring(why))
    return true
  end

  shade.stop()

  if shade.surf then shade.surf:free() end

  shade.surf, shade.key = nil, nil
  view = v
  SCRIPT.open = open
  SCRIPT.focused = open
  SCRIPT.told = nil
  SCRIPT.kept = SCRIPT.kept or open

  if open and not SCRIPT.editor then
    SCRIPT.editor = ui.editor{ x = 0, y = 0, w = SCRIPT.W - 1, h = 100,
                               code = "lua", text = SCRIPT.text }
  end

  FULL.fit(W, H)
  print(("cafesa3d: script panel %s, the view %d by %d"):format(
        open and "open" or "closed", VW, VH))
  return true
end

-- The characters, decoded, to the script's editor while it holds the
-- keyboard: Ctrl Enter runs, and the rest are the editor's. Every character
-- goes through the decoder whoever holds the keyboard, so it never keeps
-- half a sequence from before; Escape is the raw key's (`rawkey`). A
-- function of its own, so its locals are not the main chunk's, which is at
-- two hundred.
function SCRIPT.key(ev)
  local a, b = SCRIPT.decode(ev.code)

  if not (SCRIPT.open and SCRIPT.focused) then return false end

  for _, c in ipairs({ a, b }) do
    local k, mods = ui.keyparts(c)

    if k == 13 and mods == ui.CTRL then
      SCRIPT.run()
    elseif c ~= 27 then
      local before = SCRIPT.editor.version

      SCRIPT.editor:key(c)

      -- The script is the scene's, so an edit to it is a change to save.
      if SCRIPT.editor.version ~= before then FILE.changed, FILE.said = true, nil end
    end
  end

  return a ~= nil
end

-- A line for the strip under the code, and the same in the log.
function SCRIPT.say(text, colour)
  SCRIPT.out[#SCRIPT.out + 1] = { text = text, colour = colour }
  print("cafesa3d: script " .. text)
end

-- The panel's text as it is now.
function SCRIPT.current()
  return SCRIPT.editor and SCRIPT.editor:content() or SCRIPT.text
end

-- What the scene's file keeps of its script (6d), or nil when it has none.
function SCRIPT.saved_form()
  return SCRIPT.kept and { name = SCRIPT.name, text = SCRIPT.current() } or nil
end

-- A script given to the panel - a scene's, a file's, or the sample - with
-- nothing to undo back into the one before it, and nothing said of it yet.
function SCRIPT.adopt(name, text, kept)
  SCRIPT.name, SCRIPT.text, SCRIPT.kept, SCRIPT.out, SCRIPT.path = name, text, kept, {}, nil

  if SCRIPT.editor then SCRIPT.editor:set(text) end
end

-- A scene opened: its script, or the sample when it came with none - which
-- the scene then has only if the panel is open over it.
function SCRIPT.opened(script)
  if not script then return SCRIPT.adopt("staircase", SCRIPT.SAMPLE, SCRIPT.open) end

  SCRIPT.adopt(script.name, script.text, true)
  print(("cafesa3d: script %s came with the scene, %d lines"):format(script.name,
        select(2, script.text:gsub("[^\n]*\n", "")) + (script.text:match("[^\n]$") and 1 or 0)))
end

-- A file's name without its `.lua`, as a script's name.
function SCRIPT.named(path)
  local name = FILE.base(path):gsub("%.[Ll][Uu][Aa]$", "")

  return name ~= "" and name or "script"
end

-- **Open .lua...**: a script on its own into the panel, named after its
-- file. The scene's script is then that one, a change to save.
function SCRIPT.open_file()
  local start = SCRIPT.path and FILE.dir(SCRIPT.path)
                or (fs.getattr(FILE.DIR) and FILE.DIR) or "/Home"

  return FILE.panel("open", {
    title = "Open a script", start = start,
    filter = function(name) return FILE.ext(name) == "lua" end,
    on_choose = function(path)
      local text, why = fs.read(path)

      if type(text) ~= "string" or #text > (1 << 20) then
        SCRIPT.say(("could not open %s: %s"):format(path, tostring(why or "not a script")),
                   theme.bad)
        return
      end

      SCRIPT.adopt(SCRIPT.named(path), text, true)
      SCRIPT.path = path
      SCRIPT.focused = true
      FILE.changed, FILE.said = true, nil
      SCRIPT.say(("opened %s, %d lines"):format(path, #SCRIPT.editor.buf.lines))
    end,
  })
end

-- **Save .lua...**: the panel's text as a file of its own. Saved under
-- another name the script is called that from now on, and what it made is
-- still its own - so its next Run replaces it.
function SCRIPT.save_file()
  local start = SCRIPT.path and FILE.dir(SCRIPT.path) or FILE.DIR

  if start == FILE.DIR then fs.send(FILE.DIR, { type = "mkdir" }) end

  return FILE.panel("save", {
    title = "Save the script", start = start, name = SCRIPT.name .. ".lua",
    on_choose = function(path)
      if not path:lower():match("%.lua$") then path = path .. ".lua" end

      local text = SCRIPT.current()
      local done, why = fs.write(path, text)

      if not done then
        SCRIPT.say("not saved: " .. tostring(why), theme.bad)
        return
      end

      local name = SCRIPT.named(path)

      if name ~= SCRIPT.name then
        for _, t in ipairs(things) do
          if t.by == SCRIPT.name then t.by = name end
        end

        SCRIPT.name, FILE.changed, FILE.said = name, true, nil
      end

      SCRIPT.path = path
      SCRIPT.say(("saved %s, %d bytes"):format(path, #text), theme.good)
    end,
  })
end

-- The instructions a run may take before it is stopped: a few seconds'
-- worth under emulation, and far more than any scene needs.
SCRIPT.BUDGET = 200000000

--
-- **Run**: the script in a Lua environment of its own, made fresh for each
-- run, holding Lua's `math`, `string` and `table` and a `print` whose lines
-- go to the strip - and, from 6c, the scene. No `fs`, no `sys`, no `use`:
-- what a script was not handed it cannot reach. In a coroutine with a
-- budget of instructions (`sys.budget`), so a loop that never ends is
-- stopped rather than taking Cafesa3D with it. An error comes back with its
-- line, which is marked in the code.
--
function SCRIPT.run()
  local e = SCRIPT.editor

  SCRIPT.out = {}
  e:clear_marks()

  local printed = {}
  local wants = { things = {} }
  local env = {
    scene = SCRIPT.scene(wants),
    math = math, string = string, table = table,
    ipairs = ipairs, pairs = pairs, next = next, select = select,
    tostring = tostring, tonumber = tonumber, type = type,
    error = error, pcall = pcall, assert = assert,
    print = function(...)
      local parts = {}

      for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end

      printed[#printed + 1] = table.concat(parts, "  ")
    end,
  }

  local chunk, err = load(e:content(), "=" .. SCRIPT.name, "t", env)
  local ok, why = chunk ~= nil, err
  local t0 = sys.ticks()

  if chunk then
    local co = coroutine.create(chunk)

    sys.budget(co, SCRIPT.BUDGET)
    ok, why = coroutine.resume(co)
  end

  for _, line in ipairs(printed) do SCRIPT.say(line) end

  if ok then
    local shapes, lamps_made, replaced = SCRIPT.make(wants)

    SCRIPT.say(("ran in %.2f s: %d object%s%s%s"):format((sys.ticks() - t0) / HZ,
               shapes, shapes == 1 and "" or "s",
               lamps_made > 0 and (" and %d lamp%s"):format(lamps_made, lamps_made == 1 and "" or "s") or "",
               replaced > 0 and (", replacing the %d it made last time"):format(replaced) or ""),
               theme.good)
  else
    local line, text = tostring(why):match(":(%d+): (.*)$")

    if line then
      e:mark(tonumber(line), "error")
      e:go_to(tonumber(line), 1)
      SCRIPT.say(("line %s: %s - nothing was made"):format(line, text), theme.bad)
    else
      SCRIPT.say(tostring(why) .. " - nothing was made", theme.bad)
    end
  end

  return true
end

local function draw_all()
  local s = win:surface()

  draw_header(s)
  draw_tools(s)
  draw_view(s)
  SCRIPT.draw(s)
  draw_outliner(s)
  draw_props(s)
  KEYS.draw(s)
  draw_foot(s)
  return win:commit{ x = 0, y = 0, w = W, h = H }
end

--------------------------------------------------------------------------
-- The Render window: F12, as Blender's. The scene through its camera, in
-- a window of its own, getting clearer the longer it runs - drawn as
-- `docs/cafesa3d.html` has it, the picture on the left and what it is
-- doing on the right.
--------------------------------------------------------------------------

-- `win`, `job` and `pic` while it is open; `start`, `done` and `shown` of
-- the render in it; `controls`, where its buttons are.
-- `win`, `job` and `pic` while it is open - `pic` at the render's own size,
-- and shown at `dw` by `dh` when that is more than the screen has room for;
-- `start`, `done` and `shown` of the render in it, `took` once it has
-- finished, and `stopped` - its passes, rays and threads - once somebody
-- stopped it, since a stopped job is gone; `controls`, where its buttons are.
local final = { side = 270, shown = -1, controls = {} }

-- The window's size for the render's: the picture as it is, or shrunk to
-- what the screen has room for beside the panel, and never shorter than
-- the panel needs.
function final.fit()
  local screen_now = fs.read("/Devices/screen") or {}
  local room_w = math.max(320, (screen_now.width or 1920) - final.side - 80)
  local room_h = math.max(180, (screen_now.height or 1080) - L.head - 160)
  local k = math.min(1, room_w / RENDER.w, room_h / RENDER.h)

  final.dw = math.max(1, math.floor(RENDER.w * k))
  final.dh = math.max(1, math.floor(RENDER.h * k))
  final.W, final.H = final.dw + final.side, L.head + math.max(final.dh, 360)
end

function final.camera()
  for _, t in ipairs(things) do
    if t.kind == "camera" and not t.hidden then return t end
  end
end

-- Still working on it: begun, not finished, and not stopped.
function final.running()
  return final.job ~= nil and not final.done
end

function final.draw()
  local s = final.win:surface()
  local job, kept = final.job, final.stopped
  local save_w = pk.button_width("Save as PNG...")
  local again_w = math.max(pk.button_width("Render again"), pk.button_width("Stop"))
  local save_x = final.W - L.lights_in - ((final.win.lights and final.win.lights.w) or 62)
                 - L.head_edge - save_w
  local again_x = save_x - 8 - again_w
  local pw, ph = final.pic:size()
  local passes = job and job:passes() or (kept and kept.passes) or 0

  -- One button in one place: Stop while it renders, Render again after -
  -- so the header does not move under the pointer as it changes.
  pk.header(s, 0, 0, final.W, "Render", ("%s \u{b7} Camera \u{b7} %d \u{d7} %d"):format(
    FILE.name, pw, ph), again_x - 8)
  final.controls.again = { x = again_x, y = pk.centre(31), w = again_w, h = 31 }
  final.controls.save = { x = save_x, y = pk.centre(31), w = save_w, h = 31 }
  pk.button(s, { x = again_x, y = pk.centre(31), w = again_w,
                 text = final.running() and "Stop" or "Render again" })
  pk.button(s, { x = save_x, y = pk.centre(31), text = "Save as PNG...",
                 disabled = passes == 0 or nil })

  -- The picture: at its own size, or shrunk to the window's, smoothly.
  s:fill(0, L.head, final.dw, final.H - L.head, 0xff1d1f24)

  if final.dw == pw and final.dh == ph then
    s:blit(final.pic, 0, 0, pw, ph, 0, L.head)
  else
    s:stretch(final.pic, 0, 0, pw, ph, 0, L.head, final.dw, final.dh, nil, true)
  end

  -- What it is doing.
  local x, y, w = final.dw + 16, L.head + 14, final.side - 32
  local secs = final.took or (sys.ticks() - final.start) / HZ
  local rays = job and job:rays() or (kept and kept.rays) or 0
  local want = final.samples or RENDER.samples

  s:fill(final.dw, L.head, final.side, final.H - L.head, theme.window)
  s:fill(final.dw, L.head, 1, final.H - L.head, theme.line_soft)
  s:text(x, y, kept and "Samples \u{b7} stopped" or "Samples", theme.text_dim, nil, "ui")
  y = y + gfx.height() + 4
  s:text(x, y, ("%d / %d"):format(passes, want), theme.text, nil, "title")
  y = y + gfx.height("title") + 8
  s:fill_round(x, y, w, 6, theme.sunken, 3)

  if passes > 0 then
    s:fill_round(x, y, math.max(6, w * passes // want), 6, theme.accent, 3)
  end

  y = y + 14

  if secs > 0 then
    s:text(x, y, ("%.1f s, %d rays a sample"):format(secs, passes > 0 and
      rays // (passes * pw * ph) or 0), theme.text_dim, nil, small)
    s:text(x, y + gfx.height(small) + 2, ("%.1f million rays a second"):format(
      rays / secs / 1e6), theme.text_dim, nil, small)
  end

  y = y + 2 * (gfx.height(small) + 2) + 20
  s:text(x, y, "LIGHT PATHS", theme.text_dim, nil, tiny)
  y = y + gfx.height(tiny) + 8

  for _, row in ipairs({ { "Integrator", final.preview and "Preview \u{b7} Whitted's"
                                                           or "Final \u{b7} path tracing" },
                         { "Bounces", ("up to %d"):format(final.bounces or RENDER.bounces) },
                         { "Cores", ("%d thread%s, a tile each"):format(final.threads or 0,
                                   final.threads == 1 and "" or "s") },
                         { "Denoise", "not yet" } }) do
    s:text(x, y, row[1], theme.text_dim, nil, "ui")
    s:text(x + 96, y, row[2], theme.text, nil, "ui")
    y = y + gfx.height() + 8
  end

  final.shown = passes
  return final.win:commit{ x = 0, y = 0, w = final.W, h = final.H }
end

function final.begin()
  if final.job then
    final.job:stop()
    final.job = nil
  end

  local cam = final.camera()
  local job, why

  if not cam then
    print("cafesa3d: there is no camera to render through")
    return false
  end

  -- The picture at the render's size, made again when that has changed.
  local pw, ph = 0, 0

  if final.pic then pw, ph = final.pic:size() end

  if pw ~= RENDER.w or ph ~= RENDER.h then
    if final.pic then final.pic:free() end

    local made

    made, final.pic = pcall(gfx.surface, { w = RENDER.w, h = RENDER.h })

    if not made then
      final.pic = nil
      print(("cafesa3d: no memory for a %d by %d picture"):format(RENDER.w, RENDER.h))
      return false
    end
  end

  -- What this render was asked for, kept: the tab may change under it.
  final.samples, final.bounces, final.preview = RENDER.samples, RENDER.bounces, RENDER.preview

  job, why = k3.render(scene, { w = RENDER.w, h = RENDER.h, eye = cam.loc, target = cam.target,
                                fov = 2 * math.atan(18 / (cam.focal or 50)),
                                passes = RENDER.samples, bounces = RENDER.bounces,
                                preview = RENDER.preview, world = world, lights = lamps() })

  if not job then
    print("cafesa3d: " .. tostring(why))
    return false
  end

  final.job, final.threads = job, job:workers()
  final.pic:fill(0, 0, RENDER.w, RENDER.h, 0xff1d1f24)
  final.start, final.done, final.shown, final.first = sys.ticks(), false, -1, false
  final.took, final.stopped = nil, nil
  print(("cafesa3d: rendering %s through the camera, %d by %d, on %d thread%s"):format(
    FILE.name, RENDER.w, RENDER.h, job:workers(), job:workers() == 1 and "" or "s"))
  return true
end

-- Stopped by the person watching it, and kept: every tile's samples so far
-- painted into the picture - which Save as PNG still saves - and the
-- numbers read, before the job goes, since a stopped job is freed.
function final.stop()
  local job = final.job

  if not final.running() then return false end

  job:paint(final.pic, 0, 0, true)
  final.stopped = { passes = job:passes(), rays = job:rays() }
  final.took = (sys.ticks() - final.start) / HZ
  job:stop()
  final.job = nil
  print(("cafesa3d: stopped the render at %d of %d samples, after %.1f s"):format(
    final.stopped.passes, final.samples, final.took))
  final.draw()
  return true
end

function final.close()
  if final.job then
    final.job:stop()
    final.job = nil
  end

  if final.win then
    final.win:close()
    final.win = nil
  end
end

function final.open()
  if not final.camera() then
    print("cafesa3d: there is no camera to render through")
    return false
  end

  -- A window that draws its own pixels cannot be resized, so a render of
  -- another size is another window.
  local was_w, was_h = final.W, final.H

  final.fit()

  if final.win and (final.W ~= was_w or final.H ~= was_h) then
    final.win:close()
    final.win = nil
  end

  if not final.win then
    local w = ui.window{ title = "Render", w = final.W, h = final.H, direct = true, header = true,
                         x = (win.origin_x or 0) + 60, y = (win.origin_y or 0) + 80 }

    if not w or not w:surface() then
      print("cafesa3d: no window to render into")
      return false
    end

    final.win = w
    print(("cafesa3d: render window at %d,%d"):format(w.origin_x or 0, w.origin_y or 0))
  end

  if final.begin() then
    final.draw()

    -- Where its two buttons are, for whoever drives Cafesa3D from outside.
    local a, v = final.controls.again, final.controls.save

    print(("cafesa3d: render controls again %d,%d; save %d,%d"):format(
      a.x + a.w // 2, a.y + a.h // 2, v.x + v.w // 2, v.y + v.h // 2))
  end

  return true
end

-- The Render window's own events, and its picture as it clears.
function final.tend()
  if not final.win then return end

  local reply = wmproto.poll(final.win.handle, 0)

  if not reply then
    final.close()
    return
  end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      final.close()
      print("cafesa3d: render window closed")
      return
    elseif ev.type == "mouse" and ev.action == "press"
           and pk.inside(final.controls.again, ev.x, ev.y) then
      -- Stop while it renders; after, opened again rather than begun, in
      -- case the size was changed.
      if final.running() then
        final.stop()
      else
        final.open()
      end

      return
    elseif ev.type == "rawkey" and ev.down and ev.code == 1 then
      final.stop()                -- Esc, as Blender's render window has it
    elseif ev.type == "mouse" and ev.action == "press"
           and pk.inside(final.controls.save, ev.x, ev.y)
           and ((final.job and final.job:passes() > 0)
                or (final.stopped and final.stopped.passes > 0)) then
      final.save()
    elseif ev.type == "mouse" and ev.action == "press" and ev.y < L.head then
      -- The header's empty part, which is the title bar.
      final.win:take_hold(ev.x, ev.y)
    end
  end

  local job = final.job

  if not job then return end

  local painted = job:paint(final.pic, 0, 0)
  local passes = job:passes()

  if painted > 0 or passes ~= final.shown then final.draw() end

  if passes >= 1 and not final.first then
    final.first = true
    print(("cafesa3d: the render's first pass in %.1f s"):format((sys.ticks() - final.start) / HZ))
  end

  if passes >= final.samples and not final.done then
    final.done = true
    final.took = (sys.ticks() - final.start) / HZ
    print(("cafesa3d: rendered %d samples, %d rays in %.1f s on %d threads"):format(
      passes, job:rays(), final.took, job:workers()))
    final.draw()                  -- the button, Render again now

    --
    -- **And said**, for whoever is looking at another window while it
    -- works (`roadmap.md`, *Notifications*; Diego: "cafesa render
    -- finalized"). Asked of the notification server, which answers at
    -- once; a machine without one renders all the same.
    --
    local took = final.took < 60 and ("%.1f s"):format(final.took)
                 or ("%d min %d s"):format(final.took // 60, math.floor(final.took % 60))

    pcall(function()
      use("/Kosmos/Libraries/notify.lua").post{
        title = "Render finished",
        body = ("%s, %d by %d, %d samples, in %s."):format(FILE.name, RENDER.w, RENDER.h,
                                                            passes, took) }
    end)
  end
end

-- The picture as it stands, at its own size, as a PNG in /Home/Documents -
-- with the scenes, where a person keeps what they made (`roadmap.md` 6w);
-- it was /Home/Renders.
function final.save()
  local dir = "/Home/Documents"

  fs.send(dir, { type = "mkdir" })
  FILE.panel("save", {
    title = "Save the render", start = dir, name = FILE.name:gsub("%.[%w]+$", "") .. ".png",
    on_choose = function(path)
      if FILE.ext(path) ~= "png" then path = path .. ".png" end

      local ok, bytes = pcall(gfx.encode_png, final.pic)
      local done, why = ok, bytes

      if ok then done, why = fs.write(path, bytes) end

      local pw, ph = final.pic:size()

      print(done and ("cafesa3d: saved the render to %s, %d by %d, %d bytes"):format(
                       path, pw, ph, #bytes)
            or ("cafesa3d: could not save the render to %s: %s"):format(path, tostring(why)))
    end,
  })
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

  if next(chip.actions) then
    local cs = {}

    for key in pairs(chip.actions) do
      local c = controls[key]

      cs[#cs + 1] = ("%s %d,%d"):format(key, c.x + c.w // 2, c.y + c.h // 2)
    end

    table.sort(cs)
    print("cafesa3d: chips " .. table.concat(cs, "; "))
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

-- **F: the selected object, filling the view**, from the side the view
-- already looks at it - Blender's View Selected (Diego, 26 September). By
-- its triangles where they are in the world, so a mesh from a file is
-- framed by its real size rather than a guess; a lamp or a camera, which
-- has none, by a metre round where it stands. Nothing selected is Home.
local function frame_selected()
  local t = selected

  if not t then
    frame_all()
    return true
  end

  local lo, hi

  if t.id then
    local ok, points = pcall(scene.world_triangles, scene, t.id)

    if ok and type(points) == "string" and #points >= 12 then
      local x0, y0, z0, x1, y1, z1 = k3.bounds(points)

      lo, hi = { x0, y0, z0 }, { x1, y1, z1 }
    end
  end

  if not lo then
    lo = { t.loc[1] - 0.5, t.loc[2] - 0.5, t.loc[3] - 0.5 }
    hi = { t.loc[1] + 0.5, t.loc[2] + 0.5, t.loc[3] + 0.5 }
  end

  orbit.target = { (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2, (lo[3] + hi[3]) / 2 }

  -- The sphere round it inside the narrower of the view's two angles:
  -- FOV is across, and a view wider than tall is narrower up and down.
  local r = math.max(0.05, math.sqrt((hi[1] - lo[1]) ^ 2 + (hi[2] - lo[2]) ^ 2
                                     + (hi[3] - lo[3]) ^ 2) / 2)
  local half = math.min(FOV / 2, math.atan(math.tan(FOV / 2) * VH / VW))

  orbit.dist = math.max(0.25, r / math.sin(half) * 1.1)
  print(("cafesa3d: framed %s, %.2f m across, from %.2f m"):format(t.name, 2 * r, orbit.dist))
  return true
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

-- **A change kept for Undo**, and the scene marked unsaved with it: one
-- place for both, because a second copy - a move's - kept the step and not
-- the mark, and a moved object never showed "Unsaved changes" (the 0.11
-- review).
local function push_undo(snap)
  undo[#undo + 1] = snap
  FILE.changed, FILE.said = true, nil

  if #undo > 64 then table.remove(undo, 1) end

  redo = {}
end

-- Called before a change, with what it is called.
function will(label)          -- the `local will` declared above sync
  push_undo(snapshot(label))
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

--------------------------------------------------------------------------
-- **The scene a script is handed** (step 6c): `scene.box{...}` and the
-- other shapes, `scene.light`, `scene.camera`, `scene.world` and
-- `scene.find` - the calls the Properties tabs already make, by the
-- Properties tabs' names, and every number held to what they hold it to.
-- A wrong name or value is refused with a sentence, on the script's line:
-- "the texture Wod is not one of None, Checker, Brick, Tiles, Noise, Wood,
-- Marble". Colours are 0xrrggbb, as the file keeps them.
--
-- **Nothing is made while the script runs.** Each call writes down what it
-- wants; the scene changes only when the script has finished, all of it at
-- once and as one undo step - so a mistake anywhere leaves the scene as it
-- was, and Ctrl Z takes back a whole run.
--
-- In a block of its own, so the helpers are not the main chunk's locals,
-- which is at Lua's two hundred.
--------------------------------------------------------------------------

do
-- A refusal, raised on the line of the script that made the call.
local function refuse(text) error({ refused = text }, 0) end

local function script_number(v, what, spec)
  if type(v) ~= "number" then
    refuse(("%s is %s, where a number is wanted"):format(what, type(v)))
  end

  local lo, hi = spec and spec.lo, spec and spec.hi

  if (lo and v < lo) or (hi and v > hi) then
    refuse(("%s is %s, and it is from %s to %s"):format(what, fmt(v, 3),
           lo and fmt(lo, 3) or "anything", hi and fmt(hi, 3) or "anything"))
  end

  if spec and spec.int then v = math.floor(v + 0.5) end

  return v
end

local function script_colour(v, what)
  if type(v) ~= "number" or v < 0 or v > 0xffffff or v % 1 ~= 0 then
    refuse(("%s is not a colour: they are written 0xrrggbb"):format(what))
  end

  return v
end

-- Three numbers, or one for all three where that means something - a
-- scale of 2 is 2 every way.
local function script_three(v, what, spec, one_is_all)
  if type(v) == "number" and one_is_all then v = { v, v, v } end

  if type(v) ~= "table" or #v ~= 3 then
    refuse(("%s is three numbers, { x, y, z }"):format(what))
  end

  return { script_number(v[1], what .. "'s x", spec), script_number(v[2], what .. "'s y", spec),
           script_number(v[3], what .. "'s z", spec) }
end

local function script_one_of(v, what, list)
  for _, name in ipairs(list) do
    if v == name then return v end
  end

  refuse(("%s %s is not one of %s"):format(what, tostring(v), table.concat(list, ", ")))
end

-- What each shape is made of, beyond where it is: the Data tab's fields.
local SCRIPT_SHAPES = {
  plane = { size = "number" }, box = { size = "box" },
  sphere = { radius = "number", segments = "number", rings = "number" },
  ico = { radius = "number", subdivisions = "number" },
  cylinder = { radius = "number", depth = "number", segments = "number" },
  cone = { radius = "number", radius2 = "number", depth = "number", segments = "number" },
  torus = { radius = "number", radius2 = "number", segments = "number", rings = "number" },
  grid = { size = "number", segments = "number", rings = "number" },
}

local SCRIPT_COMMON = { name = true, loc = true, rot = true, scale = true, smooth = true,
                        hidden = true, material = true }

local SCRIPT_MATERIAL = { preset = true, base = true, metallic = true, rough = true,
                          trans = true, ior = true, emit = true, texture = true,
                          colour2 = true, texture_scale = true, bump = true }

local function script_material(m)
  if type(m) ~= "table" then refuse("material is a table of the Material tab's fields") end

  for k in pairs(m) do
    if not SCRIPT_MATERIAL[k] then refuse(("a material has no %s"):format(tostring(k))) end
  end

  local preset = m.preset and script_one_of(m.preset, "the preset", PRESET_ORDER) or "Plastic"
  local mat = material(m.base and script_colour(m.base, "base") or 0xcccccc, preset)

  for _, k in ipairs({ "metallic", "rough", "trans", "ior", "emit" }) do
    if m[k] ~= nil then mat[k] = script_number(m[k], k, FIELD[k]) end
  end

  if m.texture ~= nil then
    local name = script_one_of(m.texture, "the texture", LOOK.texture_order)

    if name ~= "None" then
      local tx = {}

      for k, v in pairs(LOOK.textures[name]) do tx[k] = v end

      if m.colour2 ~= nil then tx.colour2 = script_colour(m.colour2, "colour2") end
      if m.texture_scale ~= nil then tx.scale = script_number(m.texture_scale, "texture_scale", FIELD.tscale) end
      if m.bump ~= nil then tx.bump = script_number(m.bump, "bump", FIELD.bump) end

      mat.texture = tx
    end
  end

  return mat
end

-- A shape, as a thing the scene will be given when the run is over.
local function script_shape(kind, spec)
  if type(spec) ~= "table" then refuse(("scene.%s takes a table: scene.%s{ ... }"):format(kind, kind)) end

  local own = SCRIPT_SHAPES[kind]

  for k in pairs(spec) do
    if not (own[k] or SCRIPT_COMMON[k]) then
      refuse(("the %s has no %s"):format(kind, tostring(k)))
    end
  end

  local defaults

  for _, a in ipairs(ADDABLE) do
    if a.kind == kind then defaults = a end
  end

  local t = { kind = kind, name = defaults.name, loc = { 0, 0, 0 }, rot = { 0, 0, 0 },
              scale = { 1, 1, 1 }, mat = material(0xcccccc, "Plastic") }

  for k, v in pairs(defaults.fields) do t[k] = copy(v) end

  if spec.name ~= nil then
    if type(spec.name) ~= "string" or spec.name == "" then refuse("a name is words") end
    t.name = spec.name
  end

  if spec.loc ~= nil then t.loc = script_three(spec.loc, "loc") end
  if spec.rot ~= nil then t.rot = script_three(spec.rot, "rot") end
  if spec.scale ~= nil then t.scale = script_three(spec.scale, "scale", FIELD.scale, true) end
  if spec.smooth ~= nil then t.smooth = spec.smooth and true or false end
  if spec.hidden ~= nil then t.hidden = spec.hidden and true or false end
  if spec.material ~= nil then t.mat = script_material(spec.material) end

  for k in pairs(own) do
    if spec[k] ~= nil then
      if own[k] == "box" then
        t.size = script_three(spec.size, "size", FIELD.size, true)
      else
        t[k] = script_number(spec[k], k, FIELD[k])
      end
    end
  end

  return t
end

-- The scene a script sees, writing down into `wants` what it asks for.
function SCRIPT.scene(wants)
  local api = {}

  local function call(fn)
    return function(...)
      local ok, got = pcall(fn, ...)

      if not ok then
        if type(got) == "table" and got.refused then error(got.refused, 2) end
        error(got, 2)
      end

      return got
    end
  end

  for kind in pairs(SCRIPT_SHAPES) do
    api[kind] = call(function(spec)
      local t = script_shape(kind, spec)

      wants.things[#wants.things + 1] = t
      return { name = t.name }
    end)
  end

  api.light = call(function(spec)
    if type(spec) ~= "table" then refuse("scene.light takes a table") end

    for k in pairs(spec) do
      if not ({ name = true, loc = true, power = true, radius = true, colour = true })[k] then
        refuse(("a lamp has no %s"):format(tostring(k)))
      end
    end

    local t = { kind = "light", name = spec.name or "Light",
                loc = spec.loc and script_three(spec.loc, "loc") or { 0, 0, 3 },
                power = spec.power and script_number(spec.power, "power", FIELD.power) or 1000,
                radius = spec.radius and script_number(spec.radius, "radius", FIELD.radius) or 0.1,
                colour = spec.colour and script_colour(spec.colour, "colour") or 0xffffff }

    wants.things[#wants.things + 1] = t
    return { name = t.name }
  end)

  api.camera = call(function(spec)
    if type(spec) ~= "table" then refuse("scene.camera takes a table") end

    local c = {}

    for k, v in pairs(spec) do
      if k == "loc" or k == "target" then
        c[k] = script_three(v, k)
      elseif k == "focal" then
        c.focal = script_number(v, "focal", FIELD.focal)
      else
        refuse(("the camera has no %s"):format(tostring(k)))
      end
    end

    wants.camera = c
  end)

  api.world = call(function(spec)
    if type(spec) ~= "table" then refuse("scene.world takes a table") end

    local w = {}

    for k, v in pairs(spec) do
      if k == "zenith" or k == "horizon" then
        w[k] = script_colour(v, k)
      elseif k == "strength" then
        w.strength = script_number(v, "strength", FIELD.strength)
      else
        refuse(("the world has no %s"):format(tostring(k)))
      end
    end

    wants.world = w
  end)

  -- An object already in the scene, to read: a copy of where it is.
  api.find = call(function(name)
    for _, t in ipairs(things) do
      if t.name == name then
        return { name = t.name, kind = t.kind, loc = copy(t.loc), rot = copy(t.rot or { 0, 0, 0 }),
                 scale = copy(t.scale or { 1, 1, 1 }) }
      end
    end

    return nil
  end)

  return api
end

-- What a run wanted, made: the last run's things taken out and these put
-- in, as one undo step. Returns how many it made and how many it replaced.
function SCRIPT.make(wants)
  will("ran " .. SCRIPT.name)

  local kept, replaced = {}, 0

  for _, t in ipairs(things) do
    if t.by == SCRIPT.name then
      if t.id then scene:remove(t.id) end
      if selected == t then selected = nil end
      replaced = replaced + 1
    else
      kept[#kept + 1] = t
    end
  end

  things = kept

  local shapes, lamps_made = 0, 0

  for _, t in ipairs(wants.things) do
    t.name = unique(t.name)
    t.by = SCRIPT.name
    add(t)

    if t.kind == "light" then lamps_made = lamps_made + 1 else shapes = shapes + 1 end
  end

  if wants.camera then
    for _, t in ipairs(things) do
      if t.kind == "camera" then
        for k, v in pairs(wants.camera) do t[k] = v end
      end
    end
  end

  if wants.world then
    for k, v in pairs(wants.world) do world[k] = v end
  end

  scene_version = scene_version + 1
  return shapes, lamps_made, replaced
end
end

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
    { text = "Import...", on_choose = function() FILE.import() end },
  })

  if m then
    print(("cafesa3d: add menu at %d,%d, %d wide, rows of %d"):format(m.x, m.y, m.w, m.row))
  end
end

--------------------------------------------------------------------------
-- Opening a scene: the samples the image carries (`roadmap.md` 4l), read
-- out of glTF by `/Kosmos/Libraries/scenefile.lua`, which trusts nothing in the file.
-- The whole scene is replaced - one undo step - and the view goes to where
-- the scene's camera stands.
--
-- One table rather than four locals: this chunk is at Lua's limit of two
-- hundred, and has been folded twice before for it.
--------------------------------------------------------------------------

local SAMPLES = { { "house", "House" }, { "car", "Car" }, { "plane", "Plane" } }

function SAMPLES.look_from(cam)
  local d = { cam.loc[1] - cam.target[1], cam.loc[2] - cam.target[2], cam.loc[3] - cam.target[3] }
  local n = math.sqrt(d[1] ^ 2 + d[2] ^ 2 + d[3] ^ 2)

  if n < 1e-6 then return end

  orbit.target = { cam.target[1], cam.target[2], cam.target[3] }
  orbit.dist = n
  orbit.az = math.atan(d[2], d[1])
  orbit.el = math.max(-1.5, math.min(1.5, math.asin(d[3] / n)))
  view_name = VIEW_NAME
end

-- A glTF file's bytes - its text, or a binary `.glb` - read as a scene,
-- every mesh's points and triangles packed as the kit takes them: points
-- three floats apart wherever the file interleaved them, triangles listed
-- where it left them implied. `dir` is where a buffer kept in a file
-- beside the scene is read from. nil and why if it is not a scene.
function SAMPLES.read(bytes, dir)
  local doc, why, bin

  if type(bytes) == "string" and bytes:sub(1, 4) == "glTF" then
    local text

    text, bin = scenefile.from_glb(bytes)

    if text then doc, why = json.decode(text) else why, bin = bin, nil end
  else
    doc, why = json.decode(bytes or "")
  end

  local loaded

  if doc then loaded, why = scenefile.from_gltf(doc, bin ~= nil) end
  if not loaded then return nil, why end

  -- Each buffer decoded once, in C, and each mesh given its slices of it:
  -- strings, so undo and Shift D copy them for nothing.
  local buffers, missing = {}, {}

  for i, b in pairs(loaded.buffers or {}) do
    if b.base64 then
      buffers[i] = compress.unbase64(b.base64)
    elseif b.bin then
      buffers[i] = bin
    elseif b.file and dir then
      local got = fs.read(dir .. "/" .. b.file)

      buffers[i] = type(got) == "string" and got or nil
    end

    if not buffers[i] then missing[i] = b.file or "the file's own" end
  end

  local kept = {}

  for _, t in ipairs(loaded.things) do
    local m = t.mesh
    local lost

    if m then
      local pb, ib = buffers[m.buffer], buffers[m.index_buffer or m.buffer]

      if not pb or (m.indices and not ib) then
        lost = ("its buffer %s could not be read"):format(missing[m.buffer]
                                                          or missing[m.index_buffer] or "")
      else
        if m.point_stride == 12 then
          t.vertices = pb:sub(m.point_at + 1, m.point_at + m.points * 12)
        else
          t.vertices, lost = k3.gather(pb, m.point_at, m.points, m.point_stride, 12)
        end

        if m.indices then
          t.triangles = ib:sub(m.index_at + 1, m.index_at + m.indices * m.index_bytes)
          t.index_bytes = m.index_bytes
        else
          t.triangles, t.index_bytes = k3.sequence(m.points), 4
        end

        t.mesh = nil
      end
    end

    if lost then
      loaded.skipped = loaded.skipped + 1
      loaded.why[#loaded.why + 1] = ("%s: %s"):format(t.name, lost)
    else
      kept[#kept + 1] = t
    end
  end

  loaded.things = kept
  return loaded
end

-- What was read, into the scene: in place of it when opening, beside it
-- when importing - where a file's camera does not move ours, its sky does
-- not change ours, and every name is made one nothing else has.
function SAMPLES.take(loaded, file, adding)
  will((adding and "imported " or "opened ") .. loaded.name)

  if not adding then
    for _, t in ipairs(things) do
      if t.id then scene:remove(t.id) end
    end

    things = {}
  end

  local camera, added, first = nil, 0, nil

  -- What the reader passed and the kit still refuses - a mesh whose faces
  -- name points it has not got, which a mangled file is - is skipped and
  -- said, as the reader's own refusals are. It took Cafesa3D down, and with
  -- it whatever else was open, found by the suite's control on base64.
  for _, t in ipairs(loaded.things) do
    if adding and t.kind == "camera" then
      loaded.skipped = loaded.skipped + 1
      loaded.why[#loaded.why + 1] = t.name .. ": a camera, and the scene has one"
    else
      if adding then t.name = unique(t.name) end

      local ok, why = pcall(add, t)

      if ok then
        added = added + 1
        first = first or t
        if t.kind == "camera" and not camera then camera = t end
      else
        loaded.skipped = loaded.skipped + 1
        loaded.why[#loaded.why + 1] = ("%s: %s"):format(t.name, tostring(why):gsub("^.-: ", ""))
      end
    end
  end

  if adding then
    selected = first or selected
    print(("cafesa3d: imported %s, %d objects, %d triangles, %d skipped"):format(
      loaded.name, added, scene:triangles(), loaded.skipped))
  else
    if loaded.world then
      world.zenith = loaded.world.zenith or world.zenith
      world.horizon = loaded.world.horizon or world.horizon
      world.strength = loaded.world.strength or world.strength
    end

    for k, v in pairs(loaded.render or {}) do RENDER[k] = v end

    SCRIPT.opened(loaded.script)

    selected = nil
    FILE.name, FILE.title, FILE.path, FILE.changed, FILE.said = file, loaded.name, nil, false, nil
    cursor3d = { 0, 0, 0 }

    if camera then SAMPLES.look_from(camera) end

    print(("cafesa3d: opened %s, %d objects, %d triangles, %d skipped"):format(
      loaded.name, #things, scene:triangles(), loaded.skipped))
  end

  for _, w in ipairs(loaded.why) do print("cafesa3d:   skipped " .. w) end

  return true
end

function SAMPLES.open_scene(bytes, file, dir)
  local loaded, why = SAMPLES.read(bytes, dir)

  if not loaded then
    print(("cafesa3d: could not open %s: %s"):format(file, tostring(why)))
    return false
  end

  return SAMPLES.take(loaded, file)
end

function SAMPLES.open(file, name)
  local bytes = sys.asset("scenes/" .. file .. ".gltf")

  if not bytes then
    print("cafesa3d: this image carries no sample called " .. name)
    return
  end

  SAMPLES.open_scene(bytes, file .. ".gltf")
end

--------------------------------------------------------------------------
-- Saving, and opening what was saved (`roadmap.md` 4l, step 5). glTF, as
-- the samples are, written by `/Kosmos/Libraries/scenefile.lua` - so the reader that
-- opens a sample opens a saved scene, and any other program's glTF reader
-- opens it too. Through the Open and Save panel every application has,
-- into /Home/Documents unless the scene came from somewhere else - a place
-- in Tracker's sidebar since 28 September (`roadmap.md` 6w), where it was
-- /Home/Scenes.
--------------------------------------------------------------------------

FILE.DIR = "/Home/Documents"

function FILE.base(path) return path:match("([^/]+)$") or path end
function FILE.dir(path) return path:match("^(.*)/[^/]*$") end

function FILE.write(path)
  local ok, text = pcall(function()
    return json.encode(scenefile.to_gltf({ name = FILE.title, things = things, world = world,
                                           render = RENDER, script = SCRIPT.saved_form() },
                                         { base64 = compress.base64, bounds = k3.bounds }))
  end)
  local done, why = false, text

  if ok then done, why = fs.write(path, text) end

  if not done then
    FILE.said = "Not saved: " .. tostring(why)
    print(("cafesa3d: could not save %s: %s"):format(path, tostring(why)))
    return false
  end

  FILE.path, FILE.name, FILE.changed = path, FILE.base(path), false
  FILE.said = "Saved " .. FILE.name
  print(("cafesa3d: saved %s, %d objects, %d bytes"):format(path, #things, #text))
  return true
end

function FILE.save()
  if FILE.path then return FILE.write(FILE.path) end

  return FILE.save_as()
end

-- The panel runs until it is closed, as a dialog does; this window keeps
-- its last picture meanwhile. Where it opened is said, for whoever drives
-- Cafesa3D from outside (`tools/run_cafesa3d.py`).
function FILE.panel(kind, spec)
  local chooser = use("/Kosmos/Libraries/panel.lua")[kind](spec)

  if not chooser then return false end

  -- Where what the panel draws begins, under the header the kit gives it
  -- (one window chrome, 7 October), for the harness that clicks in it.
  print(("cafesa3d: %s panel at %d,%d"):format(kind, chooser.origin_x or 0,
                                               (chooser.origin_y or 0)
                                               + (chooser.head_h or 0)))
  chooser:run()
  return true
end

function FILE.save_as()
  local start = FILE.path and FILE.dir(FILE.path) or FILE.DIR

  if start == FILE.DIR then fs.send(FILE.DIR, { type = "mkdir" }) end

  return FILE.panel("save", {
    title = "Save the scene", start = start, name = FILE.name:gsub("%.[%w]+$", "") .. ".gltf",
    on_choose = function(path)
      if not path:lower():match("%.gltf$") then path = path .. ".gltf" end

      FILE.write(path)
    end,
  })
end

function FILE.open()
  local start = FILE.path and FILE.dir(FILE.path)
                or (fs.getattr(FILE.DIR) and FILE.DIR) or "/Home"

  return FILE.panel("open", {
    title = "Open a scene", start = start,
    filter = function(name) return FILE.ext(name) == "gltf" or FILE.ext(name) == "glb" end,
    on_choose = function(path)
      local bytes, why = fs.read(path)

      if type(bytes) ~= "string" then
        FILE.said = "Not opened: " .. tostring(why or "not a file")
        print(("cafesa3d: could not open %s: %s"):format(path, tostring(why)))
        return
      end

      -- A `.glb` is somebody else's file: opened, it is read, and saving
      -- it writes a `.gltf` of Cafesa3D's own rather than over theirs.
      if SAMPLES.open_scene(bytes, FILE.base(path), FILE.dir(path))
         and FILE.ext(path) == "gltf" then
        FILE.path = path
      end
    end,
  })
end

function FILE.ext(name) return (name:match("%.(%w+)$") or ""):lower() end

--------------------------------------------------------------------------
-- Other programs' formats: translators (`roadmap.md` 4l, 5c), as BeOS's
-- Translation Kit had them - one Lua file a format, in /Kosmos/Libraries/translators/
-- and in /Home/Translators for those a person adds, each saying what it
-- reads and writes. Found once, the first time one is wanted. Each is
-- handed the 3D Kit's readers and writers, a way to make a material, and
-- glTF's arithmetic for a world matrix and a linear colour - and nothing of
-- the file system: Cafesa3D reads the bytes it translates and writes what
-- comes back.
--------------------------------------------------------------------------

FILE.KIT = { read_stl = k3.read_stl, read_obj = k3.read_obj, read_fbx = k3.read_fbx,
             write_stl = k3.write_stl, write_obj = k3.write_obj }

function FILE.translators()
  if FILE.found then return FILE.found end

  FILE.found = {}

  local where = {}

  -- A folder of the libraries since 27 September (`roadmap.md` 6s c3b):
  -- the store was listed flat, and a translator was a name that began
  -- `translators/`.
  for _, name in ipairs(fs.list("/Kosmos/Libraries/translators") or {}) do
    if name:match("^[%w_%-]+%.lua$") then
      where[#where + 1] = "/Kosmos/Libraries/translators/" .. name
    end
  end

  for _, name in ipairs(fs.list("/Home/Translators") or {}) do
    if name:match("^[%w_%-]+%.lua$") then where[#where + 1] = "/Home/Translators/" .. name end
  end

  for _, path in ipairs(where) do
    local ok, t = pcall(use, path)

    if ok and type(t) == "table" and type(t.name) == "string"
       and (type(t.read) == "function" or type(t.write) == "function") then
      t.reads = type(t.reads) == "table" and t.reads or {}
      t.writes = type(t.writes) == "table" and t.writes or {}
      FILE.found[#FILE.found + 1] = t
      print(("cafesa3d: translator %s from %s, reads %s, writes %s"):format(t.name, path,
            table.concat(t.reads, " "), table.concat(t.writes, " ")))
    else
      print(("cafesa3d: %s is not a translator: %s"):format(path, tostring(t)))
    end
  end

  return FILE.found
end

function FILE.reader_for(ext)
  for _, t in ipairs(FILE.translators()) do
    for _, e in ipairs(t.reads) do
      if e == ext and type(t.read) == "function" then return t end
    end
  end
end

-- Objects out of a file, beside what is there: glTF by Cafesa3D's own
-- reader, anything else by the translator that reads it.
function FILE.import()
  local start = FILE.path and FILE.dir(FILE.path) or "/Home"

  return FILE.panel("open", {
    title = "Import", start = start,
    filter = function(name)
      local ext = FILE.ext(name)

      return ext == "gltf" or ext == "glb" or FILE.reader_for(ext) ~= nil
    end,
    on_choose = function(path)
      local bytes, why = fs.read(path)
      local ext, loaded = FILE.ext(path), nil
      local stem = FILE.base(path):gsub("%.%w+$", "")

      if type(bytes) == "string" then
        if ext == "gltf" or ext == "glb" then
          loaded, why = SAMPLES.read(bytes, FILE.dir(path))
        else
          local t = FILE.reader_for(ext)
          local context = {
            name = stem,
            material = function(base) return material(base, "Plastic") end,
            -- A world matrix, glTF's column-major and Y up, as a place, a
            -- turn and a size; and a linear colour as the tab shows it.
            place = function(m) return scenefile.place(m) end,
            srgb = function(c) return scenefile.srgb(c) end,
            sidecar = function(name)
              if type(name) ~= "string" or name:find("/") or name:find("%.%.") then return nil end

              local got = fs.read(FILE.dir(path) .. "/" .. name)

              return type(got) == "string" and got or nil
            end,
          }
          local ok

          ok, loaded, why = pcall(t.read, bytes, FILE.KIT, context)

          if not ok then loaded, why = nil, loaded end
        end
      end

      if not loaded then
        FILE.said = "Not imported: " .. tostring(why or "not a file")
        print(("cafesa3d: could not import %s: %s"):format(path, tostring(why)))
        return
      end

      SAMPLES.take(loaded, FILE.base(path), true)
    end,
  })
end

-- The scene out, in another program's format: every shape and mesh that
-- shows, each asked for its triangles where they are in the world.
function FILE.export(t)
  local ext = t.writes[1]
  local start = FILE.path and FILE.dir(FILE.path) or FILE.DIR

  if start == FILE.DIR then fs.send(FILE.DIR, { type = "mkdir" }) end

  return FILE.panel("save", {
    title = "Export as " .. t.name, start = start,
    name = FILE.name:gsub("%.[%w]+$", "") .. "." .. ext,
    on_choose = function(path)
      if FILE.ext(path) ~= ext then path = path .. "." .. ext end

      local objects = {}

      for _, thing in ipairs(things) do
        if thing.id and not thing.hidden then
          objects[#objects + 1] = {
            name = thing.name, mat = thing.mat,
            world = function(how) return scene:world_triangles(thing.id, how) end,
          }
        end
      end

      local stem = FILE.base(path):gsub("%.%w+$", "")
      local ok, bytes, beside = pcall(t.write, { name = stem, objects = objects }, FILE.KIT)
      local done = ok and type(bytes) == "string"
      local why = ok and "the translator gave nothing to write" or bytes

      if done then done, why = fs.write(path, bytes) end

      for name, extra in pairs(done and type(beside) == "table" and beside or {}) do
        if type(name) == "string" and not name:find("/") and type(extra) == "string" then
          fs.write(FILE.dir(path) .. "/" .. name, extra)
        end
      end

      FILE.said = done and ("Exported " .. FILE.base(path)) or ("Not exported: " .. tostring(why))
      print(done and ("cafesa3d: exported %s, %d objects, %d bytes"):format(path, #objects,
                                                                            #bytes)
            or ("cafesa3d: could not export %s: %s"):format(path, tostring(why)))
    end,
  })
end

-- The tutorial, `docs/cafesa3d-tutorial/`: pages and pictures the image
-- carries, which the browser reads where they lie (`asset:` addresses), so
-- it is always the tutorial for the Cafesa3D it came with. It used to be
-- copied out into /Home first, and a screenshot is more than one file of
-- the RAM filesystem holds.
local TUTORIAL = { index = "asset:tutorial/cafesa3d/index.html" }

function TUTORIAL.open()
  local ok, why = fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/browser.lua",
                                       args = TUTORIAL.index })

  print(ok and ("cafesa3d: tutorial at " .. TUTORIAL.index)
        or ("cafesa3d: could not start the browser: " .. tostring(why)))
  return ok and true or false
end

-- The dots: opening a sample, the tutorial, and what comes later.
local function more_menu(x, y)
  local samples = {}

  for _, s_ in ipairs(SAMPLES) do
    samples[#samples + 1] = { text = s_[2], on_choose = function() SAMPLES.open(s_[1], s_[2]) end }
  end

  local exports = {}

  for _, t in ipairs(FILE.translators()) do
    if #t.writes > 0 and type(t.write) == "function" then
      exports[#exports + 1] = { text = t.name .. "...", on_choose = function() FILE.export(t) end }
    end
  end

  if #exports == 0 then exports[1] = { text = "No translators", disabled = true } end

  local m = win:open_menu((win.origin_x or 0) + x, (win.origin_y or 0) + y, {
    { text = "Open a sample", submenu = samples },
    { text = "Tutorial", on_choose = TUTORIAL.open },
    { text = FULL.on and "Leave Full Screen" or "Full Screen",
      on_choose = function() FULL.toggle() end },
    { separator = true },
    { text = "Open...", on_choose = FILE.open },
    { text = "Import...", on_choose = FILE.import },
    { text = "Save", on_choose = FILE.save },
    { text = "Save As...", on_choose = FILE.save_as },
    { text = "Export", submenu = exports },
  })

  if m then
    print(("cafesa3d: more menu at %d,%d, %d wide, rows of %d"):format(m.x, m.y, m.w, m.row))
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

-- Turning, as the kit does it: X, then Y, then Z - Rz Ry Rx, row by row -
-- and back to degrees, as the scene's file reads and writes a turn.
local euler_matrix, matrix_euler = scenefile.euler_matrix, scenefile.matrix_euler

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

  push_undo(m.snap)

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
  -- The triangles, when a shape's own numbers changed them; a colour or a
  -- roughness changes none.
  local extra = f.t.id and f.holder == f.t
                and (" - the scene is %d triangles"):format(scene:triangles()) or ""

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

  if e.f.spec.colour then
    local hex = e.text:match("^#?(%x%x%x%x%x%x)$")

    v = hex and tonumber(hex, 16)
  end

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

  -- The Keys sheet closes on a click anywhere, which does nothing else.
  if KEYS.open then return KEYS.toggle() end
  if inside(controls.keys, x, y) then return KEYS.toggle() end

  -- Duplicate, as Shift D does: the copy follows the pointer at once.
  if inside(controls.duplicate, x, y) then
    if duplicate_selected() then begin("grab") end
    return true
  end

  -- The Script button and the panel: a press in the code gives it the
  -- keyboard, a press anywhere else takes the keyboard back.
  if inside(controls.script, x, y) then return SCRIPT.toggle() end

  if SCRIPT.open then
    if inside(controls["script:run"], x, y) then return SCRIPT.run() end
    if inside(controls["script:open"], x, y) then return SCRIPT.open_file() end
    if inside(controls["script:save"], x, y) then return SCRIPT.save_file() end

    local c = controls["script:code"]

    if inside(c, x, y) then
      SCRIPT.focused, SCRIPT.pressing = true, true
      SCRIPT.editor:mouse("press", x - c.x, y - c.y)
      return true
    end

    SCRIPT.focused = false
  end

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

  -- A chip: a preset, a swatch, a pattern.
  for key, go in pairs(chip.actions) do
    if inside(controls[key], x, y) then
      go()
      return true
    end
  end

  for _, name in ipairs(TABS) do
    if inside(controls["tab:" .. name], x, y) then
      tab = name
      print("cafesa3d: tab " .. name)
      return true
    end
  end

  if inside(controls.add, x, y) then add_menu(controls.add.x, HEAD) return true end
  if inside(controls.more, x, y) then more_menu(controls.more.x + 26 - 190, HEAD) return true end
  if inside(controls.wire, x, y) then set_shading("wire") return true end
  if inside(controls.solid, x, y) then set_shading("solid") return true end
  if inside(controls.rendered, x, y) then set_shading("rendered") return true end
  if inside(controls.render, x, y) then final.open() return true end

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
  -- selects it. A script's row opens the script, in its panel.
  if x >= SX and y >= OUT_Y and y < PROPS_Y then
    for _, r in ipairs(OUTLINER.rows) do
      if y >= r.y and y < r.y + ROW then
        if r.script then
          if not SCRIPT.open then SCRIPT.toggle() end

          SCRIPT.focused = true
        elseif x >= SX + SIDE - 36 then
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

  -- Nothing of its own under it in the header: the header is the title bar,
  -- so the window is taken hold of - moved, or maximised on a second press.
  if y < HEAD then
    win:take_hold(x, y)
    return true
  end

  return false
end

local function move(x, y)
  -- A drag in the code selects, wherever the pointer goes on the way.
  if SCRIPT.pressing then
    local c = controls["script:code"]

    SCRIPT.editor:mouse("move", x - c.x, y - c.y)
    return true
  end

  pointer = { x, y }

  if field_drag then
    local fdr = field_drag
    local dx = x - fdr.x

    if not fdr.moved and math.abs(dx) < 4 then return false end

    if not fdr.moved then
      fdr.moved = true
      will(("set %s of %s"):format(fdr.f.label, fdr.f.t.name))
    end

    if not fdr.f.spec.colour then field_set(fdr.f, fdr.v0 + dx * fdr.f.spec.step) end

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
  if SCRIPT.pressing then
    local c = controls["script:code"]

    SCRIPT.pressing = false
    SCRIPT.editor:mouse("release", x - c.x, y - c.y)
    return true
  end

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

--
-- The keys, their groups on the sheet, and what each does (`KEYS`, above).
-- `shift` and `ctrl` are true or false when they matter and absent when they
-- do not: the number row's views take either Control, X asks whatever is
-- held, and G, R and S want neither.
--
KEYS.list = {
  { group = 1, keys = "drag", says = "turn it" },
  { group = 1, keys = "Shift drag", says = "move it" },
  { group = 1, keys = "wheel", says = "closer, further" },
  { group = 1, code = 2, shift = false, keys = "1", says = "from the front",
    run = function() set_view("front") return true end },
  { group = 1, code = 2, shift = true, keys = "Shift 1", says = "from the back",
    run = function() set_view("back") return true end },
  { group = 1, code = 4, shift = false, keys = "3", says = "from the right",
    run = function() set_view("right") return true end },
  { group = 1, code = 4, shift = true, keys = "Shift 3", says = "from the left",
    run = function() set_view("left") return true end },
  { group = 1, code = 8, shift = false, keys = "7", says = "from the top",
    run = function() set_view("top") return true end },
  { group = 1, code = 8, shift = true, keys = "Shift 7", says = "from the bottom",
    run = function() set_view("bottom") return true end },
  { group = 1, code = 102, keys = "Home", says = "everything in view",
    run = function() frame_all() return true end },
  { group = 1, code = 33, ctrl = false, keys = "F", says = "the selection in view",
    run = function() return frame_selected() end },
  { group = 1, code = 44, ctrl = false, keys = "Z", says = "wireframe or solid",
    run = function() set_shading(shading == "solid" and "wire" or "solid") return true end },

  { group = 2, keys = "click", says = "select" },
  { group = 2, keys = "right click", says = "duplicate it or delete it" },
  { group = 2, code = 30, shift = true, keys = "Shift A", says = "add",
    run = function() add_menu(pointer[1], pointer[2]) return true end },
  { group = 2, code = 32, shift = true, keys = "Shift D", says = "duplicate, and move it",
    -- As Blender's: the copy follows the pointer at once.
    run = function() if duplicate_selected() then begin("grab") end return true end },
  { group = 2, code = 45, keys = "X", says = "delete, asking",
    run = function() delete_menu(pointer[1], pointer[2]) return true end },
  { group = 2, code = 111, keys = "Delete", says = "delete",
    run = function() return delete_selected() end },
  { group = 2, code = 44, ctrl = true, shift = false, keys = "Ctrl Z", says = "undo",
    run = function() return undo_last() end },
  { group = 2, code = 44, ctrl = true, shift = true, keys = "Ctrl Shift Z", says = "redo",
    run = function() return redo_last() end },

  { group = 3, code = 34, shift = false, ctrl = false, keys = "G", says = "move",
    run = function() return begin("grab") end },
  { group = 3, code = 19, shift = false, ctrl = false, keys = "R", says = "rotate",
    run = function() return begin("rotate") end },
  { group = 3, code = 31, shift = false, ctrl = false, keys = "S", says = "scale",
    run = function() return begin("scale") end },
  { group = 3, keys = "X Y Z", says = "then only along that axis" },
  { group = 3, keys = "Shift X Y Z", says = "then all but that axis" },
  { group = 3, keys = "0-9 . -", says = "then by exactly that much" },
  { group = 3, keys = "Enter, click", says = "then keep it" },
  { group = 3, keys = "Esc, right click", says = "then put it back" },

  { group = 4, code = 31, ctrl = true, shift = false, keys = "Ctrl S", says = "save",
    run = function() return FILE.save() or true end },
  { group = 4, code = 31, ctrl = true, shift = true, keys = "Ctrl Shift S", says = "save as",
    run = function() return FILE.save_as() or true end },
  { group = 4, code = 24, ctrl = true, keys = "Ctrl O", says = "open",
    run = function() return FILE.open() or true end },
  { group = 4, code = 88, keys = "F12", says = "render",
    run = function() final.open() return true end },
  { group = 4, code = 87, keys = "F11", says = "full screen",
    run = function() return FULL.toggle() end },
  { group = 4, code = 59, keys = "F1", says = "the tutorial",
    run = function() return TUTORIAL.open() end },
  { group = 4, code = 53, shift = true, keys = "?", says = "these keys",
    run = function() return KEYS.toggle() end },

  { group = 5, keys = "Shift F4", says = "the Script panel" },
  { group = 5, keys = "Ctrl Enter", says = "run it" },
  { group = 5, keys = "Esc", says = "the keys back to the view" },

  { group = 6, keys = "click", says = "type one" },
  { group = 6, keys = "drag across", says = "scrub it" },
  { group = 6, keys = "Tab", says = "the next field" },
  { group = 6, keys = "Enter", says = "keep it" },
  { group = 6, keys = "Esc", says = "leave it as it was" },
}

-- Raw keys: 42 and 54 are the shifts, 29 and 97 the controls, 2..11 the
-- number row, 30 A, 32 D, 44 Z, 45 X, 59 F1, 88 F12, 102 Home, 111 Delete.
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

  -- Shift F4 opens and closes the Script panel, whoever has the keyboard.
  if ev.code == 62 and shift then return SCRIPT.toggle() end

  -- Escape closes the Keys sheet before it means anything else.
  if KEYS.open and ev.code == 1 then return KEYS.toggle() end

  -- While the script holds the keyboard, keys are words there: the
  -- letters that are commands here arrive as characters, below. Escape
  -- gives the keyboard back, taken here as the key it is: in the stream of
  -- characters an Escape is not known to be one until the byte after it,
  -- so Escape then Z could reach the script as a Z.
  if SCRIPT.open and SCRIPT.focused then
    if ev.code == 1 then
      SCRIPT.focused = false
      return true
    end

    return false
  end

  -- While a field is being typed in: its keys, and nothing else's.
  if editing then
    local e = editing
    local ch = ({ [2] = "1", [3] = "2", [4] = "3", [5] = "4", [6] = "5", [7] = "6",
                  [8] = "7", [9] = "8", [10] = "9", [11] = "0", [52] = ".", [12] = "-" })[ev.code]

    -- A colour is hex: a to f as well, and the # it is shown with.
    if e.f.spec.colour then
      ch = ({ [30] = "a", [48] = "b", [46] = "c", [32] = "d", [18] = "e", [33] = "f" })[ev.code]
           or (ch ~= "." and ch ~= "-" and ch) or nil
    end

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

  -- The rest from the table the Keys sheet is drawn from.
  for _, k in ipairs(KEYS.list) do
    if k.code == ev.code and (k.shift == nil or k.shift == shift)
       and (k.ctrl == nil or k.ctrl == ctrl) then
      return k.run()
    end
  end

  return false
end

say_selected()

--------------------------------------------------------------------------
-- Full screen: F11, or the dots (`roadmap.md` 4l, 5b). The whole of
-- Cafesa3D laid out again across the screen - the 3D view taking what the
-- panels do not, the Outliner as tall as the room allows - and back again.
--
-- **A second window, not a bigger one.** A window that draws its own pixels
-- cannot be resized: its buffers are a region made for its size (`wm.lua`,
-- on `fullscreen`). The video player starts itself again at the new size,
-- and that would lose a scene that is not saved - so here a window is
-- opened at the new size and the old one closed, in this process, and the
-- scene never leaves it.
--
-- **The view keeps its vertical angle**, so a wide screen shows more to
-- either side at the same size rather than the same width closer up - which
-- is what an ultrawide monitor is for.
--------------------------------------------------------------------------

function FULL.fit(w, h)
  W, H = w, h
  VW, VH = W - TOOLS - SIDE - (SCRIPT.open and SCRIPT.W or 0), H - HEAD - FOOT
  SX = W - SIDE

  -- Properties keeps the height its tallest tab needs, and the Outliner
  -- has the rest: a car of twenty-five parts fits in it at 1440 rows.
  OUT_H = math.max(236, H - OUT_Y - FOOT - 600)
  PROPS_Y = OUT_Y + OUT_H
  FOV = 2 * math.atan(FULL.tv * VW / VH)
end

-- Where the window, its rows, its tabs and its controls are, for whoever
-- drives Cafesa3D from outside - `tools/run_cafesa3d.py` clicks where
-- these say - at the start and after every change of window.
function FULL.say()
  print(("cafesa3d: window at %d,%d"):format(win.origin_x or 0, win.origin_y or 0))

  local out = {}

  for _, r in ipairs(OUTLINER.rows) do
    if r.thing then
      out[#out + 1] = ("%s %d,%d eye %d"):format(r.thing.name, SX + 90, r.y + ROW // 2,
                                                SX + SIDE - 23)
    end
  end

  local tabs = {}

  for _, name in ipairs(TABS) do
    local c = controls["tab:" .. name]

    tabs[#tabs + 1] = ("%s %d,%d"):format(name, c.x + c.w // 2, c.y + c.h // 2)
  end

  print("cafesa3d: rows " .. table.concat(out, "; "))
  print("cafesa3d: tabs " .. table.concat(tabs, "; "))

  local header = {}

  for _, name in ipairs({ "add", "duplicate", "keys", "more", "wire", "solid",
                          "rendered", "render", "tool:select", "tool:move",
                          "tool:rotate", "tool:scale" }) do
    local c = controls[name]

    header[#header + 1] = ("%s %d,%d"):format(name, c.x + c.w // 2, c.y + c.h // 2)
  end

  print("cafesa3d: controls " .. table.concat(header, "; "))

  local sb = controls.script

  if sb then
    print(("cafesa3d: script button %d,%d"):format(sb.x + sb.w // 2, sb.y + sb.h // 2))
  end

  local code, run = controls["script:code"], controls["script:run"]

  if SCRIPT.open and code and run then
    print(("cafesa3d: script code %d,%d %dx%d; run %d,%d"):format(code.x, code.y,
          code.w, code.h, run.x + run.w // 2, run.y + run.h // 2))
  end
end

function FULL.toggle()
  local on = not FULL.on
  local screen_now = fs.read("/Devices/screen") or {}
  local w, h = FULL.was[1], FULL.was[2]

  if on then w, h = screen_now.width or W, screen_now.height or H end

  -- Everything the new size needs, made before anything is let go of: if
  -- the machine will not give it, the window stays as it was.
  local v, why = k3.view(w - TOOLS - SIDE - (SCRIPT.open and SCRIPT.W or 0),
                         h - HEAD - FOOT)
  local fresh

  if v then
    -- Back to where it came from: maximised, or centred when there was
    -- nobody to ask where that is.
    fresh, why = ui.window{ title = "Cafesa3D", w = w, h = h, direct = true,
                            header = (not on) or nil,
                            fullscreen = on or nil,
                            maximised = (not on and screen.area) and true or nil,
                            centre = (not on and not screen.area) or nil }

    if fresh and not fresh:surface() then
      fresh:close()
      fresh, why = nil, "no pixels for a window that size"
    end
  end

  if not fresh then
    FILE.said = "No full screen: " .. tostring(why)
    print(("cafesa3d: could not %s: %s"):format(on and "go full screen" or "come back",
                                               tostring(why)))
    return true
  end

  -- The Rendered view's pixels are the old view's size; it starts again.
  shade.stop()

  if shade.surf then shade.surf:free() end

  shade.surf, shade.key = nil, nil

  local old = win

  win, view, FULL.on = fresh, v, on
  FULL.fit(w, h)
  old:close()

  if not draw_all() then return true end

  FULL.say()
  say_where()
  print(("cafesa3d: %s, %d by %d, the view %d by %d, %d objects"):format(
    on and "full screen" or "a window", W, H, VW, VH, #things))
  return true
end

-- Laid out for the size it opened at - the Outliner as tall as the room
-- allows, and the view's angle the drawing's up and down - as full screen
-- is.
FULL.fit(W, H)

if not draw_all() then return end

FULL.say()
say_where()
print(("cafesa3d: %d objects, %d triangles, the view %d by %d, %s"):format(
  #things, scene:triangles(), VW, VH, shading))

local dirty = false

--
-- One pass of the loop, in a function of its own: its locals are not the
-- main chunk's, which is at Lua's two hundred. False ends the loop.
--
local function pass()
  if dirty then
    if not draw_all() then return false end
    dirty = false
  end

  local rendering = (shade.job and shade.job:passes() < RENDER.view_samples)
                    or (final.job and not final.done)
  local reply = wmproto.poll(win.handle, rendering and 8 or 25)

  if not reply then return false end

  final.tend()

  -- The view's render: new tiles are a reason to draw, and the last pass
  -- is said once, for whoever is waiting for it.
  local job = shading == "rendered" and shade.job

  if job then
    if job:paint(shade.surf, 0, 0) > 0 then dirty = true end

    if not shade.first and job:passes() >= 1 then
      shade.first = true
      print(("cafesa3d: the view's first pass in %.1f s"):format((sys.ticks() - shade.t0) / HZ))
    end

    if not shade.said and job:passes() >= RENDER.view_samples then
      shade.said = true
      print(("cafesa3d: rendered the view, %d samples, %d rays, on %d threads"):format(
        job:passes(), job:rays(), job:workers()))
    end
  end

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
      elseif ev.action == "press" and in_view(ev.x, ev.y) then
        -- **The selection's menu** (`roadmap.md` 4l, 5h): what is under the
        -- pointer selected first, as a right click in Blender acts on it,
        -- then Duplicate beside Delete - Diego's "both".
        local under = pick(ev.x, ev.y)

        if under then select(under) end

        if selected then
          local t = selected
          local m = win:open_menu((win.origin_x or 0) + ev.x, (win.origin_y or 0) + ev.y, {
            { text = "Duplicate " .. t.name,
              on_choose = function() if duplicate_selected() then begin("grab") end end },
            { text = "Delete " .. t.name, on_choose = delete_selected },
          })

          if m then
            print(("cafesa3d: selection menu at %d,%d, rows of %d"):format(m.x, m.y, m.row))
          end

          dirty = true
        end
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
      -- As close as F frames a small thing, and no closer than the near
      -- plane's 5 cm allows for.
      orbit.dist = math.max(0.25, math.min(60, orbit.dist * (0.9 ^ (ev.n or 0))))
      view_name = VIEW_NAME
      print(("cafesa3d: %s, at %.1f"):format((ev.n or 0) > 0 and "closer" or "further",
                                             orbit.dist))
      dirty = true
    elseif ev.type == "rawkey" then
      if rawkey(ev) then
        dirty = true
        said_where = true
      end
    elseif ev.type == "key" then
      if SCRIPT.key(ev) then dirty = true end
    elseif ev.type == "wheel" and ev.x >= SX and ev.y >= OUT_Y and ev.y < PROPS_Y then
      dirty = OUTLINER.wheel(ev.n or 0) or dirty
    elseif ev.type == "wheel" and SCRIPT.open
           and inside(controls["script:code"], ev.x, ev.y) then
      SCRIPT.editor:wheel(ev.n or 0)
      dirty = true
    end
  end

  -- Where things are after a key or a menu changed them, as a release does
  -- - drawn first, so the fields said are the ones on the screen.
  if said_where then
    if dirty then
      if not draw_all() then return false end
      dirty = false
    end

    say_where()
  end
  return true
end

while win.running do
  if not pass() then break end
end

shade.stop()
final.close()
