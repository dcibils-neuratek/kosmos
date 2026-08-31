-- Change a property of something that is running.
--
--   setprop /app/gallery/title  a different name
--   setprop /app/gallery/x 300
--
-- The target contains no code about any of this. It called `ui.window`,
-- and the window kit registered it with /app and answers for its
-- properties - `roadmap.md` M7's scripting architecture, and the same
-- bargain BeOS made: an application was scriptable because its author used
-- the framework, not because they supported scripting.
--
-- Started by `wm` as `setprop:/app/gallery/title=something` when there is
-- no shell available to type at, which there is not while the window
-- manager has the keyboard.

--
-- `path=value` first, then `path value`.
--
-- Order matters and the other way round is wrong: "a/b=c d e" split on the
-- space gives the path "a/b=c", which is a path nothing has, and the error
-- says "no such property" about a name nobody typed. A path never contains
-- a space or an equals sign, so an equals sign that comes before any space
-- is the separator.
--
local text = tostring(args or "")
local path, value = text:match("^%s*([^%s=]+)=(.*)$")

if not path then
  path, value = text:match("^%s*(%S+)%s+(.*)$")
end

if not path or value == nil then
  print("usage: setprop <path> <value>")
  print("       setprop <path>=<value>")
  return
end

-- The target may not have registered yet: `wm` starts everything it was
-- given at once, and there is no ordering between them. Waiting a little is
-- honest about that; waiting for ever would not be.
local hz = fs.read("/dev/cpu").counter_hz
local until_ = sys.ticks() + hz * 5
local ok, err

repeat
  ok, err = fs.write(path, value)
  if ok then break end
  sys.yield()
until sys.ticks() > until_

if not ok then
  print(("setprop: %s: %s"):format(path, tostring(err)))
  return
end

print(("%s is now %s"):format(path, value))
