-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A textured cube.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm glcube
--
-- TinyGL's `cube`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/cube.c`.

local demo = use("/lib/gldemo.lua")

demo("cube", "Cube")
