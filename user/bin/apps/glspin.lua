-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Two spinning shapes.
-- kosmos: application
-- kosmos: icon App_GLDirectMode
-- kosmos: name GL Spin
-- kosmos: section demos/GL Demos
--
--   wm glspin
--
-- TinyGL's `spin`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/Kosmos/Libraries/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/spin.c`.

local demo = use("/Kosmos/Libraries/gldemo.lua")

demo("spin", "Spin")
