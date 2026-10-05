-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The browser's settings, on the Mac (`user/lib/browserprefs.lua`,
-- `roadmap.md` 6zz d5): the defaults with no file, what is written read
-- back - `false` included, which `ok and v or default` loses - its folder
-- made, a choice that is not one, a value of the wrong kind and a key
-- nobody knows refused for the default, a choice's name, and the open tabs
-- kept and read back with which was shown.

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
  if msg.type == "mkdir" then dirs[path] = true end
  return true
end

--------------------------------------------------------------------------

do
  local s = prefs.read()

  check(s.home == "about:start" and s.opens == "home" and s.bar == true
        and s.zoom == 100 and s.search == "duckduckgo" and s.history_days == 30,
        "no file is the defaults")
end

do
  local s = prefs.read()

  s.bar, s.images, s.zoom, s.search, s.home = false, false, 150, "google",
                                              "https://www.lua.org/"

  check(prefs.write(s) and dirs["/Home/Preferences"] and dirs[prefs.DIR],
        "written, the folders above it made")

  local back = prefs.read()

  check(back.bar == false and back.images == false,
        "false read back as false, not as the default")
  check(back.zoom == 150 and back.search == "google"
        and back.home == "https://www.lua.org/" and back.costs == true,
        "the rest as written, and what was not changed its default")
end

do
  files[prefs.FILE] = { zoom = 133, search = "altavista", bar = "yes",
                        history_days = 30.5, home = "", nonsense = 1,
                        opens = "tabs" }

  local s = prefs.read()

  check(s.zoom == 100 and s.search == "duckduckgo" and s.bar == true
        and s.history_days == 30 and s.home == "about:start",
        "a choice that is not one, the wrong kind, a fraction and an empty "
        .. "home page are the defaults")
  check(s.opens == "tabs" and s.nonsense == nil,
        "what is valid kept, and what nobody knows not read")

  s.nonsense = "kept?"
  prefs.write(s)
  check(files[prefs.FILE].nonsense == nil, "and not written either")
end

do
  local s = prefs.read()

  check(s.agent == "lynx" and prefs.agent(s, "0.10.200")
        == "Lynx/2.9.0 (Kosmos 0.10.200; NetSurf/3.11)",
        "it says it is a plain browser, Lynx, and Kosmos, unless told otherwise")

  s.agent = "netsurf"
  check(prefs.agent(s, "1") == "NetSurf/3.11 (Kosmos 1)", "its engine, as it is")

  s.agent, s.agent_words = "own", "Mozilla/5.0 (Kosmos; like nothing else)"
  prefs.write(s)
  check(prefs.agent(prefs.read(), "1") == "Mozilla/5.0 (Kosmos; like nothing else)",
        "words of its own, written and read back")

  s.agent_words = "Evil/1.0\r\nX-Injected: yes"
  prefs.write(s)

  local back = prefs.read()

  check(back.agent_words == "" and prefs.agent(back, "1"):match("^Lynx/"),
        "words with a line's end in them refused, so no header can be made of them")
  s.agent, s.agent_words = "lynx", ""
  prefs.write(s)
end

do
  check(select("#", prefs.tabs()) == 2 and #prefs.tabs() == 0,
        "no tabs kept is an empty list")

  prefs.save_tabs({ "about:start", "", "https://en.wikipedia.org/wiki/Dam" }, 2)

  local list, shown = prefs.tabs()

  check(#list == 2 and list[2] == "https://en.wikipedia.org/wiki/Dam" and shown == 2,
        "the open tabs kept in order, an empty one left out, and which was shown")

  files[prefs.TABS] = { tabs = { "a", 7, "b" }, shown = 9 }

  local again, at = prefs.tabs()

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
        and to("about:start") == "about:start" and to("/Home/notes.html") == "/Home/notes.html",
        "a scheme this browser speaks, and a path, are addresses")
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
  print(("PASS: %d checks on the browser's settings, on this machine."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the browser's settings."):format(failures, checks))
os.exit(1)
