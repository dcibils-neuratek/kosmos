-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Texture objects.
-- kosmos: application
-- kosmos: section demos/GLDemos
--
--   wm gltexobj
--
-- TinyGL's `texobj`, unmodified upstream C, rasterised in software on a
-- machine with no GPU. The window and the loop are `/lib/gldemo.lua`; the
-- triangles are `runtime/upstream/tinygl/examples/texobj.c`.

local demo = use("/lib/gldemo.lua")

demo("texobj", "Texobj")
