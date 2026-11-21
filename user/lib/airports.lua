-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Airports and their runways** (`docs/flight.md` F8): every paved runway
-- an airport with a code has, from OurAirports (public domain), as
-- `tools/airports.py` makes it small - one line an airport, sorted by its
-- code, its runways at the end of it.
--
-- A library rather than Flight's own, because what it answers is not
-- about flying: which airport is "SBRJ", which are called "heathrow",
-- which are near a place. Flight is its first user; a map that marks
-- airports, or the Radio Kit choosing the tower nearest a field (F9), asks
-- the same three questions.
--
--     local airports = use("/Kosmos/Libraries/airports.lua")
--     local db = airports.open("/Home/Apps/Flight/airports.tsv")
--     db:find("SDU")             -> an airport, or nil
--     db:search("santos", 8)     -> up to 8 airports, a code's first
--     db:near(-43.2, -22.9, 30)  -> those within 30 km, nearest first
--
-- An airport is { ident, iata, name, town, country, size, lon, lat, feet,
-- runways } - `size` "large", "medium" or "small" -, and a runway { name, length, width, ends }, its two ends each
-- { name, lon, lat, feet, heading } - the threshold, its elevation, and
-- the true heading of a take-off or landing from it, towards the other.
--
-- The file is held as one string, 1.6 MB, and a question is a `find` over
-- it: no table per airport is made until one is asked for, so the
-- collector walks one string and not nine thousand tables.

local airports = {}

local db = {}
db.__index = db

function airports.open(path)
  local text, why = fs.read(path)
  if type(text) ~= "string" or #text == 0 then
    return nil, "no airports in " .. tostring(path) .. ": " .. tostring(why or "empty")
  end
  return setmetatable({ text = "\n" .. text, lower = nil }, db)
end

-- The true heading from one place to another, in degrees from north.
local function bearing(lon1, lat1, lon2, lat2)
  local p1, p2 = math.rad(lat1), math.rad(lat2)
  local dl = math.rad(lon2 - lon1)
  local y = math.sin(dl) * math.cos(p2)
  local x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
  return (math.deg(math.atan(y, x)) + 360) % 360
end
airports.bearing = bearing

-- Kilometres between two places, on a sphere.
local function km(lon1, lat1, lon2, lat2)
  local p1, p2 = math.rad(lat1), math.rad(lat2)
  local a = math.sin((p2 - p1) / 2) ^ 2
            + math.cos(p1) * math.cos(p2) * math.sin(math.rad(lon2 - lon1) / 2) ^ 2
  return 12742 * math.asin(math.min(1, math.sqrt(a)))
end
airports.km = km

local SIZES = { l = "large", m = "medium", s = "small" }
local RANK = { l = 1, m = 2, s = 3 }

-- One line, made an airport.
local function parse(line)
  local ident, iata, name, town, country, size, lat, lon, feet, rest =
    line:match("^([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([lms])\t([^\t]*)\t([^\t]*)\t"
               .. "([^\t]*)\t([^\t]*)$")
  if not ident then return nil end
  local a = { ident = ident, iata = iata ~= "" and iata or nil, name = name, town = town,
              country = country, size = SIZES[size], lat = tonumber(lat), lon = tonumber(lon),
              feet = tonumber(feet) or 0, runways = {} }
  for r in rest:gmatch("[^|]+") do
    local le, he, length, width, la1, lo1, f1, la2, lo2, f2 =
      r:match("^([^;]*);([^;]*);([^;]*);([^;]*);([^;]*);([^;]*);([^;]*);([^;]*);([^;]*);([^;]*)$")
    if le then
      la1, lo1, la2, lo2 = tonumber(la1), tonumber(lo1), tonumber(la2), tonumber(lo2)
      a.runways[#a.runways + 1] = {
        name = le .. "/" .. he, length = tonumber(length), width = tonumber(width),
        ends = {
          { name = le, lon = lo1, lat = la1, feet = tonumber(f1) or a.feet,
            heading = bearing(lo1, la1, lo2, la2) },
          { name = he, lon = lo2, lat = la2, feet = tonumber(f2) or a.feet,
            heading = bearing(lo2, la2, lo1, la1) },
        },
      }
    end
  end
  table.sort(a.runways, function(x, y) return x.length > y.length end)
  return a
end
airports.parse = parse

-- The line around a place in the text.
local function line_at(text, at)
  local first = at
  while first > 1 and text:byte(first - 1) ~= 10 do first = first - 1 end
  local last = text:find("\n", at, true) or #text + 1
  return text:sub(first, last - 1), last
end

-- An airport by its code - ICAO's or the local one, or IATA's - any case.
function db:find(code)
  code = tostring(code or ""):upper()
  if not code:match("^[%w%-]+$") then return nil end
  local at = self.text:find("\n" .. code .. "\t", 1, true)
  if at then return parse((line_at(self.text, at + 1))) end
  at = self.text:find("\n[^\t\n]*\t" .. code .. "\t")
  if at then return parse((line_at(self.text, at + 1))) end
  return nil
end

-- Airports whose code is `text`, then those whose name or town has it,
-- the large first, then the medium and the small - so "paris" is Charles
-- de Gaulle and Orly before Paris, Texas - up to `most`.
function db:search(text, most)
  most = most or 8
  local found, seen = {}, {}
  local exact = self:find(text)
  if exact then found[1], seen[exact.ident] = exact, true end
  local want = tostring(text or ""):lower()
  if #want < 3 then return found end
  self.lower = self.lower or self.text:lower()
  local hits, at = {}, 1
  while #hits < 400 do
    local hit = self.lower:find(want, at, true)
    if not hit then break end
    local line, after = line_at(self.text, hit)
    at = after
    -- The name and the town, not a number that happens to hold the text.
    local ident, _, name, town, _, size =
      line:match("^([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([lms])\t")
    if ident and not seen[ident] and (name:lower():find(want, 1, true)
                                      or town:lower():find(want, 1, true)) then
      seen[ident] = true
      hits[#hits + 1] = { rank = RANK[size], order = #hits, line = line }
    end
  end
  table.sort(hits, function(x, y)
    if x.rank ~= y.rank then return x.rank < y.rank end
    return x.order < y.order
  end)
  for i = 1, #hits do
    if #found >= most then break end
    found[#found + 1] = parse(hits[i].line)
  end
  return found
end

-- Airports within `within` km of a place, nearest first.
function db:near(lon, lat, within, most)
  local found = {}
  local dlat = within / 111
  for line in self.text:gmatch("\n([^\n]+)") do
    local la, lo = line:match("^[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[lms]\t([^\t]*)\t([^\t]*)\t")
    la, lo = tonumber(la), tonumber(lo)
    if la and math.abs(la - lat) < dlat then
      local d = km(lon, lat, lo, la)
      if d <= within then found[#found + 1] = { d = d, line = line } end
    end
  end
  table.sort(found, function(x, y) return x.d < y.d end)
  local out = {}
  for i = 1, math.min(#found, most or #found) do
    local a = parse(found[i].line)
    a.km = found[i].d
    out[#out + 1] = a
  end
  return out
end

return airports
