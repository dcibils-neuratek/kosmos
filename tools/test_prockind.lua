-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What kind of thing a process is, tested on this machine.
--
--   build/host/lua tools/test_prockind.lua
--
-- `/lib/prockind.lua` over rows shaped as `sys.processes()` gives them, and
-- a `/bin` with the Drives app in it - the name the drives server shares.

local prockind = dofile("user/lib/prockind.lua")

local passed, failed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("FAIL: " .. what)
  end
end

local BIN = { drives = "app", tracker = "app", wm = "program", ps = "program",
              htop = "program" }

local function kind(p)
  return prockind.of(p, BIN)
end

local DEV = prockind.DEVICES

check(kind{ id = 11, name = "xhci", parent = 1, owns = DEV } == "driver",
      "the USB driver is a driver - Diego: \"isnt the e1000 a driver, not a "
      .. "server?\"")
check(kind{ id = 10, name = "e1000", parent = 1, owns = DEV } == "driver",
      "and so is the Ethernet driver")
check(kind{ id = 1, name = "init", parent = 0, owns = DEV | 1 | 2 }
      == "server", "init holds device authority to hand on, and is a server")
check(kind{ id = 15, name = "drives", parent = 1, owns = 0 } == "server",
      "the drives server is init's, whatever /bin calls a drives")
check(kind{ id = 40, name = "drives", parent = 22, owns = 0 } == "app",
      "and the Drives app, launched, is an app")
check(kind{ id = 16, name = "shell", parent = 1, owns = 1 | 2 } == "program",
      "the shell is init's and a program")
check(kind{ id = 18, name = "wm", parent = 16, owns = 2 } == "program",
      "a launched program is what its header says")
check(kind{ id = 30, name = "strip", parent = 18, owns = 0 } == "program",
      "and one launched from outside /bin is a program")
check(kind{ id = 9, name = "audio", parent = 1, owns = 0 } == "server",
      "a server init started")

if failed > 0 then
  print(("\nFAIL: %d of %d checks on a process's kind."):format(failed,
        passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on what kind of thing a process is (a driver by its "
       .. "device authority, a server by init starting it, the rest by /bin)")
      :format(passed))
