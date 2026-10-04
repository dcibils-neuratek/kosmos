-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Texture objects.
-- kosmos: application
-- kosmos: icon App_GLDirectMode
-- kosmos: name GL Textures
-- kosmos: section demos/GL Demos
--
--   wm gltexobj
--
-- TinyGL's `texobj`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/Kosmos/Libraries/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/texobj.c`.

local demo = use("/Kosmos/Libraries/gldemo.lua")

demo("texobj", "Texobj")
