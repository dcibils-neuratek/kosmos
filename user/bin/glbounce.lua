-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A bouncing ball.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm glbounce
--
-- TinyGL's `bounce`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/bounce.c`.

local demo = use("/lib/gldemo.lua")

demo("bounce", "Bounce")
