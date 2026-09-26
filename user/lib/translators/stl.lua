-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- STL for Cafesa3D: 3D printing's format, and the largest free libraries
-- of models there are (Thingiverse, Printables).
--
-- **A translator** (`roadmap.md` 4l, 5c), as BeOS's Translation Kit had
-- them: one file in `/lib/translators/`, found by Cafesa3D when it starts,
-- saying what it reads and writes. It reaches only what it is handed - the
-- 3D Kit's loops over bytes, and a way to make a material - and nothing of
-- the file system: Cafesa3D reads the bytes and writes what comes back.
--
-- **Millimetres.** An STL has no unit, and the printing world draws in
-- millimetres, so a model is brought in at a size of 0.001 - which the
-- Object tab shows, rather than a mesh silently shrunk - and written out
-- times a thousand. Z is up, as it is here.

local stl = {
  name = "STL",
  reads = { "stl" },
  writes = { "stl" },
}

function stl.read(bytes, kit, context)
  local points, triangles = kit.read_stl(bytes)

  if not points then return nil, triangles end

  return {
    name = context.name,
    skipped = 0, why = {},
    things = { {
      name = context.name, kind = "mesh",
      loc = { 0, 0, 0 }, rot = { 0, 0, 0 }, scale = { 0.001, 0.001, 0.001 },
      smooth = true, smooth_angle = 30,
      mat = context.material(0xcccccc),
      vertices = points, triangles = triangles, index_bytes = 4,
    } },
  }
end

function stl.write(scene, kit)
  local parts = {}

  for _, o in ipairs(scene.objects) do
    local points, triangles = o.world()

    parts[#parts + 1] = { points = points, triangles = triangles }
  end

  return kit.write_stl(parts, 1000)
end

return stl
