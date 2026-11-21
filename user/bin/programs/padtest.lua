-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs pad-test
--
-- padtest: a game controller that is not there, for the suites
-- (`docs/flight.md`, F7). It writes one slot of the pads' page as the USB
-- driver would (`padring.h`) - the count made odd, the slot, the count made
-- even - so the window manager, and the window in front, see a pad that
-- QEMU has no way to plug in.
--
--   padtest <slot> lx ly rx ry lt rt [buttons]   sticks -1..1, triggers 0..1
--   padtest <slot> off                           the pad unplugged
--
-- It is given the writable page only when the boot line says
-- `opt/kosmos/pad=test` (init's `padtest_grant`), so on a machine in use it
-- has nothing to write to.

if not pad_write then
  print("padtest: no pads' page to write - boot with opt/kosmos/pad=test")
  return
end

local words = {}
for w in (args or ""):gmatch("%S+") do words[#words + 1] = w end

local slot = math.tointeger(tonumber(words[1]))

if not slot or slot < 0 or slot > 3 then
  print("usage: padtest <slot 0-3> lx ly rx ry lt rt [buttons] | padtest <slot> off")
  return
end

local function axis(v)
  v = math.max(-1, math.min(1, tonumber(v) or 0))
  return math.floor(v < 0 and v * 32768 or v * 32767 + 0.5)
end

local function trigger(v)
  return math.floor(math.max(0, math.min(1, tonumber(v) or 0)) * 255 + 0.5)
end

local at = 8 + 20 * slot
local present, bytes = 1, nil

if words[2] == "off" then
  present = 0
  bytes = string.pack("<I4hhhhBBBB", 0, 0, 0, 0, 0, 0, 0, 0, 0)
else
  bytes = string.pack("<I4hhhhBBBB", math.tointeger(tonumber(words[8])) or 0,
                      axis(words[2]), axis(words[3]), axis(words[4]), axis(words[5]),
                      trigger(words[6]), trigger(words[7]), present, 0)
end

-- The magic and the count of slots, as the driver's `pads_map` writes them.
sys.region_write(pad_write, 0, string.pack("<I4I4", 0x4450414b, 4))

local seq = sys.region_load32(pad_write, at)
local odd = seq | 1

sys.region_write(pad_write, at, string.pack("<I4", odd))
sys.region_write(pad_write, at + 4, bytes)
sys.region_write(pad_write, at, string.pack("<I4", odd + 1))

print(("padtest: slot %d %s (seq %d)"):format(slot, present == 1 and "written" or "unplugged", odd + 1))
