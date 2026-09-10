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

--
-- How many lines a bare `log` shows, and the most any filter prints.
--
-- A screenful, roughly. The point of both limits is the same: this is read
-- at a prompt with no scrollback, so anything above the last screen is not
-- merely unhelpful, it has pushed the useful part off the top.
--
local LINES = 40

local want = args:match("^%s*(%S*)") or ""

--
-- The whole ring, every time.
--
-- A quarter of a megabyte in one Lua string is heavy for a program that ran
-- twice a second, which is why `logview` still asks for less - but this one
-- runs when somebody types it, and the thing it must never do is search a
-- fraction of the log and answer as though it had searched all of it.
--
local text = sys.log(262144)

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
  --
  -- **What the machine said, not what you typed - and the difference took
  -- an afternoon to see.**
  --
  -- The console records everything through `kputc`, and that includes its
  -- own echo of the command line. So `log poll` put the word "poll" into
  -- the ring *by being run*, found exactly that one line, and printed it
  -- back: on screen, indistinguishable from the shell echoing the command a
  -- second time. Every search matched itself, which meant the "nothing
  -- found" message below could never fire for any query at all - the one
  -- answer a search most needs to be able to give.
  --
  -- It read as a bug in the console, or in the shell, or in `print`. It was
  -- a search engine indexing the search.
  --
  -- Lines beginning with the prompt are what a person typed, and a log
  -- search is asking what the machine answered. `log all` still shows them.
  --
  local hits = {}

  for _, line in ipairs(lines) do
    if not line:find("kosmos> ", 1, true)
       and line:lower():find(want:lower(), 1, true) then
      hits[#hits + 1] = line
    end
  end

  if #hits == 0 then
    print(("log: nothing about %q in %d lines"):format(want, #lines))
    return
  end

  --
  -- **The last screenful of the matches, not all of them.**
  --
  -- `log wm` against a traced desktop matches fifteen hundred lines, and
  -- printing them all is not merely long: every line scrolls the console,
  -- and a console that scrolls repaints its whole grid, so it is fifteen
  -- hundred repaints of a 1920x1080 screen. It looked, on the machine, like
  -- the command had printed nothing at all - the prompt came back and the
  -- output was somewhere in the middle of a flood that had scrolled past.
  --
  -- Which is the same mistake as bounding the window manager's trace by
  -- pass count: a limit in the place it does no good and none in the place
  -- it does. A log is read from the end, so the end is what is printed, and
  -- the count below says what was left out and how to ask for it.
  --
  local from = math.max(1, #hits - LINES + 1)

  for i = from, #hits do
    print(hits[i])
  end

  if from > 1 then
    print(("-- %d of %d matches; `log all` for everything"):format(
          #hits - from + 1, #hits))
  end

  return
end

local from = 1

if want == "all" then
  from = 1
else
  from = #lines - (count or LINES) + 1
  if from < 1 then from = 1 end
end

for i = from, #lines do
  print(lines[i])
end

if from > 1 then
  print(("-- %d of %d lines; `log all` for the rest"):format(#lines - from + 1, #lines))
end
