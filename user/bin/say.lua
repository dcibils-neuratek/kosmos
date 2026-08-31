-- Prints something, optionally after waiting.
--
--   say hello there
--   say 3 hello there      after three seconds
--
-- The delay is what makes this more than `echo`. A test that wants to know
-- whether output reached the screen has to take a picture before the output
-- and a picture after it, and every other program here prints the moment it
-- starts - so the first picture already has the answer in it. That is not
-- hypothetical: the check for the console staying off the framebuffer was
-- written with `hello`, and it passed with the bug deliberately put back.

local text = tostring(args or "")
local seconds, rest = text:match("^%s*(%d+)%s+(.*)$")

if seconds then
  local hz = fs.read("/dev/cpu").counter_hz
  local until_ = sys.ticks() + hz * tonumber(seconds)

  while sys.ticks() < until_ do
    sys.yield()
  end

  text = rest
end

if text == "" then
  text = "nothing in particular"
end

-- Several lines rather than one, because one line at the top of a screen is
-- easy to miss and a scroll moves everything.
for i = 1, 6 do
  print(("%d: %s"):format(i, text))
end
