-- Puts the machine under load, so the meters have something to say.
--
--   benchmark        one busy process for ten seconds
--   benchmark 4      four of them
--
-- Each one is `spin`, started detached: this program asks for them to be
-- *started*, not finished, so it comes back to the prompt immediately
-- while they run. Watch with `htop 5` or `monitor watch`.
--
-- It is a program, in /bin, launching other programs in /bin. Nothing
-- about that is special-cased anywhere: `run` is a function this process
-- was handed by whoever started it, and it can pass on no more than it
-- holds.

local n = tonumber(args) or 1

if n < 1 or n > 8 then
  print("usage: benchmark [1-8]")
  print("  that many processes spinning for ten seconds each")
  return
end

for i = 1, n do
  local ok, err = run("/bin/spin.lua", "10", true)

  if not ok then
    print(("could not start %d of %d: %s"):format(i, n, tostring(err)))
    return
  end
end

print(("%d process%s burning for ten seconds."):format(n, n == 1 and "" or "es"))
print("Watch with `htop 5`, or `monitor watch` on the screen.")
