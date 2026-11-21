-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A Boeing 747-400 (`docs/flight.md`, F12b), at a landing weight rather
-- than its full 397 tonnes, so it flies from a runway the Cessna's
-- airports have. The fields are `cessna172.lua`'s; what is new here is a
-- jet engine, a model drawn in a frame of its own (`pose`), and the
-- numbers a big aircraft needs that a small one could leave out: the
-- speeds it starts at, where the chase camera sits and the pilot's eye.

return {
  name = "Boeing 747",

  -- The airframe: 300 t on 525 m^2 of wing, with flaps that add a great
  -- deal of lift at 30 degrees - about 140 kt on the approach.
  mass = 300000, wing = 525,
  cl0 = 0.25, cla = 5.5, stall = math.rad(14),
  cd0 = 0.022, k = 0.045,
  flaps_cl = 0.9, flaps_cd = 0.060,
  roll_rate = math.rad(25), alpha_most = math.rad(10),
  gear = 6.0,                                    -- its point's height on its wheels, m

  -- **Four jets**, 282 kN each standing at sea level. Thrust thins with
  -- the air, falls a little with speed (`lapse`, a share a m/s), idles at
  -- 6%, and follows the throttle over about six seconds - which is the
  -- difference from a propeller that a landing feels first.
  engine = {
    kind = "jet",
    static = 4 * 282000,                         -- N
    idle = 0.06, lapse = 0.0012, spool_s = 6,
    idle_rpm = 1500, full_rpm = 4500,            -- the whine's rate, for the sound
  },

  model = "boeing_747.glb",                      -- assets/models/b747, CC BY 3.0
  -- The model has no moving parts, so `moving` is left out. It is drawn
  -- about 1.7 units long in a frame of its own: its nose and its up in
  -- that frame, found from the geometry (the airframe's flattest axis is
  -- up, the cockpit glass is ahead), 41.13 metres a unit for its 70.7 m,
  -- and the point under its middle, a metre below the belly - it has no
  -- wheels - that sits on the runway.
  pose = {
    forward = { 0.22750, -0.46126, -0.85760 },
    up = { 0.21663, 0.88260, -0.41724 },
    origin = { -0.01560, -0.09980, 0.00303 },
    scale = 41.13,
  },

  speeds = { cruise = 128, cruise_throttle = 0.55,       -- 250 kt at 1,500 ft
             approach = 74, approach_throttle = 0.45, approach_flaps = 1 },
  chase = { back = 110, high = 24 },
  eye = { ahead = 27, high = 3.3 },              -- the flight deck, 27 m forward

  -- A jet's sound: the piston's voice with its wobble taken out, a deeper
  -- sub and more noise - a roar with a whine through it.
  sound = { kind = "jet", note = 40, hz = 82.41,
            voice = { w1 = 1, w2 = 4, mix = 0.55, noise = 0.45, sub = 0.7, lfoC = 0,
                      res = 0.35 } },
}
