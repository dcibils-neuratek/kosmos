-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Every example in /Kosmos/Examples copied out as New Project copies it,
-- and the ones with C built with their project's defines (`tools/run_tcc.py`,
-- the IDE's examples, 7 October).
local tccbuild = use("/Kosmos/Libraries/tccbuild.lua")
local files = use("/Kosmos/Libraries/files.lua")
local names = fs.list("/Kosmos/Examples") or {}
table.sort(names)
print("EXAMPLES " .. table.concat(names, " "))
for _, name in ipairs(names) do
  local from, to = "/Kosmos/Examples/" .. name, "/Home/E/" .. name
  files.make_folder(to)
  local cs, image, defines = {}, nil, {}
  for _, f in ipairs(fs.list(from) or {}) do
    local body = fs.read(from .. "/" .. f)
    fs.write(to .. "/" .. f, body)
    if f:match("%.c$") then cs[#cs + 1] = to .. "/" .. f end
    local img = type(body) == "string" and body:match("kosmos:%s*image%s+build/(%S+)")
    if img then image, defines = to .. "/build/" .. img, tccbuild.defines_of(body) end
  end
  if #cs > 0 then
    local r, why = tccbuild.build{ sources = cs, out = image, defines = defines }
    print(("EXAMPLE %s built %s %s"):format(name, tostring(r and r.ok),
          tostring(why or (r and r.problems[1] and r.problems[1].text) or "")))
  else
    print(("EXAMPLE %s nothing to build"):format(name))
  end
end
