-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Cafesa3D's scene files on this machine (`roadmap.md` 4l): the three
-- samples read back as the scenes `tools/cafesa3d_samples.py` describes, and
-- the file's two descriptions of every object held to each other.
--
--   lua tools/test_scenefile.lua DIR      DIR holding house, car and plane
--
-- **The two descriptions are the point.** Each node says where it is twice:
-- as glTF says - Y up, a quaternion, written by Python - and as Cafesa3D
-- keeps it, Z up and Euler angles. The reader takes the second and can
-- convert the first, so converting every node's glTF transform must land
-- exactly where its own numbers say; the writer's conversion and the
-- reader's inverse are two pieces of arithmetic in two languages, and this
-- is the only thing that can tell they disagree. Likewise each camera's
-- rotation against its target, and each material's linear colour against
-- its hex.
--
-- And the refusals, because a scene is a file from outside: a sphere of two
-- segments, a box with no sides, a file that is not glTF, a mesh of another
-- program's triangles, and more objects than a scene may have.

local json = assert(loadfile("user/lib/json.lua"))()
local scenefile = assert(loadfile("user/lib/scenefile.lua"))()
local dir = arg[1] or "build/host/scenes"

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local function euler_matrix(r)
  local ax, ay, az = math.rad(r[1]), math.rad(r[2]), math.rad(r[3])
  local cx, sx, cy, sy = math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay)
  local cz, sz = math.cos(az), math.sin(az)

  return { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
           sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
           -sy, cy * sx, cy * cx }
end

-- Two turns alike: their matrices, since one turn has more than one set of
-- Euler angles.
local function same_turn(a, b)
  local A, B = euler_matrix(a), euler_matrix(b)

  for i = 1, 9 do
    if math.abs(A[i] - B[i]) > 1e-5 then return false end
  end

  return true
end

local function close3(a, b, eps)
  return math.abs(a[1] - b[1]) < eps and math.abs(a[2] - b[2]) < eps and math.abs(a[3] - b[3]) < eps
end

local function read(name)
  local f = io.open(dir .. "/" .. name .. ".gltf", "rb")

  if not f then return nil, "no " .. name .. ".gltf in " .. dir end

  local text = f:read("a")

  f:close()

  local doc, why = json.decode(text)

  if not doc then return nil, why end

  return doc
end

local expect = { house = { 202, "House" }, car = { 76, "Car" }, plane = { 106, "Plane" } }

-- base64, by hand, for the host: the app decodes with `k3.unbase64`, in C.
local B64 = {}

for i = 1, 64 do
  B64[("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"):byte(i)] = i - 1
end

local function unbase64(text)
  local out, acc, bits = {}, 0, 0

  for i = 1, #text do
    local v = B64[text:byte(i)]

    if not v then break end

    acc = ((acc << 6) | v) & 0xffffff
    bits = bits + 6

    if bits >= 8 then
      bits = bits - 8
      out[#out + 1] = string.char((acc >> bits) & 0xff)
    end
  end

  return table.concat(out)
end
local scenes = {}

for _, name in ipairs({ "house", "car", "plane" }) do
  local doc, why = read(name)

  check(doc ~= nil, name .. ".gltf is JSON: " .. tostring(why))

  if doc then
    local scene = scenefile.from_gltf(doc)

    scenes[name] = scene
    check(scene and scene.name == expect[name][2] and #scene.things == expect[name][1]
          and scene.skipped == 0,
          ("%s reads as %d objects named %s, none skipped: %d, %s, %d skipped"):format(
            name, expect[name][1], expect[name][2], scene and #scene.things or -1,
            scene and scene.name or "?", scene and scene.skipped or -1))

    local names, lights, cameras, consistent, looks, colours = {}, 0, 0, true, true, true

    for i, t in ipairs(scene and scene.things or {}) do
      names[t.name] = (names[t.name] or 0) + 1
      lights = lights + (t.kind == "light" and 1 or 0)
      cameras = cameras + (t.kind == "camera" and 1 or 0)

      local node = doc.nodes[i]
      local loc, rot, scale = scenefile.transform(node)
      local own = node.extras.cafesa3d

      if t.kind ~= "camera" and t.kind ~= "light" then
        if not (close3(loc, own.loc, 1e-5) and same_turn(rot, own.rot)
                and close3(scale, own.scale, 1e-9)) then
          consistent = false
          print(("    %s: glTF gives %.4f %.4f %.4f / %.2f %.2f %.2f, own %.4f %.4f %.4f / %.2f %.2f %.2f")
                :format(t.name, loc[1], loc[2], loc[3], rot[1], rot[2], rot[3],
                        own.loc[1], own.loc[2], own.loc[3], own.rot[1], own.rot[2], own.rot[3]))
        end

        -- The linear colour glTF has, against the hex Cafesa3D kept.
        local m = doc.materials[own.material + 1]
        local f = m.pbrMetallicRoughness.baseColorFactor
        local hex = tonumber(m.extras.cafesa3d.base:sub(2), 16)

        for k = 1, 3 do
          local c = math.max(0, math.min(1, f[k]))
          local s = c <= 0.0031308 and c * 12.92 or 1.055 * c ^ (1 / 2.4) - 0.055

          if math.abs(s * 255 - ((hex >> (24 - 8 * k)) & 0xff)) > 1 then colours = false end
        end
      elseif t.kind == "camera" then
        -- Where its glTF rotation points, against its own target.
        local copy = { name = node.name, translation = node.translation, rotation = node.rotation,
                       camera = node.camera }
        local derived = scenefile.from_gltf({ asset = { version = "2.0" }, nodes = { copy },
                                              scenes = { { nodes = { 0 } } } }).things[1]
        local d1 = { t.target[1] - t.loc[1], t.target[2] - t.loc[2], t.target[3] - t.loc[3] }
        local d2 = { derived.target[1] - derived.loc[1], derived.target[2] - derived.loc[2],
                     derived.target[3] - derived.loc[3] }
        local n1 = math.sqrt(d1[1] ^ 2 + d1[2] ^ 2 + d1[3] ^ 2)
        local n2 = math.sqrt(d2[1] ^ 2 + d2[2] ^ 2 + d2[3] ^ 2)

        looks = close3({ d1[1] / n1, d1[2] / n1, d1[3] / n1 }, { d2[1] / n2, d2[2] / n2, d2[3] / n2 }, 1e-4)
      end
    end

    local unique = true

    for _, n in pairs(names) do unique = unique and n == 1 end

    -- Every mesh against its own accessors: each index a point it has, and
    -- every point inside the bounds the file records for its accessor.
    local bytes = scene and scene.buffers[1] and unbase64(scene.buffers[1])
    local meshes, inside, indexed = 0, true, true

    for i, t in ipairs(scene and scene.things or {}) do
      if t.kind == "mesh" then
        local m = t.mesh
        local acc = doc.accessors[doc.meshes[doc.nodes[i].mesh + 1].primitives[1].attributes.POSITION + 1]

        meshes = meshes + 1

        for k = 0, m.points - 1 do
          local x, y, z = string.unpack("<fff", bytes, m.point_at + k * 12 + 1)

          for a, v in ipairs({ x, y, z }) do
            if v < acc.min[a] - 1e-4 or v > acc.max[a] + 1e-4 then inside = false end
          end
        end

        for k = 0, m.indices - 1 do
          local index = string.unpack("<I" .. m.index_bytes, bytes, m.index_at + k * m.index_bytes + 1)

          if index >= m.points then indexed = false end
        end
      end
    end

    check(meshes > 0 and inside, name .. ": every mesh's points inside its accessor's bounds")
    check(indexed, name .. ": every mesh's triangles name points it has")

    check(unique, name .. ": every object's name its own")
    check(lights == 1 and cameras == 1, name .. ": one lamp and one camera")
    check(consistent, name .. ": every node's glTF transform lands where its own numbers say")
    check(looks, name .. ": the camera's glTF rotation looks at its own target")
    check(colours, name .. ": every material's linear colour is its hex, to a step")
  end
end

-- A few objects exactly as the samples describe them.
local function find(scene, n)
  for _, t in ipairs(scene and scene.things or {}) do
    if t.name == n then return t end
  end
end

local roof = find(scenes.house, "Roof")

check(roof and roof.kind == "mesh" and roof.mat.texture and roof.mat.texture.pattern == "shingles"
      and roof.mat.texture.bump > 0,
      "the house's roof is a mesh of overlapping tiles, standing proud of each other")

local wall = find(scenes.house, "Wall")

check(wall and wall.kind == "box" and wall.mat.texture and wall.mat.texture.pattern == "brick"
      and wall.mat.texture.scale == 13 and wall.mat.texture.colour2 == 0xd8d0c2,
      "its walls are boxes of brick, thirteen courses a metre, in pale mortar")

local tyre = find(scenes.car, "Tyre.003")

check(tyre and tyre.kind == "mesh" and same_turn(tyre.rot, { 90, 0, 0 })
      and close3(tyre.loc, { -1.36, -0.8, 0.365 }, 1e-6),
      "the car's fourth tyre is a mesh turned a quarter, where it belongs")

local front = find(scenes.car, "Tyre")

check(front and same_turn(front.rot, { 90, 0, 12 }), "its front tyre is steered twelve degrees")

local glasshouse = find(scenes.car, "Glasshouse")

check(glasshouse and glasshouse.kind == "mesh" and glasshouse.mat.base == 0x0e1014
      and glasshouse.mat.rough == 0.04, "its glasshouse is a mesh of dark, glossy glass")

local head = find(scenes.car, "Headlight")

check(head and head.mat.emit == 8 and head.mat.base == 0xfff6d8, "a headlight glows")

local wing = find(scenes.plane, "Wing")

check(wing and wing.kind == "mesh" and wing.smooth_angle == 40, "the plane's wing is a lofted mesh")

local cap = find(scenes.plane, "Tail cap")

check(cap and cap.kind == "sphere", "and its tail is closed with a sphere")

-- Refusals.
local function one(own, extra)
  local node = { name = "X", extras = { cafesa3d = own } }

  for k, v in pairs(extra or {}) do node[k] = v end

  return scenefile.from_gltf({ asset = { version = "2.0" }, nodes = { node },
                               scenes = { { nodes = { 0 } } } })
end

local s = one({ kind = "sphere", radius = 1, segments = 2, rings = 8 })

check(s and #s.things == 0 and s.skipped == 1 and s.why[1]:find("segments"),
      "a sphere of two segments is skipped and said to be")
s = one({ kind = "box" })
check(s and #s.things == 0 and s.skipped == 1, "a box with no sides is skipped")
s = one({ kind = "sphere", radius = -1 })
check(s and #s.things == 0 and s.skipped == 1, "a negative radius is skipped")
s = one({ kind = "sphere", radius = 1, segments = 7.5 })
check(s and #s.things == 0 and s.skipped == 1, "half a segment is skipped")
s = one({}, { mesh = 0 })
check(s and s.skipped == 1 and s.why[1]:find("mesh"), "a mesh the file does not have is skipped")

-- A mesh whose accessor says more points than its buffer holds, and one
-- whose buffer is somewhere else.
do
  local function doc_with(count, uri)
    return {
      asset = { version = "2.0" }, scenes = { { nodes = { 0 } } },
      nodes = { { name = "M", mesh = 0 } },
      meshes = { { primitives = { { attributes = { POSITION = 0 }, indices = 1 } } } },
      accessors = { { bufferView = 0, componentType = 5126, count = count, type = "VEC3" },
                    { bufferView = 1, componentType = 5125, count = 3, type = "SCALAR" } },
      bufferViews = { { buffer = 0, byteOffset = 0, byteLength = 36 },
                      { buffer = 0, byteOffset = 36, byteLength = 12 } },
      buffers = { { byteLength = 48, uri = uri } },
    }
  end

  local good = "data:application/octet-stream;base64," .. string.rep("A", 64)

  s = scenefile.from_gltf(doc_with(3, good))
  check(s and #s.things == 1 and s.things[1].kind == "mesh", "another program's mesh is read")
  s = scenefile.from_gltf(doc_with(300, good))
  check(s and #s.things == 0 and s.skipped == 1, "a mesh that says more than its buffer holds is skipped")
  s = scenefile.from_gltf(doc_with(3, "mesh.bin"))
  check(s and #s.things == 0 and s.why[1]:find("not in the file"),
        "a mesh whose buffer is another file is skipped, and says why")
end
check(scenefile.from_gltf({ asset = { version = "1.0" } }) == nil, "glTF 1.0 is not read")
check(scenefile.from_gltf("text") == nil, "a string is not a scene")

do
  local nodes, list = {}, {}

  for i = 1, 2100 do
    nodes[i] = { name = "C" .. i, extras = { cafesa3d = { kind = "box", size = { 1, 1, 1 } } } }
    list[i] = i - 1
  end

  local big = scenefile.from_gltf({ asset = { version = "2.0" }, nodes = nodes,
                                    scenes = { { nodes = list } } })

  check(big and #big.things == 2000 and big.skipped == 1, "a scene stops at 2000 objects")
end

if failed > 0 then
  print(("FAIL: %d of %d checks on the scene files"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the scene files (the house, the car and the plane read back as "
       .. "described; every node's glTF transform, every camera's rotation and every "
       .. "material's linear colour held to Cafesa3D's own numbers; every mesh held to its "
       .. "accessors; another program's mesh read; nine refusals)"):format(passed))
