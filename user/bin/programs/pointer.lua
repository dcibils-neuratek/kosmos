-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- pointer: how fast the pointer moves.
--
--   pointer          what it is now
--   pointer 48       set it
--
-- **How far the pointer travels per count of movement**, in the device's
-- own units. A count is one step of a mouse or one sample of a TrackPoint's
-- strain gauge; the window manager maps the device's whole range onto the
-- screen, so a bigger number here is a pointer that crosses the screen in
-- fewer counts.
--
-- The number that matters is this against the range, not the number alone.
-- The i8042 driver invents a range of 32767, which on a 1920-wide panel is
-- about seventeen units to the pixel - so 32 is roughly 1.9 pixels a count
-- and 8, which it used to be, was under half a pixel. That is measured on
-- the machine this was written for, and it is why the default moved.
--
-- **It does not survive a restart, and that is not laziness.** A setting
-- belongs in a file, `/home` is the place for one, and on a machine with no
-- disk `/home` does not outlive the power. Writing it there would be a
-- preference that silently forgets - worse than one you type, because you
-- would stop expecting to. When there is a disk this grows two lines.
--
-- A board whose pointer is *absolute* - a tablet under emulation - answers
-- zero: it reports where it is rather than how far it moved, and there is
-- nothing for a multiplier to act on.

local want = args:match("^%s*(%S*)")

if want == "" then
  local now = sys.pointer_speed()

  if now == 0 then
    print("pointer: this board's pointer is absolute; there is no speed to set")
  else
    print(("pointer: %d units per count"):format(now))
  end

  return
end

local units = tonumber(want)

if not units or units < 1 then
  print("pointer: pointer <number>        a bigger number is a faster pointer")
  return
end

local now = sys.pointer_speed(math.floor(units))

if now == 0 then
  print("pointer: this board's pointer is absolute; nothing was changed")
else
  print(("pointer: %d units per count"):format(now))
end
