-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Flight's model on the host** (`docs/flight.md` F10b): the Cessna's and
-- the 747's own files read the way Flight reads them, each put in level
-- flight and handed to the autopilot, and flown for minutes of their time
-- in well under a second. The autopilot holds what it was engaged on, then
-- follows a new heading, height and speed, and must arrive at each and stay.
--
--     build/host/lua tools/test_flightmodel.lua build/airports/airports.tsv [frozen|no-descent|no-flare]
--
-- Then the route (F10c): Carrasco to Ezeiza on the real airports file,
-- each aircraft taken from 1,500 ft over Carrasco to Ezeiza's final fix,
-- 10 nm out, by `M.route` and `M.guide` alone.
--
-- `frozen` is the first control: the autopilot's two integrators held where
-- engaging put them, so the yoke and throttle answer only the error now.
-- A held climb or speed needs a pitch and a power that are not where the
-- aircraft began, so without the integrators it settles off the mark, and
-- the checks that it arrived must fail.
-- `no-descent` is the second: the route's top of descent taken away, so
-- the aircraft arrives at its cruise height and must fail the fix's.
-- `no-flare` is the third: the landing flown down the glide path into the
-- runway, which a 747 does at about 800 fpm.
local M = dofile("user/installed/Flight/model.lua")
fs = { read = function(path)
  local f = io.open(path, "rb")
  if not f then return nil, "no file" end
  local text = f:read("a")
  f:close()
  return text
end }
local airports = dofile("user/lib/airports.lua")
local db = assert(airports.open(arg[1]))
local control = arg[2]
local frozen = control == "frozen"
if frozen then M.AP_GAIN.pitch_i, M.AP_GAIN.throttle_i = 0, 0 end

local fails, checks = 0, 0
local function check(ok, what)
  if ok then checks = checks + 1 else fails = fails + 1 print("  " .. what) end
end

local function read(path)
  local f = assert(io.open(path, "rb"))
  local t = f:read("a")
  f:close()
  return t
end

local function deg(r) return math.deg(r) % 360 end
local function off(a, b) return math.abs((a - b + 180) % 360 - 180) end

for _, key in ipairs{ "cessna172", "b747" } do
  local P, why = M.aircraft(read("user/installed/Flight/Aircraft/" .. key .. ".lua"), key, "")
  check(P, key .. " was not read: " .. tostring(why))
  if not P then goto next end
  do
    -- Level at its cruise, 1500 m up, heading north, no wind.
    local S = { x = 0, z = 0, y = 1500, v = P.speeds.cruise, psi = 0, phi = 0,
                gamma = 0, alpha = 0.04, flaps = 0, throttle = P.speeds.cruise_throttle,
                ground = false }
    local W = { kt = 0, from = 270 }
    local AP = {}
    local dt = 1 / 30
    local said = {}
    local function say(s) said[#said + 1] = s end
    local function fly(seconds)
      for _ = 1, math.floor(seconds / dt) do
        local pitch, roll = M.autopilot(AP, S, P, dt)
        M.fly(S, P, W, dt, pitch, roll, 0, false, say)
      end
    end
    -- Settle the aircraft by itself first: the yoke at what level flight wants.
    M.engage(AP, S, P)
    fly(120)
    check(not S.crashed and not S.ground, key .. " came down while settling")
    local h0, a0, v0 = deg(AP.heading), AP.alt, AP.speed
    check(off(deg(S.psi), h0) < 2 and math.abs(S.y - a0) < 30 and math.abs(S.v - v0) < 2,
          ("%s did not hold what it was engaged on: heading %.0f/%.0f, %.0f/%.0f m, %.1f/%.1f m/s")
          :format(key, deg(S.psi), h0, S.y, a0, S.v, v0))

    -- Now a new heading, 90 degrees right, 300 m higher and a tenth faster.
    AP.heading = AP.heading + math.pi / 2
    AP.alt = AP.alt + 300
    AP.speed = AP.speed * 1.1
    local most_bank, most_climb = 0, 0
    for _ = 1, 300 * 30 do
      local pitch, roll = M.autopilot(AP, S, P, dt)
      M.fly(S, P, W, dt, pitch, roll, 0, false, say)
      most_bank = math.max(most_bank, math.abs(S.phi))
      most_climb = math.max(most_climb, S.v * math.sin(S.gamma))
    end
    local h, line = deg(S.psi), ("%s after 300 s: heading %.1f for %.1f, %.0f m for %.0f, %.1f m/s for %.1f, banked %.0f at most, climbed %.1f m/s at most")
      :format(key, deg(S.psi), deg(AP.heading), S.y, AP.alt, S.v, AP.speed, math.deg(most_bank), most_climb)
    print(line)
    check(off(h, deg(AP.heading)) < 1, key .. " did not come round to its new heading")
    check(math.abs(S.y - AP.alt) < 15, key .. " did not arrive at its new height")
    check(math.abs(S.v - AP.speed) < 1, key .. " did not arrive at its new speed")
    check(most_bank < M.AP_BANK_MOST + math.rad(3), key .. " banked past the autopilot's most")
    check(most_climb < AP.vs_most * 1.2, key .. " climbed more than a fifth past the autopilot's most")
    check(not S.crashed, key .. " crashed under the autopilot")
  end
  ::next::
end

-- **The route** (F10c): Carrasco to Ezeiza, the wind calm.
local mvd, eze = db:find("MVD"), db:find("EZE")
check(mvd and eze, "Carrasco or Ezeiza is not in the airports file")
for _, key in ipairs{ "cessna172", "b747" } do
  local P = mvd and eze and M.aircraft(read("user/installed/Flight/Aircraft/" .. key .. ".lua"), key, "")
  if not P then goto skip end
  do
    local plan = assert(M.route(mvd, eze, P, nil, airports))
    if control == "no-descent" then plan.tod_nm = -1e9 end
    -- Positions as longitude and latitude, moved by the model's metres:
    -- Flight's own frame does the same around its anchor.
    local lon0, lat0 = mvd.lon, mvd.lat
    local S = { x = 0, z = 0, y = mvd.feet * 0.3048 + 457, v = P.speeds.cruise,
                psi = math.rad(plan.track), phi = 0, gamma = 0, alpha = 0.04, flaps = 0,
                throttle = P.speeds.cruise_throttle, ground = false }
    local W, AP, dt = { kt = 0, from = 270 }, {}, 1 / 30
    local phases = {}
    local function say(s) local p = s:match("^route (%a+)") if p then phases[#phases + 1] = p end end
    M.engage(AP, S, P)
    local R = 6371000
    local function lonlat()
      return lon0 + math.deg(S.x / (R * math.cos(math.rad(lat0)))), lat0 - math.deg(S.z / R)
    end
    local closest, at = math.huge, nil
    local t = 0
    while t < 3 * 3600 do
      local lon, lat = lonlat()
      M.guide(plan, AP, S, P, lon, lat, airports, say)
      local pitch, roll = M.autopilot(AP, S, P, dt)
      M.fly(S, P, W, dt, pitch, roll, 0, false, say)
      t = t + dt
      if S.crashed then break end
      local d = airports.km(lon, lat, plan.fix[1], plan.fix[2]) / 1.852
      if d < closest then
        closest = d
        local along, right = M.on_final(plan, lon, lat, airports)
        at = { nm = d, ft = S.y / 0.3048, heading = math.deg(S.psi) % 360, right = right / 1.852,
               t = t }
      elseif closest < 2 and d > closest + 1 then
        break                                   -- past the fix, and going away
      end
    end
    print(("%s route %s %s to %s %s, %.0f nm, cruise %d ft, descent %.0f nm out; at the fix after %.0f min: "
           .. "%.2f nm off it, %.2f nm right of the line, %.0f ft for %d, heading %.0f for %.0f; phases %s")
          :format(key, mvd.iata, plan.dep.name, eze.iata, plan.arr.name, plan.nm, plan.cruise_ft,
                  plan.tod_nm, at and at.t / 60 or 0, at and at.nm or -1, at and at.right or 0,
                  at and at.ft or 0, plan.final_ft, at and at.heading or 0, plan.arr.heading,
                  table.concat(phases, ",")))
    check(not S.crashed, key .. " crashed on the route: " .. tostring(S.crashed))
    check(at and at.nm < 1, key .. " did not pass within a nautical mile of the final fix")
    check(at and math.abs(at.ft - plan.final_ft) < 300, key .. " was not at 3,000 ft over Ezeiza at the fix")
    check(at and off(at.heading, plan.arr.heading) < 20, key .. " was not lined up with the runway at the fix")
    -- Each phase once; the way's in its order and the height's in its own.
    local seen = table.concat(phases, ",")
    local function before(a, b) return (seen:find(a, 1, true) or 1e9) < (seen:find(b, 1, true) or -1) end
    check(#phases == 6 and before("enroute", "intercept") and before("intercept", "final")
          and before("climb", "cruise") and before("cruise", "descent"),
          key .. " did not fly each phase once, in order: " .. seen)
  end
  ::skip::
end

-- **Runway to runway** (F10d): the same route begun on Carrasco's runway
-- at rest, taken off, flown, landed on Ezeiza's 29 and stopped. The
-- ground is each field's elevation near it.
for _, key in ipairs{ "cessna172", "b747" } do
  local P = mvd and eze and M.aircraft(read("user/installed/Flight/Aircraft/" .. key .. ".lua"), key, "")
  if not P then goto skip2 end
  do
    local plan = assert(M.route(mvd, eze, P, nil, airports))
    local dep = plan.dep
    local lon0, lat0 = dep.lon, dep.lat
    local field0, field1 = dep.feet * 0.3048, plan.arr.feet * 0.3048
    local S = { x = 0, z = 0, y = field0 + P.gear, v = 0, psi = math.rad(dep.heading), phi = 0,
                gamma = 0, alpha = 0.03, flaps = 0, throttle = 0, ground = true }
    local W, AP, dt = { kt = 0, from = 270 }, {}, 1 / 30
    local ways = {}
    local function say(s) local p = s:match("^route (%a+)") if p then ways[#ways + 1] = p end end
    M.engage(AP, S, P)
    if control == "no-flare" then M.FLARE_OFF = true end
    local R = 6371000
    local function lonlat()
      return lon0 + math.deg(S.x / (R * math.cos(math.rad(lat0)))), lat0 - math.deg(S.z / R)
    end
    local lifted, touched, t, rolled = nil, nil, 0, 0
    while t < 3 * 3600 do
      local lon, lat = lonlat()
      local near_dep = airports.km(lon, lat, dep.lon, dep.lat) < airports.km(lon, lat, plan.arr.lon, plan.arr.lat)
      local ground = near_dep and field0 or field1
      local way = M.guide(plan, AP, S, P, lon, lat, airports, say, ground)
      if way == "stopped" then break end
      local pitch, roll = M.autopilot(AP, S, P, dt)
      local was = S.ground
      M.fly(S, P, W, dt, pitch, roll, ground, AP.brake, function() end)
      t = t + dt
      if S.crashed then break end
      if was and not S.ground and not lifted then
        lifted = { m = math.sqrt(S.x ^ 2 + S.z ^ 2), kt = S.v * 1.94384 }
      end
      if not was and S.ground and lifted and not touched then
        local along, right = M.on_final(plan, lon, lat, airports)
        touched = { fpm = S.landed or 0, along = along * 1000, right = right * 1000,
                    kt = S.v * 1.94384 }
      end
    end
    local lon, lat = lonlat()
    local along, right = M.on_final(plan, lon, lat, airports)
    local rw = plan.arr_runway
    print(("%s runway to runway %s %s to %s %s in %.0f min: lifted off at %.0f kt after %.0f m of %.0f; "
           .. "touched down at %.0f fpm, %.0f m past the threshold, %.1f m off the centre line; stopped "
           .. "%.0f m along, %.1f m off; %s")
          :format(key, mvd.iata, dep.name, eze.iata, plan.arr.name, t / 60,
                  lifted and lifted.kt or 0, lifted and lifted.m or 0, plan.dep_runway.length * 0.3048,
                  touched and touched.fpm or 0, touched and -touched.along or 0,
                  touched and touched.right or 0, -along * 1000, right * 1000,
                  S.crashed and ("crashed: " .. S.crashed) or table.concat(ways, ",")))
    check(not S.crashed, key .. " crashed from runway to runway: " .. tostring(S.crashed))
    check(lifted and lifted.m < plan.dep_runway.length * 0.3048, key .. " did not leave the ground on Carrasco's runway")
    check(touched and touched.fpm < 600, key .. " did not touch down under 600 fpm")
    check(touched and -touched.along > 0 and -touched.along < rw.length * 0.3048
          and math.abs(touched.right) < rw.width * 0.3048 / 2,
          key .. " did not touch down on Ezeiza's runway")
    check(S.v < 0.5 and -along * 1000 < rw.length * 0.3048 and math.abs(right) * 1000 < rw.width * 0.3048 / 2,
          key .. " did not stop on the runway")
  end
  ::skip2::
end

-- **Where the aircraft is** (F10d): on the wheels at either end of
-- Carrasco's runway the take-off is from that end; and engaged 3 nm out
-- on Ezeiza's 29, lined up, the route is already on its final and lands.
if mvd and eze then
  local P = M.aircraft(read("user/installed/Flight/Aircraft/cessna172.lua"), "cessna172", "")
  for _, e in ipairs(mvd.runways[1].ends) do
    local plan = M.route(mvd, eze, P, nil, airports,
                         { lon = e.lon, lat = e.lat, heading = e.heading, ground = true })
    check(plan and plan.dep == e, "a take-off from MVD " .. e.name .. " was not from that end")
  end
  local off = M.route(mvd, eze, P, nil, airports,
                      { lon = mvd.lon + 0.2, lat = mvd.lat, heading = 0, ground = true })
  check(not off, "a route on the wheels off any runway was made")
  local e29
  for _, r in ipairs(eze.runways) do for _, e in ipairs(r.ends) do if e.name == "29" then e29 = e end end end
  local lon, lat = airports.ahead(e29.lon, e29.lat, (e29.heading + 180) % 360, 3 * 1.852)
  local field = e29.feet * 0.3048
  local plan = M.route(eze, eze, P, 270, airports, { lon = lon, lat = lat, heading = e29.heading })
  check(plan and plan.arr == e29, "lined up on EZE 29, the route did not land on it")
  if plan then
    local R, lon0, lat0 = 6371000, lon, lat
    local S = { x = 0, z = 0, y = field + P.gear + 3 * 1852 * M.GLIDE + 15, v = P.speeds.approach,
                psi = math.rad(e29.heading), phi = 0, gamma = -0.05, alpha = 0.05, flaps = 0.5,
                throttle = 0.3, ground = false }
    local AP, first, t = {}, nil, 0
    M.engage(AP, S, P)
    while t < 900 do
      local x, y = lon0 + math.deg(S.x / (R * math.cos(math.rad(lat0)))), lat0 - math.deg(S.z / R)
      local way = M.guide(plan, AP, S, P, x, y, airports, nil, field)
      first = first or way
      if way == "stopped" or S.crashed then break end
      local pitch, roll = M.autopilot(AP, S, P, 1 / 30)
      M.fly(S, P, { kt = 0 }, 1 / 30, pitch, roll, field, AP.brake, function() end)
      t = t + 1 / 30
    end
    print(("engaged 3 nm out on EZE 29: began at %s, ended %s after %.0f s, %s"):format(first,
          plan.way, t, S.crashed or ("landed at %.0f fpm"):format(S.landed or 0)))
    check(first == "final" and plan.way == "stopped" and not S.crashed,
          "engaged on EZE 29's final, it did not land from there")
  end
end

print(("flight model: %d checks, %d failed%s"):format(checks + fails, fails, control and (" (" .. control .. ", a control)") or ""))
os.exit(fails == 0 and 0 or 1)
