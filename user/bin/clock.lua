-- kosmos: application
-- A window with a clock in it, and an offer to give the clock away.
--
--   wm clock
--   wm clock,tracker        and something that adopts it
--
-- The clock is a replicant: source, state, and a list of what it needs. It
-- is published into /data so that another process can pick it up - in BeOS
-- you dragged it, and dragging is what a pointer is for, which this machine
-- does not have yet. The mechanism is the same either way and the pointer
-- is the part that is missing.

local ui = use("/lib/ui.lua")

local source = fs.read("/lib/clock-replicant.lua")

if not source then
  print("clock: /lib/clock-replicant.lua is not there")
  return
end

local description = {
  source = source,
  state  = { label = "up" },
  needs  = { "/dev/cpu" },
}

-- Offered rather than sent: whoever wants it comes and gets it. This is
-- where a drag would deliver it, and until there is a pointer this is the
-- desktop that holds it.
local ok, err = fs.write("/data/replicants/clock", description)

if not ok then
  print("clock: could not publish it: " .. tostring(err))
  return
end

local win = ui.window{ title = "clock", w = 260, h = 130, x = 70, y = 90 }

if not win then
  print("clock: no window")
  return
end

win:add(ui.label{ x = 12, y = 10, text = "a replicant lives here" })
win:add(ui.label{ x = 12, y = 26, text = "and is offered in /data",
                  color = ui.theme.text_dim })

local view, why = ui.replicant{ x = 12, y = 52, w = 236, h = 40,
                                source = description.source,
                                state = description.state,
                                needs = description.needs }

if not view then
  print("clock: " .. tostring(why))
  return
end

win:add(view)
win:run()
