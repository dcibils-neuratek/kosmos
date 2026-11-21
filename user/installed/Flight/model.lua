-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Flight's model (`docs/flight.md` F10b): the arithmetic of flying, with
-- nothing of the screen in it - an aircraft file's fields and their
-- checks, the wind, one step of the flight model, and the autopilot. It
-- takes everything it works on as arguments and reaches for nothing: no
-- `fs`, no kits, no settings. That is what lets `tools/test_flightmodel.lua`
-- fly the Cessna and the 747 on this Mac for minutes of their time in a
-- second, which is how the autopilot is tuned and held. `flight.lua` is
-- its other user, and the only one that draws.

local M = {}
local G = 9.81
M.G = G

-- **An aircraft file's fields** (F12): what every file must say, what each
-- kind of engine needs, the moving parts a model may name, and what a
-- file may leave out with the Cessna's numbers standing in.
local FIELDS = {
  needs = { "mass", "wing", "cl0", "cla", "stall", "cd0", "k", "flaps_cl", "flaps_cd",
            "roll_rate", "alpha_most", "gear" },
  engines = { propeller = { "power", "static", "prop_drag", "idle_rpm", "full_rpm" },
              jet = { "static", "idle", "lapse", "spool_s", "idle_rpm", "full_rpm" } },
  parts = { "elevator", "flaps", "right", "left", "rudder", "prop" },
  -- What a file may leave out, and the Cessna's numbers for it: the speeds
  -- a flight starts at in the air and on a final approach, m/s, with the
  -- throttle and flaps there; the chase camera's distance behind and
  -- height; the pilot's eye ahead of the aircraft's point and above it.
  optional = {
    speeds = { cruise = 55, cruise_throttle = 0.7, approach = 36, approach_throttle = 0.35,
               approach_flaps = 0.5 },
    chase = { back = 28, high = 7 },
    eye = { ahead = 0, high = 1.0 },
  },
}

function M.aircraft(text, key, folder)
  if type(text) ~= "string" then return nil, "not read: " .. tostring(text) end
  local chunk, err = load(text, "=" .. key, "t", { math = math })
  if not chunk then return nil, tostring(err) end
  local ok, a = pcall(chunk)
  if not ok then return nil, tostring(a) end
  if type(a) ~= "table" then return nil, "it returns no table" end
  if type(a.name) ~= "string" or type(a.model) ~= "string" then return nil, "no name or model" end
  for _, f in ipairs(FIELDS.needs) do
    if type(a[f]) ~= "number" then return nil, "no " .. f end
  end
  local e = a.engine
  local wants = type(e) == "table" and FIELDS.engines[e.kind]
  if not wants then return nil, "no engine Flight knows: " .. tostring(type(e) == "table" and e.kind) end
  for _, f in ipairs(wants) do
    if type(e[f]) ~= "number" then return nil, "no engine." .. f end
  end
  -- The moving parts its model has, each by its name there: a model with
  -- none - the 747's - moves none, and the rest of it flies the same.
  if a.moving ~= nil and type(a.moving) ~= "table" then return nil, "moving is not a table" end
  a.parts, a.surface = {}, {}
  for _, part in ipairs(FIELDS.parts) do
    local name = a.moving and a.moving[part]
    if name ~= nil then
      if type(name) ~= "string" then return nil, "moving." .. part .. " is not a name" end
      a.parts[#a.parts + 1] = name
      a.surface[part] = #a.parts
    end
  end
  -- **Its pose** (`pose`): a model drawn in a frame of its own, as most
  -- are, says which way is its nose and which is up in that frame, how
  -- many metres one of its units is, and the point that sits on the
  -- runway. Without one the model is Flight's frame already: +x the nose,
  -- +y up, metres, the wheels at nought - the Cessna's.
  local pose = a.pose
  if pose ~= nil then
    local function vec(v) return type(v) == "table" and type(v[1]) == "number"
                                 and type(v[2]) == "number" and type(v[3]) == "number" end
    if type(pose) ~= "table" or not vec(pose.forward) or not vec(pose.up) or not vec(pose.origin)
       or type(pose.scale) ~= "number" then
      return nil, "pose needs forward, up, origin and scale"
    end
    local f, u = pose.forward, pose.up
    local r = { f[2] * u[3] - f[3] * u[2], f[3] * u[1] - f[1] * u[3], f[1] * u[2] - f[2] * u[1] }
    a.axes = { f, u, r }
  end
  local sd = a.sound
  if type(sd) ~= "table" or type(sd.note) ~= "number" or type(sd.hz) ~= "number" then
    return nil, "no sound's note and hz"
  end
  for name, defaults in pairs(FIELDS.optional) do
    local given = a[name]
    if given ~= nil and type(given) ~= "table" then return nil, name .. " is not a table" end
    local t = {}
    for f, v in pairs(defaults) do
      local x = given and given[f]
      if x ~= nil and type(x) ~= "number" then return nil, name .. "." .. f .. " is not a number" end
      t[f] = x or v
    end
    a[name] = t
  end
  a.key, a.path = key, (folder or "") .. "/" .. a.model
  return a
end

-- Every file in the folder, read: the ones that fly for the sheet's row,

-- **The wind** (F6, W2): `W` holds its strength in knots, the bearing it
-- blows from, and its own clock; the strength gusts by two slow waves, a
-- fifth of it at most. Its velocity now, metres a second east and south.
function M.wind(W, dt)
  W.t = (W.t or 0) + dt
  local v = (W.kt or 0) * 0.5144
  v = v * (1 + 0.12 * math.sin(0.7 * W.t) + 0.08 * math.sin(1.9 * W.t + 1))
  local from = math.rad(W.from or 270)
  W.x, W.z = -v * math.sin(from), v * math.cos(from)
  return W.x, W.z
end

-- One step of the flight model, `dt` seconds: speed, climb and heading from
-- the forces in the path's frame, as a point mass with an attitude - which
-- is how a trainer feels, and a few dozen multiplications.
function M.fly(S, P, W, dt, pitch, roll, ground_h, brake, say)
  S.simt = (S.simt or 0) + dt          -- the flight's own seconds, the time faster's too
  local rho = 1.225 * math.exp(-S.y / 8500)
  -- On its wheels S.v is the ground's speed; the wings feel the headwind's
  -- share on top of it (W2).
  local wx, wz = M.wind(W, dt)
  local head = -(wx * math.sin(S.psi) - wz * math.cos(S.psi))
  local air = S.ground and math.max(0, S.v + head) or S.v
  local q = 0.5 * rho * air * air

  -- The yoke sets the angle of attack it asks for; the aircraft gets there
  -- quickly but not at once, as a real one's pitch does.
  local trim = 0.03 + 0.04 * S.flaps
  local want = trim + pitch * P.alpha_most
  S.alpha = S.alpha + (want - S.alpha) * math.min(1, 3 * dt)

  local cl = P.cl0 + P.cla * S.alpha + P.flaps_cl * S.flaps
  if S.alpha > P.stall then cl = cl * 0.55 end   -- stalled
  local cd = P.cd0 + P.k * cl * cl + P.flaps_cd * S.flaps
  local lift, drag = q * P.wing * cl, q * P.wing * cd
  -- **The engine's thrust** (F12): a propeller's falls as the speed rises,
  -- its power spread over more metres a second; a jet's spools towards the
  -- throttle over seconds, idles at a share of its thrust, thins with the
  -- air and falls a little with speed.
  local en, thrust = P.engine, 0
  if en.kind == "jet" then
    S.spool = (S.spool or S.throttle)
    S.spool = S.spool + (S.throttle - S.spool) * math.min(1, dt / en.spool_s)
    thrust = (en.idle + (1 - en.idle) * S.spool) * en.static * (rho / 1.225) ^ 0.75
             * math.max(0.5, 1 - en.lapse * air)
  else
    S.spool = S.throttle
    thrust = S.throttle * math.min(en.static, en.power / math.max(air, 1))
  end
  local m = P.mass

  if S.ground then
    -- Rolling: no bank, the wheels' friction - and the brakes', B (F8),
    -- about half a g on a dry runway - and off when lift is enough.
    S.phi = 0
    local friction = (0.04 + 0.45 * (brake or 0)) * math.max(0, m * G - lift)
    -- An idling propeller holds the aircraft back, a windmill's drag that
    -- fades as the throttle opens. Without it a landing at idle coasted
    -- about a kilometre on 0.04 g of rolling alone (Diego, 21 November:
    -- "i am at 0% throttle and the plane is still running on the floor");
    -- with it a C172's roll is a few hundred metres, brakes off.
    local windmill = en.prop_drag and (1 - S.throttle) * 0.5 * rho * air * air * en.prop_drag or 0
    S.v = math.max(0, S.v + (thrust - drag - windmill - friction) / m * dt)
    S.gamma = 0
    if lift > m * G * 1.02 and pitch > 0 then
      S.ground = false
      S.v = air                          -- the air's speed from here on
      local r = S.rolled
      say(("lifted off at %.0f kt after %s"):format(air * 1.94384, r and ("%.0f m on the runway in %.1f s")
          :format(math.sqrt((S.x - r.x) ^ 2 + (S.z - r.z) ^ 2), S.simt - r.t) or "a roll not started from rest"))
      S.rolled = nil
    end
    S.psi = S.psi + roll * math.rad(10) * dt * math.min(1, S.v / 10)
    -- On its wheels it follows the ground, which a start on a runway
    -- reckons from the airfield's elevation before the tiles are in.
    if ground_h then S.y = ground_h + P.gear end
  else
    S.phi = S.phi + roll * P.roll_rate * dt
    -- Let go of the ailerons and a trainer rolls back towards level.
    if roll == 0 then S.phi = S.phi * (1 - 0.4 * dt) end
    S.phi = math.max(-math.rad(75), math.min(math.rad(75), S.phi))
    local v = math.max(S.v, 5)
    S.v = S.v + ((thrust * math.cos(S.alpha) - drag) / m - G * math.sin(S.gamma)) * dt
    S.v = math.max(S.v, 0)
    S.gamma = S.gamma + ((lift * math.cos(S.phi) + thrust * math.sin(S.alpha))
                         / (m * v) - G * math.cos(S.gamma) / v) * dt
    S.gamma = math.max(-1.4, math.min(1.4, S.gamma))
    S.psi = S.psi + lift * math.sin(S.phi) / (m * v * math.cos(S.gamma)) * dt
  end

  local horizontal = S.v * math.cos(S.gamma)
  S.x = S.x + horizontal * math.sin(S.psi) * dt
  S.z = S.z - horizontal * math.cos(S.psi) * dt
  if not S.ground then                  -- the air it flies in moves
    S.x, S.z = S.x + wx * dt, S.z + wz * dt
  end
  S.y = S.y + S.v * math.sin(S.gamma) * dt

  -- The ground: touched gently it is a landing, hard it is the end - and
  -- gently is under about 600 feet a minute (F8), a trainer's firm one,
  -- and wings within twenty degrees of level.
  if ground_h and S.y < ground_h + P.gear then
    local sink = -S.v * math.sin(S.gamma)
    if not S.ground then
      if sink > 600 / 196.85 or math.abs(S.phi) > math.rad(20) then
        S.crashed = ("touched down at %.0f fpm, %.0f degrees of bank")
                    :format(sink * 196.85, math.deg(math.abs(S.phi)))
      else
        S.landed = sink * 196.85
        -- What the wheels meet is the ground's speed: the air's, less the
        -- headwind - a landing into the wind is slower over the runway.
        S.touch_air, S.touch_ground = S.v, math.max(0, S.v - head)
        S.v = S.touch_ground
      end
    end
    S.y = ground_h + P.gear
    S.ground = true
  end
end

-- **The autopilot** (F10b): three holds, each a small controller over the
-- controls a pilot has - the yoke's pitch and roll, and the throttle - so
-- the flight model above is not touched and flies the same under it.
--
-- - **Heading**: a bank towards the heading wanted, as steep as the error
--   is large and never past 25 degrees, and the ailerons towards that bank.
-- - **Altitude**: a climb or descent towards the height wanted, at most
--   `vs_most` m/s, and the yoke towards that rate - proportional and
--   integral, since holding a rate needs a pitch that is not nought.
-- - **Speed**: the throttle towards the speed wanted, the same way.
--
-- `AP.on` switches all three; engaging takes what the aircraft is doing
-- as what to hold, and the yoke and throttle where they are, so nothing
-- jumps.
M.AP_BANK_MOST = math.rad(25)

-- The gains, found by `tools/test_flightmodel.lua` on both aircraft:
-- heading in radians of bank per radian of error, the seconds the
-- ailerons take to reach the bank wanted, altitude in m/s of climb per
-- metre off, and the yoke's and throttle's per m/s and per m/s of error.
local AP_GAIN = {
  heading = 1.2, roll_s = 2,
  alt = 0.08,
  pitch_p = 0.06, pitch_i = 0.01,
  speed = 0.1, accel_most = 0.5, throttle_p = 0.3, throttle_i = 1,
}
M.AP_GAIN = AP_GAIN

function M.engage(AP, S, P)
  AP.on = true
  AP.heading, AP.alt, AP.speed = S.psi, S.y, S.v
  local trim = 0.03 + 0.04 * S.flaps
  AP.pitch_i = math.max(-1, math.min(1, (S.alpha - trim) / P.alpha_most))
  AP.throttle_i = S.throttle
  AP.vs_most = AP.vs_most or 5
end

-- The thrust the engine would give at full throttle, here and now: what
-- one more unit of throttle is worth, for the speed hold.
function M.full_thrust(S, P)
  local en, rho = P.engine, 1.225 * math.exp(-S.y / 8500)
  if en.kind == "jet" then
    return en.static * (rho / 1.225) ^ 0.75 * math.max(0.5, 1 - en.lapse * S.v)
  end
  return math.min(en.static, en.power / math.max(S.v, 1))
end

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

-- One step: the yoke's pitch and roll it asks for, and the throttle set.
function M.autopilot(AP, S, P, dt)
  -- Heading: the error the short way round.
  local e = (AP.heading - S.psi + math.pi) % (2 * math.pi) - math.pi
  local bank = clamp(e * AP_GAIN.heading, -M.AP_BANK_MOST, M.AP_BANK_MOST)
  local roll = clamp((bank - S.phi) / (P.roll_rate * AP_GAIN.roll_s), -1, 1)
  -- Altitude, through the rate of climb.
  local vs = S.v * math.sin(S.gamma)
  local vs_want = clamp((AP.alt - S.y) * AP_GAIN.alt, -AP.vs_most, AP.vs_most)
  local ve = vs_want - vs
  AP.pitch_i = clamp(AP.pitch_i + AP_GAIN.pitch_i * ve * dt, -1, 1)
  local pitch = clamp(AP.pitch_i + AP_GAIN.pitch_p * ve, -1, 1)
  -- Speed, through the throttle: an acceleration wanted, a tenth of the
  -- error a second and never more than `accel_most`, against the one
  -- measured, and the throttle moved by the difference in this aircraft's
  -- own terms - its mass over its full thrust here and now - so a Cessna
  -- and a 747 are one loop with one set of gains.
  local a = (S.v - (AP.v_last or S.v)) / dt
  AP.v_last = S.v
  AP.a = (AP.a or 0) + (a - (AP.a or 0)) * math.min(1, dt / 0.5)   -- smoothed
  local a_want = clamp((AP.speed - S.v) * AP_GAIN.speed, -AP_GAIN.accel_most, AP_GAIN.accel_most)
  local per = P.mass / math.max(1, M.full_thrust(S, P))
  AP.throttle_i = clamp(AP.throttle_i + AP_GAIN.throttle_i * (a_want - AP.a) * per * dt, 0, 1)
  S.throttle = clamp(AP.throttle_i + AP_GAIN.throttle_p * (a_want - AP.a) * per, 0, 1)
  return pitch, roll
end

return M
