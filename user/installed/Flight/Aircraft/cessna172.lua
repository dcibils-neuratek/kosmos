-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A Cessna 172 (`docs/flight.md`, F12): its numbers, near enough to fly
-- like one. An aircraft is a file in Flight's `Aircraft` folder that
-- returns a table like this one. Flight reads it as data: it runs with
-- nothing but `math` to hand, and what it returns is checked field by field
-- before anything flies on it, so a file that leaves a number out is
-- refused with that number's name.

return {
  name = "Cessna 172",

  -- The airframe.
  mass = 1043, wing = 16.2,                      -- kg, m^2
  cl0 = 0.30, cla = 5.0, stall = math.rad(15),   -- lift: CL0 + CLa * alpha
  cd0 = 0.030, k = 0.050,                        -- drag: CD0 + k CL^2
  flaps_cl = 0.45, flaps_cd = 0.030,             -- at full flap, 30 degrees
  roll_rate = math.rad(70), alpha_most = math.rad(12),
  gear = 1.4,                                    -- the eye's height on its wheels, m

  -- **The engine.** A propeller's: its power at the propeller, which gives
  -- thrust falling as the speed rises, capped by what it pulls standing
  -- still; and its drag while it idles, on the ground.
  engine = {
    kind = "propeller",
    power = 96000, static = 2600,                -- W at the propeller, N standing
    prop_drag = 0.8,                             -- m^2
    idle_rpm = 700, full_rpm = 2700,             -- the engine's turns a minute
  },

  -- **What it looks like**: a glTF file in Flight's folder, and the names
  -- of its moving parts in it.
  model = "cessna_172.glb",                      -- assets/models/cessna172, CC BY 4.0
  moving = { elevator = "elevator_0", flaps = "flaps_1", right = "aileron-right_2",
             left = "aileron-left_3", rudder = "rudder_4", prop = "prop_5" },

  -- **What it sounds like**: a piston engine, four cylinders firing twice
  -- a turn, its note bent from E2.
  sound = { kind = "piston", note = 40, hz = 82.41 },
}
