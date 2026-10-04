-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Utah teapot, lit.
-- kosmos: application
-- kosmos: icon App_Teapot
-- kosmos: name GL Teapot
-- kosmos: section demos/GL Demos
--
--   wm glteapot
--
-- TinyGL's `teapot`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/Kosmos/Libraries/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/teapot.c`.

local demo = use("/Kosmos/Libraries/gldemo.lua")

demo("teapot", "Teapot")
