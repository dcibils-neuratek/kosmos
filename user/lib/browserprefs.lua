-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The browser's settings (`roadmap.md` 6zz d5, `docs/browser.html`).
--
-- **One table, `/Home/Preferences/browser/settings`**, written with
-- `fs.write` and read back with `fs.read` as every settings file in Kosmos
-- is - beside the history, in the folder that is the browser's own (d4).
-- And the tabs that were open, `browser/tabs`, for "When the browser
-- opens: The tabs it had".
--
-- **What is read is what the browser can use**: each key it knows, of the
-- kind it should be and - for a choice - one of the choices; anything else
-- is the default. A file edited by hand, or written by a browser older or
-- newer than this one, is read for what it still says rather than trusted
-- whole, and what is written is only what is known.
--
-- Pure: `fs` is the global every process has, and the host test hands it
-- one kept in memory (`tools/test_browserprefs.lua`).

local prefs = {}

-- The browser's own folder, where the settings kit keeps it (`prefs.lua`).
prefs.DIR = use("/Kosmos/Libraries/prefs.lua").path("browser")
prefs.FILE = prefs.DIR .. "/settings"
prefs.TABS = prefs.DIR .. "/tabs"

-- The page Settings is, in a tab of its own; and the one its Show opens,
-- the authorities this machine trusts.
prefs.PAGE = "about:settings"
prefs.ROOTS = "about:authorities"

prefs.DEFAULTS = {
  home = "about:start",         -- Home page
  opens = "home",               -- When the browser opens
  bar = true,                   -- Show the favorites bar
  zoom = 100,                   -- Zoom, per cent: all of the page
  images = true,                -- Load images
  costs = true,                 -- What each page cost, in the status line
  search = "duckduckgo",        -- Search with (d6)
  history_days = 30,            -- Keep history for
  agent = "lynx",               -- What it tells sites it is
  agent_words = "",             -- ... in words of its own
}

-- A choice's value and what it is called, in the order offered.
prefs.CHOICES = {
  opens = { { "home", "The home page" }, { "tabs", "The tabs it had" } },
  --
  -- **Zoom's four**, in Diego's words, 1 October: "just put 4 options",
  -- "100% normal", "125% larger", "150% even larger", "200% largest".
  --
  zoom = { { 100, "100%, normal" }, { 125, "125%, larger" },
           { 150, "150%, even larger" }, { 200, "200%, largest" } },
  search = { { "duckduckgo", "DuckDuckGo" }, { "google", "Google" } },
  history_days = { { 7, "A week" }, { 30, "30 days" }, { 90, "90 days" },
                   { 365, "A year" } },
  agent = { { "lynx", "A plain browser, as Lynx" }, { "netsurf", "Its engine, NetSurf" },
            { "console", "A game console, as a 3DS" }, { "own", "Words of its own" } },
}

--
-- **What the browser tells a site it is** - Diego, 1 October: "we have a
-- very basic browser so we need to announce that to the server", "dont
-- send complex sites, send simple ones", and to keep changing it as the
-- browser grows. Measured before any was offered, against Google,
-- DuckDuckGo, the BBC, Wikipedia, GitHub and YouTube from the Mac: most
-- send everyone the same page. **Lynx is the one that is sent simpler
-- ones** - DuckDuckGo's front page in plain HTML 4 rather than its script -
-- so it is the default; a smart TV's was refused by Google and sent
-- YouTube's "not supported", and a 3DS's only trimmed YouTube. Every one
-- still says Kosmos. Google sends no results to any of them: it stopped
-- answering searches without scripts.
--
local AGENTS = {
  lynx = "Lynx/2.9.0 (Kosmos %s; NetSurf/3.11)",
  netsurf = "NetSurf/3.11 (Kosmos %s)",
  console = "Mozilla/5.0 (Nintendo 3DS; U; ; en) Version/1.7412.EU Kosmos/%s",
}

-- The words sent, for these settings and this build's version.
function prefs.agent(t, version)
  if t.agent == "own" and t.agent_words ~= "" then return t.agent_words end

  return (AGENTS[t.agent] or AGENTS.lynx):format(version or "0")
end

local function made(dir)
  if fs.getattr(dir) then return true end

  local at = ""

  for part in dir:gmatch("[^/]+") do
    at = at .. "/" .. part

    if not fs.getattr(at) then fs.send(at, { type = "mkdir" }) end
  end

  return fs.getattr(dir) ~= nil
end

local function valid(key, value)
  local default = prefs.DEFAULTS[key]

  if default == nil or type(value) ~= type(default) then return false end

  if type(value) == "number" and value ~= math.floor(value) then return false end

  local choices = prefs.CHOICES[key]

  if choices then
    for _, c in ipairs(choices) do
      if c[1] == value then return true end
    end

    return false
  end

  -- No control character in any of them: an address, a name - and the
  -- words sent as a header, where a line's end would be a header of its own.
  if type(value) == "string" then
    return value ~= "" and #value <= 512 and not value:find("%c")
  end

  return true
end

-- What the file says, over the defaults, each value checked.
function prefs.read(file)
  local got = fs.read(file or prefs.FILE)
  local out = {}

  -- Not `ok and v or default`, which is the default for every `false` -
  -- and "Show the favorites bar" off is one.
  for key, default in pairs(prefs.DEFAULTS) do
    local v = nil

    if type(got) == "table" then v = got[key] end

    if v ~= nil and valid(key, v) then out[key] = v else out[key] = default end
  end

  return out
end

-- The settings written, what is known and valid of them; true when kept.
function prefs.write(t, file)
  file = file or prefs.FILE

  if not made(file:match("^(.*)/[^/]+$")) then return false end

  local out = {}

  for key, default in pairs(prefs.DEFAULTS) do
    local v = t[key]

    if v ~= nil and valid(key, v) then out[key] = v else out[key] = default end
  end

  return fs.write(file, out) and true or false
end

-- What a choice is called: "30 days" for 30.
function prefs.name_of(key, value)
  for _, c in ipairs(prefs.CHOICES[key] or {}) do
    if c[1] == value then return c[2] end
  end

  return tostring(value)
end

--
-- **The tabs that are open**, their addresses in order and which is shown,
-- kept as they change, for the next time the browser opens when Settings
-- says it opens on them.
--
function prefs.save_tabs(addresses, shown, file)
  file = file or prefs.TABS

  if not made(file:match("^(.*)/[^/]+$")) then return false end

  local list = {}

  for _, a in ipairs(addresses) do
    if type(a) == "string" and a ~= "" then list[#list + 1] = a end
  end

  return fs.write(file, { tabs = list, shown = tonumber(shown) or 1 }) and true or false
end

-- What was kept: the addresses and which was shown, or an empty list.
function prefs.tabs(file)
  local got = fs.read(file or prefs.TABS)
  local list = {}

  if type(got) == "table" and type(got.tabs) == "table" then
    for _, a in ipairs(got.tabs) do
      if type(a) == "string" and a ~= "" then list[#list + 1] = a end
    end
  end

  local shown = type(got) == "table" and tonumber(got.shown) or 1

  return list, math.max(1, math.min(shown, #list))
end

--
-- **Searching from the address field** (`roadmap.md` 6zz d6): the engines'
-- pages for a search, `%s` the words as a form sends them. DuckDuckGo's is
-- its page without scripts, which answers this browser whatever it says it
-- is; Google's is offered because the drawing offers it, and answers no
-- browser without scripts now.
--
prefs.SEARCH = {
  duckduckgo = "https://html.duckduckgo.com/html/?q=%s",
  google = "https://www.google.com/search?q=%s",
}

-- Words as a form sends them: a space a +, letters, digits and -_.~ as
-- they are, every other byte %XX - so `ñandú` is its UTF-8, encoded.
function prefs.form_encode(words)
  return (tostring(words):gsub("[^%w%-_.~ ]", function(c)
    return ("%%%02X"):format(c:byte())
  end):gsub(" ", "+"))
end

local SCHEMES = { http = true, https = true, about = true, asset = true,
                  file = true, kosmos = true }

--
-- **Where what is typed goes**: an address when it looks like one - a
-- scheme this browser speaks, a path on this machine, `localhost`, four
-- numbers, or a host with a dot and a name of letters after it, a port and
-- a path allowed - and otherwise a search at the engine Settings chose.
-- Anything with a space in it is words. Returns the address, and whether
-- it is a search.
--
function prefs.destination(text, engine)
  local t = tostring(text or ""):match("^%s*(.-)%s*$")

  if t == "" then return nil end

  if not t:find("%s") then
    local scheme = t:match("^(%a[%w+.%-]*):")

    if (scheme and SCHEMES[scheme:lower()]) or t:sub(1, 1) == "/" then
      return t, false
    end

    local name = (t:match("^([^/?#]+)") or ""):gsub(":%d+$", "")

    if name:lower() == "localhost" or name:match("^%d+%.%d+%.%d+%.%d+$")
       or (name:match("^[%w%-.]+$") and name:match("%.%a[%a%-]*$")) then
      return t, false
    end
  end

  return (prefs.SEARCH[engine] or prefs.SEARCH.duckduckgo):format(prefs.form_encode(t)), true
end

return prefs
