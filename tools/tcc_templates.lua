-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Every template in /Kosmos/Templates copied out as New Project copies it,
-- and the ones with C built (`tools/run_tcc.py`, step C6).
local tccbuild = use("/Kosmos/Libraries/tccbuild.lua")
local files = use("/Kosmos/Libraries/files.lua")
print("TEMPLATES " .. table.concat(fs.list("/Kosmos/Templates") or {}, " "))
for _, name in ipairs({ "HelloWindow", "Mandelbrot", "SumBothWays", "Primes" }) do
  local from, to = "/Kosmos/Templates/" .. name, "/Home/P/" .. name
  files.make_folder(to)
  local cs, image = {}, nil
  for _, f in ipairs(fs.list(from) or {}) do
    local body = fs.read(from .. "/" .. f)
    fs.write(to .. "/" .. f, body)
    if f:match("%.c$") then cs[#cs + 1] = to .. "/" .. f end
    local img = type(body) == "string" and body:match("kosmos:%s*image%s+build/(%S+)")
    if img then image = to .. "/build/" .. img end
  end
  if #cs > 0 then
    local r, why = tccbuild.build{ sources = cs, out = image }
    print(("TEMPLATE %s built %s %s"):format(name, tostring(r and r.ok), tostring(why or (r and r.problems[1] and r.problems[1].text) or "")))
  else
    print(("TEMPLATE %s nothing to build"):format(name))
  end
end
