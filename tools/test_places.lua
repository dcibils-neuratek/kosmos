-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Tracker's shortcut places, checked on this computer with no machine booted.
--
-- `user/lib/places.lua` decides what a place remembers and how it is found
-- again, and the decision that matters is one a person would only notice
-- after it went wrong: that a shortcut opens *its own* drive after a replug,
-- whatever that drive is called now and whatever else is plugged in.
--
--   build/host/lua tools/test_places.lua

package.path = "user/lib/?.lua;" .. package.path

local places = dofile("user/lib/places.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

--
-- What `fs.volumes("/drives")` answers, trimmed to the fields a place reads.
-- The unit is there on purpose: it changes between these tables, as it does
-- on every replug, and nothing may depend on it.
--
local plugged = {
  { name = "PHOTOS", id = "fat:1A2B-3C4D", unit = 0, partition = 0 },
  { name = "BACKUP", id = "fat:0BAD-CAFE", unit = 0, partition = 1 },
  { name = "KOSMOS HOME", id = nil, unit = 1, partition = 0 },
}

--------------------------------------------------------------------------
-- Making a place.
--------------------------------------------------------------------------

local a, why = places.from_path("/drives/PHOTOS/Italy", plugged)

check(a and a.kind == "place" and a.volume == "fat:1A2B-3C4D"
      and a.within == "/Italy" and a.volume_name == "PHOTOS" and not a.path,
      "a folder on a drive remembers the volume's identity and the path in it: "
      .. tostring(why))

local root = places.from_path("/drives/PHOTOS", plugged)

check(root and root.within == "/", "a volume itself is its root, `/`")

local slashed = places.from_path("/drives/PHOTOS/Italy/", plugged)

check(slashed and slashed.within == "/Italy",
      "a trailing slash is not part of the place")

local home = places.from_path("/home/Music", plugged)

check(home and home.path == "/home/Music" and not home.volume,
      "a folder not on a drive is remembered by its path")

local none, none_why = places.from_path("/drives/KOSMOS HOME/notes", plugged)

check(none == nil and tostring(none_why):find("nothing to know it by"),
      "a volume with no identity is refused, not remembered by its name")

local gone, gone_why = places.from_path("/drives/ELSEWHERE/x", plugged)

check(gone == nil and tostring(gone_why):find("not plugged in"),
      "a volume that is not there cannot be made a place")

check(places.suggest("/drives/PHOTOS") == "PHOTOS"
      and places.suggest("/drives/PHOTOS/Italy") == "Italy"
      and places.suggest("/home/Music/") == "Music",
      "the offered name is the folder's, or the volume's at its root")

--------------------------------------------------------------------------
-- Finding it again - the part that is the whole point.
--------------------------------------------------------------------------

local now, called = places.resolve(a, plugged)

check(now == "/drives/PHOTOS/Italy" and called == "PHOTOS",
      "a place resolves to its folder while its drive is plugged in")

check(places.resolve(root, plugged) == "/drives/PHOTOS",
      "a place that is a whole volume resolves to the volume")

--
-- **Replugged: a new unit, and a new name** because another `PHOTOS` arrived
-- first. The shortcut follows the volume, not the name.
--
local replugged = {
  { name = "PHOTOS", id = "fat:9999-0000", unit = 5, partition = 0 },
  { name = "PHOTOS 2", id = "fat:1A2B-3C4D", unit = 6, partition = 0 },
}

now, called = places.resolve(a, replugged)

check(now == "/drives/PHOTOS 2/Italy" and called == "PHOTOS 2",
      "after a replug under another name and unit, it finds its own volume: "
      .. tostring(now))

--
-- **The case that separates identity from name.** The real stick is away and
-- a *different* stick called `PHOTOS` is plugged in. Keyed on the name, the
-- shortcut would silently open somebody else's drive.
--
local imposter = {
  { name = "PHOTOS", id = "fat:5555-6666", unit = 2, partition = 0 },
}

now, why = places.resolve(a, imposter)

check(now == nil and why == "unplugged",
      "another drive with the same name is not this place's drive: "
      .. tostring(now))

now, why = places.resolve(a, {})

check(now == nil and why == "unplugged", "nothing plugged in is unplugged")

check(places.resolve(home, {}) == "/home/Music",
      "a place not on a drive is always there")

check(places.resolve({ kind = "note" }, plugged) == nil,
      "something that is not a place resolves to nothing")

--------------------------------------------------------------------------
-- Reading `/home/Places`.
--------------------------------------------------------------------------

local tree = {
  ["/home/Places/myPhotos"] = { kind = "place", volume = "fat:1A2B-3C4D",
                                within = "/Italy", volume_name = "PHOTOS" },
  ["/home/Places/Backup"]   = { kind = "place", volume = "fat:0BAD-CAFE",
                                within = "/" },
  ["/home/Places/Music"]    = { kind = "place", path = "/home/Music" },
  ["/home/Places/readme"]   = { kind = "file" },
}

local store = {
  list = function(path)
    local names = {}

    for full in pairs(tree) do
      local rest = full:match("^" .. path:gsub("%-", "%%-") .. "/([^/]+)$")

      if rest then names[#names + 1] = rest end
    end

    table.sort(names)
    return names
  end,
  getattr = function(path) return tree[path] end,
}

local list = places.read(store)
local order = {}

for i, p in ipairs(list) do order[i] = p.name end

check(table.concat(order, ",") == "Backup,Music,myPhotos",
      "only places are listed, sorted without regard to case: "
      .. table.concat(order, ","))

check(list[3] and list[3].file == "/home/Places/myPhotos"
      and list[3].attrs.within == "/Italy",
      "each place carries its file and what it points at")

check(#places.read({ list = function() return nil end,
                     getattr = function() return nil end }) == 0,
      "no `/home/Places` yet is no places, not an error")

if failed == 0 then
  print(("PASS: %d checks on Tracker's shortcut places - made, named, and "
         .. "found again by what their volume is, on this machine."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
