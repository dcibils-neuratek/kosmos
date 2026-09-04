-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A walking mech, and the largest of them.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm glmech
--
-- TinyGL's `mech`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/mech.c`.

local demo = use("/lib/gldemo.lua")

demo("mech", "Mech")
