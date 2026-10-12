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

return packages
