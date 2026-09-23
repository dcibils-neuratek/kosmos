-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kill: end a process by its number.
--
--   kill 14
--
-- kosmos: needs processes
--
-- **The declaration is a request, never a grant.** The kernel refuses a
-- flag the parent does not hold, so this program gets process control only
-- when whoever started it had it to give - which is why it is written down
-- here rather than assumed, and why running it from somewhere unprivileged
-- fails at `sys.kill` rather than at the door.
--
-- `ps` lists the numbers - it did not until this was written, which is the
-- other half of the same hole: the Processes application has been able to
-- end things since it was written, so you could kill with the mouse and
-- neither name nor end one from the prompt.

local want = args:match("^%s*(%S+)")

if not want then
  print("kill: kill <id or name>        `ps` lists both")
  return
end

local id = tonumber(want:match("^%d+$") or "")

--
-- **A name as well as a number, because a number is not what anybody has.**
--
-- `ps` prints both and the name is the half a person remembers; an id is
-- something you copy from a listing you had to run first. This resolves one
-- to the other rather than making you do it.
--
-- **Two matches is a refusal, not a guess.** Ending the wrong process is
-- not recoverable, and "which of the two `spin`s did you mean" is a
-- question only the person asking can answer. Nothing here picks the
-- newest, or the first, or the one using the most processor.
--
if not id then
  local found = {}

  for _, r in ipairs(sys.processes() or {}) do
    if r.name == want and not r.exited then
      found[#found + 1] = r.id
    end
  end

  if #found == 0 then
    print("kill: nothing running is called " .. want)
    return
  end

  if #found > 1 then
    print(("kill: %d processes are called %s: %s")
          :format(#found, want, table.concat(found, " ")))
    print("  say which one by its id")
    return
  end

  id = found[1]
end

local ok, why = sys.kill(id)

if not ok then
  print(("kill: %d: %s"):format(id, tostring(why)))
  return
end

print("ended " .. id)
