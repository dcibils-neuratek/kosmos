-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- log: everything this machine has printed, at the prompt.
--
--   log              the last forty lines
--   log 100          the last hundred
--   log all          all of it
--   log wm           every line mentioning `wm`
--   log screen       every line mentioning `screen`
--
-- **The console half of `logview`, and it exists because the window half
-- cannot be trusted to run.**
--
-- `logview` shows the same ring in a window, which is the nicer way to read
-- it and is useless in the one situation that matters: a machine whose
-- desktop is the thing being diagnosed. A window is drawn by the window
-- manager, so asking the window manager to show you why the window manager
-- is unwell is a question that cannot be answered when the answer is most
-- wanted. Every instrument built for the first real machine had that shape,
-- and every one of them went dark at the same moment.
--
-- This does not. It is a program at the prompt: it needs the shell, the
-- console and the keyboard, which are exactly the three things that were
-- still working on a laptop whose desktop was not. Control-C leaves the
-- desktop, this prints what happened while it was up, and the boot log -
-- stages one to six included, long since scrolled off the screen - is still
-- in the ring to be read.
--
-- On a board with a serial cable none of this is needed, because the cable
-- has been carrying it all along. That is precisely why it did not exist
-- until there was a machine without one.

local want = args:match("^%s*(%S*)") or ""

local text = sys.log(65536)

if not text or text == "" then
  print("log: the ring is empty, which should not happen while you are reading this")
  return
end

--
-- Split first, filter after. The ring is bytes and hands back whatever it
-- was holding, which begins mid-line whenever it has wrapped - so the first
-- line is a fragment and is kept rather than dropped: a truncated line is
-- still evidence, and silently removing it would make the oldest thing in
-- the log the one thing this refuses to show.
--
local lines = {}

for line in (text .. "\n"):gmatch("([^\n]*)\n") do
  -- Carriage returns are for a serial terminal and are noise on a screen
  -- that has never heard of one.
  line = line:gsub("\r", "")

  if line ~= "" then
    lines[#lines + 1] = line
  end
end

local count = tonumber(want)

--
-- A word rather than a count, and the two cannot be confused because one of
-- them is a number. `log 40` is the last forty lines; `log wm` is every
-- line about the window manager, however far back it is.
--
-- Plain matching, not a pattern: somebody looking for `[7/12]` should get
-- the stage rather than a complaint about magic characters, and this is a
-- program for reading a log at two in the morning.
--
if want ~= "" and want ~= "all" and count == nil then
  local found = 0

  for _, line in ipairs(lines) do
    if line:lower():find(want:lower(), 1, true) then
      print(line)
      found = found + 1
    end
  end

  if found == 0 then
    print(("log: nothing about %q in %d lines"):format(want, #lines))
  end

  return
end

local from = 1

if want == "all" then
  from = 1
else
  from = #lines - (count or 40) + 1
  if from < 1 then from = 1 end
end

for i = from, #lines do
  print(lines[i])
end

if from > 1 then
  print(("-- %d of %d lines; `log all` for the rest"):format(#lines - from + 1, #lines))
end
