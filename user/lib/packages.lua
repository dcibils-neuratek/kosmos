-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Packages from a package server: the index, a package's manifest, and
-- putting one in place - the one copy `pkg`, `install` and the Packages
-- window share (`docs/packages.md`).
--
--   local packages = use("/Kosmos/Libraries/packages.lua")
--   packages.server()                   the boot's server, or nil
--   packages.index(server)              the packages offered, or nil, why
--   packages.installed(name)            what is installed, its manifest, or nil
--   packages.install(p, server)         fetched, checked, put in place
--   packages.plan(names, by_name)       those and what they depend on, in order
--   packages.needed_by(name)            installed packages that depend on it
--   packages.remove(name)               exactly the files it installed
--   packages.command(words, say)        `pkg`, at the prompt: list, install,
--                                       remove, update
--
-- **What a package is**: a zip of its folder with `package.txt` inside - a
-- line a fact, a key, spaces, the value; a file's line its size, its
-- SHA-256 and its path last - written by `tools/packages.py` from the
-- application's own header. The index is every manifest without its files,
-- a package a paragraph.
--
-- **Checked before anything installed is touched**: the manifest's
-- protocols must be this build's (`sys.protostamp`), and every file must
-- have the size and the SHA-256 the manifest gives; a package that fails
-- either is left in /Temporary's leftovers and nothing installed changes.
-- **Put in place without a moment of nothing there**: unpacked beside the
-- installed folder on the same disk, swapped in by renames, the old one
-- removed. **Kept**: the manifest it was installed from, in /Home/Packages -
-- what is installed, from which version, and every file it put there.

local files = use("/Kosmos/Libraries/files.lua")
local http = use("/Kosmos/Libraries/http.lua")
local zip = use("/Kosmos/Libraries/zip.lua")

local packages = {}

packages.KEPT = "/Home/Packages"

-- The boot's package server, `opt/kosmos/packages` - the Mac, named by
-- `make netboot` - or nil.
function packages.server()
  local s = sys.boot and sys.boot("opt/kosmos/packages") or nil

  return (type(s) == "string" and s ~= "") and s or nil
end

--
-- **A manifest or an index, read**: a list of packages, each its facts by
-- key and, for a manifest, `files`, each `{ path, size, sha }`. Paragraphs
-- are packages; `#` starts a comment.
--
function packages.parse(text)
  local out, cur = {}, nil

  for line in (tostring(text or "") .. "\n"):gmatch("([^\n]*)\n") do
    if line:match("^%s*$") then
      cur = nil
    elseif not line:match("^#") then
      local key, value = line:match("^(%S+)%s*(.-)%s*$")

      if key then
        if not cur then
          cur = { files = {} }
          out[#out + 1] = cur
        end

        if key == "file" then
          local size, sha, path = value:match("^(%d+)%s+(%x+)%s+(.+)$")

          if path then
            cur.files[#cur.files + 1] = { path = path, size = tonumber(size), sha = sha:lower() }
          end
        else
          cur[key] = value
        end
      end
    end
  end

  for _, p in ipairs(out) do
    p.bytes, p.zip = tonumber(p.bytes) or 0, tonumber(p.zip) or 0
    p.needs = p.needs or ""
    p.depends = p.depends or ""
  end

  return out
end

local function get(server, name)
  local reply, why = http.get("http://" .. server .. "/" .. name)

  if not reply then return nil, tostring(why) end

  local status, _, body = http.parse(reply)

  if status ~= 200 then return nil, ("the server said %d for %s"):format(status, name) end

  return body
end

function packages.index(server)
  local text, why = get(server, "index.txt")

  if not text then return nil, why end

  local list, by_name = packages.parse(text), {}

  for _, p in ipairs(list) do by_name[p.name] = p end

  return list, by_name
end

--
-- **What is installed**: the manifest kept when it was, or nil. A record
-- from before manifests - one line, the build - is said as that build.
--
function packages.installed(name)
  local kept = fs.read(packages.KEPT .. "/" .. name)

  if type(kept) ~= "string" then return nil end

  local m = packages.parse(kept)[1]

  if m and m.version then return m end

  return { name = name, version = kept:gsub("%s+$", ""), files = {} }
end

-- Whether what is installed is the version offered.
function packages.current(p)
  local m = packages.installed(p.name)

  return m ~= nil and m.version == p.version and (m.build or "") == (p.build or "")
end

--
-- **Every file as its manifest says**, in `folder`: its size, and its
-- SHA-256 by the Crypto Kit. Nil and the first that is not.
--
local function verify(folder, manifest)
  local crypto = use("/Kosmos/Kits/crypto")

  for _, f in ipairs(manifest.files) do
    local where = files.join(folder, f.path)
    local data = fs.read(where)

    if type(data) ~= "string" then return nil, f.path .. " is missing" end
    if #data ~= f.size then
      return nil, ("%s is %d bytes, not %d"):format(f.path, #data, f.size)
    end
    if crypto.sha256(data) ~= f.sha then
      return nil, f.path .. " is not what was made: its SHA-256 differs"
    end
  end

  return true
end

--
-- **One package fetched, checked and put in place.** True, or nil and why.
--
function packages.install(p, server)
  local body, err = get(server, p.name .. ".zip")

  if not body then return nil, err end

  local archive = "/Temporary/install-" .. p.name .. ".zip"
  local fresh, old = p.into .. ".new", p.into .. ".old"

  local ok, oops = fs.write(archive, body)

  body = nil
  if not ok then return nil, "no room for it in /Temporary: " .. tostring(oops) end

  -- Leftovers of an install that stopped halfway, first.
  if fs.getattr(fresh) then files.remove(fresh) end
  if fs.getattr(old) then files.remove(old) end

  files.make_folder(files.parent(p.into))
  ok, oops = zip.extract{ from = archive, into = fresh }
  files.remove(archive)

  if not ok then return nil, "it would not open: " .. tostring(oops) end

  -- **Its manifest, held to this build and to its files.**
  local text = fs.read(files.join(fresh, "package.txt"))
  local manifest = type(text) == "string" and packages.parse(text)[1] or nil
  local refused

  if not manifest or manifest.name ~= p.name then
    refused = "it carries no manifest of its own"
  elseif sys.protostamp and manifest.protocols ~= sys.protostamp then
    refused = ("it was made for other protocols (%s; this is %s) - build its "
               .. "package again from this tree"):format(tostring(manifest.protocols),
                                                         tostring(sys.protostamp))
  else
    local whole, why = verify(fresh, manifest)

    if not whole then refused = why end
  end

  if refused then
    files.remove(fresh)
    return nil, refused
  end

  if fs.getattr(p.into) then
    ok, oops = files.move(p.into, old)
    if not ok then files.remove(fresh) return nil, "the installed one would not move: " .. tostring(oops) end
  end

  ok, oops = files.move(fresh, p.into)

  if not ok then
    if fs.getattr(old) then files.move(old, p.into) end
    return nil, "the new one would not go in place: " .. tostring(oops)
  end

  if fs.getattr(old) then files.remove(old) end

  files.make_folder(packages.KEPT)
  fs.write(packages.KEPT .. "/" .. p.name, text)
  return true, manifest
end

-- One file or one empty folder gone, as `files.remove` deletes each.
local function delete(path)
  return fs.send(path, { type = "delete" })
end

local function words_of(text)
  local out = {}

  for w in tostring(text or ""):gmatch("%S+") do out[#out + 1] = w:lower() end

  return out
end

--
-- **What to install, in order**: each name and what it depends on, the
-- dependencies first, each once - and nothing already installed at the
-- version offered unless it was asked for by name. Nil and why for a name or
-- a dependency the server does not offer, or a circle of them.
--
function packages.plan(names, by_name)
  local out, placed, visiting = {}, {}, {}

  local function visit(name, asked)
    if placed[name] then return true end
    if visiting[name] then return nil, name .. " depends on itself, by way of others" end

    local p = by_name[name]

    if not p then return nil, name .. " - the server offers no such package" end

    visiting[name] = true

    for _, dep in ipairs(words_of(p.depends)) do
      local ok, why = visit(dep, false)

      if not ok then return nil, ("%s needs %s: %s"):format(name, dep, why) end
    end

    visiting[name] = nil
    placed[name] = true

    if asked or not packages.current(p) then out[#out + 1] = p end

    return true
  end

  for _, name in ipairs(names) do
    local ok, why = visit(name, true)

    if not ok then return nil, why end
  end

  return out
end

-- Every package installed here, by its kept manifest.
function packages.all_installed()
  local out = {}

  for _, name in ipairs(fs.list(packages.KEPT) or {}) do
    local m = packages.installed(name)

    if m then out[#out + 1] = m end
  end

  return out
end

-- The installed packages that depend on `name`.
function packages.needed_by(name)
  local out = {}

  for _, m in ipairs(packages.all_installed()) do
    for _, dep in ipairs(words_of(m.depends)) do
      if dep == name then out[#out + 1] = m.name end
    end
  end

  return out
end

--
-- **Exactly the files it installed**, by its kept manifest, and its own
-- `package.txt`; then each folder they leave empty, deepest first, and the
-- record. A file a person put beside them stays, and so does its folder.
--
function packages.remove(name)
  local m = packages.installed(name)

  if not m then return nil, name .. " is not installed" end

  if not m.into then
    return nil, name .. " was installed before manifests, so what it put there is not known - install it again first"
  end

  local folders = {}

  for _, f in ipairs(m.files) do
    local where = files.join(m.into, f.path)

    if fs.getattr(where) then delete(where) end

    local dir = files.parent(where)

    while dir and #dir > #m.into do
      folders[dir] = true
      dir = files.parent(dir)
    end
  end

  if fs.getattr(files.join(m.into, "package.txt")) then delete(files.join(m.into, "package.txt")) end

  local deepest = {}

  for dir in pairs(folders) do deepest[#deepest + 1] = dir end
  table.sort(deepest, function(a, b) return #a > #b end)
  deepest[#deepest + 1] = m.into

  for _, dir in ipairs(deepest) do
    local inside = fs.list(dir)

    if inside and #inside == 0 then delete(dir) end
  end

  delete(packages.KEPT .. "/" .. name)
  return true, m
end

--
-- **`pkg`, at the prompt** - and `install`, its short form: what is asked in
-- `words`, said through `say`, and whether all of it went. The window calls
-- the functions above; this is their text.
--
--   pkg                       every package and its state
--   pkg install NAME...       those and what they depend on; `all` for every one
--   pkg remove NAME...        refused while an installed package needs one
--   pkg update                every installed package with another version
--   ... --from ADDRESS:PORT   a server other than the boot's
--
function packages.command(words, say)
  local server, rest = packages.server(), {}
  local i = 1

  while i <= #words do
    if words[i] == "--from" then
      server, i = words[i + 1], i + 2
    else
      rest[#rest + 1], i = words[i]:lower(), i + 1
    end
  end

  local verb = rest[1] or "list"

  table.remove(rest, 1)

  -- Removing asks nothing of the server.
  if verb == "remove" then
    if #rest == 0 then say("which package? pkg remove <name>") return false end

    local going, all = {}, true

    for _, name in ipairs(rest) do going[name] = true end

    for _, name in ipairs(rest) do
      local still = {}

      for _, other in ipairs(packages.needed_by(name)) do
        if not going[other] then still[#still + 1] = other end
      end

      if #still > 0 then
        say(("%s is needed by %s - remove both with: pkg remove %s %s")
            :format(name, table.concat(still, ", "), name, table.concat(still, " ")))
        return false
      end
    end

    for _, name in ipairs(rest) do
      local ok, why = packages.remove(name)

      if ok then
        say(("%s - removed from %s"):format(name, why.into))
      else
        say(("%s - not removed: %s"):format(name, tostring(why)))
        all = false
      end
    end

    return all
  end

  if not server then
    say("no package server - the boot names none (opt/kosmos/packages); say one with --from <address:port>")
    return false
  end

  local list, by_name = packages.index(server)

  if not list then
    say("the package server at " .. server .. " did not answer: " .. tostring(by_name))
    return false
  end

  if verb == "list" then
    say(("%d packages from %s"):format(#list, server))

    for _, p in ipairs(list) do
      local m = packages.installed(p.name)
      local state = not m and "not installed"
                    or packages.current(p) and "installed"
                    or ("%s installed, %s here"):format(m.version, p.version)
      local deps = p.depends ~= "" and (" - depends on " .. p.depends) or ""

      say(("  %-10s %-16s %9s  %s%s - %s"):format(p.name, p.title, files.size(p.zip),
                                                state, deps, p.about))
    end

    return true
  end

  local names

  if verb == "update" then
    names = {}

    for _, p in ipairs(list) do
      if packages.installed(p.name) and not packages.current(p) then names[#names + 1] = p.name end
    end

    if #names == 0 then say("everything installed is the version offered") return true end
  elseif verb == "install" then
    names = rest

    if names[1] == "all" then
      names = {}
      for _, p in ipairs(list) do names[#names + 1] = p.name end
    end

    if #names == 0 then say("which package? pkg install <name>") return false end
  else
    say("pkg: list, install, remove or update - not " .. verb)
    return false
  end

  local plan, why = packages.plan(names, by_name)

  if not plan then say(why) return false end

  local asked = {}

  for _, n in ipairs(names) do asked[n] = true end

  for _, p in ipairs(plan) do
    if not asked[p.name] then say(("%s first, which another depends on"):format(p.name)) end
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
  local all = true

  for _, p in ipairs(plan) do
    local before = packages.installed(p.name)
    local started = sys.ticks()
    local ok, err = packages.install(p, server)

    if ok then
      say(("%s - %s%s in %s, %.1f s"):format(p.name,
          before and (before.version .. " -> ") or "", p.version, p.into,
          (sys.ticks() - started) / hz))
    else
      say(("%s - not installed: %s"):format(p.name, tostring(err)))
      all = false
      break
    end
  end

  return all
end

return packages
