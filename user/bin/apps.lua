-- What is running and what it exposes.
--
--   apps                    the names
--   apps gallery            that one's properties
--
-- An application appears here because it called `ui.window`, and for no
-- other reason. It contains no scripting code: the window kit registers it
-- and answers for its properties, the way BeOS made an application
-- scriptable by its author having used BApplication.

local which = tostring(args or ""):match("^%s*(%S+)")

if not which then
  local names, err = fs.list("/app")

  if not names then
    print("apps: " .. tostring(err))
    return
  end

  if #names == 0 then
    print("nothing is registered. Try `wm gallery` in another window.")
    return
  end

  for _, name in ipairs(names) do
    print("  " .. name)
  end

  print()
  print("`apps <name>` lists what one exposes.")
  return
end

local names, err = fs.list("/app/" .. which)

if not names then
  print("apps: " .. tostring(err))
  return
end

for _, name in ipairs(names) do
  local value = fs.read("/app/" .. which .. "/" .. name)
  print(("  %-10s %s"):format(name, tostring(value)))
end

print()
print(("write one with: write /app/%s/title something"):format(which))
