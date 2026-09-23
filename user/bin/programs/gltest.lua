-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Does the GL Kit draw anything at all?
--
-- A console check, deliberately without a window: if this passes and the
-- application is blank, the fault is in the application; if this fails, it
-- is in the kit. Two questions that look identical on a screen.
local gl  = use("/kits/gl")
local gfx_ = gfx

local list = gl.demos()

print(("gltest: demos -> %s entries, type %s")
      :format(tostring(list and #list), type(list)))

for _, d in ipairs(list or {}) do
  print(("  %-9s %s"):format(d.name, d.what))
end

local W, H = 160, 120
local ctx, why = gl.context(W, H)

if not ctx then print("gltest: no context: " .. tostring(why)) return end

print("gltest: context made")

local ok, oops = gl.start("gears", W, H)

if not ok then print("gltest: start failed: " .. tostring(oops)) return end

print("gltest: gears started")

gl.frame()
print("gltest: one frame drawn")

local surf = gfx_.surface{ w = W, h = H }

if not surf then print("gltest: no surface") return end

gl.blit(ctx, surf, 0, 0)

-- Count what is not the clear colour. A rasteriser that ran produces some.
local lit, sampled = 0, 0

for y = 0, H - 1, 3 do
  for x = 0, W - 1, 3 do
    local p = surf:get(x, y)

    sampled = sampled + 1

    if p and (p & 0x00ffffff) ~= 0 then lit = lit + 1 end
  end
end

print(("gltest: %d of %d sampled pixels are not background"):format(lit, sampled))

if lit == 0 then
  print("gltest: NOTHING WAS DRAWN")
else
  print("gltest: the rasteriser works")
end
