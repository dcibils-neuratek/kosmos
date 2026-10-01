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

prefs.DIR = "/Home/Preferences/browser"
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
  text = 100,                   -- Text size, per cent
  images = true,                -- Load images
  costs = true,                 -- What each page cost, in the status line
  search = "duckduckgo",        -- Search with (d6)
  history_days = 30,            -- Keep history for
}

-- A choice's value and what it is called, in the order offered.
prefs.CHOICES = {
  opens = { { "home", "The home page" }, { "tabs", "The tabs it had" } },
  text = { { 80, "80%" }, { 90, "90%" }, { 100, "100%" }, { 110, "110%" },
           { 125, "125%" }, { 150, "150%" }, { 175, "175%" }, { 200, "200%" } },
  search = { { "duckduckgo", "DuckDuckGo" }, { "google", "Google" } },
  history_days = { { 7, "A week" }, { 30, "30 days" }, { 90, "90 days" },
                   { 365, "A year" } },
}

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

  if type(value) == "string" then return value ~= "" end

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

return prefs
