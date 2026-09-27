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
--
-- And saving (`scenefile.to_gltf`), which is the same file the other way:
-- each sample written and read back must be the scene it was.

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
    local bytes = scene and scene.buffers[1] and unbase64(scene.buffers[1].base64)
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

-- Which script made an object: a name, or the object is one made by hand.
s = one({ kind = "box", size = { 1, 1, 1 }, by = "staircase" })
check(s and #s.things == 1 and s.things[1].by == "staircase", "an object a script made says which")

for _, bad in ipairs({ 7, "", "two\nlines", { "staircase" } }) do
  s = one({ kind = "box", size = { 1, 1, 1 }, by = bad })
  check(s and #s.things == 1 and s.things[1].by == nil,
        "a by of " .. tostring(bad) .. " leaves the object made by hand, and kept")
end

-- The scene's script: a name and a text, or it is skipped and the scene
-- opens without it.
local function with_script(script)
  return scenefile.from_gltf({ asset = { version = "2.0" }, scenes = { { nodes = {} } },
                               extras = { cafesa3d = { script = script } } })
end

s = with_script({ name = "staircase", text = "scene.box{}\n" })
check(s and s.script and s.script.name == "staircase" and s.script.text == "scene.box{}\n"
      and s.skipped == 0, "a scene's script is read, its name and its text")
s = with_script({ name = "staircase" })
check(s and s.script == nil and s.skipped == 1 and s.why[1]:find("the script"),
      "a script with no text is skipped and said to be")
s = with_script({ text = "print(1)\n" })
check(s and s.script == nil and s.skipped == 1, "a script with no name is skipped")
s = with_script("print(1)")
check(s and s.script == nil and s.skipped == 1, "a script that is only a string is skipped")
s = with_script({ name = "big", text = string.rep("-", (1 << 20) + 1) })
check(s and s.script == nil and s.skipped == 1 and s.why[1]:find("megabyte"),
      "a script longer than a megabyte is skipped and said to be")
s = with_script({ name = string.rep("n", 100), text = "" })
check(s and s.script and #s.script.name == 63, "a script's name is cut to 63")

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
  check(s and #s.things == 1 and s.buffers[1] and s.buffers[1].file == "mesh.bin",
        "a mesh whose buffer is a file beside it is read, naming the file for the caller")

  for _, far in ipairs({ "http://example.com/mesh.bin", "/Home/mesh.bin", "../mesh.bin" }) do
    s = scenefile.from_gltf(doc_with(3, far))
    check(s and #s.things == 0 and s.why[1]:find("somewhere else"),
          "a mesh whose buffer is " .. far .. " is skipped: opening fetches nothing")
  end
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

--
-- **Saving, and reading back what was saved** (`scenefile.to_gltf`). Each
-- sample is read as Cafesa3D reads it - its meshes' bytes sliced out of the
-- buffer, as `open_scene` does - then written, encoded, decoded and read
-- again, and must come back the same scene: every object's name, kind,
-- numbers, place, turn and size, material and texture, a mesh's bytes, the
-- lamps, the camera and the sky. One object is hidden first, and must stay
-- hidden. The two loops over bytes are this file's own here; in Kosmos
-- they are the 3D Kit's.
--
local B64_DIGITS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local codec = {}

function codec.base64(bytes)
  local out = {}

  for i = 1, #bytes, 3 do
    local a, b, c = bytes:byte(i, i + 2)
    local v = (a << 16) | ((b or 0) << 8) | (c or 0)

    out[#out + 1] = B64_DIGITS:sub((v >> 18) + 1, (v >> 18) + 1)
                    .. B64_DIGITS:sub(((v >> 12) & 63) + 1, ((v >> 12) & 63) + 1)
                    .. (b and B64_DIGITS:sub(((v >> 6) & 63) + 1, ((v >> 6) & 63) + 1) or "=")
                    .. (c and B64_DIGITS:sub((v & 63) + 1, (v & 63) + 1) or "=")
  end

  return table.concat(out)
end

function codec.bounds(points)
  local lo, hi = {}, {}

  for i = 1, #points, 12 do
    local p = { string.unpack("<fff", points, i) }

    for k = 1, 3 do
      lo[k] = (lo[k] == nil or p[k] < lo[k]) and p[k] or lo[k]
      hi[k] = (hi[k] == nil or p[k] > hi[k]) and p[k] or hi[k]
    end
  end

  return lo[1], lo[2], lo[3], hi[1], hi[2], hi[3]
end

-- A scene's meshes as Cafesa3D holds them: their points packed three floats
-- apart and their triangles listed, whatever the file did - which is the
-- work `open_scene` hands to `k3.gather` and `k3.sequence` in Kosmos.
local function as_app(scene, bin)
  local buffers = {}

  for i, b in pairs(scene.buffers) do
    buffers[i] = b.base64 and unbase64(b.base64) or (b.bin and bin) or nil
  end

  for _, t in ipairs(scene.things) do
    if t.kind == "mesh" then
      local m, bytes = t.mesh, buffers[t.mesh.buffer]
      local points = {}

      for k = 0, m.points - 1 do
        points[#points + 1] = bytes:sub(m.point_at + k * m.point_stride + 1,
                                        m.point_at + k * m.point_stride + 12)
      end

      t.vertices = table.concat(points)

      if m.indices then
        local ib = buffers[m.index_buffer]

        t.triangles = ib:sub(m.index_at + 1, m.index_at + m.indices * m.index_bytes)
        t.index_bytes = m.index_bytes
      else
        local list = {}

        for k = 0, m.points - 1 do list[#list + 1] = string.pack("<I4", k) end

        t.triangles, t.index_bytes = table.concat(list), 4
      end

      t.mesh = nil
    end
  end

  return scene
end

-- Two values alike, a number to ten significant digits, which is how
-- `json.encode` writes one; the first difference, as a path, or nil.
local function differs(a, b, path)
  if type(a) == "number" and type(b) == "number" then
    return math.abs(a - b) > 1e-8 * math.max(1, math.abs(a)) and path or nil
  end

  if type(a) ~= "table" or type(b) ~= "table" then
    return a ~= b and path or nil
  end

  for k, v in pairs(a) do
    local d = differs(v, b[k], path .. "." .. tostring(k))

    if d then return d end
  end

  for k in pairs(b) do
    if a[k] == nil then return path .. "." .. tostring(k) end
  end
end

for _, name in ipairs({ "house", "car", "plane" }) do
  local doc = read(name)
  local first = doc and as_app(scenefile.from_gltf(doc))

  if first then
    first.things[3].hidden = true
    first.render = { w = 3440, h = 1440, samples = 512, view_samples = 32, bounces = 8,
                     preview = true }
    -- A script, and an object it made: every character of the text back,
    -- quotes, a backslash, a tab and a line with no end included.
    first.script = { name = "staircase",
                     text = 'scene.box{ name = "Step" } -- a \\ and a\ttab\nprint("done")' }
    first.things[4].by = "staircase"

    local text = json.encode(scenefile.to_gltf(first, codec))
    local again = as_app(scenefile.from_gltf(json.decode(text)))
    local d

    check(#again.things == #first.things and again.skipped == 0,
          ("%s saved and read back has %d objects of %d"):format(name, #again.things,
                                                                #first.things))

    for i = 1, math.min(#first.things, #again.things) do
      d = d or differs(first.things[i], again.things[i], name .. "[" .. i .. "]")
    end

    check(d == nil, name .. " saved is not the scene it was: " .. tostring(d))
    check(differs(first.world, again.world, "world") == nil, name .. "'s sky came back otherwise")
    local rd = differs(first.render, again.render, "render")

    check(rd == nil, name .. "'s render settings came back otherwise: " .. tostring(rd))
    check(again.name == first.name, name .. " came back named " .. tostring(again.name))
    check(again.things[3].hidden == true and again.things[4].hidden == nil,
          name .. "'s hidden object did not come back hidden, and only it")
    check(again.things[4].by == "staircase" and again.things[3].by == nil
          and again.things[5].by == nil,
          name .. "'s object made by its script did not come back so, and only it")
    check(differs(first.script, again.script, "script") == nil,
          name .. "'s script came back otherwise: " .. tostring((differs(first.script, again.script, "script"))))

    -- What another program sees: every node's glTF transform where its own
    -- numbers say. The round trip above cannot tell, because the reader
    -- takes the numbers of Cafesa3D's own when the file has them.
    local written, wrong = json.decode(text), nil

    for _, node in ipairs(written.nodes) do
      local own = node.extras.cafesa3d

      if own.rot then
        local loc, rot, scale = scenefile.transform(node)

        if not (close3(loc, own.loc, 1e-6) and same_turn(rot, own.rot)
                and close3(scale, own.scale, 1e-6)) then
          wrong = wrong or node.name
        end
      end
    end

    check(wrong == nil, name .. " written, " .. tostring(wrong)
          .. "'s glTF transform is not where its own numbers say")

    -- And the writing is stable: the same scene written twice is the same
    -- file, so a save that changed nothing changes no bytes.
    check(json.encode(scenefile.to_gltf(again, codec)) == text,
          name .. " written twice is two different files")
  end
end

--
-- **Other programs' files** - the shapes Blender, Sketchfab and Poly Haven
-- write and Cafesa3D's own never does: nodes inside nodes, a matrix instead
-- of a translation, a rotation and a scale, a mirrored node, a mesh of
-- several parts each with its own material, triangles not listed, points
-- interleaved with the rest of a vertex, a mesh compressed with Draco, and
-- the whole of it as a binary `.glb`.
--
do
  local function tri(x) return string.pack("<fffffffff", x, 0, 0, x + 1, 0, 0, x, 1, 0) end
  local bytes = tri(0) .. tri(5) .. string.pack("<I4I4I4", 0, 1, 2)
  local uri = "data:application/octet-stream;base64," .. codec.base64(bytes)
  local q = math.sqrt(0.5)
  local doc = {
    asset = { version = "2.0" }, scene = 0,
    scenes = { { name = "Theirs", nodes = { 0, 3, 4, 5 } } },
    nodes = {
      { name = "Car", translation = { 1, 2, 3 }, rotation = { 0, q, 0, q }, children = { 1 } },
      { name = "Wheel", translation = { 0, 0, 1 }, mesh = 0, children = { 2 } },
      { name = "Hubcap", matrix = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 5, 0, 0, 1 }, mesh = 1 },
      { name = "Mirrored", scale = { -1, 1, 1 }, mesh = 1 },
      { name = "Squeezed", mesh = 2 },
      { name = "Strided", mesh = 3 },
    },
    meshes = {
      { primitives = { { attributes = { POSITION = 0 }, indices = 2, material = 0 },
                       { attributes = { POSITION = 1 }, material = 1 } } },
      { primitives = { { attributes = { POSITION = 0 }, indices = 2 } } },
      { primitives = { { attributes = { POSITION = 0 },
                         extensions = { KHR_draco_mesh_compression = { bufferView = 0 } } } } },
      { primitives = { { attributes = { POSITION = 3 } } } },
    },
    materials = { { pbrMetallicRoughness = { baseColorFactor = { 1, 0, 0, 1 } } },
                  { pbrMetallicRoughness = { baseColorFactor = { 0, 0, 1, 1 } } } },
    accessors = {
      { bufferView = 0, componentType = 5126, count = 3, type = "VEC3" },
      { bufferView = 1, componentType = 5126, count = 3, type = "VEC3" },
      { bufferView = 2, componentType = 5125, count = 3, type = "SCALAR" },
      { bufferView = 3, componentType = 5126, count = 3, type = "VEC3" },
    },
    bufferViews = { { buffer = 0, byteOffset = 0, byteLength = 36 },
                    { buffer = 0, byteOffset = 36, byteLength = 36 },
                    { buffer = 0, byteOffset = 72, byteLength = 12 },
                    { buffer = 0, byteOffset = 0, byteLength = 72, byteStride = 24 } },
    buffers = { { byteLength = #bytes, uri = uri } },
  }

  local function by(scene, name)
    for _, t in ipairs(scene.things) do if t.name == name then return t end end
  end

  local theirs = scenefile.from_gltf(doc)
  local wheel, second = theirs and by(theirs, "Wheel"), theirs and by(theirs, "Wheel.001")
  local hub, mirror = theirs and by(theirs, "Hubcap"), theirs and by(theirs, "Mirrored")
  local strided = theirs and by(theirs, "Strided")

  -- The wheel: a metre along the car's own Z, which the car's quarter turn
  -- about glTF's Y makes a metre along X - here, (2, -3, 2), turned a
  -- quarter about Z, since glTF's Y is here's Z.
  check(wheel and close3(wheel.loc, { 2, -3, 2 }, 1e-9) and same_turn(wheel.rot, { 0, 0, 90 }),
        "a node inside a node is where its parent puts it: " .. tostring(wheel and
        table.concat(wheel.loc, " ")))
  check(wheel and second and wheel.mat.base == 0xff0000 and second.mat.base == 0x0000ff
        and second.mesh.indices == nil,
        "a mesh of two parts is two objects with their own materials, the second's triangles unlisted")
  -- The hubcap: five along the wheel's own X, which the car's turn makes
  -- five along glTF's -Z - so (2, 2, -2) there, (2, 2, 2) here.
  check(hub and close3(hub.loc, { 2, 2, 2 }, 1e-9) and same_turn(hub.rot, { 0, 0, 90 }),
        "a matrix inside a node inside a node is where both put it: "
        .. tostring(hub and table.concat(hub.loc, " ")))
  check(hub and close3(hub.scale, { 2, 2, 2 }, 1e-9),
        "a node's matrix gives its size: " .. tostring(hub and table.concat(hub.scale, " ")))
  check(mirror and close3(mirror.scale, { -1, 1, 1 }, 1e-9) and same_turn(mirror.rot, { 0, 0, 0 }),
        "a mirrored node keeps its handedness in a negative size along X")
  check(strided and strided.mesh.point_stride == 24 and #as_app(scenefile.from_gltf(doc))
        .things > 0, "points interleaved with the rest of a vertex are read at their spacing")

  local squeezed = false

  for _, w in ipairs(theirs and theirs.why or {}) do
    squeezed = squeezed or (w:find("^Squeezed") and w:find("Draco")) ~= nil
  end

  check(squeezed, "a mesh compressed with Draco is skipped, and says so")

  local app = as_app(scenefile.from_gltf(doc))
  local s2 = by(app, "Strided")

  -- Every 24 bytes from the start: the first triangle's first and third
  -- points, then the second triangle's second.
  check(s2 and s2.vertices == string.pack("<fffffffff", 0, 0, 0, 0, 1, 0, 6, 0, 0),
        "interleaved points come out packed, each the one at its spacing")

  -- **The same car, as a `.glb`**: the JSON chunk with its buffer's `uri`
  -- gone, and the buffer's bytes in the binary chunk.
  local car = read("car")
  local raw = unbase64(car.buffers[1].uri:match("base64,(.*)$"))

  car.buffers[1].uri = nil

  local text = json.encode(car)
  local pad = function(s, with) return s .. string.rep(with, (4 - #s % 4) % 4) end
  local j, b = pad(text, " "), pad(raw, "\0")
  local glb = string.pack("<c4I4I4", "glTF", 2, 12 + 8 + #j + 8 + #b)
              .. string.pack("<I4c4", #j, "JSON") .. j .. string.pack("<I4c4", #b, "BIN\0") .. b
  local got_text, got_bin = scenefile.from_glb(glb)
  local from_glb = got_text and scenefile.from_gltf(json.decode(got_text), true)
  local from_gltf = scenefile.from_gltf(read("car"))

  check(from_glb and #from_glb.things == #from_gltf.things and from_glb.buffers[1].bin == true,
        "the car as a .glb reads as the car")
  check(from_glb and differs(as_app(from_gltf).things, as_app(from_glb, got_bin).things, "car")
        == nil, "the car as a .glb is the same scene, mesh bytes and all")
  check(scenefile.from_glb("glTF" .. string.pack("<I4I4", 1, 20) .. string.rep("\0", 8)) == nil,
        "a binary glTF of version 1 is not read")
  check(scenefile.from_glb(string.sub(glb, 1, 100)) == nil, "a binary glTF cut short is not read")
end

if failed > 0 then
  print(("FAIL: %d of %d checks on the scene files"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the scene files (the house, the car and the plane read back as "
       .. "described; every node's glTF transform, every camera's rotation and every "
       .. "material's linear colour held to Cafesa3D's own numbers; every mesh held to its "
       .. "accessors; another program's mesh read; nine refusals, and a script's and a by's; "
       .. "and each saved, read back the same scene with its hidden object hidden, its "
       .. "script and what it made, its glTF transforms where "
       .. "its own numbers say, and the same file when written twice)"):format(passed))
