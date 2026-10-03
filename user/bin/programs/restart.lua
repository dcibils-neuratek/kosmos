-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- restart: start the machine again.
--
--   restart
--
-- kosmos: needs processes
--
-- **The Kosmos menu's Restart, at the prompt** (`testing.md` 18.352; Diego,
-- 3 October: "Can you work on the restart function on in kosmos? That is
-- helpful for the user as well"). And the step the build cycle needs over
-- Telnet: restart the M700, and it boots whatever the Mac now serves
-- (`roadmap.md`, build, boot and test the M700 in a loop).
--
-- **Through the desktop when there is one**, as the menu goes: the window
-- manager tells every window to close and then asks the kernel, so an
-- application is told before the machine goes. With no desktop - a prompt
-- on the serial line, or before `wm` has started - the kernel is asked
-- directly. Either way it is `SYS_POWER`, which only a process holding
-- process control may call; that is what `needs processes` asks for, and
-- it is granted only by a parent that has it to give.
--
-- Nothing after this line runs when it works. When it does not, the reason
-- is said: the firmware's own way, the chipset's reset port, the keyboard
-- controller and a triple fault have all been tried by then
-- (`hal/pc/power.c`), each said in the log as it was.

print("restart: restarting")

if fs.getattr("/Running/wm") then
  local ok, why = fs.send("/Running/wm", { type = "power", action = "restart" })

  if not ok then
    print("restart: the desktop could not: " .. tostring(why))
  end
end

local _, why = sys.power("restart")

print("restart: " .. tostring(why))
