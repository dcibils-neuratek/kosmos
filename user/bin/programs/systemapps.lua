-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
-- systemapps: Kosmos's own applications on the disk, held to this build.
--
--   systemapps                 each one looked at, and brought from the
--                              boot's package server when it is not this
--                              build's
--
-- **Kosmos's own, too big for the image** (Diego, 15 November 2026:
-- "/Home/System, seen as /Kosmos"): JetSki is 208 MB, so it lives on the
-- disk in /Home/System/Apps/JetSki and every program sees it, read only, as
-- /Kosmos/Apps/JetSki (`ns.SYSTEM_APPS`). A stick's build writes it there
-- with its `package.txt`, so a stick's is its own build's already.
--
-- **A netboot brings the rest of the system and not the disk**, so the
-- copy there is whatever the last stick or netboot left. Started by the
-- window manager at login, as `maild` is, this asks the boot's server
-- (`opt/kosmos/packages`, the Mac) for its index, and every package of kind
-- `system` whose copy here is missing, for other protocols, or of another
-- build is installed from it - fetched to the disk, held to its manifest,
-- swapped in by renames (`packages.install`).
--
-- **Without a server** nothing can be brought, and a copy for other
-- protocols is said, since Kosmos refuses to start its image.

local files = use("/Kosmos/Libraries/files.lua")
local ipv4 = use("/Kosmos/Libraries/ipv4.lua")
local packages = use("/Kosmos/Libraries/packages.lua")

local HERE = "/Home/System/Apps"
local ADDRESS_S = 60                      -- seconds to wait for DHCP's answer

local function say(line)
  print("systemapps: " .. line)
end

-- The manifest a folder carries, or nil.
local function on_disk(into)
  local text = fs.read(files.join(into, "package.txt"))

  return type(text) == "string" and packages.parse(text)[1] or nil
end

-- Why the copy here is not the one to keep, or nil when it is: `p` is what
-- the server offers, nil without one.
local function stale(m, p)
  if not m then return "not on this disk" end

  if sys.protostamp and m.protocols ~= sys.protostamp then
    return ("another build's, for other protocols (%s; this is %s)")
           :format(tostring(m.protocols), tostring(sys.protostamp))
  end

  if p and (m.build or "") ~= (p.build or "") then
    return ("build %s, and the server's is %s"):format(tostring(m.build), tostring(p.build))
  end

  return nil
end

local server = packages.server()

if not server then
  for _, entry in ipairs(fs.list(HERE) or {}) do
    local name = tostring(type(entry) == "table" and entry.name or entry)
    local why = stale(on_disk(files.join(HERE, name)))

    if why then
      say(("%s is %s, with no server to bring this build's from - write the stick "
           .. "again, or netboot"):format(name, why))
    end
  end

  return
end

-- The network's address first: DHCP answers after login on a netboot.
local tick_hz = (sys.info() or {}).tick_hz or 250
local waited = 0

while not ipv4.given((fs.net_info("/Network") or {}).address) do
  if waited == 0 then say("waiting for an address to ask " .. server) end
  if waited >= ADDRESS_S then
    say(("no address after %d seconds; nothing brought from %s"):format(ADDRESS_S, server))
    return
  end

  sys.sleep(tick_hz)
  waited = waited + 1
end

local list, why = packages.index(server)

if not list then
  say(("could not ask %s: %s"):format(server, tostring(why)))
  return
end

local failed = false

--
-- **A copy from before it was Kosmos's own**: JetSki was a package that
-- installed into /Home/Apps/JetSki until 0.11.431. Once this build's is in
-- place, a folder of the same name there whose manifest names the same
-- package is that old copy - 208 MB, and a second JetSki in every list -
-- and goes. A folder with any other manifest, or none, is a person's, and
-- stays.
--
local function old_copy(p)
  local folder = files.join("/Home/Apps", tostring(p.into):match("([^/]+)$") or "")
  local m = on_disk(folder)

  if folder ~= "/Home/Apps/" and m and m.name == p.name and m.into ~= p.into then
    local ok, oops = files.remove(folder)

    if ok then
      say(("%s's copy from before it was Kosmos's own, in %s, is gone"):format(p.name, folder))
    else
      say(("%s's old copy in %s stayed: %s"):format(p.name, folder, tostring(oops)))
    end
  end
end

for _, p in ipairs(list) do
  if p.kind == "system" and p.into then
    local reason = stale(on_disk(p.into), p)

    if not reason then
      say(("%s is this build's, %s"):format(p.name, tostring(p.build)))
      old_copy(p)
    elseif sys.protostamp and p.protocols ~= sys.protostamp then
      say(("%s here is %s, and the server's is for other protocols too (%s) - make "
           .. "the packages again from this tree"):format(p.name, reason, tostring(p.protocols)))
      failed = true
    else
      say(("%s here is %s: bringing %d MB from %s"):format(p.name, reason,
                                                           (p.zip or 0) // (1024 * 1024), server))
      local ok, oops = packages.install(p, server)

      if ok then
        say(("%s is in %s, build %s"):format(p.name, p.into, tostring(p.build)))
        old_copy(p)
      else
        say(("%s was not brought: %s"):format(p.name, tostring(oops)))
        failed = true
      end
    end
  end
end

if failed then sys.exit(1) end
