-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A textured cube.
-- kosmos: application
-- kosmos: icon App_GLDirectMode
-- kosmos: name GL Cube
-- kosmos: section demos/GL Demos
--
--   wm glcube
--
-- TinyGL's `cube`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/Kosmos/Libraries/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/cube.c`.

local demo = use("/Kosmos/Libraries/gldemo.lua")

demo("cube", "Cube")
