-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The desktop: your files, behind everything else.
--
-- kosmos: application
-- kosmos: icon Prefs_Backgrounds
-- kosmos: section system
--
-- **Tracker in backdrop mode, and the only reason this file exists is that
-- there was no way to ask for it.** `tracker desktop` has drawn the icon
-- desktop for as long as Tracker has had one, but the Deskbar and the
-- Startup panel both list what is in `/bin` marked as an application - so
-- `tracker` appeared once, started a window, and the backdrop was reachable
-- only by knowing to type `wm tracker:desktop`.
--
-- What that cost is not cosmetic. **The desktop is where a drag lands.**
-- Without one running, the area behind the windows belongs to no window at
-- all: `window_at` in the compositor finds nothing there, and a file
-- dragged out of a Tracker window is answered with "nothing there takes a
-- drop". It reads exactly like drag and drop being unimplemented, and it is
-- a desktop that was never started.
--
-- A launcher rather than a copy. Tracker is one program with one behaviour
-- and a flag; two files that both drew a desktop would be two files to keep
-- in step, and the second one would be the one that went stale.

--
-- **One desktop, and this is where that is decided.**
--
-- `wm` starts this by itself, and the Startup panel lists it as well - it
-- lists every application in `/bin` and this is one. Both are right and
-- together they would open a second backdrop behind the first: two Trackers
-- drawing the same folder, one of them invisible for ever, and a drop
-- landing on whichever the compositor happened to hit first.
--
-- So it asks. The window manager is the only process that knows what is on
-- screen, and it now says which window is the backdrop rather than only
-- that it is chrome - the strip is chrome too.
--
-- Refused with a sentence rather than silently exiting: somebody who ticked
-- this in Startup should be told that the desktop was already there, not
-- left wondering whether the tick did anything.
--
local seen = fs.send("/app/wm", { type = "windows" })

for _, w in ipairs(seen and seen.windows or {}) do
  if w.backdrop then
    print("desktop: there is already one")
    return
  end
end

--
-- Started by the window manager, not by `run`.
--
-- `run` gives the child a namespace in which `/app/wm` is looked up in the
-- registry by name, and a name is not the same thing as *this* window
-- manager. It was the bug that started this: the registry kept a name whose
-- holder had gone, so the desktop worked on the first `wm` of a boot and on
-- the second Tracker died at once saying "no such path: /app/wm".
--
-- The registry is fixed - a name lasts as long as the endpoint registered
-- under it - and this is still right, because a launch through the window
-- manager hands the child *this* window manager's endpoint directly, which
-- is how everything else started from the desktop gets it. A lookup would
-- find whichever one holds the name, which is only the same thing while
-- there is one.
--
local ok, why = fs.send("/app/wm", { type = "launch", program = "tracker",
                                     args = "desktop" })

if not ok then
  print("desktop: " .. tostring(why))
end
