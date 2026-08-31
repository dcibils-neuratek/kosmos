-- A live query: the same question as `find`, answered when the answer
-- changes.
--
--   watch kind=note        report the next three changes
--   watch kind=note 1      report the next one and stop
--
-- Run it detached and then write something that matches from the prompt.
--
--------------------------------------------------------------------------
-- What "live" means here, and what it does not.
--
-- This process is not polling. It is not on a timer and it is not asking
-- again and again: it makes one call to the filesystem, and the filesystem
-- does not answer until the answer is different. In between, this process
-- is blocked - not runnable, not scheduled, costing nothing. `htop` shows
-- it at zero.
--
-- That is why the filesystem parks the reply instead of calling back. A
-- server that calls a client blocks on that client, and a filesystem that
-- can be blocked by one slow watcher is a filesystem that one slow watcher
-- can stop.
--------------------------------------------------------------------------

local where = {}
local rounds = 3
local terms = 0

for word in tostring(args or ""):gmatch("%S+") do
  local name, value = word:match("^([^=]+)=(.*)$")

  if name then
    where[name] = tonumber(value) or value
    terms = terms + 1
  elseif tonumber(word) then
    rounds = tonumber(word)
  end
end

if terms == 0 then
  print("usage: watch name=value [count]")
  return
end

local known = fs.query("/data", where) or {}

print(("watching for %d change(s); %d match now"):format(rounds, #known))

for i = 1, rounds do
  local paths, err = fs.watch("/data", where, known)

  if not paths then
    print("watch: " .. tostring(err))
    return
  end

  known = paths
  print(("change %d: %d match now"):format(i, #paths))

  for _, path in ipairs(paths) do
    print("  " .. path)
  end
end
