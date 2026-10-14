-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **The verbs an automation is made of** (`docs/automations.md` A2): open
-- an application and press, choose, type into and read its controls by
-- name; send a mail; check a web page; copy, move, notify, wait, run.
--
--   local auto = use("/Kosmos/Libraries/automate.lua")
--   local calc = auto.open("Calculator")          -- opened, or found open
--   calc:press("7"); calc:press("×"); calc:press("6"); calc:press("=")
--   calc:read("Result")                           -> "42"
--   local notes = auto.app("Text Editor", { file = "notes.md" })
--   notes:menu("more", "Find and replace")
--   auto.mail{ to = "ana@example.com", subject = "Hi", text = "..." }
--   local page = auto.check("https://example.com", { has = "Example" })
--   auto.copy("/Home/Documents/a.txt", "/Drives/STICK")
--   auto.notify("Done", "Copied a.txt")
--   auto.wait(2)                                  -- seconds
--   auto.run("hello", "world")                    -- a program, to its end
--
-- **Every verb returns what it found, or nil and why** - in words, as an
-- automation's history will show them - and never raises: a step that
-- fails is a step, not a stack trace.
--
-- **A control is named by what a person sees** (`automations.md`, *How a
-- control is named*): `button/Send` exactly, or the label alone - "Send" -
-- when only one control has it. A label that names two is refused with both
-- addresses, so the caller is told how to say which rather than one being
-- pressed at random. What an application publishes comes from the UI kit
-- (`window:controls`, A1); nothing here knows any application.
--
-- **Who uses it**: `autod` and the Automations window when they exist, a
-- script handed `auto`, the `automate` program at the prompt, a test - one
-- door, as `mailout.lua` is for the Outbox. A program that uses it has its
-- own powers beside it; a script handed only `auto` has these and nothing
-- else (A6).

local auto = {}

local files = use("/Kosmos/Libraries/files.lua")

-- How long a control or a window is waited for when it is not there yet.
auto.PATIENCE = 5

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local tick_hz = (sys.info() or {}).tick_hz or 250

local function seconds_since(t0)
  return (sys.ticks() - t0) / counter_hz
end

-- Asleep for `s` seconds, in the scheduler's ticks.
local function nap(s)
  sys.sleep(math.max(1, math.floor(s * tick_hz + 0.5)))
end

local function ask(path, req)
  local ok, reply, why = pcall(fs.send, path, req)

  if not ok then return nil, tostring(reply) end
  if not reply then return nil, why end
  return reply
end

--------------------------------------------------------------------------
-- Applications, by their names in /Running.
--------------------------------------------------------------------------

local App = {}
App.__index = App

-- Every name in /Running now.
local function running()
  local out = {}

  for _, n in ipairs(fs.list("/Running") or {}) do
    out[#out + 1] = tostring(type(n) == "table" and n.name or n)
  end

  return out
end

-- Whether the name a window registered under is the application asked for:
-- the name itself, a second window of it (`Mail2`), or a title that ends in
-- it (`notes.md - Text Editor`). Letters' case does not count.
function auto.names(registered, wanted)
  local r, w = registered:lower(), wanted:lower()

  return r == w or r:match("^" .. w:gsub("%p", "%%%0") .. "%d+$") ~= nil
         or r:sub(-#w - 3) == " - " .. w
end

local function app_of(name)
  return setmetatable({ name = name, path = "/Running/" .. name }, App)
end

-- Every window of the application now, each an App.
function auto.windows(wanted)
  local out = {}

  for _, n in ipairs(running()) do
    if auto.names(n, tostring(wanted)) then out[#out + 1] = app_of(n) end
  end

  return out
end

-- The first window of the application, or nil and why. `opts.file` picks
-- the one whose title starts with that file's name.
function auto.app(wanted, opts)
  opts = opts or {}

  for _, a in ipairs(auto.windows(wanted)) do
    if not opts.file or a.name:sub(1, #opts.file) == opts.file then return a end
  end

  return nil, ("%s is not open"):format(tostring(wanted))
end

-- Every application's name, as its header says it (`kosmos: name
-- Calculator`), to its program: the shipped ones and those installed in
-- /Home/Apps. Read once a process - an automation opens a few things a
-- minute, and the list does not change under it.
local by_name

local function application_names()
  if by_name then return by_name end

  local filetypes = use("/Kosmos/Libraries/filetypes.lua")

  by_name = {}

  for _, f in ipairs(fs.list("/Kosmos/Apps") or {}) do
    local file = tostring(type(f) == "table" and f.name or f)
    local path = "/Kosmos/Apps/" .. file
    local source = file:match("%.lua$") and fs.read(path)
    local name = type(source) == "string" and filetypes.declared(source, "name")

    if name then by_name[name:lower()] = path end
  end

  for _, app in ipairs(filetypes.installed() or {}) do
    local name = filetypes.declared(app.source, "name") or app.folder

    if name then by_name[tostring(name):lower()] = app.program end
  end

  return by_name
end

-- The program a name starts: a file by its path, an application by the
-- name it declares ("Calculator" is `calc.lua`), else a program by its own
-- name.
local function program_for(wanted)
  -- A program's own file, by its path.
  if wanted:find("/", 1, true) then
    return fs.getattr(wanted) and wanted or nil
  end

  local path = application_names()[wanted:lower()]

  if path then return path end

  for _, n in ipairs({ wanted, wanted:lower() }) do
    path = fs.program(n)

    if path and fs.getattr(path) then return path end
  end
end

--
-- The application open: found when it already is, started when not and
-- waited for until its window has a name in /Running. `opts.file` opens that
-- file with it; `opts.program` names the program when its name is not the
-- application's (`calc` for Calculator).
--
function auto.open(wanted, opts)
  opts = opts or {}
  wanted = tostring(wanted)

  local open = auto.app(wanted, { file = opts.file and opts.file:match("([^/]+)$") })

  if open then return open end

  local program = opts.program and (fs.program(opts.program) or opts.program) or program_for(wanted)

  if not program then return nil, ("there is no application called %s"):format(wanted) end

  local before = {}

  for _, n in ipairs(running()) do before[n] = true end

  local r, why = ask("/Running/wm", { type = "launch", program = program,
                                         args = opts.file and files.quote(opts.file) })

  if not r or r.ok == false then
    return nil, ("%s did not start: %s"):format(wanted, tostring(why or (r and r.error)))
  end

  local t0 = sys.ticks()
  local fresh

  while seconds_since(t0) < (opts.patience or auto.PATIENCE * 4) do
    for _, n in ipairs(running()) do
      if not before[n] then
        if auto.names(n, wanted) then return app_of(n) end
        fresh = fresh or n
      end
    end

    nap(0.1)
  end

  -- A window under another name than the one asked by - Calculator started
  -- as `calc` - is still the one this started.
  if fresh then return app_of(fresh) end

  return nil, ("%s started and opened no window"):format(wanted)
end

-- Until the application is open, or nil and why after `seconds`.
function auto.wait_for(wanted, seconds)
  local t0 = sys.ticks()

  repeat
    local a = auto.app(wanted)

    if a then return a end

    nap(0.1)
  until seconds_since(t0) >= (seconds or auto.PATIENCE)

  return nil, ("%s did not open within %g s"):format(tostring(wanted), seconds or auto.PATIENCE)
end

--------------------------------------------------------------------------
-- Its controls.
--------------------------------------------------------------------------

-- Every control it publishes now: { address, kind, read, write, press,
-- disabled } each, or nil and why.
function App:controls()
  local out, from = {}, 1

  -- A page at a time: four letters, `rwpd` or `-` each, then the address.
  while from do
    local r, why = ask(self.path .. "/controls", { type = "addresses", from = from })

    if not r then return nil, ("%s: %s"):format(self.name, tostring(why)) end

    for _, e in ipairs(r.entries or {}) do
      local flags, address = tostring(e):sub(1, 4), tostring(e):sub(5)

      out[#out + 1] = { address = address, kind = address:match("^[^/]+"),
                        read = flags:sub(1, 1) == "r", write = flags:sub(2, 2) == "w",
                        press = flags:sub(3, 3) == "p", disabled = flags:sub(4, 4) == "d" }
    end

    from = r.next
  end

  return out
end

--
-- The address a label means, among the controls of `kinds` when given: an
-- address exactly, or the controls whose label - the address past its kind
-- - is the words given. One, or nil and why: none, or several and which.
--
function auto.resolve(list, label, kinds)
  label = tostring(label)

  local found = {}

  for _, c in ipairs(list) do
    if c.address == label then return c end
  end

  for _, c in ipairs(list) do
    local own = c.address:sub(#c.kind + 2):gsub("#%d+$", "")

    if (not kinds or kinds[c.kind]) and (own == label or c.address:match("/([^/]+)$") == label) then
      found[#found + 1] = c
    end
  end

  if #found == 1 then return found[1] end

  if #found > 1 then
    local said = {}

    for _, c in ipairs(found) do said[#said + 1] = c.address end

    return nil, ("%s names %d controls: %s"):format(label, #found, table.concat(said, ", "))
  end

  return nil, ("no control called %s"):format(label)
end

-- The control `label` names, waited for while the window is still making
-- it - a dialog not shown yet - up to `auto.PATIENCE` seconds.
function App:find(label, kinds)
  local t0 = sys.ticks()
  local last

  repeat
    local list, why = self:controls()

    if not list then return nil, why end

    local c, no = auto.resolve(list, label, kinds)

    if c then return c end

    -- Two of a name is an answer; waiting will not make it one.
    if no and no:find(" names %d+ controls") then return nil, ("%s: %s"):format(self.name, no) end

    last = no
    nap(0.1)
  until seconds_since(t0) >= auto.PATIENCE

  return nil, ("%s: %s"):format(self.name, last)
end

function App:wait(label, seconds)
  local keep = auto.PATIENCE

  auto.PATIENCE = seconds or keep

  local c, why = self:find(label)

  auto.PATIENCE = keep
  return c and c.address, why
end

local function act(self, c, req)
  local r, why = ask(self.path .. "/controls/" .. c.address, req)

  if not r then return nil, ("%s: %s - %s"):format(self.name, c.address, tostring(why)) end

  return r
end

-- Pressed, exactly as a click would: a button, a menu item, a key.
function App:press(label)
  local c, why = self:find(label)

  if not c then return nil, why end

  local r, no = act(self, c, { type = "press" })

  return r and c.address, no
end

-- A menu's item: `menu/<menu>/<item>`.
function App:menu(menu, item)
  return self:press("menu/" .. tostring(menu) .. "/" .. tostring(item))
end

-- What it shows.
function App:read(label)
  local c, why = self:find(label)

  if not c then return nil, why end

  local r, no = act(self, c, { type = "read" })

  return r and r.value, no
end

-- Typed into: a field's text replaced by `text`.
function App:type(label, text)
  local c, why = self:find(label, { field = true })

  if not c then return nil, why end

  local r, no = act(self, c, { type = "write", value = tostring(text) })

  return r and c.address, no
end

-- Set: a switch or checkbox on or off, a slider to a number.
function App:set(label, value)
  local c, why = self:find(label, { switch = true, checkbox = true, slider = true })

  if not c then return nil, why end

  if type(value) == "boolean" then value = value and "on" or "off" end

  local r, no = act(self, c, { type = "write", value = tostring(value) })

  return r and c.address, no
end

--
-- Chosen: a tab or a segment by its words - `choose("Rendered")` - or a
-- dropdown's item - `choose("Theme", "Dark")`.
--
function App:choose(label, value)
  if value == nil then
    local c, why = self:find(label, { tab = true, segment = true })

    if not c then return nil, why end

    local r, no = act(self, c, { type = "press" })

    return r and c.address, no
  end

  local c, why = self:find(label, { dropdown = true })

  if not c then return nil, why end

  local r, no = act(self, c, { type = "write", value = tostring(value) })

  return r and c.address, no
end

--------------------------------------------------------------------------
-- The system's own verbs.
--------------------------------------------------------------------------

-- A mail, through the Outbox `maild` sends from (`mailout.lua`): its id,
-- or nil and why.
function auto.mail(m)
  local ok, mailout = pcall(use, "/Kosmos/Libraries/mailout.lua")

  if not ok then return nil, "mail is not in this system" end

  return mailout.send(m)
end

-- What a page's text says, tags left out, for `has` and `lacks`.
local function words_of(body)
  return (body:gsub("<script.-</script>", " "):gsub("<style.-</style>", " "):gsub("<[^>]*>", " ")
              :gsub("&nbsp;", " "):gsub("&amp;", "&"):gsub("%s+", " "))
end

local SEEN = "/Temporary/automate"

--
-- A web page fetched: `{ url, status, title, text }`, or nil and why - and
-- nil too when it does not pass what `opts` asks: `has` words it must have,
-- `lacks` words it must not, `changed = true` that it differs from when it
-- was last checked here. Then a third answer, "unmet": the page was there
-- and did not say what was asked, which an automation takes as "stop
-- here", not as a failure.
--
function auto.check(url, opts)
  opts = opts or {}

  local ok, http = pcall(use, "/Kosmos/Libraries/http.lua")

  if not ok then return nil, "the web is not in this system" end

  local reply, why = http.get(tostring(url))

  if not reply then return nil, ("%s could not be fetched: %s"):format(tostring(url), tostring(why)) end

  local status, _, body = http.parse(reply)

  if status >= 400 then return nil, ("%s answered %d"):format(tostring(url), status) end

  local title = body:match("<[Tt][Ii][Tt][Ll][Ee][^>]*>(.-)</[Tt][Ii][Tt][Ll][Ee]>") or ""
  local text = words_of(body)
  local page = { url = tostring(url), status = status,
                 title = (title:gsub("%s+", " "):gsub("^ ", ""):gsub(" $", "")), text = text }

  if opts.has and not text:find(opts.has, 1, true) then
    return nil, ("%s does not say \u{201c}%s\u{201d}"):format(page.url, opts.has), "unmet"
  end

  if opts.lacks and text:find(opts.lacks, 1, true) then
    return nil, ("%s says \u{201c}%s\u{201d}"):format(page.url, opts.lacks), "unmet"
  end

  if opts.changed then
    local key = SEEN .. "/" .. page.url:gsub("[^%w]", "_"):sub(1, 120)
    local before = fs.read(key)

    files.make_folder(SEEN)
    fs.write(key, text)

    if before == text then return nil, page.url .. " has not changed", "unmet" end
  end

  return page
end

-- Where a file goes: into `to` when it is a folder, else to `to` itself.
local function destination(from, to)
  local attrs = fs.getattr(to)

  if attrs and attrs.kind == "directory" then
    return to:gsub("/$", "") .. "/" .. (tostring(from):match("([^/]+)/?$") or from)
  end

  return to
end

-- Copied, or moved: where it went, or nil and why.
function auto.copy(from, to)
  local where = destination(from, to)
  local ok, why = files.copy(from, where)

  if not ok then return nil, ("%s was not copied: %s"):format(tostring(from), tostring(why)) end

  return where
end

function auto.move(from, to)
  local where = destination(from, to)
  local ok, why = files.move(from, where)

  if not ok then return nil, ("%s was not moved: %s"):format(tostring(from), tostring(why)) end

  return where
end

-- A banner: its number, or nil and why.
function auto.notify(title, body)
  local ok, notify = pcall(use, "/Kosmos/Libraries/notify.lua")

  if not ok then return nil, "notifications are not in this system" end

  if type(title) == "table" then return notify.post(title) end

  return notify.post{ title = tostring(title), body = body and tostring(body) or nil }
end

-- A while, in seconds.
function auto.wait(seconds)
  nap(tonumber(seconds) or 1)
  return true
end

--
-- A program run to its end, as the prompt runs it: a Lua file by its path,
-- or one of Kosmos's programs by its name. True, or nil and why.
--
function auto.run(program, argument)
  program = tostring(program)

  local path = program:find("/", 1, true) and program or fs.program(program)

  if not path or not fs.getattr(path) then return nil, ("there is no program %s"):format(program) end

  local ok, why = run(path, argument and tostring(argument) or "", false)

  if not ok then return nil, ("%s: %s"):format(program, tostring(why)) end

  return true
end

return auto
