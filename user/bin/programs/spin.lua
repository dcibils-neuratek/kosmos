-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Burns a core for a while, so there is something for a meter to show.
--
-- Deliberately does not yield. A process that hands the core back politely
-- is not what a workload looks like: the point is to be something the
-- scheduler has to preempt, so the numbers read what real work would.
--
--   spin            ten seconds
--   spin 3          three
--   spin 3 display  three, in the display band, where every program is
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

--
-- **First: get out of the compositor's band.**
--
-- `process_grant_screen` promotes to SCHED_PRIO_DISPLAY and `init.lua` hands
-- the screen to every program it launches - its own comment calls that
-- "wrong and staying for now" - so this program starts life in the same band
-- as the window manager. A workload that outranks the desktop is not a
-- workload, it is a denial of service: with three of these running, `cores`
-- stopped answering its own buttons and the machine looked hung.
--
-- Which is exactly the failure `cores` predicted in writing and attributed
-- to the wrong cause. Its header said "a spinner runs at NORMAL and this
-- window runs at DISPLAY, so the buttons keep answering while a core is
-- pinned" - and the spinner did not run at NORMAL, so they did not.
--
-- `sys.step_down` only ever gives a band up, so this grants nothing and can
-- be refused without consequence: an older kernel without it leaves this
-- program where it was, which is where it has always been.
--
local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

--
-- **Unless asked to stay**, with `display`: a busy application is in that
-- band, because the screen is granted to every program, and that is the
-- load the audio band has to hold its own against (`roadmap.md` 4i) - so
-- `run_synth.py` makes it, on every core, while Groove plays.
--
if words[2] ~= "display" and sys.step_down then
  pcall(sys.step_down, 2)                             -- 2 is NORMAL
end

local seconds = tonumber(words[1]) or 10
local hz = fs.read("/Devices/cpu").counter_hz
local until_ = sys.ticks() + hz * seconds

while sys.ticks() < until_ do end
