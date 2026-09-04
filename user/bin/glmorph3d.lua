-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Morphing platonic solids.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm glmorph3d
--
-- TinyGL's `morph3d`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/morph3d.c`.

local demo = use("/lib/gldemo.lua")

demo("morph3d", "Morph 3D")
