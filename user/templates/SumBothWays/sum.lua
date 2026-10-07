-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: image build/sum.elf
--
-- Sum, both ways: one loop written twice, in Lua here and in C in sum.c,
-- each timed - what C is for. A program: Run, and read Output.

local c = use("sum.elf")
local hz = fs.read("/Devices/cpu").counter_hz
local N = 10000000

local function ms(since) return (sys.ticks() - since) * 1000 // hz end

local started = sys.ticks()
local total = 0

for i = 1, N do total = total + i % 7 end

local in_lua = ms(started)

started = sys.ticks()
local from_c = c.sum(N)
local in_c = ms(started)

print(("sum of i %% 7 for i = 1 .. %d"):format(N))
print(("  Lua: %d in %d ms"):format(total, in_lua))
print(("  C:   %d in %d ms"):format(from_c, in_c))
print(total == from_c and "  the same answer" or "  different answers!")
