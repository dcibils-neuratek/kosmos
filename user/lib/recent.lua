-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The applications opened last, newest first: what the launcher's
-- Recently used row shows (`docs/launcher.html`, agreed 7 October).
--
-- Diego: "can we add a new extra menu called "Recently used" and we will be
-- adding all the recently opened apps as a shortcut?". Nothing kept such a
-- list - the window manager knows what is running, not what ran - so the
-- window manager writes it here as an application opens its first window,
-- whoever started it, and the launcher reads it. **One list, one shape,
-- and this the one door to it**: both sides use these functions, and
-- `tools/test_launchgrid.lua` holds them on the Mac.
--
-- It is kept by the settings kit as `recent` (`prefs.read`, `prefs.write`):
-- a list, newest first, of `{ program = <path> }` - text a person can read.
-- **An application, not a start of one**: the words it was started with are
-- not kept, so Tracker opened on a folder and Tracker as the desktop are
-- one Tracker (Diego, 8 October: "tracker appears twice in the recently
-- used apps").
--

local recent = {}

recent.NAME = "recent"     -- its settings name: `prefs.read(recent.NAME)`
recent.MOST = 15           -- three rows of the launcher's grid

-- What the settings kit read as entries, `{ program }`, in its order and
-- each program once. One that is not a path is passed over, not trusted.
function recent.parse(t)
  local out, seen = {}, {}

  for _, e in ipairs(type(t) == "table" and t or {}) do
    local program = type(e) == "table" and e.program

    if type(program) == "string" and program:match("^/[^%c]*$")
       and not seen[program] and #out < recent.MOST then
      seen[program] = true
      out[#out + 1] = { program = program }
    end
  end

  return out
end

-- The entries as the settings kit keeps them.
function recent.format(list)
  local out = {}

  for _, e in ipairs(list or {}) do out[#out + 1] = { program = e.program } end

  return out
end

-- The list with `program` at the top: once, so opening it again moves it
-- up rather than adding it twice, and no more than `MOST`.
function recent.add(list, program)
  local out = { { program = program } }

  for _, e in ipairs(list or {}) do
    if #out >= recent.MOST then break end

    if e.program ~= program then out[#out + 1] = e end
  end

  return out
end

--
-- Whether an application's window is worth remembering, from its
-- program's attributes: an application - a window a person opens - and not
-- one filed as `section none`, which is something else's window (the
-- launcher itself, the desktop, a banner, a dialog).
--
function recent.counts(attrs)
  return type(attrs) == "table" and attrs.kind == "application"
         and attrs.section ~= "none"
end

return recent
