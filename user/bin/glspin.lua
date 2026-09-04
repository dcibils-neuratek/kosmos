-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Two spinning shapes.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm glspin
--
-- TinyGL's `spin`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/spin.c`.

local demo = use("/lib/gldemo.lua")

demo("spin", "Spin")
