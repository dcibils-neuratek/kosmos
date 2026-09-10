-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- What opens when the desktop does, read by the two programs that care.
--
-- **A module rather than the same four lines in both**, and the reason is
-- what happens when they drift. `startup.lua` draws the panel of tick
-- boxes; `deskbar.lua` opens what is ticked. If one of them decided on a
-- default and the other did not, the desktop would come up with three
-- windows on it and the panel would show nothing ticked - which is not a
-- cosmetic disagreement, it is a machine telling you two different things
-- about itself, and this codebase has been bitten by exactly that more than
-- once today.
--
local M = {}

M.SETTINGS = "/home/.startup"

--
-- What a machine that has never been told opens.
--
-- **A default rather than an empty desktop**, because an empty desktop
-- shows you nothing about the machine you just booted - and on a first
-- boot, on new hardware, what a person wants to see is whether it works.
-- Tracker says the filesystem answers, Processes says the scheduler is
-- running things, and the monitor says the machine is alive.
--
-- **And the log, which is the one that earned its place on hardware.** A
-- laptop has no serial port, so the kernel's ring is the only complete
-- record of what the machine said about itself - and the moment it is
-- wanted is the first boot on a machine nobody has run this on before,
-- which is precisely the moment nobody has opened a window yet.
--
--
-- `topbar` first, because it is a strip: the window manager gives it the
-- top of the screen and everything else opens below it, so opening it last
-- would mean every other window had already chosen a place that is now one
-- bar too high.
--
M.DEFAULT = { "topbar", "tracker", "sysmon", "procs", "logview" }

--
-- **Absent and empty are different, and the difference is the whole
-- point.** No file means nobody has chosen, so the default stands. A file
-- holding an empty list means somebody unticked everything, and bringing
-- the default back would be the preferences panel refusing to be used.
--
function M.items()
  local saved = fs.read(M.SETTINGS)

  if type(saved) == "table" and type(saved.items) == "table" then
    return saved.items, false
  end

  return M.DEFAULT, true
end

return M
