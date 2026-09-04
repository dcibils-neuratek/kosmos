-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Utah teapot, lit.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm glteapot
--
-- TinyGL's `teapot`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/teapot.c`.

local demo = use("/lib/gldemo.lua")

demo("teapot", "Teapot")
