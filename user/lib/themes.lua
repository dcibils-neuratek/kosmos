-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The looks that ship: their order and their names. Each look itself is a
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

function themes.file(key)
  local title = themes.titles[key]

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
