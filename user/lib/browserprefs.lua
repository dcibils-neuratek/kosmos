-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- JetSki's settings, and where what is typed in its address field goes.
--
-- **One table, the file JetSki names** (`/Home/Settings/JetSki/settings`),
-- written with `fs.write` and read back with `fs.read` as every settings
-- file in Kosmos is; and the tabs that were open, beside it, for "When
-- JetSki opens: The tabs it had".
--
-- **What is read is what JetSki can use**: each key it knows, of the kind
-- it should be and - for a choice - one of the choices; anything else is
-- the default. A file edited by hand, or written by an older JetSki - or by
-- the browser before it, whose favorites bar, cost line and user agents
-- went in the review before 0.12.0 (`docs/cleanup.md`) - is read for what
-- it still says rather than trusted whole, and what is written is only
-- what is known.
--
-- Pure: `fs` is the global every process has, and the host test hands it
-- one kept in memory (`tools/test_browserprefs.lua`).

local prefs = {}

-- Its folder made, and the ones above it, by the file library.
local files = use("/Kosmos/Libraries/files.lua")

-- The page Settings is, in a tab of its own.
prefs.PAGE = "about:settings"

prefs.DEFAULTS = {
  home = "https://en.wikipedia.org/wiki/BeOS",  -- Home page
  opens = "home",               -- When JetSki opens
  zoom = 100,                   -- Zoom, per cent: all of the page
  images = true,                -- Load images
  javascript = true,            -- JavaScript
  search = "duckduckgo",        -- Search with (d6)
  history_days = 30,            -- Keep history for
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
}

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

  -- No control character in any of them: an address is a line.
  if type(value) == "string" then
    return value ~= "" and #value <= 512 and not value:find("%c")
  end

  return true
end

-- What the file says, over the defaults, each value checked.
function prefs.read(file)
  local got = fs.read(file)
  local out = {}

  -- Not `ok and v or default`, which is the default for every `false` -
  -- and "Load images" off is one.
  for key in pairs(prefs.DEFAULTS) do
    local default = prefs.DEFAULTS[key]
    local v = nil

    if type(got) == "table" then v = got[key] end

    if v ~= nil and valid(key, v) then out[key] = v else out[key] = default end
  end

  return out
end

-- The settings written, what is known and valid of them; true when kept.
function prefs.write(t, file)

  if not files.make_folder(files.parent(file)) then return false end

  local out = {}

  for key in pairs(prefs.DEFAULTS) do
    local default = prefs.DEFAULTS[key]
    local v = t[key]

    if v ~= nil and valid(key, v) then out[key] = v else out[key] = default end
  end

  return fs.write(file, out) and true or false
end

--
-- **The tabs that are open**, their addresses in order and which is shown,
-- kept as they change, for the next time JetSki opens when Settings says
-- it opens on them.
--
function prefs.save_tabs(addresses, shown, file)

  if not files.make_folder(files.parent(file)) then return false end

  local list = {}

  for _, a in ipairs(addresses) do
    if type(a) == "string" and a ~= "" then list[#list + 1] = a end
  end

  return fs.write(file, { tabs = list, shown = tonumber(shown) or 1 }) and true or false
end

-- What was kept: the addresses and which was shown, or an empty list.
function prefs.tabs(file)
  local got = fs.read(file)
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
-- its page without scripts, which was the only one the browser before
-- JetSki could use, and is quick for it too.
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
-- scheme JetSki speaks, a path on this machine, `localhost`, four
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
