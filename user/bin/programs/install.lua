-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Packages from the package server, on demand.
--
--   install                    every package the server offers, and which
--                              are installed here, from which build
--   install doom               one, fetched and put in place
--   install doom quake         several
--   install all                every one
--   install --from 192.168.1.38:47701 ...   a server other than the boot's
--
-- Diego, 12 October 2026: "perhaps add a program in kosmos that requests
-- those from the mac as needed?", "where we show all available packages",
-- "and we can install them on demand", "from the server (which is the mac
-- now)". Doom, Quake, the SNES and the developer files carry this build's
-- protocols, so they had to be sent again after every change; now the
-- machine asks for them when it wants them (`roadmap.md`, Kosmos App Store,
-- whose first brick this is).
--
-- **The server** is the boot's, `opt/kosmos/packages` on the command line
-- (`make netboot` puts the Mac's address there), serving what
-- `tools/packages.py` made: `index.txt`, a line a package, and each
-- package's zip, its folder inside.
--
-- **Put in place without a moment of nothing there**: unpacked beside the
-- installed folder on the same disk, then the old one renamed away, the new
-- one renamed in, and the old one removed - so a download that fails, or a
-- zip that will not open, leaves what was installed as it was.

local files = use("/Kosmos/Libraries/files.lua")
local http = use("/Kosmos/Libraries/http.lua")
local zip = use("/Kosmos/Libraries/zip.lua")

local KEPT = "/Home/Packages"           -- a file a package: the build installed

local words = files.words(args)
local server = sys.boot and sys.boot("opt/kosmos/packages") or nil
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

if not server or server == "" then
  print("install: no package server - the boot names none (opt/kosmos/packages);")
  print("         say one with --from <address:port>")
  return
end

local base = "http://" .. server .. "/"

local function get(name)
  local reply, why = http.get(base .. name)

  if not reply then return nil, tostring(why) end

  local status, _, body = http.parse(reply)

  if status ~= 200 then return nil, ("the server said %d for %s"):format(status, name) end

  return body
end

-- The index: name, what it is called, where it goes, bytes, build, a sentence.
local text, why = get("index.txt")

if not text then
  print("install: the package server at " .. server .. " did not answer: " .. tostring(why))
  return
end

local packages, by_name = {}, {}

for line in text:gmatch("[^\n]+") do
  local f = {}

  for field in (line .. "\t"):gmatch("([^\t]*)\t") do f[#f + 1] = field end

  if #f >= 6 then
    local p = { name = f[1], title = f[2], folder = f[3], bytes = tonumber(f[4]) or 0,
                build = f[5], about = f[6] }

    packages[#packages + 1] = p
    by_name[p.name] = p
  end
end

local function installed(p)
  local kept = fs.read(KEPT .. "/" .. p.name)

  return type(kept) == "string" and kept:gsub("%s+$", "") or nil
end

-- **Nothing asked: the list.**
if #wanted == 0 then
  print(("install: %d packages from %s"):format(#packages, server))

  for _, p in ipairs(packages) do
    local have = installed(p)
    local state = not have and "not installed"
                  or have == p.build and "installed"
                  or "installed, from " .. have

    print(("install:   %-10s %-16s %9s  %s - %s"):format(p.name, p.title, files.size(p.bytes),
                                                      state, p.about))
  end

  print("install: install <name> to install one, install all for every one")
  return
end

if wanted[1] == "all" then
  wanted = {}
  for _, p in ipairs(packages) do wanted[#wanted + 1] = p.name end
end

local function put_in_place(p)
  local body, err = get(p.name .. ".zip")

  if not body then return nil, err end

  local archive = "/Temporary/install-" .. p.name .. ".zip"
  local fresh, old = p.folder .. ".new", p.folder .. ".old"

  local ok, oops = fs.write(archive, body)

  body = nil
  if not ok then return nil, "no room for it in /Temporary: " .. tostring(oops) end

  -- Leftovers of an install that stopped halfway, first.
  if fs.getattr(fresh) then files.remove(fresh) end
  if fs.getattr(old) then files.remove(old) end

  files.make_folder(files.parent(p.folder))
  ok, oops = zip.extract{ from = archive, into = fresh }
  files.remove(archive)

  if not ok then return nil, "it would not open: " .. tostring(oops) end

  if fs.getattr(p.folder) then
    ok, oops = files.move(p.folder, old)
    if not ok then files.remove(fresh) return nil, "the installed one would not move: " .. tostring(oops) end
  end

  ok, oops = files.move(fresh, p.folder)

  if not ok then
    if fs.getattr(old) then files.move(old, p.folder) end
    return nil, "the new one would not go in place: " .. tostring(oops)
  end

  if fs.getattr(old) then files.remove(old) end

  files.make_folder(KEPT)
  fs.write(KEPT .. "/" .. p.name, p.build .. "\n")
  return true
end

local failed = 0

for _, name in ipairs(wanted) do
  local p = by_name[name]

  if not p then
    print(("install: %s - the server offers no such package"):format(name))
    failed = failed + 1
  else
    local started = sys.ticks()
    local ok, err = put_in_place(p)
    local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1

    if ok then
      print(("install: %s - %s in %s, %.1f s, from %s"):format(p.name, files.size(p.bytes),
            p.folder, (sys.ticks() - started) / hz, p.build))
    else
      print(("install: %s - not installed: %s"):format(p.name, tostring(err)))
      failed = failed + 1
    end
  end
end

if failed > 0 then sys.exit(1) end
