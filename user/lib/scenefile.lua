-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A Cafesa3D scene, out of a glTF file's JSON.
--
--   local json = use("/lib/json.lua")
--   local scenefile = use("/lib/scenefile.lua")
--   local scene, why = scenefile.from_gltf(json.decode(text))
--   -- scene.name, scene.things (what Cafesa3D adds), scene.world, scene.skipped
--
-- **glTF 2.0, as `tools/cafesa3d_samples.py` writes it** (`roadmap.md` 4l):
-- each shape a node whose `extras.cafesa3d` holds its kind, its own numbers,
-- and its place, turn and size in Cafesa3D's Z-up space; materials in
-- glTF's metallic-roughness terms; the lamp as `KHR_lights_punctual`; the
-- camera as glTF's. Where a node has no place of Cafesa3D's own, its glTF
-- transform - Y up, a quaternion - is converted, which is also what the
-- host test holds the two to each other with.
--
-- **The file is from outside**, so nothing in it is trusted: every field is
-- checked for its type and its range, and an object that fails is skipped
-- and counted rather than handed to the kit, which raises on nonsense.
-- Nodes that are meshes of triangles - another program's glTF - are
-- counted too; reading those is step five's.

local scenefile = {}

local KINDS = { plane = true, box = true, sphere = true, cylinder = true, ico = true,
                cone = true, torus = true, grid = true }
local MAX_OBJECTS = 2000

local function number(v, lo, hi)
  return type(v) == "number" and v == v and v >= lo and v <= hi and v or nil
end

local function vec3(v, lo, hi)
  if type(v) ~= "table" then return nil end

  local a, b, c = number(v[1], lo, hi), number(v[2], lo, hi), number(v[3], lo, hi)

  return (a and b and c) and { a, b, c } or nil
end

-- "#rrggbb" to 0xRRGGBB.
local function hexcolour(s)
  local h = type(s) == "string" and s:match("^#(%x%x%x%x%x%x)$")

  return h and tonumber(h, 16) or nil
end

-- A linear colour, glTF's, as the 0..255 sRGB the app shows.
local function srgb(lin)
  local out = 0

  for i = 1, 3 do
    local c = math.max(0, math.min(1, lin[i]))

    c = c <= 0.0031308 and c * 12.92 or 1.055 * c ^ (1 / 2.4) - 0.055
    out = (out << 8) | math.floor(c * 255 + 0.5)
  end

  return out
end

--------------------------------------------------------------------------
-- glTF's Y-up to Cafesa3D's Z-up: (x, y, z) in glTF is (x, -z, y) here -
-- the inverse of Blender's export, which writes (x, z, -y).
--------------------------------------------------------------------------

local function quat_matrix(q)
  local x, y, z, w = q[1], q[2], q[3], q[4]

  return { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
           2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
           2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y) }
end

-- R here is C^T Rg C, C being glTF-from-here: rows (1,0,0) (0,0,1) (0,-1,0).
local function from_gltf_matrix(G)
  local function g(i, j) return G[(i - 1) * 3 + j] end

  -- C^T Rg C, written out: here's rows and columns 2 and 3 are glTF's 3 and
  -- 2, with a sign wherever exactly one of them is here's Y.
  return { g(1, 1),  -g(1, 3),  g(1, 2),
          -g(3, 1),   g(3, 3), -g(3, 2),
           g(2, 1),  -g(2, 3),  g(2, 2) }
end

-- Rz Ry Rx to Euler degrees, as the app reads a turn back.
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

-- A node's place, turn and size from its glTF transform alone.
function scenefile.transform(node)
  local t = vec3(node.translation, -1e6, 1e6) or { 0, 0, 0 }
  local s = vec3(node.scale, -1e4, 1e4) or { 1, 1, 1 }
  local q = node.rotation
  local rot = { 0, 0, 0 }

  if type(q) == "table" and number(q[1], -2, 2) and number(q[2], -2, 2)
     and number(q[3], -2, 2) and number(q[4], -2, 2) then
    local n = math.sqrt(q[1] ^ 2 + q[2] ^ 2 + q[3] ^ 2 + q[4] ^ 2)

    if n > 1e-9 then
      rot = matrix_euler(from_gltf_matrix(quat_matrix({ q[1] / n, q[2] / n, q[3] / n, q[4] / n })))
    end
  end

  return { t[1], -t[3], t[2] }, rot, { s[1], s[3], s[2] }
end

--------------------------------------------------------------------------
-- Materials, lamps, cameras and shapes.
--------------------------------------------------------------------------

local function material(doc, index)
  local m = type(doc.materials) == "table" and math.type(index) == "integer"
            and doc.materials[index + 1]

  if type(m) ~= "table" then
    return { base = 0xcccccc, preset = "Plastic", metallic = 0, rough = 0.5, trans = 0,
             ior = 1.5, emit = 0 }
  end

  local pbr = type(m.pbrMetallicRoughness) == "table" and m.pbrMetallicRoughness or {}
  local ext = type(m.extensions) == "table" and m.extensions or {}
  local own = type(m.extras) == "table" and type(m.extras.cafesa3d) == "table"
              and m.extras.cafesa3d or {}
  local base = hexcolour(own.base)

  if not base then
    local f = type(pbr.baseColorFactor) == "table" and pbr.baseColorFactor or {}
    local lin = { number(f[1], 0, 1) or 1, number(f[2], 0, 1) or 1, number(f[3], 0, 1) or 1 }

    base = srgb(lin)
  end

  local function ext_number(name, field, lo, hi, default)
    local e = type(ext[name]) == "table" and ext[name] or {}

    return number(e[field], lo, hi) or default
  end

  local emissive = type(m.emissiveFactor) == "table" and number(m.emissiveFactor[1], 0, 1)
                   and (m.emissiveFactor[1] + (m.emissiveFactor[2] or 0)
                        + (m.emissiveFactor[3] or 0)) > 0

  return {
    base = base,
    preset = type(own.preset) == "string" and own.preset or nil,
    metallic = number(pbr.metallicFactor, 0, 1) or 1,
    rough = number(pbr.roughnessFactor, 0, 1) or 1,
    trans = ext_number("KHR_materials_transmission", "transmissionFactor", 0, 1, 0),
    ior = ext_number("KHR_materials_ior", "ior", 1, 3, 1.5),
    emit = emissive and ext_number("KHR_materials_emissive_strength", "emissiveStrength",
                                   0, 1000, 1) or 0,
  }
end

local function name_of(node, fallback)
  local n = type(node.name) == "string" and node.name or fallback

  return n:gsub("[%c]", ""):sub(1, 63)
end

-- A shape's own numbers, each held to what the kit takes; nil and why if
-- one is not a number the kit would build.
local function shape(own, kind)
  local t = { kind = kind }
  local limits = {
    radius = { 1e-4, 1e5 }, radius2 = { 0, 1e5 }, depth = { 1e-4, 1e5 },
    segments = { kind == "grid" and 1 or 3, 256 },
    rings = { kind == "grid" and 1 or kind == "torus" and 3 or 2, 128 },
    subdivisions = { 1, 6 },
  }

  for field, range in pairs(limits) do
    if own[field] ~= nil then
      local v = number(own[field], range[1], range[2])

      if not v then return nil, field .. " out of range" end
      if field == "segments" or field == "rings" or field == "subdivisions" then
        if math.type(v) ~= "integer" and v ~= math.floor(v) then return nil, field .. " not whole" end
        v = math.floor(v)
      end

      t[field] = v
    end
  end

  if kind == "box" then
    t.size = vec3(own.size, 1e-4, 1e5)
    if not t.size then return nil, "a box's size" end
  elseif kind == "plane" or kind == "grid" then
    t.size = number(own.size, 1e-4, 1e5)
    if not t.size then return nil, "a plane's size" end
  end

  t.smooth = own.smooth == true
  return t
end

function scenefile.from_gltf(doc)
  if type(doc) ~= "table" or type(doc.asset) ~= "table" or doc.asset.version ~= "2.0" then
    return nil, "not a glTF 2.0 file"
  end

  local nodes = type(doc.nodes) == "table" and doc.nodes or {}
  local scenes = type(doc.scenes) == "table" and doc.scenes or {}
  local which = math.type(doc.scene) == "integer" and doc.scene or 0
  local sc = type(scenes[which + 1]) == "table" and scenes[which + 1] or {}
  local list = type(sc.nodes) == "table" and sc.nodes or {}
  local lights = type(doc.extensions) == "table"
                 and type(doc.extensions.KHR_lights_punctual) == "table"
                 and type(doc.extensions.KHR_lights_punctual.lights) == "table"
                 and doc.extensions.KHR_lights_punctual.lights or {}
  local out = { name = type(sc.name) == "string" and sc.name:sub(1, 63) or "Scene",
                things = {}, skipped = 0, why = {} }

  local world = type(doc.extras) == "table" and type(doc.extras.cafesa3d) == "table"
                and type(doc.extras.cafesa3d.world) == "table" and doc.extras.cafesa3d.world
  if world then
    out.world = { zenith = hexcolour(world.zenith), horizon = hexcolour(world.horizon),
                  strength = number(world.strength, 0, 10) }
  end

  local function skip(name, why)
    out.skipped = out.skipped + 1
    out.why[#out.why + 1] = name .. ": " .. why
  end

  for n, index in ipairs(list) do
    local node = math.type(index) == "integer" and nodes[index + 1]

    if #out.things >= MAX_OBJECTS then
      skip("the rest", "more than " .. MAX_OBJECTS .. " objects")
      break
    end

    if type(node) ~= "table" then
      skip("node " .. tostring(index), "not there")
    else
      local name = name_of(node, "Object " .. n)
      local own = type(node.extras) == "table" and type(node.extras.cafesa3d) == "table"
                  and node.extras.cafesa3d or {}
      local loc, rot, scale = scenefile.transform(node)

      -- Cafesa3D's own numbers, exact, when the file has them.
      loc = vec3(own.loc, -1e6, 1e6) or loc
      rot = vec3(own.rot, -1e5, 1e5) or rot
      scale = vec3(own.scale, -1e4, 1e4) or scale

      local lamp = type(node.extensions) == "table"
                   and type(node.extensions.KHR_lights_punctual) == "table"
                   and node.extensions.KHR_lights_punctual.light

      if KINDS[own.kind] then
        local t, why = shape(own, own.kind)

        if t then
          t.name, t.loc, t.rot, t.scale = name, loc, rot, scale
          t.mat = material(doc, own.material)
          out.things[#out.things + 1] = t
        else
          skip(name, why)
        end
      elseif own.kind == "light" or math.type(lamp) == "integer" then
        local l = math.type(lamp) == "integer" and type(lights[lamp + 1]) == "table"
                  and lights[lamp + 1] or {}
        local power = number(own.power, 0, 1e6)
                      or (number(l.intensity, 0, 1e6) and l.intensity * 4 * math.pi) or 1000

        out.things[#out.things + 1] = {
          name = name, kind = "light", loc = loc,
          radius = number(own.radius, 0.001, 100) or 0.1, power = power,
          colour = hexcolour(own.colour)
                   or (type(l.color) == "table" and srgb({ number(l.color[1], 0, 1) or 1,
                                                           number(l.color[2], 0, 1) or 1,
                                                           number(l.color[3], 0, 1) or 1 }))
                   or 0xffffff,
        }
      elseif own.kind == "camera" or math.type(node.camera) == "integer" then
        -- Where it looks: its own target, or ten metres down its -Z.
        local target = vec3(own.target, -1e6, 1e6)

        if not target then
          local _, r = scenefile.transform(node)
          local ax, ay, az = math.rad(r[1]), math.rad(r[2]), math.rad(r[3])
          local cx, sx, cy, sy = math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay)
          local cz, sz = math.cos(az), math.sin(az)
          -- Its -Z here is glTF's -Z turned: the third column of R, negated
          -- and taken through (x, -z, y) - which is here's +Y column.
          local col = { cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx }

          target = { loc[1] + col[1] * 10, loc[2] + col[2] * 10, loc[3] + col[3] * 10 }
        end

        out.things[#out.things + 1] = { name = name, kind = "camera", loc = loc,
                                        target = target,
                                        focal = number(own.focal, 1, 500) or 50 }
      elseif math.type(node.mesh) == "integer" then
        skip(name, "a mesh of triangles (step five)")
      end
    end
  end

  return out
end

return scenefile
