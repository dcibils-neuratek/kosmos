-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A bouncing ball.
-- kosmos: application
-- kosmos: icon App_GLDirectMode
-- kosmos: name GL Bounce
-- kosmos: section demos/GL Demos
--
--   wm glbounce
--
-- TinyGL's `bounce`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/Kosmos/Libraries/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/bounce.c`.

local demo = use("/Kosmos/Libraries/gldemo.lua")

demo("bounce", "Bounce")
