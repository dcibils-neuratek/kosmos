-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Automations as files, and what running one means** (`docs/automations.md`
-- A3): the model `autod` runs, the Automations window (A4) edits and
-- `automate` asks after.
--
--   local automations = use("/Kosmos/Libraries/automations.lua")
--   automations.list()                    -> { "Good morning", ... }
--   automations.read(name)                -> the automation, or nil and why
--   automations.check(a)                  -> true, or nil and what is wrong
--   automations.write(a)                  -> kept in /Home/Automations
--   automations.next_time(when, now, last) -> epoch, or nil for "not by time"
--   automations.fill(text, found)         -> "{page title}" made the title
--   automations.run(a, found, between)    -> the run, as the history keeps it
--   automations.history(name)             -> its runs, newest last
--
-- **An automation is a table in text, and nothing in it is executable**:
--
--   { name = "Good morning", on = true,
--     when = { kind = "at", time = "8:00", days = { "mon", "tue", "wed" } },
--     steps = {
--       { verb = "open", app = "Mail" },
--       { verb = "press", app = "Mail", control = "Get Mail" },
--       { verb = "check", url = "https://kosmos-os.org/builds", has = "-stable" },
--       { verb = "notify", title = "Good morning - {page title}" },
--     } }
--
-- `when.kind` is one of `at` (a `time` and its `days`, every day without
-- them), `every` (`minutes`), `start` (when Kosmos starts), `folder` (a
-- file appearing in `path`), `drive` (one arriving, by `name`, any without)
-- and `hand` (only when asked). Mail arriving waits for `maild` to tell.
--
-- **Each verb is `automate.lua`'s** - this decides what a step means and
-- in what order, never how a button is pressed. What a step finds is named
-- for the next: `check` gives `{page title}` and `{page url}`, `read`
-- gives `{value}` or the name in its `as`, `copy` and `move` `{file}`, a
-- folder trigger `{file}` and a drive trigger `{drive}`. `stop_unless`
-- stops the run, as a success, when its `text` does not have `has` - and
-- so does a `check` whose page does not say what was asked.

local automations = {}

local auto = use("/Kosmos/Libraries/automate.lua")
local files = use("/Kosmos/Libraries/files.lua")

automations.DIR = "/Home/Automations"
automations.HISTORY = automations.DIR .. "/History"
automations.KEPT_RUNS = 50

local DAYS = { sun = 0, mon = 1, tue = 2, wed = 3, thu = 4, fri = 5, sat = 6 }

local KINDS = { at = true, every = true, start = true, folder = true, drive = true, hand = true }

-- What each verb needs, by name, and what it is said as.
local VERBS = {
  open = { "app" }, press = { "app", "control" }, menu = { "app", "menu", "item" },
  type = { "app", "control", "text" }, choose = { "app", "control" },
  set = { "app", "control", "value" }, read = { "app", "control" },
  mail = { "to" }, check = { "url" }, copy = { "from", "to" }, move = { "from", "to" },
  notify = { "title" }, wait = { "seconds" }, wait_for = { "app" }, run = { "program" },
  stop_unless = { "text" },
}

automations.VERBS = VERBS

--------------------------------------------------------------------------
-- The files.
--------------------------------------------------------------------------

local function path_of(name)
  return automations.DIR .. "/" .. tostring(name) .. ".automation"
end

-- Every automation's name, sorted.
function automations.list()
  local out = {}

  for _, n in ipairs(fs.list(automations.DIR) or {}) do
    local name = tostring(type(n) == "table" and n.name or n):match("^(.+)%.automation$")

    if name then out[#out + 1] = name end
  end

  table.sort(out)
  return out
end

-- What is wrong with an automation, in words, or true.
function automations.check(a)
  if type(a) ~= "table" then return nil, "it is not an automation" end
  if type(a.name) ~= "string" or a.name == "" or a.name:find("[/%c]") then
    return nil, "it has no name it can be kept by"
  end

  local w = a.when

  if type(w) ~= "table" or not KINDS[w.kind] then
    return nil, a.name .. ": what starts it is not one of at, every, start, folder, drive, hand"
  end

  if w.kind == "at" then
    local h, m = tostring(w.time or ""):match("^(%d%d?):(%d%d)$")

    if not h or tonumber(h) > 23 or tonumber(m) > 59 then
      return nil, a.name .. ": the time is not hours:minutes"
    end

    for _, d in ipairs(w.days or {}) do
      if DAYS[tostring(d)] == nil then return nil, a.name .. ": " .. tostring(d) .. " is not a day" end
    end
  elseif w.kind == "every" and not (tonumber(w.minutes) and tonumber(w.minutes) >= 1) then
    return nil, a.name .. ": every so many minutes needs minutes, one or more"
  elseif w.kind == "folder" and type(w.path) ~= "string" then
    return nil, a.name .. ": a folder trigger needs the folder"
  end

  if type(a.steps) ~= "table" or #a.steps == 0 then return nil, a.name .. ": it has no steps" end

  for i, step in ipairs(a.steps) do
    local needs = type(step) == "table" and VERBS[step.verb]

    if not needs then
      return nil, ("%s: step %d does nothing Automations knows"):format(a.name, i)
    end

    for _, field in ipairs(needs) do
      if step[field] == nil then
        return nil, ("%s: step %d, %s, needs its %s"):format(a.name, i, step.verb, field)
      end
    end
  end

  return true
end

-- One automation, checked: the table, or nil and why.
function automations.read(name)
  local a = fs.read(path_of(name))

  if type(a) ~= "table" then return nil, tostring(name) .. " is not an automation here" end

  local ok, why = automations.check(a)

  if not ok then return nil, why end
  return a
end

function automations.write(a)
  local ok, why = automations.check(a)

  if not ok then return nil, why end

  files.make_folder(automations.DIR)
  return fs.write(path_of(a.name), a)
end

--------------------------------------------------------------------------
-- When.
--------------------------------------------------------------------------

--
-- The next moment `when` fires after `now` - a clock table, `clock.now()`'s
-- - as an epoch; nil when it does not fire by the time. `last` is when it
-- last ran, which `every` counts from (from `now` when it never has).
--
function automations.next_time(when, now, last)
  if type(when) ~= "table" or not now then return nil end

  if when.kind == "every" then
    local step = math.floor(tonumber(when.minutes) or 0) * 60

    if step <= 0 then return nil end

    return (last or now.epoch) + step
  end

  if when.kind ~= "at" then return nil end

  local h, m = tostring(when.time or ""):match("^(%d%d?):(%d%d)$")

  if not h then return nil end

  local allowed = nil

  if when.days and #when.days > 0 then
    allowed = {}
    for _, d in ipairs(when.days) do allowed[DAYS[tostring(d)] or -1] = true end
  end

  -- Midnight here, then the time asked for, today and on the week's days.
  local midnight = now.epoch - now.hour * 3600 - now.min * 60 - now.sec
  local at = midnight + tonumber(h) * 3600 + tonumber(m) * 60

  for d = 0, 7 do
    local t = at + d * 86400
    local day = (now.weekday + d) % 7

    if t > now.epoch and (not allowed or allowed[day]) then return t end
  end

  return nil
end

--------------------------------------------------------------------------
-- What a step found, in the next one's words.
--------------------------------------------------------------------------

-- `text` with each `{name}` made what `found` holds under it; a name it
-- does not hold is left as it was written, so the history shows which.
function automations.fill(text, found)
  if type(text) ~= "string" then return text end

  return (text:gsub("{([^{}]+)}", function(name)
    local v = found and found[name]

    if v == nil then return nil end
    return tostring(v)
  end))
end

local function filled(step, found)
  local out = {}

  for k, v in pairs(step) do out[k] = automations.fill(v, found) end
  return out
end

-- A step as a sentence, as the history and the window say it.
function automations.said(step)
  local v = step.verb

  if v == "open" then return "Open " .. tostring(step.app) end
  if v == "press" then return ("Press %s in %s"):format(step.control, step.app) end
  if v == "menu" then return ("Choose %s > %s in %s"):format(step.menu, step.item, step.app) end
  if v == "type" then return ("Type into %s in %s"):format(step.control, step.app) end
  if v == "choose" then
    return step.item and ("Choose %s in %s's %s"):format(step.item, step.app, step.control)
           or ("Choose %s in %s"):format(step.control, step.app)
  end
  if v == "set" then return ("Set %s in %s to %s"):format(step.control, step.app, tostring(step.value)) end
  if v == "read" then return ("Read %s in %s"):format(step.control, step.app) end
  if v == "mail" then return "Send a mail to " .. tostring(step.to) end
  if v == "check" then return "Check " .. tostring(step.url) end
  if v == "copy" then return ("Copy %s to %s"):format(step.from, step.to) end
  if v == "move" then return ("Move %s to %s"):format(step.from, step.to) end
  if v == "notify" then return "Notify: " .. tostring(step.title) end
  if v == "wait" then return ("Wait %s s"):format(tostring(step.seconds)) end
  if v == "wait_for" then return "Wait for " .. tostring(step.app) end
  if v == "run" then return "Run " .. tostring(step.program) end
  if v == "stop_unless" then return ("Stop unless it has %s"):format(tostring(step.has)) end
  return tostring(v)
end

--------------------------------------------------------------------------
-- Running.
--------------------------------------------------------------------------

local function app_of(name)
  return auto.app(name) or auto.wait_for(name, auto.PATIENCE)
end

--
-- One step: true and what it found, or nil and why - and "stop" in third
-- place when the run should end here as a success.
--
local function do_step(s, found)
  local v = s.verb

  if v == "open" then
    local a, why = auto.open(s.app, { file = s.file })
    return a and true, why
  end

  if v == "press" or v == "menu" or v == "type" or v == "choose" or v == "set" or v == "read" then
    local a, why = app_of(s.app)

    if not a then return nil, why end

    if v == "press" then return a:press(s.control) end
    if v == "menu" then return a:menu(s.menu, s.item) end
    if v == "type" then return a:type(s.control, s.text) end
    if v == "choose" then return a:choose(s.control, s.item) end
    if v == "set" then return a:set(s.control, s.value) end

    local value, no = a:read(s.control)

    if value == nil then return nil, no end
    found[s.as or "value"] = value
    return true
  end

  if v == "mail" then
    local id, why = auto.mail{ to = s.to, cc = s.cc, subject = s.subject, text = s.text,
                               files = s.files, account = s.account }
    if not id then return nil, why end
    found["mail"] = id
    return true
  end

  if v == "check" then
    local page, why, unmet = auto.check(s.url, { has = s.has, lacks = s.lacks, changed = s.changed })

    if not page then return nil, why, unmet and "stop" end

    found["page title"], found["page url"] = page.title, page.url
    return true
  end

  if v == "copy" or v == "move" then
    local where, why = auto[v](s.from, s.to)

    if not where then return nil, why end
    found["file"] = where
    return true
  end

  if v == "notify" then
    local n, why = auto.notify(s.title, s.body)
    return n and true, why
  end

  if v == "wait" then return auto.wait(tonumber(s.seconds) or 1) end

  if v == "wait_for" then
    local a, why = auto.wait_for(s.app, tonumber(s.seconds))
    return a and true, why
  end

  if v == "run" then return auto.run(s.program, s.args) end

  if v == "stop_unless" then
    if tostring(s.text):find(tostring(s.has or ""), 1, true) then return true end
    return nil, ("%s does not have %s"):format(tostring(s.text), tostring(s.has)), "stop"
  end

  return nil, "a step Automations does not know: " .. tostring(v)
end

--
-- Every step of `a` in turn, each with what the ones before found - `found`
-- starting as what the trigger gave. Stops at the first that fails, or at
-- a stop. `between`, when given, is called after each step: `autod`
-- answers whoever asked meanwhile. Returns the run as the history keeps it:
-- `{ at, trigger, ok, stopped, why, ms, steps = { { said, ok, why, ms } } }`.
--
function automations.run(a, found, between, trigger)
  local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local now = use("/Kosmos/Libraries/clock.lua").now()
  local run = { at = now and now.epoch or 0, trigger = trigger or "by hand", ok = true, steps = {} }
  local t0 = sys.ticks()

  found = found or {}

  for i, step in ipairs(a.steps or {}) do
    local s = filled(step, found)
    local started = sys.ticks()
    local ok, result, why, stop = pcall(do_step, s, found)

    -- A step that raised is a step that failed, with what it said.
    if not ok then result, why, stop = nil, tostring(result), nil end

    local record = { said = automations.said(s), ok = result ~= nil and result ~= false,
                     ms = (sys.ticks() - started) * 1000 // counter_hz }

    if not record.ok then record.why = tostring(why) end

    run.steps[i] = record

    if between then pcall(between, i, record) end

    if not record.ok then
      if stop == "stop" then
        run.stopped = true
        run.why = "stopped at step " .. i .. ": " .. record.why
      else
        run.ok = false
        run.why = ("step %d, %s: %s"):format(i, record.said, record.why)
      end
      break
    end
  end

  run.ms = (sys.ticks() - t0) * 1000 // counter_hz
  return run
end

--------------------------------------------------------------------------
-- History.
--------------------------------------------------------------------------

function automations.history(name)
  local h = fs.read(automations.HISTORY .. "/" .. tostring(name))

  return type(h) == "table" and h.runs or {}
end

-- A run kept, the oldest let go past `KEPT_RUNS`.
function automations.keep(name, run)
  local runs = automations.history(name)

  runs[#runs + 1] = run

  while #runs > automations.KEPT_RUNS do table.remove(runs, 1) end

  files.make_folder(automations.DIR)
  files.make_folder(automations.HISTORY)
  return fs.write(automations.HISTORY .. "/" .. tostring(name), { runs = runs })
end

return automations
