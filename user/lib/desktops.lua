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
               chrome = true, menus = true }

-- The title bars the kit can draw (`ui.header`): Astra's, and Platinum's
-- since Pewter's P2.
M.CHROMES = { astra = true, platinum = true }

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
-- background = { a = true }, chrome, menus }`, and the lines it did not understand. A
-- desktop that names no shell is refused, since choosing it would start
-- nothing and leave a screen with no way to start anything.
--
function M.parse(text, stem)
  local d = { name = stem, says = "", shell = {}, background = {}, chrome = "astra",
              menus = "window" }
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
