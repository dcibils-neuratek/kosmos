-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- FBX for Cafesa3D: Autodesk's format, which game asset stores and Mixamo
-- hand out, read through ufbx in the 3D Kit (`k3d_fbx.c`).
--
-- **A translator** (`roadmap.md` 4l, 5c): handed the kit's reader and
-- nothing of the file system. The reader has already turned the file's
-- axes and unit into Y up and metres, and 3ds Max's pivots into points;
-- what is decided here is how its parts and materials become Cafesa3D's.
--
-- **Each part keeps its place, turn and size**, worked out from its matrix
-- by `context.place` - the same arithmetic that reads a glTF node, so a
-- mirrored object comes in with a size of -1 along X, as it does from
-- glTF. Read only: Export writes glTF, OBJ and STL, which every program
-- that reads FBX reads as well.

local fbx = {
  name = "Autodesk FBX",
  reads = { "fbx" },
  writes = {},
}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

-- A material in the Material tab's terms. The file's colours are linear,
-- as glTF's are; the tab's are what the eye sees.
local function material_of(m, context)
  local mat = context.material(context.srgb(m.base))

  mat.metallic = clamp(m.metallic, 0, 1)
  mat.rough = clamp(m.rough, 0, 1)
  mat.trans = clamp(m.trans, 0, 1)
  mat.ior = clamp(m.ior, 1, 3)

  local glow = math.max(m.emit[1], m.emit[2], m.emit[3])

  if glow > 0.01 then
    mat.emit, mat.base, mat.preset = 5 * glow, context.srgb(m.emit), "Light"
  elseif mat.trans >= 0.5 then
    mat.preset = "Glass"
  elseif mat.metallic >= 0.5 then
    mat.preset = "Metal"
  end

  return mat
end

function fbx.read(bytes, kit, context)
  local model, why = kit.read_fbx(bytes)

  if not model then return nil, why end

  local mats, things, skipped, whys = {}, {}, 0, {}

  for i, m in ipairs(model.materials) do mats[i] = material_of(m, context) end

  for _, part in ipairs(model.parts) do
    local loc, rot, scale = context.place(part.matrix)

    if loc then
      things[#things + 1] = {
        name = part.name ~= "" and part.name or context.name, kind = "mesh",
        loc = loc, rot = rot, scale = scale, hidden = part.hidden or nil,
        smooth = true, smooth_angle = 30,
        mat = part.material and mats[part.material] or context.material(0xcccccc),
        vertices = part.points, triangles = part.triangles, index_bytes = 4,
      }
    else
      skipped = skipped + 1
      whys[#whys + 1] = part.name .. ": squashed flat, so it has no size to show"
    end
  end

  -- Lamps and cameras are the file's, and not read yet: said, so a scene
  -- that comes in dark is not a mystery.
  if model.lamps + model.cameras > 0 then
    skipped = skipped + model.lamps + model.cameras
    whys[#whys + 1] = ("%d lamp%s and %d camera%s: not read from FBX yet"):format(
      model.lamps, model.lamps == 1 and "" or "s", model.cameras, model.cameras == 1 and "" or "s")
  end

  return { name = context.name, things = things, skipped = skipped, why = whys }
end

return fbx
