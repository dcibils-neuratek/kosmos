-- Adopts whatever replicant was left in /data, and runs it.
--
--   wm clock,tracker
--
-- This program knows nothing about clocks. It reads a description - source,
-- state, and a list of what that source may reach - and instantiates it. In
-- BeOS this was a binary add-on loaded into this address space with the full
-- run of it; here it is Lua source loaded into an environment built from the
-- `needs` list, so a replicant that asked for /dev/cpu has no name for
-- anything else.

local ui = use("/lib/ui.lua")

local win = ui.window{ title = "tracker", w = 300, h = 150, x = 420, y = 260 }

if not win then
  print("tracker: no window")
  return
end

win:add(ui.label{ x = 12, y = 10, text = "adopted from /data:" })

-- The publisher may not have got there yet: `wm` starts everything at once
-- and there is no ordering between them.
local hz = fs.read("/dev/cpu").counter_hz
local until_ = sys.ticks() + hz * 5
local description

repeat
  description = fs.read("/data/replicants/clock")
  if description then break end
  sys.yield()
until sys.ticks() > until_

if type(description) ~= "table" then
  win:add(ui.label{ x = 12, y = 40, text = "nothing was offered",
                    color = ui.theme.bad })
  win:run()
  return
end

local view, why = ui.replicant{ x = 12, y = 40, w = 276, h = 40,
                                source = description.source,
                                state  = { label = "adopted" },
                                needs  = description.needs }

if not view then
  win:add(ui.label{ x = 12, y = 40, text = tostring(why),
                    color = ui.theme.bad })
  win:run()
  return
end

win:add(view)

--
-- What it reached, as reported by it.
--
-- The replicant tries both paths itself, from inside its own environment,
-- and leaves the answers on the instance. This program does not repeat the
-- experiment, because it cannot: building the same restricted namespace and
-- probing that measures the function and not the environment - which was the
-- first version, and it went on reporting a refusal with the sandbox
-- deliberately opened.
--
local inside = view.instance

win:add(ui.label{ x = 12, y = 92,
                  text = "/dev/cpu, which it declared:  "
                         .. (inside.declared and "reached" or "MISSING"),
                  color = inside.declared and ui.theme.text_dim
                                          or ui.theme.bad })

win:add(ui.label{ x = 12, y = 108,
                  text = "/data, which it did not:  "
                         .. (inside.escaped and "REACHED IT" or "no such path"),
                  color = inside.escaped and ui.theme.bad or ui.theme.good })

win:run()
