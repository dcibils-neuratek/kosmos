-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A Cafesa3D scene, out of a glTF file's JSON.
--
--   local json = use("/Kosmos/Libraries/json.lua")
--   local scenefile = use("/Kosmos/Libraries/scenefile.lua")
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
--
-- **Meshes are read** - Cafesa3D's own, and any other program's whose
-- buffer is in the file as a `data:` URI and whose mesh is triangles: each
-- becomes a `mesh` thing naming a buffer and where in it its points and
-- indices are. The buffers stay base64, in `scene.buffers`, for whoever has
-- a decoder in C to decode once (`compress.unbase64`); a mesh whose accessors say
-- more than its buffer holds is skipped, as any other lie would be. A
-- material's texture - which glTF has no word for - comes from its
-- `extras.cafesa3d`, held to the ranges the kit holds it to.
--
-- **And the scene's script** (`roadmap.md` 6n, 6d): the Script panel's name
-- and text in the file's own `extras.cafesa3d.script`, and on each object a
-- script made, `by` - that script's name - so a Run after the file is
-- opened again still replaces what the last one made.

local scenefile = {}

local KINDS = { plane = true, box = true, sphere = true, cylinder = true, ico = true,
                cone = true, torus = true, grid = true }
local PATTERNS = { plain = true, checker = true, brick = true, shingles = true, noise = true,
                   wood = true, marble = true }
local MAX_OBJECTS = 2000
local MAX_POINTS = 1 << 24
local MAX_SCRIPT = 1 << 20              -- a megabyte of Lua is not a scene's script

-- A script's name, or nil: words on one line, cut to what a name holds.
local function script_name(v)
  if type(v) ~= "string" or v == "" or v:find("%c") then return nil end

  return v:sub(1, 63)
end

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

  -- The texture, each number in the range the kit takes it in; one out of
  -- range and the surface is plain, which is a look rather than an error.
  local texture
  local tx = own.texture

  if type(tx) == "table" and PATTERNS[tx.pattern] then
    texture = { pattern = tx.pattern, colour2 = hexcolour(tx.colour2) }

    for field, range in pairs({ scale = { 0.01, 1000 }, detail = { 0, 8 },
                                distortion = { 0, 20 }, bump = { 0, 0.2 },
                                mortar = { 0, 0.9 }, ratio = { 0.1, 10 },
                                offset = { 0, 1 } }) do
      if tx[field] ~= nil then
        texture[field] = number(tx[field], range[1], range[2])

        if texture[field] == nil then
          texture = nil
          break
        end
      end
    end
  end

  local emissive = type(m.emissiveFactor) == "table" and number(m.emissiveFactor[1], 0, 1)
                   and (m.emissiveFactor[1] + (m.emissiveFactor[2] or 0)
                        + (m.emissiveFactor[3] or 0)) > 0

  -- Cafesa3D's own numbers where the file has them, which a file it saved
  -- always does; glTF's otherwise. A glowing black would otherwise come
  -- back dark, glTF's emission being the colour times the strength.
  return {
    base = base,
    preset = type(own.preset) == "string" and own.preset or nil,
    metallic = number(own.metallic, 0, 1) or number(pbr.metallicFactor, 0, 1) or 1,
    rough = number(own.rough, 0, 1) or number(pbr.roughnessFactor, 0, 1) or 1,
    trans = number(own.trans, 0, 1)
            or ext_number("KHR_materials_transmission", "transmissionFactor", 0, 1, 0),
    ior = number(own.ior, 1, 3) or ext_number("KHR_materials_ior", "ior", 1, 3, 1.5),
    emit = number(own.emit, 0, 100) or (emissive and ext_number("KHR_materials_emissive_strength",
                                                               "emissiveStrength", 0, 1000, 1)
                                        or 0),
    texture = texture,
  }
end

--------------------------------------------------------------------------
-- A mesh node: where its points and triangles are in which buffer, every
-- number held to what the file's own description says it has room for.
--------------------------------------------------------------------------

-- Where a buffer's bytes are: the file's own base64 (`base64`), a binary
-- glTF's chunk (`bin`), or a file beside this one (`file`, whose name is
-- the caller's to read - this file does no reading of its own). A buffer
-- somewhere on the network is refused: opening a scene fetches nothing.
local function buffer_of(doc, index, has_bin)
  local b = type(doc.buffers) == "table" and doc.buffers[index + 1]

  if type(b) ~= "table" then return nil, "a buffer the file does not have" end

  local length = number(b.byteLength, 0, 1 << 30)
  local uri = b.uri

  if uri == nil and index == 0 and has_bin then return { bin = true }, length end
  if type(uri) ~= "string" then return nil, "a buffer not in the file" end

  local data = uri:match("^data:application/[%w%-]+;base64,(.*)$")

  if data then return { base64 = data }, length end

  if uri:find("^%a[%w+.%-]*:") or uri:find("^/") or uri:find("%.%.") then
    return nil, "a buffer somewhere else than beside the file"
  end

  return { file = uri }, length
end

-- An accessor's place: its buffer, where in it, how many, how wide each is
-- and how far apart - points may be interleaved with the rest of a vertex.
local function accessor(doc, index, want_type, sizes)
  local a = type(doc.accessors) == "table" and math.type(index) == "integer"
            and doc.accessors[index + 1]

  if type(a) ~= "table" or a.type ~= want_type or not sizes[a.componentType]
     or a.sparse ~= nil then
    return nil
  end

  local v = type(doc.bufferViews) == "table" and math.type(a.bufferView) == "integer"
            and doc.bufferViews[a.bufferView + 1]
  local each = sizes[a.componentType] * (want_type == "VEC3" and 3 or 1)

  if type(v) ~= "table" or math.type(v.buffer) ~= "integer" then return nil end

  local stride = v.byteStride == nil and each or number(v.byteStride, each, 252)
  local count = number(a.count, 1, MAX_POINTS * 3)
  local inside = number(a.byteOffset or 0, 0, 1 << 30)
  local start = number(v.byteOffset or 0, 0, 1 << 30)
  local length = number(v.byteLength, 0, 1 << 30)

  if not stride or not count or math.type(count) ~= "integer" or not inside or not start
     or not length or inside + (count - 1) * stride + each > length then
    return nil
  end

  return { buffer = v.buffer, offset = start + inside, count = count,
           bytes = sizes[a.componentType], stride = stride }
end

-- One primitive of a mesh: where its points and triangles are. Triangles
-- need not be listed - three points in a row are one - and a mesh squeezed
-- by an extension this file cannot undo is refused by name.
local function primitive_of(doc, prim)
  if type(prim) ~= "table" or type(prim.attributes) ~= "table" then
    return nil, "a mesh with nothing in it"
  end

  if prim.mode ~= nil and prim.mode ~= 4 then return nil, "a mesh not of triangles" end

  if type(prim.extensions) == "table" and prim.extensions.KHR_draco_mesh_compression then
    return nil, "a mesh compressed with Draco, which Cafesa3D does not read yet"
  end

  local pos = accessor(doc, prim.attributes.POSITION, "VEC3", { [5126] = 4 })

  if not pos then return nil, "a mesh whose points are not plain floats in its buffer" end
  if pos.count > MAX_POINTS then return nil, "a mesh of more points than a scene may have" end

  local out = { buffer = pos.buffer + 1, points = pos.count, point_at = pos.offset,
                point_stride = pos.stride, material = prim.material }

  if prim.indices ~= nil then
    local idx = accessor(doc, prim.indices, "SCALAR", { [5121] = 1, [5123] = 2, [5125] = 4 })

    if not idx or idx.count % 3 ~= 0 or idx.stride ~= idx.bytes then
      return nil, "a mesh whose triangles do not fit its buffer"
    end

    out.index_buffer, out.indices = idx.buffer + 1, idx.count
    out.index_at, out.index_bytes = idx.offset, idx.bytes
  elseif pos.count % 3 ~= 0 then
    return nil, "a mesh of unlisted triangles that is not a whole number of them"
  end

  return out
end

-- Every primitive of a mesh, each held to its buffer's length; the first
-- refusal, if there is one.
local function mesh_of(doc, index, has_bin)
  local m = type(doc.meshes) == "table" and math.type(index) == "integer"
            and doc.meshes[index + 1]

  if type(m) ~= "table" or type(m.primitives) ~= "table" or #m.primitives == 0 then
    return nil, "a mesh with nothing in it"
  end

  local parts = {}

  for i, prim in ipairs(m.primitives) do
    local part, why = primitive_of(doc, prim)

    if not part then return nil, why end

    for _, which in ipairs({ "buffer", "index_buffer" }) do
      if part[which] then
        local where, length = buffer_of(doc, part[which] - 1, has_bin)

        if not where then return nil, length end

        local last = which == "buffer"
                     and part.point_at + (part.points - 1) * part.point_stride + 12
                     or part.index_at + part.indices * part.index_bytes

        if length and last > length then return nil, "a mesh past the end of its buffer" end
      end
    end

    parts[i] = part
  end

  return parts, type(m.name) == "string" and m.name or nil
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

--------------------------------------------------------------------------
-- Other programs' nodes: a tree, each placed inside its parent, by a matrix
-- or by a translation, a rotation and a scale - all glTF's, Y up. What
-- Cafesa3D keeps is a place, a turn and a size in its own Z-up space, so
-- the tree is walked and each node's transform in the world worked out and
-- turned into those three.
--------------------------------------------------------------------------

-- A node's own transform, as glTF's column-major 4x4.
local function local_matrix(node)
  if type(node.matrix) == "table" then
    local m = {}

    for i = 1, 16 do
      m[i] = number(node.matrix[i], -1e6, 1e6)

      if not m[i] then return nil end
    end

    return m
  end

  local t = vec3(node.translation, -1e6, 1e6) or { 0, 0, 0 }
  local s = vec3(node.scale, -1e4, 1e4) or { 1, 1, 1 }
  local q = node.rotation
  local R = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }

  if type(q) == "table" and number(q[1], -2, 2) and number(q[2], -2, 2)
     and number(q[3], -2, 2) and number(q[4], -2, 2) then
    local n = math.sqrt(q[1] ^ 2 + q[2] ^ 2 + q[3] ^ 2 + q[4] ^ 2)

    if n > 1e-9 then R = quat_matrix({ q[1] / n, q[2] / n, q[3] / n, q[4] / n }) end
  end

  return { R[1] * s[1], R[4] * s[1], R[7] * s[1], 0,
           R[2] * s[2], R[5] * s[2], R[8] * s[2], 0,
           R[3] * s[3], R[6] * s[3], R[9] * s[3], 0,
           t[1],        t[2],        t[3],        1 }
end

local function mat_mul(a, b)
  local c = {}

  for j = 0, 3 do
    for i = 1, 4 do
      local v = 0

      for k = 0, 3 do v = v + a[k * 4 + i] * b[j * 4 + k + 1] end

      c[j * 4 + i] = v
    end
  end

  return c
end

-- A glTF world transform as Cafesa3D's place, turn and size, or nil for
-- one squashed flat. A mirrored node - a left wheel made from a right one
-- by a scale of -1 - keeps its handedness in a negative size along X.
local function decompose(m)
  local A = from_gltf_matrix({ m[1], m[5], m[9], m[2], m[6], m[10], m[3], m[7], m[11] })
  local R, size = {}, {}

  for j = 1, 3 do
    local x, y, z = A[j], A[3 + j], A[6 + j]

    size[j] = math.sqrt(x * x + y * y + z * z)

    if size[j] < 1e-9 then return nil end

    R[j], R[3 + j], R[6 + j] = x / size[j], y / size[j], z / size[j]
  end

  local det = R[1] * (R[5] * R[9] - R[6] * R[8]) - R[2] * (R[4] * R[9] - R[6] * R[7])
              + R[3] * (R[4] * R[8] - R[5] * R[7])

  if det < 0 then
    size[1] = -size[1]
    R[1], R[4], R[7] = -R[1], -R[4], -R[7]
  end

  return { m[13], -m[15], m[14] }, matrix_euler(R), size
end

-- The same two for a translator whose format places its objects by a
-- matrix too, as glTF's world transform - FBX's does - and says its colours
-- linear, as glTF does.
scenefile.place = decompose
scenefile.srgb = srgb

--
-- `has_bin` says a binary glTF's chunk came with the file (`from_glb`),
-- which is its first buffer when that buffer names no file of its own.
--
function scenefile.from_gltf(doc, has_bin)
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
                things = {}, skipped = 0, why = {}, buffers = {} }

  local world = type(doc.extras) == "table" and type(doc.extras.cafesa3d) == "table"
                and type(doc.extras.cafesa3d.world) == "table" and doc.extras.cafesa3d.world
  if world then
    out.world = { zenith = hexcolour(world.zenith), horizon = hexcolour(world.horizon),
                  strength = number(world.strength, 0, 10) }
  end

  -- How pictures of it are made, each number held to what the Render tab
  -- takes; one out of range and that one keeps Cafesa3D's own.
  local render = type(doc.extras) == "table" and type(doc.extras.cafesa3d) == "table"
                 and type(doc.extras.cafesa3d.render) == "table" and doc.extras.cafesa3d.render
  if render then
    local function whole(v, lo, hi)
      v = number(v, lo, hi)
      return v and math.floor(v) == v and math.floor(v) or nil
    end

    out.render = { w = whole(render.width, 16, 8192), h = whole(render.height, 16, 8192),
                   samples = whole(render.samples, 1, 4096),
                   view_samples = whole(render.view_samples, 1, 1024),
                   bounces = whole(render.bounces, 1, 32) }

    if render.integrator == "Preview" or render.integrator == "Final" then
      out.render.preview = render.integrator == "Preview"
    end
  end

  local function skip(name, why)
    out.skipped = out.skipped + 1
    out.why[#out.why + 1] = name .. ": " .. why
  end

  -- The scene's script, as the Script panel had it; one that is not a
  -- name and a text, or is longer than a megabyte, is skipped and said, and
  -- the scene opens without it.
  local script = type(doc.extras) == "table" and type(doc.extras.cafesa3d) == "table"
                 and doc.extras.cafesa3d.script

  if script then
    if type(script) ~= "table" or type(script.text) ~= "string" or not script_name(script.name) then
      skip("the script", "not a name and a text")
    elseif #script.text > MAX_SCRIPT then
      skip("the script", "longer than a megabyte")
    else
      out.script = { name = script_name(script.name), text = script.text }
    end
  end

  -- One node's objects: a shape, a lamp, the camera, or a mesh's parts.
  local function emit(node, name, own, loc, rot, scale)
    local lamp = type(node.extensions) == "table"
                 and type(node.extensions.KHR_lights_punctual) == "table"
                 and node.extensions.KHR_lights_punctual.light
    local before = #out.things

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
        local ax, ay, az = math.rad(rot[1]), math.rad(rot[2]), math.rad(rot[3])
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
    elseif math.type(node.mesh) == "integer" or own.kind == "mesh" then
      local parts, why = mesh_of(doc, math.type(node.mesh) == "integer" and node.mesh
                                      or own.mesh, has_bin)

      if parts then
        for i, part in ipairs(parts) do
          if #out.things >= MAX_OBJECTS then break end

          -- A part of its own material each: a Sketchfab car is one mesh
          -- of a dozen parts, paint and glass and chrome.
          local material_index = (#parts == 1 and math.type(own.material) == "integer")
                                  and own.material or part.material

          for _, b in ipairs({ part.buffer, part.index_buffer }) do
            if b and not out.buffers[b] then out.buffers[b] = (buffer_of(doc, b - 1, has_bin)) end
          end

          out.things[#out.things + 1] = {
            name = i == 1 and name or ("%s.%03d"):format(name, i - 1), kind = "mesh",
            loc = { loc[1], loc[2], loc[3] }, rot = { rot[1], rot[2], rot[3] },
            scale = { scale[1], scale[2], scale[3] }, smooth = true,
            smooth_angle = number(own.smooth_angle, 0, 180) or 30,
            mat = material(doc, material_index), mesh = part,
          }
        end
      else
        skip(name, why)
      end
    end

    -- Hidden with its eye closed, as it was saved; anything else shows.
    if #out.things > before and own.hidden == true then
      for i = before + 1, #out.things do out.things[i].hidden = true end
    end

    -- Made by a script, which the next Run of it replaces; a `by` that is
    -- not a name leaves the object made by hand, and kept.
    if script_name(own.by) then
      for i = before + 1, #out.things do out.things[i].by = script_name(own.by) end
    end
  end

  local seen = {}

  local function walk(index, parent, depth, n)
    if #out.things >= MAX_OBJECTS then
      if not out.full then
        out.full = true
        skip("the rest", "more than " .. MAX_OBJECTS .. " objects")
      end

      return
    end

    local node = math.type(index) == "integer" and nodes[index + 1]

    if type(node) ~= "table" then return skip("node " .. tostring(index), "not there") end
    if seen[index] then return skip(name_of(node, "node " .. index), "reached twice") end
    if depth > 64 then return skip(name_of(node, "node " .. index), "nested too deep") end

    seen[index] = true

    local name = name_of(node, "Object " .. n)
    local own = type(node.extras) == "table" and type(node.extras.cafesa3d) == "table"
                and node.extras.cafesa3d or nil
    local here = local_matrix(node)
    local world_m = here and (parent and mat_mul(parent, here) or here)

    if own then
      -- Cafesa3D's own numbers, exact, when the file has them; its nodes
      -- are never nested, so their place is their own.
      local loc, rot, scale = scenefile.transform(node)

      emit(node, name, own, vec3(own.loc, -1e6, 1e6) or loc, vec3(own.rot, -1e5, 1e5) or rot,
           vec3(own.scale, -1e4, 1e4) or scale)
    elseif not world_m then
      skip(name, "a matrix that is not sixteen numbers")
    else
      local loc, rot, scale = decompose(world_m)

      if loc then
        emit(node, name, {}, loc, rot, scale)
      elseif node.mesh ~= nil then
        skip(name, "squashed flat")
      end
    end

    if type(node.children) == "table" and world_m then
      for i, child in ipairs(node.children) do walk(child, world_m, depth + 1, n .. "." .. i) end
    end
  end

  for n, index in ipairs(list) do walk(index, nil, 1, tostring(n)) end

  return out
end

--
-- A binary glTF - `.glb`, what Sketchfab and Poly Haven hand out: a header,
-- the JSON as the first chunk and the buffer as the second. Returns the
-- JSON's text and the buffer's bytes, or nil and why; `from_gltf` is then
-- told the buffer came along.
--
function scenefile.from_glb(bytes)
  if type(bytes) ~= "string" or #bytes < 20 then return nil, "not a binary glTF file" end

  local magic, version, length = string.unpack("<c4I4I4", bytes)

  if magic ~= "glTF" then return nil, "not a binary glTF file" end
  if version ~= 2 then return nil, "a binary glTF of version " .. version .. ", not 2" end
  if length > #bytes then return nil, "a binary glTF cut short" end

  local at, text, bin = 13, nil, nil

  while at + 8 <= length + 1 do
    local size, kind = string.unpack("<I4c4", bytes, at)

    if at + 8 + size - 1 > length then return nil, "a chunk running past the file's end" end

    if kind == "JSON" and not text then
      text = bytes:sub(at + 8, at + 7 + size)
    elseif kind == "BIN\0" and not bin then
      bin = bytes:sub(at + 8, at + 7 + size)
    end

    at = at + 8 + size
  end

  if not text then return nil, "a binary glTF with no JSON in it" end

  return text, bin
end

--------------------------------------------------------------------------
-- Writing: a scene as the file `from_gltf` reads, and any other program's
-- glTF reader too.
--
--   local doc = scenefile.to_gltf({ name = ..., things = ..., world = ... },
--                                 { base64 = compress.base64, bounds = k3.bounds })
--   fs.write(path, json.encode(doc))
--
-- **The same file the samples are**, so there is one reader and it is
-- already held to the host test: each object a node whose `extras.cafesa3d`
-- keeps Cafesa3D's own numbers exactly - its kind, its shape's numbers,
-- its place, turn and size - and whose glTF transform says the same in
-- glTF's Y-up terms for everyone else. Materials in glTF's
-- metallic-roughness terms with the preset and the texture in `extras`;
-- lamps as `KHR_lights_punctual`; the camera as glTF's; the sky in the
-- file's own `extras`.
--
-- **Meshes are written as they were read**: their points already in
-- glTF's Y-up floats and their triangles in the width they came in, into
-- one buffer carried in the file as base64. A mesh copied with Shift D
-- shares its bytes with the original, and is written once.
--
-- `codec` holds the two loops over bytes, which in Kosmos are the
-- Compression Kit's `base64` and the 3D Kit's `bounds`, and on the host a
-- test's own.
--------------------------------------------------------------------------

-- Cafesa3D to glTF: (x, y, z) here is (x, z, -y) there.
local function to_gltf_vec(v) return { v[1], v[3], -v[2] } end

-- 0xRRGGBB as "#rrggbb", and as glTF's linear factors.
local function hex(c) return ("#%06x"):format(c & 0xffffff) end

local function linear(c)
  local out = {}

  for i = 1, 3 do
    local v = ((c >> (24 - 8 * i)) & 0xff) / 255

    out[i] = v <= 0.04045 and v / 12.92 or ((v + 0.055) / 1.055) ^ 2.4
  end

  return out
end

-- Rz Ry Rx from Euler degrees: the turn `matrix_euler` reads back.
local function euler_matrix(r)
  local x, y, z = math.rad(r[1]), math.rad(r[2]), math.rad(r[3])
  local cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y),
                                 math.cos(z), math.sin(z)

  return { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
           sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
           -sy,     cy * sx,                cy * cx }
end

-- G = C R C^T, the inverse of `from_gltf_matrix`.
local function to_gltf_matrix(R)
  local function r(i, j) return R[(i - 1) * 3 + j] end

  return { r(1, 1),  r(1, 3), -r(1, 2),
           r(3, 1),  r(3, 3), -r(3, 2),
          -r(2, 1), -r(2, 3),  r(2, 2) }
end

-- A rotation matrix as glTF's quaternion, x y z w.
local function quaternion(M)
  local function m(i, j) return M[(i - 1) * 3 + j] end

  local tr = m(1, 1) + m(2, 2) + m(3, 3)
  local x, y, z, w

  if tr > 0 then
    local s = math.sqrt(tr + 1) * 2

    w, x, y, z = s / 4, (m(3, 2) - m(2, 3)) / s, (m(1, 3) - m(3, 1)) / s, (m(2, 1) - m(1, 2)) / s
  elseif m(1, 1) > m(2, 2) and m(1, 1) > m(3, 3) then
    local s = math.sqrt(1 + m(1, 1) - m(2, 2) - m(3, 3)) * 2

    w, x, y, z = (m(3, 2) - m(2, 3)) / s, s / 4, (m(1, 2) + m(2, 1)) / s, (m(1, 3) + m(3, 1)) / s
  elseif m(2, 2) > m(3, 3) then
    local s = math.sqrt(1 + m(2, 2) - m(1, 1) - m(3, 3)) * 2

    w, x, y, z = (m(1, 3) - m(3, 1)) / s, (m(1, 2) + m(2, 1)) / s, s / 4, (m(2, 3) + m(3, 2)) / s
  else
    local s = math.sqrt(1 + m(3, 3) - m(1, 1) - m(2, 2)) * 2

    w, x, y, z = (m(2, 1) - m(1, 2)) / s, (m(1, 3) + m(3, 1)) / s, (m(2, 3) + m(3, 2)) / s, s / 4
  end

  return { x, y, z, w }
end

local function list3(v) return { v[1], v[2], v[3] } end

-- A material as glTF's, and what it says of itself that glTF cannot.
local function material_entry(m, name)
  local lin = linear(m.base or 0xcccccc)
  local own = { base = hex(m.base or 0xcccccc), preset = m.preset,
                metallic = m.metallic, rough = m.rough, trans = m.trans, ior = m.ior,
                emit = m.emit }
  local tx = m.texture

  if type(tx) == "table" then
    own.texture = { pattern = tx.pattern, colour2 = tx.colour2 and hex(tx.colour2) or nil,
                    scale = tx.scale, detail = tx.detail, distortion = tx.distortion,
                    bump = tx.bump, mortar = tx.mortar, ratio = tx.ratio, offset = tx.offset }
  end

  local entry = {
    name = name,
    pbrMetallicRoughness = { baseColorFactor = { lin[1], lin[2], lin[3], 1 },
                             metallicFactor = m.metallic or 0, roughnessFactor = m.rough or 0.5 },
    extras = { cafesa3d = own },
  }
  local ext = {}

  if (m.trans or 0) > 0 then
    ext.KHR_materials_transmission = { transmissionFactor = m.trans }
    ext.KHR_materials_ior = { ior = m.ior or 1.5 }
  end

  if (m.emit or 0) > 0 then
    entry.emissiveFactor = lin
    ext.KHR_materials_emissive_strength = { emissiveStrength = m.emit }
  end

  if next(ext) then entry.extensions = ext end

  return entry
end

-- What a material says, as one string: two objects of the same paint share
-- one material in the file, as they would in Blender.
local function material_key(m)
  local tx = type(m.texture) == "table" and m.texture or {}

  return table.concat({ tostring(m.base), tostring(m.preset), tostring(m.metallic),
                        tostring(m.rough), tostring(m.trans), tostring(m.ior),
                        tostring(m.emit), tostring(tx.pattern), tostring(tx.colour2),
                        tostring(tx.scale), tostring(tx.detail), tostring(tx.distortion),
                        tostring(tx.bump), tostring(tx.mortar), tostring(tx.ratio),
                        tostring(tx.offset) }, "|")
end

local SHAPE_FIELDS = { "size", "radius", "radius2", "depth", "segments", "rings",
                       "subdivisions" }

function scenefile.to_gltf(s, codec)
  local nodes, materials, lights, meshes, accessors, views = {}, {}, {}, {}, {}, {}
  local cameras = {}
  local by_key, by_points, by_mesh = {}, {}, {}
  local pieces, at = {}, 0

  local function material_index(m, name)
    local key = material_key(m)

    if not by_key[key] then
      materials[#materials + 1] = material_entry(m, name)
      by_key[key] = #materials - 1
    end

    return by_key[key]
  end

  -- Bytes into the one buffer, four-aligned, as a view of it.
  local function view(bytes, target)
    local pad = (4 - at % 4) % 4

    if pad > 0 then
      pieces[#pieces + 1] = string.rep("\0", pad)
      at = at + pad
    end

    views[#views + 1] = { buffer = 0, byteOffset = at, byteLength = #bytes, target = target }
    pieces[#pieces + 1] = bytes
    at = at + #bytes

    return #views - 1
  end

  -- The strings themselves are the keys, so a copy's bytes - which are
  -- the same string - are found without comparing megabytes.
  local function mesh_index(t, mat)
    local of = by_points[t.vertices] or {}
    local acc = of[t.triangles]

    by_points[t.vertices] = of

    if not acc then
      local x0, y0, z0, x1, y1, z1 = codec.bounds(t.vertices)
      local width = t.index_bytes or 4

      accessors[#accessors + 1] = { bufferView = view(t.vertices, 34962),
                                    componentType = 5126, count = #t.vertices // 12,
                                    type = "VEC3", min = { x0, y0, z0 },
                                    max = { x1, y1, z1 } }
      accessors[#accessors + 1] = { bufferView = view(t.triangles, 34963),
                                    componentType = ({ [1] = 5121, [2] = 5123,
                                                       [4] = 5125 })[width],
                                    count = #t.triangles // width, type = "SCALAR" }
      acc = { pos = #accessors - 2, idx = #accessors - 1 }
      of[t.triangles] = acc
    end

    local key = acc.pos .. " " .. mat

    if not by_mesh[key] then
      meshes[#meshes + 1] = { name = t.name, primitives = {
        { attributes = { POSITION = acc.pos }, indices = acc.idx, material = mat } } }
      by_mesh[key] = #meshes - 1
    end

    return by_mesh[key]
  end

  for _, t in ipairs(s.things) do
    local own = { kind = t.kind, loc = list3(t.loc), hidden = t.hidden or nil, by = t.by }
    local node = { name = t.name, translation = to_gltf_vec(t.loc), extras = { cafesa3d = own } }

    if t.kind == "light" then
      local lin = linear(t.colour or 0xffffff)

      own.radius, own.power, own.colour = t.radius, t.power, hex(t.colour or 0xffffff)
      lights[#lights + 1] = { name = t.name, type = "point", color = lin,
                              intensity = (t.power or 1000) / (4 * math.pi) }
      node.extensions = { KHR_lights_punctual = { light = #lights - 1 } }
    elseif t.kind == "camera" then
      -- Looking down its own -Z with +Y up, in glTF's space.
      local p, q = to_gltf_vec(t.loc), to_gltf_vec(t.target)
      local f = { q[1] - p[1], q[2] - p[2], q[3] - p[3] }
      local n = math.sqrt(f[1] ^ 2 + f[2] ^ 2 + f[3] ^ 2)

      own.target, own.focal = list3(t.target), t.focal or 50

      if n > 1e-9 then
        f = { f[1] / n, f[2] / n, f[3] / n }

        local r = { -f[3], 0, f[1] }
        local rn = math.sqrt(r[1] ^ 2 + r[3] ^ 2)

        if rn > 1e-9 then
          r = { r[1] / rn, 0, r[3] / rn }

          local u = { r[2] * f[3] - r[3] * f[2], r[3] * f[1] - r[1] * f[3],
                      r[1] * f[2] - r[2] * f[1] }

          node.rotation = quaternion({ r[1], u[1], -f[1],
                                       r[2], u[2], -f[2],
                                       r[3], u[3], -f[3] })
        end
      end

      -- A 36 mm sensor seen at 16:9, as the samples' cameras are.
      cameras[#cameras + 1] = { type = "perspective", name = t.name,
                                perspective = { yfov = 2 * math.atan(10.125 / (t.focal or 50)),
                                                aspectRatio = 16 / 9, znear = 0.1,
                                                zfar = 1000 } }
      node.camera = #cameras - 1
    else
      local mat = material_index(t.mat or {}, t.name)

      own.rot, own.scale, own.material = list3(t.rot), list3(t.scale), mat
      own.smooth = t.smooth or false
      node.rotation = quaternion(to_gltf_matrix(euler_matrix(t.rot)))
      node.scale = { t.scale[1], t.scale[3], t.scale[2] }

      if t.kind == "mesh" then
        own.mesh = mesh_index(t, mat)
        own.smooth, own.smooth_angle = true, t.smooth_angle or 30
        node.mesh = own.mesh
      else
        for _, field in ipairs(SHAPE_FIELDS) do
          local v = t[field]

          own[field] = type(v) == "table" and list3(v) or v
        end
      end
    end

    nodes[#nodes + 1] = node
  end

  local list = {}

  for i = 1, #nodes do list[i] = i - 1 end

  local doc = {
    asset = { version = "2.0", generator = "Cafesa3D, Kosmos" },
    scene = 0,
    scenes = { { name = s.name or "Scene", nodes = list } },
    nodes = nodes,
    extensionsUsed = { "KHR_lights_punctual", "KHR_materials_transmission",
                       "KHR_materials_ior", "KHR_materials_emissive_strength" },
  }

  if #materials > 0 then doc.materials = materials end
  if #cameras > 0 then doc.cameras = cameras end
  if #lights > 0 then doc.extensions = { KHR_lights_punctual = { lights = lights } } end

  local own = {}

  if s.world then
    own.world = { zenith = hex(s.world.zenith), horizon = hex(s.world.horizon),
                  strength = s.world.strength }
  end

  if s.render then
    local r = s.render

    own.render = { width = r.w, height = r.h, samples = r.samples,
                   view_samples = r.view_samples, bounces = r.bounces,
                   integrator = r.preview and "Preview" or "Final" }
  end

  if s.script then own.script = { name = s.script.name, text = s.script.text } end

  if next(own) then doc.extras = { cafesa3d = own } end

  if #meshes > 0 then
    local bytes = table.concat(pieces)

    doc.meshes, doc.accessors, doc.bufferViews = meshes, accessors, views
    doc.buffers = { { byteLength = #bytes,
                      uri = "data:application/octet-stream;base64," .. codec.base64(bytes) } }
  end

  return doc
end

return scenefile
