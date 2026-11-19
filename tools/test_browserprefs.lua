-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- JetSki's settings, on the Mac (`user/lib/browserprefs.lua`): the
-- defaults with no file, what is written read back - `false` included,
-- which `ok and v or default` loses - its folder made, a choice that is not
-- one, a value of the wrong kind and a key nobody knows refused for the
-- default (the old browser's three among them), an address with a line's
-- end refused, the open tabs kept and read back with which was shown, and
-- where what is typed in the address field goes.

-- The settings kit, as `use` reaches it in a process.
use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local prefs = assert(loadfile("user/lib/browserprefs.lua"))()

local failures, checks = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failures = failures + 1
    print(("not ok %d - %s"):format(checks, what))
  end
end

local files, dirs = {}, { ["/Home"] = true }

local function copy(v)
  if type(v) ~= "table" then return v end

  local out = {}

  for k, x in pairs(v) do out[k] = copy(x) end

  return out
end

fs = {}

function fs.getattr(path)
  if dirs[path] then return { kind = "directory" } end
  if files[path] ~= nil then return { kind = "file" } end
  return nil
end

function fs.read(path) return copy(files[path]) end

function fs.write(path, data)
  files[path] = copy(data)
  return true
end

function fs.send(path, msg)
  -- One folder, in one that is there, as a filesystem makes them.
  if msg.type == "mkdir" then
    if not dirs[path:match("^(.*)/[^/]+$")] then return nil, "no such folder" end

    dirs[path] = true
  end

  return true
end

--------------------------------------------------------------------------

local FILE = "/Home/Settings/JetSki/settings"
local TABS = "/Home/Settings/JetSki/tabs"
local BEOS = "https://en.wikipedia.org/wiki/BeOS"

do
  local s = prefs.read(FILE)

  check(s.home == BEOS and s.opens == "home" and s.javascript == true
        and s.zoom == 100 and s.search == "duckduckgo" and s.history_days == 30,
        "no file is the defaults, JetSki's home page among them: " .. tostring(s.home))
end

do
  local s = prefs.read(FILE)

  s.images, s.javascript, s.zoom, s.search, s.home = false, false, 150, "google",
                                                     "https://www.lua.org/"

  check(prefs.write(s, FILE) and dirs["/Home/Settings"] and dirs["/Home/Settings/JetSki"],
        "written, the folders above it made")

  local back = prefs.read(FILE)

  check(back.images == false and back.javascript == false,
        "false read back as false, not as the default")
  check(back.zoom == 150 and back.search == "google"
        and back.home == "https://www.lua.org/" and back.history_days == 30,
        "the rest as written, and what was not changed its default")
end

do
  -- An old file: the browser before JetSki wrote its favorites bar, its
  -- cost line and its user agent here, and they are read as nothing.
  files[FILE] = { zoom = 133, search = "altavista", images = "yes",
                  history_days = 30.5, home = "", nonsense = 1,
                  opens = "tabs", bar = false, costs = true, agent = "lynx" }

  local s = prefs.read(FILE)

  check(s.zoom == 100 and s.search == "duckduckgo" and s.images == true
        and s.history_days == 30 and s.home == BEOS,
        "a choice that is not one, the wrong kind, a fraction and an empty "
        .. "home page are the defaults")
  check(s.opens == "tabs" and s.nonsense == nil and s.bar == nil
        and s.costs == nil and s.agent == nil,
        "what is valid kept, and what nobody knows - the old browser's three "
        .. "among it - not read")

  s.nonsense = "kept?"
  prefs.write(s, FILE)
  check(files[FILE].nonsense == nil and files[FILE].agent == nil,
        "and not written either")

  s.home = "https://example.com/\r\nX-Injected: yes"
  prefs.write(s, FILE)
  check(prefs.read(FILE).home == BEOS, "an address with a line's end in it refused")
end

do
  check(select("#", prefs.tabs(TABS)) == 2 and #prefs.tabs(TABS) == 0,
        "no tabs kept is an empty list")

  prefs.save_tabs({ "about:settings", "", "https://en.wikipedia.org/wiki/Dam" }, 2, TABS)

  local list, shown = prefs.tabs(TABS)

  check(#list == 2 and list[2] == "https://en.wikipedia.org/wiki/Dam" and shown == 2,
        "the open tabs kept in order, an empty one left out, and which was shown")

  files[TABS] = { tabs = { "a", 7, "b" }, shown = 9 }

  local again, at = prefs.tabs(TABS)

  check(#again == 2 and at == 2, "a hand-edited list read for what it says, "
        .. "the shown one within it")
end

do
  local function to(text, engine)
    local where, searched = prefs.destination(text, engine or "duckduckgo")

    return tostring(where) .. (searched and " (search)" or "")
  end

  check(to("wikipedia.org") == "wikipedia.org"
        and to("en.wikipedia.org/wiki/Uruguay") == "en.wikipedia.org/wiki/Uruguay"
        and to("example.com:8080/x?y=1") == "example.com:8080/x?y=1",
        "a host with a dot and a name of letters is an address, its port and path too")
  check(to("10.0.2.2:8000/") == "10.0.2.2:8000/" and to("localhost:8080") == "localhost:8080",
        "four numbers, and localhost, are addresses")
  check(to("https://duckduckgo.com") == "https://duckduckgo.com"
        and to("about:settings") == "about:settings" and to("/Home/notes.html") == "/Home/notes.html",
        "a scheme JetSki speaks, and a path, are addresses")
  check(to("kosmos rocks") == "https://html.duckduckgo.com/html/?q=kosmos+rocks (search)",
        "words are a search, at DuckDuckGo's page without scripts: " .. to("kosmos rocks"))
  check(to("uruguay") == "https://html.duckduckgo.com/html/?q=uruguay (search)"
        and to("3.14") == "https://html.duckduckgo.com/html/?q=3.14 (search)",
        "one word, and a number with a dot, are searches")
  check(to("ñandú & co?") == "https://html.duckduckgo.com/html/?q=%C3%B1and%C3%BA+%26+co%3F (search)",
        "the words encoded as a form sends them: " .. to("ñandú & co?"))
  check(to("lua manual", "google") == "https://www.google.com/search?q=lua+manual (search)",
        "Google when Settings says Google")
  check(to("   ") == "nil", "nothing typed goes nowhere")
end

if failures == 0 then
  print(("PASS: %d checks on JetSki's settings, on this machine."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on JetSki's settings."):format(failures, checks))
os.exit(1)
