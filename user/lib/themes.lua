-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The looks: the ones that ship, their order and their names, and a
-- person's own from the Themes folder of their preferences (below). Each look itself is a
-- file, `/Kosmos/Themes/<Name>.theme` - `user/themes/` in the tree, carried in
-- the image beside the applications (`roadmap.md` 6s c3b; Diego, 27
-- September: "Themes in /Kosmos/Themes, yes").
--
-- Not tables. Text, in exactly the format a `.theme` file on the disk uses,
-- parsed by exactly the parser that reads one - so the format is the thing
-- that ships rather than a thing bolted on beside it, and a bug in the
-- parser is a bug in the desktop rather than a bug in a feature nobody uses.
-- It was text inside this file until the looks became files a person can
-- open and read; `themes.plex` still gives the text, read from its file the
-- first time it is asked for.
--
-- **Six looks, and nothing else to choose** (`roadmap.md` 5y). Diego, 22
-- September 2026: "Too many config options make the system vulnerable to
-- changes and complicated", and "Let's just make 3 or 4 good design options
-- in colors and fonts and stick to those". Each is a whole - its colours,
-- its faces, its Deskbar - designed together and approved: Plex, Plex
-- Night, Classic and Studio in `docs/looks.html` ("Those 4 looks are
-- great"), Endeavour on 23 September and Night on 3 October, each in its
-- own file. Somebody picks a look, not its parts. They share their faces,
-- IBM Plex at the sizes the fixed layout holds, and differ in colour.
--
-- The four this file held before - Photon, BeOS, Platinum, IRIX, each a
-- system that solved `ui.md` 16.8b's dimensional look its own way - gave
-- way to them the same day. BeOS lives on as Classic, its researched
-- values and notes kept; the others are in the history.
--
-- Where a system used a pinstripe there is a flat colour that reads as it.
-- The chrome - window tabs and menu bars - is shaded from the one colour
-- named here, by `theme.chrome`, so a palette names one surface and gets
-- both ends of it.

local themes = {}

-- The order they are offered in, the default first.
--
-- **Endeavour first since 24 September.** Diego, once the title bar's three
-- were coloured circles (`roadmap.md` 5zq): "yellow window bars will make
-- the yellow window button look lost", and "we might need the endeavor
-- theme to be the default now as its colors match the current style
-- better". Plex, Plex Night and Classic have yellow or amber tabs, and the
-- amber minimise sits on its own colour there; Endeavour's tab is a light
-- blue the three read cleanly on. Being first is the whole of being the
-- default: the window manager and Preferences both take `order[1]`.
-- Night, the sixth, last (`roadmap.md`, a dock at the bottom): offered
-- beside the others, never the default.
--
-- **Five more on 8 October** (Diego: "add 5 more theme colors, 3 dark ones
-- and 2 light ones with different tones that make the desktop look cool.
-- create the names as well for those"): Aurora, Amethyst and Ember dark,
-- Sakura and Meadow light, after Night.
themes.order = { "endeavour", "plex", "plexnight", "classic", "studio", "night",
                 "aurora", "amethyst", "ember", "sakura", "meadow" }

-- What each is called where a person reads it.
themes.titles = {
  plex = "Plex", plexnight = "Plex Night", classic = "Classic",
  studio = "Studio", endeavour = "Endeavour", night = "Night",
  aurora = "Aurora", amethyst = "Amethyst", ember = "Ember",
  sakura = "Sakura", meadow = "Meadow",
}

--
-- Where the looks are, and each one's file: its name, without a space.
--
themes.DIR = "/Kosmos/Themes"

--
-- **And a person's own, in their preferences' `Themes` folder**
-- (`/Home/Preferences/Themes`). Diego, 13 October 2026: "all settings and
-- preferences files should be editable as per our premise", "add
-- /Home/Themes to the roadmap and build it", and then "all preferences
-- should be under preferences". The looks above are in the system's image,
-- which nobody edits; a `.theme` file there is text a person writes, in the
-- same format, read by the same parser:
--
--   - **named like one that ships** - `Night.theme` - it is that look, in
--     its place in the list and under its name: copy `/Kosmos/Themes/
--     Night.theme` there and change it, and Night is changed; delete the
--     copy and Night is the system's again;
--   - **named anything else** - `My Night.theme` - it is one more look,
--     after the ones that ship, called by its file's name.
--
-- A look's key is its file's name in lower case without spaces, as the
-- ones that ship are (`plexnight` for `PlexNight.theme`), so a copy finds
-- its original whichever way it is spelled. Read once, when this file is
-- first used: a look added is offered from the next time Preferences opens.
-- The folder is named by the settings kit, which alone names that folder;
-- Preferences makes it, so it is there to be found.
--
local prefs_ok, prefs = pcall(use, "/Kosmos/Libraries/prefs.lua")

themes.HOME = prefs_ok and type(prefs) == "table" and prefs.DIR and (prefs.DIR .. "/Themes") or nil

local own = {}                      -- key -> its file in the Themes folder

local function key_of(stem)
  return (stem:lower():gsub("[^%w]", ""))
end

for _, file in ipairs((themes.HOME and fs and fs.list and fs.list(themes.HOME)) or {}) do
  local stem = type(file) == "string" and file:match("^(.+)%.theme$")
  local key = stem and key_of(stem)

  if key and key ~= "" and not own[key] then
    own[key] = themes.HOME .. "/" .. file

    if not themes.titles[key] then
      themes.order[#themes.order + 1] = key
      themes.titles[key] = stem
    end
  end
end

-- Whether a look is a person's file rather than the system's, for whoever
-- wants to say so.
function themes.own(key) return own[key] ~= nil end

function themes.file(key)
  local title = themes.titles[key]

  if own[key] then return own[key] end

  return title and (themes.DIR .. "/" .. title:gsub(" ", "") .. ".theme") or nil
end

--
-- `themes.plex` is Plex's text, read from its file the first time and kept:
-- a look does not change while the machine runs, and the window manager and
-- Appearance each ask for it more than once.
--
setmetatable(themes, {
  __index = function(t, key)
    local path = type(key) == "string" and rawget(t, "file")(key)
    local text = path and fs and fs.read(path)

    if type(text) ~= "string" then return nil end

    rawset(t, key, text)
    return text
  end,
})

return themes
