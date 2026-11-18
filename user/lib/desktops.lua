-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The desktops this machine can start, and which one was chosen last.
--
-- **A desktop is a file** (`docs/pewter.md`, agreed 7 November 2026):
-- `/Kosmos/Desktops/<Name>.desktop`, `user/desktops/` in the tree, in a
-- look's `key = value` format and read by its line reader
-- (`theme.lines`). It names the programs that are the desktop, and the
-- window manager starts them when it is chosen. There is one window
-- manager under every desktop; a second desktop is a second file, not a
-- second `wm.lua`.
--
-- **Read by the two programs that care**, as `startup.lua` is read by the
-- Deskbar and Preferences: the chooser lists what is here, and the window
-- manager starts what the chosen one names. If each read the files its own
-- way, the chooser could offer a desktop the window manager would refuse.
--
local theme = use("/Kosmos/Libraries/theme.lua")

local M = {}

M.DIR = "/Kosmos/Desktops"

-- The setting the last choice is kept in, as the settings kit names it.
M.SETTING = "desktop"

-- What a machine that has never chosen starts with.
M.DEFAULT = "Astra"

-- The keys a desktop file may say. Anything else is a complaint, so a
-- word misspelt is told rather than quietly doing nothing.
local KEYS = { name = true, says = true, shell = true, background = true,
               chrome = true, menus = true, look = true,
               cut = true, star = true, plate = true,
               flat = true, title_bars = true, shadow = true }

--
-- **The style a desktop wears, which no look can** (Diego, 18 November
-- 2026: "desktop style is the way it works and looks", "the theme is
-- colors and transparency"). Choosing Liquid Glass in Astra put
-- Starlight's star and amber plate on Astra's windows, because the look
-- said `star` and `plate`. They are the desktop's now, each `yes` or `no`:
--
--   - `cut`: what floats - the dock, menus - cut at its top-left (S2);
--   - `star`: Kosmos's four-point star on the plates and glints (S5);
--   - `plate`: the window in front's title on an amber plate (S1);
--
-- and since 0.11.506 the three that were still a look's (Diego, 18
-- November: "yes move all three to the desktop") - choosing Plex took the
-- room for a title bar from a maximised window and Night gave it back:
--
--   - `flat`: a hairline where a bevel would be (`theme.lua`);
--   - `title_bars`: a maximised window leaves room for a bar, or its
--     header is its title bar and it has the screen's width;
--   - `shadow`: how far a window's shadow reaches, in points.
--
-- `theme.STYLE_NONE` is each where a file does not say it.
--
M.STYLE = theme.STYLE

-- The title bars the kit can draw (`ui.header`): Astra's, Platinum's since
-- Pewter's P2, and Starlight's - Astra's with the window in front's title on
-- the window manager's plate (`docs/astra.md`, *Identity*, S1).
M.CHROMES = { astra = true, platinum = true, starlight = true }

-- Where an application's menus go (P3): in its window, or handed to the
-- window manager for the bar across the top of the screen.
M.MENUS = { window = true, screen = true }

-- A comma-separated list, each word trimmed, the empty ones dropped.
local function words(value)
  local out = {}

  for word in tostring(value or ""):gmatch("[^,]+") do
    word = word:match("^%s*(.-)%s*$")

    if word ~= "" then out[#out + 1] = word end
  end

  return out
end

--
-- A desktop from a file's text: `{ name, says, shell = { "a", "b:words" },
-- background = { a = true }, chrome, menus, look }`, and the lines it did not understand. A
-- desktop that names no shell is refused, since choosing it would start
-- nothing and leave a screen with no way to start anything.
--
function M.parse(text, stem)
  local d = { name = stem, says = "", shell = {}, background = {}, chrome = "astra",
              menus = "window" }

  for k, v in pairs(theme.STYLE_NONE) do d[k] = v end
  local said = {}

  for n, key, value in theme.lines(text) do
    if not key then
      said[#said + 1] = ("line %d: not `key = value`"):format(n)
    elseif not KEYS[key] then
      said[#said + 1] = ("line %d: a desktop has no `%s`"):format(n, key)
    elseif key == "shell" then
      d.shell = words(value)
    elseif key == "background" then
      for _, w in ipairs(words(value)) do d.background[w] = true end
    elseif key == "chrome" then
      if M.CHROMES[value] then
        d.chrome = value
      else
        said[#said + 1] = ("line %d: the kit draws no `%s` chrome"):format(n, value)
      end
    elseif key == "menus" then
      if M.MENUS[value] then
        d.menus = value
      else
        said[#said + 1] = ("line %d: menus go in the `window` or on the `screen`, not `%s`")
                          :format(n, value)
      end
    elseif key == "shadow" then
      local v = math.tointeger(tonumber(value))

      if v and v >= 0 and v <= theme.METRIC_MOST then
        d.shadow = v
      else
        said[#said + 1] = ("line %d: `shadow` is a number of points from 0 to %d, not `%s`")
                          :format(n, theme.METRIC_MOST, value)
      end
    elseif theme.STYLE_NONE[key] ~= nil then
      if value == "yes" or value == "no" then
        d[key] = (value == "yes")
      else
        said[#said + 1] = ("line %d: `%s` is yes or no, not `%s`"):format(n, key, value)
      end
    elseif key == "look" then
      -- The look it wears (`user/themes`), by its key - `starlight` - which
      -- the window manager applies before the desktop's first window.
      d.look = value:lower():gsub("[^%w]", "")
    else
      d[key] = value
    end
  end

  if #d.shell == 0 then
    return nil, "it names no shell to start", said
  end

  return d, nil, said
end

-- One, by its name: `Astra` reads `/Kosmos/Desktops/Astra.desktop`.
function M.read(name)
  name = tostring(name or "")

  if not name:match("^[%w][%w _%-]*$") then
    return nil, ("%q is not a desktop's name"):format(name)
  end

  local text, why = fs.read(M.DIR .. "/" .. name .. ".desktop")

  if type(text) ~= "string" then
    return nil, ("there is no desktop called %s (%s)"):format(name, tostring(why))
  end

  return M.parse(text, name)
end

-- Every one there is, by name, Astra first and the rest in order.
function M.list()
  local out = {}

  for _, file in ipairs(fs.list(M.DIR) or {}) do
    local stem = type(file) == "string" and file:match("^(.+)%.desktop$")

    if stem then out[#out + 1] = stem end
  end

  table.sort(out, function(a, b)
    if a == M.DEFAULT then return b ~= M.DEFAULT end
    if b == M.DEFAULT then return false end
    return a < b
  end)

  return out
end

-- The one chosen last, or the default on a machine that never chose.
function M.chosen()
  local prefs = use("/Kosmos/Libraries/prefs.lua")
  local name = prefs.read(M.SETTING).name

  return type(name) == "string" and name or M.DEFAULT
end

-- Kept, for the chooser to start on next time.
function M.remember(name)
  return use("/Kosmos/Libraries/prefs.lua").write(M.SETTING, { name = tostring(name) })
end

return M
