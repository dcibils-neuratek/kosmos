-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Cube in C
-- kosmos: image build/cube.elf
--
-- Cube in C: the cube of Cube in Lua with all of it in cube.c - the maths,
-- the sort and the triangle fill. Run both and compare the time a frame in
-- their corners. This line starts it, and gives it the counter's rate.
print(use("cube.elf").main(fs.read("/Devices/cpu").counter_hz))
