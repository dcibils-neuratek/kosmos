-- Burns a core for a while, so there is something for a meter to show.
--
-- Deliberately does not yield. A process that hands the core back politely
-- is not what a workload looks like: the point is to be something the
-- scheduler has to preempt, so the numbers read what real work would.
--
--   spin        ten seconds
--   spin 3      three
--
-- There is no way to kill a process yet, so it stops on its own rather
-- than looping for ever. That is a real limitation and this is written
-- around it rather than pretending otherwise.

local seconds = tonumber(args) or 10
local hz = fs.read("/dev/cpu").counter_hz
local until_ = sys.ticks() + hz * seconds

while sys.ticks() < until_ do end
