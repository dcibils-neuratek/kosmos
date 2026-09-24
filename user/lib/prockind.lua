-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What kind of thing a process is, for a window that lists them: a driver,
-- a server, an app or a program.
--
-- Here rather than inside Processes so that it can be checked without a
-- screen: `tools/test_prockind.lua` runs it on the build machine.
--
-- **Two facts the kernel reports, and one a file declares** (`roadmap.md`
-- 6g). Diego, 24 September: "isnt the e1000 a driver, not a server? i see it
-- as a server in the process viewer". Processes took a kind from a `/bin`
-- file of the same name and called everything else a server, under a note
-- that every driver lived in the kernel - untrue since the power button's.
-- And the drives *server* read "app", after the Drives app it shares a name
-- with; open the app and there are two processes called `drives`.
--
--   - **A driver holds device authority** (`proc_info.owns`, 16). Only init
--     hands it on, and only to the processes that drive hardware - init
--     holds it to hand on, and is the one exception. This is the one place
--     a grant is read as a kind, and it is exact because nothing else is
--     ever given it: the screen, which the shell holds only to pass it to
--     the desktop, is the counter-example that makes that worth saying.
--   - **A server is what init started** (`proc_info.parent`, 1): the
--     system's own, kept for the life of the machine. The shell is init's
--     too, and is a program.
--   - **Everything else was launched**, and its file in `/bin` says what it
--     is in its header: `app`, `program`, or `server`. Something launched
--     from a file outside `/bin` is a program.

local prockind = {}

prockind.INIT    = 1         -- the first process's id
prockind.DEVICES = 16        -- `proc_info.owns`: device authority

-- `p` is a row of `sys.processes()`. `from_bin` maps a file in `/bin`, less
-- its `.lua`, to the kind its header declares.
function prockind.of(p, from_bin)
  if p.name == "shell" then return "program" end

  if p.id == prockind.INIT then return "server" end

  if ((p.owns or 0) & prockind.DEVICES) ~= 0 then return "driver" end

  if p.parent == prockind.INIT then return "server" end

  return (from_bin or {})[p.name] or "program"
end

return prockind
