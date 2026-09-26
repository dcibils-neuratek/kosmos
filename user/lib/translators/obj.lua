-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Wavefront OBJ for Cafesa3D, with its MTL: the oldest common format and
-- still everywhere (TurboSquid, CGTrader, Free3D, NASA).
--
-- **A translator** (`roadmap.md` 4l, 5c): one file in `/lib/translators/`,
-- handed the 3D Kit's loops over bytes and nothing of the file system. An
-- OBJ's materials are in another file, the MTL it names; `context.sidecar`
-- is how that one is asked for, and only by its name beside the OBJ.
--
-- **Each run of faces under one object and one material is an object**,
-- as Blender splits them. Y is up, as glTF's is, which is how a mesh is
-- kept here, so points go through untouched.

local obj = {
  name = "Wavefront OBJ",
  reads = { "obj" },
  writes = { "obj" },
}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

-- An MTL colour, three numbers nought to one, as 0xRRGGBB. MTL's colours are
-- what the eye sees - sRGB, as the colours here are - so they go through.
local function colour(r, g, b)
  local function c(v) return math.floor(clamp(tonumber(v) or 0, 0, 1) * 255 + 0.5) end

  return (c(r) << 16) | (c(g) << 8) | c(b)
end

-- The materials an MTL names, in the Material tab's terms: `Kd` the colour,
-- `Ns` how sharp the shine (a high exponent is a smooth surface), `d` or
-- `Tr` how much light passes, `Ni` how much it bends, `Ke` a glow - and the
-- PBR extension's `Pm` and `Pr` where an exporter wrote them.
local function materials(text, context)
  local out, m = {}, nil

  for line in (text or ""):gmatch("[^\r\n]+") do
    local key, rest = line:match("^%s*(%S+)%s*(.-)%s*$")
    local a, b, c = (rest or ""):match("^(%S+)%s+(%S+)%s+(%S+)")

    if key == "newmtl" then
      m = context.material(0xcccccc)
      out[rest] = m
    elseif m and key == "Kd" and a then
      m.base = colour(a, b, c)
    elseif m and key == "Ns" and tonumber(rest) then
      m.rough = clamp(math.sqrt(2 / (tonumber(rest) + 2)), 0, 1)
    elseif m and key == "d" and tonumber(rest) then
      m.trans = clamp(1 - tonumber(rest), 0, 1)
    elseif m and key == "Tr" and tonumber(rest) then
      m.trans = clamp(tonumber(rest), 0, 1)
    elseif m and key == "Ni" and tonumber(rest) then
      m.ior = clamp(tonumber(rest), 1, 3)
    elseif m and key == "Ke" and a then
      local glow = math.max(tonumber(a) or 0, tonumber(b) or 0, tonumber(c) or 0)

      if glow > 0 then
        m.emit, m.base, m.preset = 5 * glow, colour(a, b, c), "Light"
      end
    elseif m and key == "Pm" and tonumber(rest) then
      m.metallic = clamp(tonumber(rest), 0, 1)
    elseif m and key == "Pr" and tonumber(rest) then
      m.rough = clamp(tonumber(rest), 0, 1)
    end
  end

  -- Glass is what lets light through: the preset follows the number.
  for _, mat in pairs(out) do
    if (mat.trans or 0) >= 0.5 then mat.preset = "Glass" end
  end

  return out
end

function obj.read(bytes, kit, context)
  local model, why = kit.read_obj(bytes)

  if not model then return nil, why end

  local mats, skipped, whys = {}, 0, {}

  if model.mtllib ~= "" then
    local text = context.sidecar(model.mtllib)

    if text then
      mats = materials(text, context)
    else
      skipped = skipped + 1
      whys[#whys + 1] = model.mtllib .. ": not beside the file, so every part is grey"
    end
  end

  local things = {}

  for _, part in ipairs(model.parts) do
    things[#things + 1] = {
      name = part.name ~= "" and part.name or context.name, kind = "mesh",
      loc = { 0, 0, 0 }, rot = { 0, 0, 0 }, scale = { 1, 1, 1 },
      smooth = true, smooth_angle = 30,
      mat = mats[part.material] or context.material(0xcccccc),
      vertices = part.points, triangles = part.triangles, index_bytes = 4,
    }
  end

  return { name = context.name, things = things, skipped = skipped, why = whys }
end

-- The MTL beside it: each material's colour, shine, glass and glow, the
-- inverse of the reading above.
local function mtl_of(list)
  local out = { "# Cafesa3D, Kosmos" }

  for _, entry in ipairs(list) do
    local m, name = entry.mat, entry.name
    local base = m.base or 0xcccccc
    local r, g, b = (base >> 16 & 255) / 255, (base >> 8 & 255) / 255, (base & 255) / 255
    local rough = clamp(m.rough or 0.5, 0.02, 1)

    out[#out + 1] = ("newmtl %s"):format(name)
    out[#out + 1] = ("Kd %.4f %.4f %.4f"):format(r, g, b)
    out[#out + 1] = ("Ns %.1f"):format(2 / (rough * rough) - 2)
    out[#out + 1] = ("Pr %.3f"):format(m.rough or 0.5)
    out[#out + 1] = ("Pm %.3f"):format(m.metallic or 0)
    out[#out + 1] = ("d %.3f"):format(1 - (m.trans or 0))
    out[#out + 1] = ("Ni %.3f"):format(m.ior or 1.5)

    if (m.emit or 0) > 0 then
      out[#out + 1] = ("Ke %.4f %.4f %.4f"):format(r, g, b)
    end
  end

  return table.concat(out, "\n") .. "\n"
end

function obj.write(scene, kit)
  local parts, materials_used = {}, {}

  for _, o in ipairs(scene.objects) do
    local points, triangles = o.world("yup")
    local key = o.name:gsub("%s", "_")

    materials_used[#materials_used + 1] = { name = key, mat = o.mat or {} }
    parts[#parts + 1] = { name = o.name, material = key, points = points, triangles = triangles }
  end

  local mtl = scene.name .. ".mtl"

  return kit.write_obj(parts, mtl), { [mtl] = mtl_of(materials_used) }
end

return obj
