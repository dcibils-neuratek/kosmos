-- Burns a core for a while, so there is something for a meter to show.
--
-- Deliberately does not yield. A process that hands the core back politely
-- is not what a workload looks like: the point is to be something the
-- scheduler has to preempt, so the numbers read what real work would.
--
--   spin        ten seconds
--   spin 3      three
--
-- It stops on its own rather than looping for ever, and it is the one
-- program here that Control-C does not stop. That is not an oversight, it
-- is the same fact from the other side: interruption is cooperative, a
-- process is stopped by asking whether it should be, and asking is an IPC
-- round trip - which is precisely the yield this program exists not to do.
--
-- So it is the counterexample. `monitor` and `htop` stop when you ask; this
-- one runs its ten seconds. Stopping it would need a way to end a process
-- from outside, which the kernel does not have.

local seconds = tonumber(args) or 10
local hz = fs.read("/dev/cpu").counter_hz
local until_ = sys.ticks() + hz * seconds

while sys.ticks() < until_ do end
