-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The applications started last, newest first: what the launcher's
-- Recently used row shows (`docs/launcher.html`, agreed 7 October).
--
-- Diego: "can we add a new extra menu called "Recently used" and we will be
-- adding all the recently opened apps as a shortcut?". Nothing kept such a
-- list - the window manager knows what is running, not what ran - so the
-- window manager writes it here as it starts an application, whoever asked
-- (the launcher, the dock, the Deskbar's menu, Tracker, `open`), and the
-- launcher reads it. **One list, one shape, and this the one door to it**:
-- both sides use these functions, and `tools/test_launchgrid.lua` holds
-- them on the Mac.
--
-- It is kept by the settings kit as `recent` (`prefs.read`, `prefs.write`):
-- a list, newest first, of `{ program = <path>, args = <words> }`, `args`
-- left out when it was started with none - text a person can read.
--

local recent = {}

recent.NAME = "recent"     -- its settings name: `prefs.read(recent.NAME)`
recent.MOST = 15        -- three rows of the launcher's grid

-- What the settings kit read as entries, `{ program, args }`, in its order.
-- One that is not a path is passed over rather than trusted.
function recent.parse(t)
  local out = {}

  for _, e in ipairs(type(t) == "table" and t or {}) do
    local program = type(e) == "table" and e.program

    if type(program) == "string" and program:match("^/[^%c]*$") and #out < recent.MOST then
      local args = type(e.args) == "string" and e.args or ""

      out[#out + 1] = { program = program, args = args }
    end
  end

  return out
end

-- The entries as the settings kit keeps them: `args` only when there are some.
function recent.format(list)
  local out = {}

  for _, e in ipairs(list or {}) do
    local args = tostring(e.args or "")

    out[#out + 1] = { program = e.program, args = (args ~= "") and args or nil }
  end

  return out
end

--
-- The list with `program` (and its `args`) at the top: once, so starting
-- it again moves it up rather than adding it twice, and no more than
-- `MOST`. Arguments with a control character in them are left off rather
-- than kept: a list a person reads should not hide a line break.
--
function recent.add(list, program, args)
  args = tostring(args or "")

  if args:find("%c") then args = "" end

  local out = { { program = program, args = args } }

  for _, e in ipairs(list or {}) do
    if #out >= recent.MOST then break end

    if not (e.program == program and (e.args or "") == args) then
      out[#out + 1] = e
    end
  end

  return out
end

--
-- Whether a program's start is worth remembering, from its attributes:
-- an application - a window a person opens - and not one filed as `section
-- none`, which is something else's window (the launcher itself, the
-- desktop, a banner, a dialog).
--
function recent.counts(attrs)
  return type(attrs) == "table" and attrs.kind == "application"
         and attrs.section ~= "none"
end

return recent
