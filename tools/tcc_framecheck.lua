-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: image build/framecheck.elf
--
-- The Window Kit's frame path, held (`tools/tcc_framecheck.c`): what 100
-- frames allocate in Lua, and the window manager refusing what it must.
print(use("framecheck.elf").frames(fs.capability("/Running/wm")))
