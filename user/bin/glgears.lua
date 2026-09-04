-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Brian Paul's gears, the oldest OpenGL demo there is.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm glgears
--
-- TinyGL's `gears`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/gears.c`.

local demo = use("/lib/gldemo.lua")

demo("gears", "Gears")
