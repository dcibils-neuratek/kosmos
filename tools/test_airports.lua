-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **The Airports library on the host** (`docs/flight.md` F8): the file
-- `tools/airports.py` made, asked the three questions Flight asks.
--
--     build/host/lua tools/test_airports.lua build/airports/airports.tsv
fs = { read = function(path)
  local f = io.open(path, "rb")
  if not f then return nil, "no file" end
  local text = f:read("a")
  f:close()
  return text
end }
local airports = dofile("user/lib/airports.lua")
local fails, checks = 0, 0
local function check(ok, what)
  if ok then checks = checks + 1 else fails = fails + 1 print("  " .. what) end
end

local db = assert(airports.open(arg[1]))

-- A code each way: ICAO's, IATA's, any case.
local sdu, sbrj = db:find("sdu"), db:find("SBRJ")
check(sdu and sbrj and sdu.ident == "SBRJ" and sbrj.iata == "SDU",
      "SDU and SBRJ are not Santos Dumont")
check(db:find("NOPE") == nil and db:find("a b") == nil, "a code nobody has found an airport")

-- Santos Dumont's longest runway first, 02R/20L, 4341 ft, its 02R end at
-- the threshold `CITIES` had by hand and heading about north.
local r = sbrj and sbrj.runways[1]
check(r and r.name == "02R/20L" and r.length == 4341, "SDU's longest runway is not 02R/20L")
local e = r and r.ends[1]
check(e and math.abs(e.lat + 22.916389) < 1e-6 and math.abs(e.lon + 43.1625) < 1e-6
      and e.feet == 9, "02R's threshold is not where OurAirports has it")
check(e and (e.heading > 355 or e.heading < 1), "02R does not point north: " .. tostring(e and e.heading))
check(r and math.abs((r.ends[1].heading + 180) % 360 - r.ends[2].heading) < 0.5,
      "a runway's two ends do not point opposite ways")

-- Heathrow by name, and its two parallels heading 090 and 270.
local found = db:search("heathrow", 4)
check(#found >= 1 and found[1].ident == "EGLL", "searching heathrow did not find EGLL first")
local egll = found[1]
check(egll and #egll.runways == 2 and math.abs(egll.runways[1].ends[1].heading - 90) < 1.5,
      "Heathrow's runways do not head 090")

-- "paris": the large airports first - Charles de Gaulle and Orly - and
-- not Paris, Texas.
local paris = db:search("paris", 3)
check(#paris == 3 and paris[1].size == "large" and paris[1].ident:sub(1, 3) == "LFP"
      and paris[2].ident:sub(1, 3) == "LFP",
      "searching paris did not find Paris's large airports first: "
      .. table.concat((function() local t = {} for _, a in ipairs(paris) do t[#t + 1] = a.ident end return t end)(), " "))

-- Near Rio: Santos Dumont and Galeao within 30 km, nearest first.
local near = db:near(-43.205, -22.955, 30)
local idents = {}
for i, a in ipairs(near) do idents[a.ident] = i end
check(idents.SBRJ and idents.SBGL and idents.SBRJ < idents.SBGL,
      "near Rio is not SBRJ then SBGL")
check(#near > 0 and near[1].km <= near[#near].km, "near is not nearest first")

-- Every line parses, and every runway has two ends with places.
local lines, runways, bad = 0, 0, 0
for line in db.text:gmatch("\n([^\n]+)") do
  lines = lines + 1
  local a = airports.parse(line)
  if not a or #a.runways == 0 then bad = bad + 1 else
    for _, w in ipairs(a.runways) do
      runways = runways + 1
      if not (w.ends[1].lat and w.ends[2].lon and w.length >= 1500) then bad = bad + 1 end
    end
  end
end
check(lines > 5000 and bad == 0, ("%d of %d lines did not parse to runways"):format(bad, lines))

-- A place along a heading (F10c): 10 nm out from SDU's 02R threshold on
-- the reciprocal of its heading is 18.52 km away, back on that bearing.
do
  local e = sdu and sdu.runways[1].ends[1]
  local back = e and (e.heading + 180) % 360
  local lon, lat
  if e then lon, lat = airports.ahead(e.lon, e.lat, back, 18.52) end
  check(lon and math.abs(airports.km(e.lon, e.lat, lon, lat) - 18.52) < 0.01
        and math.abs((airports.bearing(e.lon, e.lat, lon, lat) - back + 180) % 360 - 180) < 0.1,
        "ahead did not put a place 18.52 km out on the bearing asked")
end

if fails > 0 then
  print(("FAIL: %d of %d checks on the Airports library"):format(fails, fails + checks))
  os.exit(1)
end
print(("PASS: %d checks on the Airports library - %d airports and %d runways, SDU and "
       .. "SBRJ, Heathrow by name, Rio's two fields nearest first"):format(checks, lines, runways))
