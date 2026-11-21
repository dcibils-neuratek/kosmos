-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Tiles: the one door to `/Tiles`, the server that fetches a map's tiles
-- from the network into `/Home/Cache/Maps` (`user/servers/tiles.c`,
-- `tileproto.h`, `docs/flight.md` F1).
--
--   local tiles = use("/Kosmos/Libraries/tiles.lua")
--   local src = tiles.source(url, "png")    a source, or nil and why
--   src.number, src.cache                   its number, its cache folder
--   src:want({ z, x, y, z, x, y, ... })     these wanted, the first most
--   local came = src:arrived()              { { z =, x =, y = }, ... } since
--                                           the last ask, or nil and why
--   src.outstanding, src.failed, src.why    as the last answer said
--   src:path(z, x, y)                       where a tile is kept
--   tiles.numbered(n)                       a source by its number
--   tiles.finder(url)                       where places are looked up
--   local n = tiles.find(words)             a search's number
--   tiles.found()                           the last search answered, and
--                                           its HTTP status (0: not asked)
--   tiles.places(n, status)                 that search's places: { { name,
--                                           where, class, rank, lat, lon } },
--                                           or nil and why
--
-- **Why a library**: the protocol is a declared shape, packed with
-- `string.pack` to `tileproto.h`, and it was packed inside Maps alone -
-- Flight, wanting imagery and elevation from the same server, would have
-- been its second copy (`CLAUDE.md`, *Kits supply, applications
-- orchestrate*). When the header changes, this is the one Lua file that
-- changes with it.
--
-- **Several sources at once**: each `source` is its own number at the
-- server, with its own queue and arrivals, so a slow one never holds
-- another back; the same address and format asked for again is the same
-- source. `format` is the extension the tiles are kept under - "pbf",
-- "png", "jpg" - because a picture is decoded by its name, and the caller
-- chose the source so knows what it sends.
--
-- Nothing here waits: every request is answered at once, and what has come
-- is asked for on the caller's own clock.
--
-- A program reaches `/Tiles` only if it declares `kosmos: needs tiles` and
-- its starter holds it - the window manager does, the prompt's shell does
-- not.

local tiles = {}

local OP_SOURCE, OP_WANT, OP_ARRIVED, OP_FIND, OP_FINDER = 1, 2, 3, 4, 5
local MAX = 64                                              -- TILES_MAX
local REQUEST = "<I4I4I4I4c8" .. string.rep("I4", MAX * 3)  -- 792 bytes
local TEXT = "<I4I4I4I4c8c768"                              -- the same, a URL
local REPLY = "<I4I4I4I4I4I4I4I4c64c128" .. string.rep("I4", MAX * 3)
local REPLY_BYTES = 992

tiles.MAX = MAX
local CACHE = "/Home/Cache/Maps"                            -- tiles.c's CACHE_ROOT

local ZEROS = {}

for i = 1, MAX * 3 do ZEROS[i] = 0 end

local function zstring(s) return (s:gsub("%z.*$", "")) end

local finding = { found = 0, status = 0 }

-- One exchange; the reply as a table, or nil and why.
local function ask(packed)
  local reply, why = fs.raw("/Tiles", packed, nil, "tiles")

  if type(reply) ~= "string" or #reply < REPLY_BYTES then return nil, why or "no answer" end

  local v = { string.unpack(REPLY, reply) }
  local out = { status = v[1], count = v[2], seq = v[3], outstanding = v[4], failed = v[5],
                found = v[6], found_status = v[7], source = v[8], cache = zstring(v[9]),
                why = zstring(v[10]), tiles = {} }

  for i = 1, out.count do
    local at = 10 + (i - 1) * 3

    out.tiles[i] = { z = v[at + 1], x = v[at + 2], y = v[at + 3] }
  end

  finding.found, finding.status = out.found, out.found_status

  if out.status ~= 0 then return nil, out.why ~= "" and out.why or ("refused, " .. out.status) end

  return out
end

local Source = {}
Source.__index = Source

-- These tiles wanted, the first most: a flat list of z, x, y, at most
-- `tiles.MAX` tiles. It replaces this source's last list.
function Source:want(ids)
  local count = #ids // 3

  if count > MAX then return nil, "at most " .. MAX .. " tiles at once" end

  local packed = { table.unpack(ids, 1, count * 3) }

  for i = count * 3 + 1, MAX * 3 do packed[i] = 0 end

  local said, why = ask(string.pack(REQUEST, OP_WANT, count, 0, self.number, "",
                                    table.unpack(packed)))

  if not said then return nil, why end

  self.outstanding, self.failed, self.why = said.outstanding, said.failed, said.why
  return said
end

-- What has come since the last ask: the tiles, each now in the cache - or
-- an empty file, a tile there is none of.
function Source:arrived()
  local said, why = ask(string.pack(REQUEST, OP_ARRIVED, 0, self.since, self.number, "",
                                    table.unpack(ZEROS)))

  if not said then return nil, why end

  self.since = said.seq
  self.outstanding, self.failed, self.why = said.outstanding, said.failed, said.why
  return said.tiles, said
end

function Source:path(z, x, y)
  return ("%s/%d/%d-%d.%s"):format(self.cache, z, x, y, self.format)
end

-- A source: an address with {z}, {x} and {y} in it, or a TileJSON's; and
-- the format its tiles are kept under.
function tiles.source(url, format)
  local said, why = ask(string.pack(TEXT, OP_SOURCE, 0, 0, 0, format, url))

  if not said then return nil, why end

  return setmetatable({ number = said.source, cache = said.cache, format = format, url = url,
                        since = 0, outstanding = 0, failed = said.failed, why = said.why },
                      Source)
end

-- A source by the number the server gave it - for a program told the
-- number rather than the address. Its folder is not known here.
function tiles.numbered(number, format)
  return setmetatable({ number = number, cache = "", format = format or "", since = 0,
                        outstanding = 0, failed = 0, why = "" }, Source)
end

-- Where places are looked up: an address "?q=..." is added to.
function tiles.finder(url)
  local said, why = ask(string.pack(TEXT, OP_FINDER, 0, 0, 0, "", url))

  return said ~= nil, why
end

-- A place looked up by name: the search's number. Its answer is written to
-- `/Home/Cache/Maps/places/<number>.json`, and `found` says when.
function tiles.find(words)
  local said, why = ask(string.pack(TEXT, OP_FIND, 0, 0, 0, "", words))

  if not said then return nil, why end

  return said.seq
end

-- The last search answered, and how: as the last answer to anything said.
function tiles.found()
  return finding.found, finding.status
end

-- A search's places, once `found` says it was answered, read from where the
-- server wrote Nominatim's answer: what a person would call each, where it
-- is, its kind and Nominatim's `place_rank`, and its longitude and
-- latitude. Maps and Flight both read them here, so the answer's shape is
-- known in one place.
local json
function tiles.places(number, status)
  if status ~= 200 then return nil, "the search could not be answered" end

  local text = fs.read(CACHE .. "/places/" .. number .. ".json")

  if type(text) ~= "string" then return nil, "the answer could not be read" end

  json = json or use("/Kosmos/Libraries/json.lua")

  local list, why = json.decode(text)

  if type(list) ~= "table" then return nil, "the answer is not JSON: " .. tostring(why) end

  local out = {}

  for _, r in ipairs(list) do
    local lat, lon = tonumber(r.lat), tonumber(r.lon)

    if lat and lon then
      local name = type(r.name) == "string" and r.name ~= "" and r.name
                   or tostring(r.display_name or ""):match("^[^,]+") or "?"
      local where = tostring(r.display_name or ""):gsub("^[^,]*,%s*", "")
      -- What a person would call it: Nominatim's `type` is "administrative"
      -- for a city drawn as a boundary, and its `addresstype` says "city".
      local class = (r.type == "administrative" or not r.type) and r.addresstype or r.type

      out[#out + 1] = { name = name, where = where, class = tostring(class or "place"),
                        rank = tonumber(r.place_rank) or 30, lat = lat, lon = lon }
    end
  end

  return out
end

return tiles
