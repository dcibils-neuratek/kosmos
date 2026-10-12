-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Packages from the package server, on demand.
--
--   install                    every package the server offers, and which
--                              are installed here, from which version
--   install doom               one, fetched, checked and put in place
--   install doom quake         several
--   install all                every one
--   install --from 192.168.1.38:47701 ...   a server other than the boot's
--
-- Diego, 12 October 2026: "perhaps add a program in kosmos that requests
-- those from the mac as needed?", "where we show all available packages",
-- "and we can install them on demand", "from the server (which is the mac
-- now)". What a package is and how one is put in place is
-- `/Kosmos/Libraries/packages.lua`'s, which `pkg` and the Packages window
-- share (`docs/packages.md`); this is `pkg install`'s short form.

local files = use("/Kosmos/Libraries/files.lua")
local packages = use("/Kosmos/Libraries/packages.lua")

local words = files.words(args)
local server = packages.server()
local wanted = {}

do
  local i = 1

  while i <= #words do
    if words[i] == "--from" then
      server, i = words[i + 1], i + 2
    else
      wanted[#wanted + 1], i = words[i]:lower(), i + 1
    end
  end
end

if not server then
  print("install: no package server - the boot names none (opt/kosmos/packages);")
  print("         say one with --from <address:port>")
  return
end

local list, by_name = packages.index(server)

if not list then
  print("install: the package server at " .. server .. " did not answer: " .. tostring(by_name))
  return
end

-- **Nothing asked: the list.**
if #wanted == 0 then
  print(("install: %d packages from %s"):format(#list, server))

  for _, p in ipairs(list) do
    local m = packages.installed(p.name)
    local state = not m and "not installed"
                  or packages.current(p) and "installed"
                  or ("%s installed, %s here"):format(m.version, p.version)

    print(("install:   %-10s %-16s %9s  %s - %s"):format(p.name, p.title, files.size(p.zip),
                                                      state, p.about))
  end

  print("install: install <name> to install one, install all for every one")
  return
end

if wanted[1] == "all" then
  wanted = {}
  for _, p in ipairs(list) do wanted[#wanted + 1] = p.name end
end

local failed = 0
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1

for _, name in ipairs(wanted) do
  local p = by_name[name]

  if not p then
    print(("install: %s - the server offers no such package"):format(name))
    failed = failed + 1
  else
    local started = sys.ticks()
    local ok, err = packages.install(p, server)

    if ok then
      print(("install: %s - %s in %s, %.1f s, %s"):format(p.name, files.size(p.zip),
            p.into, (sys.ticks() - started) / hz, p.version))
    else
      print(("install: %s - not installed: %s"):format(p.name, tostring(err)))
      failed = failed + 1
    end
  end
end

if failed > 0 then sys.exit(1) end
