-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- What there is to configure, in one list.
--
-- **This is the whole design of Preferences**, and it is a list rather than a
-- server for a reason worth knowing. The question when Diego asked for "one
-- place to configure all kosmos" was where the settings should *live*: a
-- window that reads and writes each application's own file is a second copy
-- of every format, and one that owns them all is a settings server and a
-- protocol, which is the shape this system normally reaches for.
--
-- The survey answered it, on 23 September 2026, and the answer is neither.
-- **Every settings file in Kosmos is already the same thing**: a Lua table
-- written with `fs.write` and read back with `fs.read`, through the system's
-- own serialiser. There is no second format to copy because there is only
-- one, and there is nothing for a server to own that a file does not own
-- already.
--
-- So what was missing is not storage. It is a list of *what there is* -
-- which file, which key, what kind of thing it is, and one line saying what
-- it does. Preferences draws itself from this table, and every application
-- goes on reading its own file exactly as it did.
--
-- **A setting that belongs to one window is not here.** A Terminal's text
-- size is a property of that Terminal and lives in its View menu;
-- Preferences sets what a *new* one starts at. The test is: if changing it
-- should change one window, it does not belong in this list.
--
-- `docs/preferences.html` is the page this was drawn from, and
-- `roadmap.md` 5zh the agreement.
--

local settings = {}

--
-- The categories, in the order the sidebar shows them.
--
-- Nine, after Diego folded the desktop's icons and the Deskbar into
-- Appearance: "they are what the machine looks like". `after` marks where a
-- gap goes, which is how the sidebar groups them without a second list.
--
settings.CATEGORIES = {
  { id = "appearance", name = "Appearance" },
  { id = "displays",   name = "Displays" },
  { id = "sound",      name = "Sound" },
  { id = "power",      name = "Power", gap_after = true },
  { id = "network",    name = "Network" },
  { id = "keyboard",   name = "Keyboard" },
  { id = "startup",    name = "Startup" },
  { id = "datetime",   name = "Date & Time", gap_after = true },
  { id = "system",     name = "System" },
}

--
-- The files settings live in, named once so a typo is a missing value rather
-- than a second file nobody reads.
--
settings.APPEARANCE = "/home/.appearance"
settings.TRACKER    = "/home/.tracker"
settings.CLOCK      = "/home/.clock"
settings.STARTUP    = "/home/.startup"

--
-- One setting.
--
--   category  which page it appears on
--   group     the heading above it, and the card it shares
--   label     what it is called
--   note      one line under the label, or nil for none
--   kind      "choice" | "switch" | "text" | "action" | "boot"
--   file/key  where it lives; nil for something read from elsewhere
--   choices   for a choice: { { value, name }, ... }
--   default   what it is when the file does not say
--
-- **`boot` is a kind of its own and that is the point.** `opt/kosmos/smp`,
-- `video=WxH` and `irq=pic` can only be set by rebuilding a stick or editing
-- a line on one, so they are shown, explained, and marked as needing a
-- restart. A machine you can only configure by rebuilding it is the thing
-- this application exists to end; listing them is the first step and lets
-- somebody see what a machine is running before anything can change it.
--
local function item(t) return t end

settings.ITEMS = {
  --------------------------------------------------------------- appearance
  item{ category = "appearance", group = "Look",
        label = "Theme", note = "The colours and faces every window uses",
        kind = "choice", file = settings.APPEARANCE, key = "palette",
        default = "plex",
        --
        -- **The looks that ship, and it has to be all of them.** Endeavour
        -- arrived in `themes.lua` and not here, so the dropdown showed the
        -- four it knew and the fifth as the raw word `endeavour` - which is
        -- what `name_of` falls back to and exactly what it should have
        -- looked like. `tools/test_settings.lua` holds this list to
        -- `themes.order` now, because a list of the same thing in two files
        -- is a list that drifts.
        --
        choices = { { "plex", "Plex" }, { "plexnight", "Plex Night" },
                    { "classic", "Classic" }, { "studio", "Studio" },
                    { "endeavour", "Endeavour" } } },

  item{ category = "appearance", group = "Look",
        label = "Wallpaper", note = "Carried in the image, or a picture in /home",
        kind = "choice", file = settings.APPEARANCE, key = "wallpaper",
        default = "", choices = nil },   -- filled at run time from the image

  item{ category = "appearance", group = "Size",
        label = "Scale",
        note = "Everything larger, for a small screen at a high resolution",
        kind = "choice", file = settings.APPEARANCE, key = "scale",
        default = 100,
        choices = { { 100, "100%" }, { 125, "125%" }, { 150, "150%" },
                    { 200, "200%" } } },

  --
  -- **Two the window manager draws rather than any window.** They live in
  -- `/home/.appearance` like the look, and like the look, the scale and the
  -- wallpaper they have to reach the manager the moment they change - a
  -- setting that took effect at the next restart is one nobody believes in.
  --
  -- **How that happens is not declared here.** It was, for a version, as
  -- `live = "corner"`, and that shape could only ever send one field - so
  -- the look, which is a resolved colour table and a set of faces, could
  -- not use it and quietly did nothing. `preferences.lua` keys its applies
  -- by a setting's own `key`, so there is no second list in this file to
  -- fall out of step with that one.
  --
  item{ category = "appearance", group = "Windows",
        label = "Rounded corners", note = "The four corners of every window",
        kind = "switch", file = settings.APPEARANCE, key = "corner",
        default = true },

  item{ category = "appearance", group = "Windows",
        label = "Drop shadows",
        note = "A soft edge under every window. Costly on a slow machine",
        kind = "switch", file = settings.APPEARANCE, key = "shadow",
        default = false },

  item{ category = "appearance", group = "Icons",
        label = "On the desktop", note = "Small 16, Normal 32, Large 64",
        kind = "choice", file = settings.TRACKER, key = "desktop_icon_px",
        default = 32,
        choices = { { 16, "Small" }, { 32, "Normal" }, { 64, "Large" } } },

  item{ category = "appearance", group = "Icons",
        label = "In a Tracker window",
        kind = "choice", file = settings.TRACKER, key = "window_icon_px",
        default = 32,
        choices = { { 16, "Small" }, { 32, "Normal" }, { 64, "Large" } } },

  ----------------------------------------------------------------- displays
  item{ category = "displays", group = "Screen",
        label = "Resolution",
        note = "The largest the firmware offers, unless told, video=WxH",
        kind = "boot", boot = "video=WxH" },

  item{ category = "displays", group = "Screen",
        label = "Scale", note = "The same setting Appearance has, here too",
        kind = "choice", file = settings.APPEARANCE, key = "scale",
        default = 100,
        choices = { { 100, "100%" }, { 125, "125%" }, { 150, "150%" },
                    { 200, "200%" } } },

  -------------------------------------------------------------------- sound
  item{ category = "sound", group = "Output",
        label = "Volume", kind = "level" },
  item{ category = "sound", group = "Output",
        label = "Mute", kind = "switch" },

  -------------------------------------------------------------------- power
  item{ category = "power", group = "Power button",
        label = "When it is pressed", kind = "choice",
        default = "off",
        choices = { { "off", "Shut down" }, { "ask", "Ask" },
                    { "nothing", "Do nothing" } } },

  ------------------------------------------------------------------ network
  item{ category = "network", group = "Address",
        label = "Configure", kind = "choice", default = "dhcp",
        choices = { { "dhcp", "Automatic" }, { "static", "Manual" } } },
  item{ category = "network", group = "Address",
        label = "Address", kind = "text" },
  item{ category = "network", group = "Address",
        label = "Gateway", kind = "text" },

  ----------------------------------------------------------------- keyboard
  item{ category = "keyboard", group = "The Super key",
        label = "Press it alone", kind = "choice", default = "launcher",
        choices = { { "launcher", "Launcher" }, { "nothing", "Nothing" } } },
  item{ category = "keyboard", group = "Shortcuts",
        label = "Show all shortcuts",
        note = "Every key the window manager answers", kind = "action" },

  ------------------------------------------------------------------ startup
  item{ category = "startup", group = "What runs when the machine starts",
        label = "", kind = "startup", file = settings.STARTUP, key = "items" },

  ---------------------------------------------------------------- date/time
  item{ category = "datetime", group = "Clock",
        label = "Time zone",
        note = "The board keeps UTC; this is the offset from it",
        kind = "choice", file = settings.CLOCK, key = "offset", default = 0,
        choices = nil },   -- filled at run time: the offsets in minutes

  ------------------------------------------------------------------- system
  item{ category = "system", group = "Processors",
        label = "Use", note = "How many threads are spread across, smp=N",
        kind = "boot", boot = "smp=N" },

  item{ category = "system", group = "Interrupts",
        label = "Controller",
        note = "The legacy pair, for a machine the APIC path fails on, irq=pic",
        kind = "boot", boot = "irq=pic" },

  item{ category = "system", group = "About", label = "Kosmos",
        kind = "fact", fact = "version" },
  item{ category = "system", group = "About", label = "Machine",
        kind = "fact", fact = "machine" },
  item{ category = "system", group = "About", label = "Memory",
        kind = "fact", fact = "memory" },
  item{ category = "system", group = "About",
        label = "Licences", note = "What is in this image and under what terms",
        kind = "action" },
}

--
-- The items of one category, in order, already divided into their groups.
--
-- Returns `{ { name = "Look", items = { ... } }, ... }`. The order is the
-- order of `ITEMS`, so a group is wherever its first item is and the table
-- above reads top to bottom exactly as the window does.
--
function settings.groups(category)
  local out, seen = {}, {}

  for _, it in ipairs(settings.ITEMS) do
    if it.category == category then
      local g = seen[it.group]

      if not g then
        g = { name = it.group, items = {} }
        seen[it.group] = g
        out[#out + 1] = g
      end

      g.items[#g.items + 1] = it
    end
  end

  return out
end

--
-- What a setting is set to, or its default.
--
-- `read` is the namespace read, passed in so the host can test this without
-- a filesystem; applications call `settings.get(item)` and get `fs.read`.
--
function settings.get(it, read)
  read = read or fs.read

  if not it or not it.file or not it.key then return it and it.default end

  local saved = read(it.file)

  if type(saved) ~= "table" then return it.default end

  local v = saved[it.key]

  if v == nil then return it.default end

  return v
end

--
-- Set one, by reading the file, changing one key and writing it back.
--
-- **Read, change, write, and never a whole file composed from what this
-- process happens to know.** Two places share `/home/.appearance` and two
-- share `/home/.tracker`, so a write that rebuilt the table would drop
-- whatever the other one had put there - which is the mistake `iconsize.lua`
-- documents having avoided for the same reason.
--
-- **The default is stored as nothing.** A setting nobody chose follows a
-- default that may change rather than freezing the one that was in force the
-- day somebody opened the window, which is `textsize.lua`'s rule and worth
-- keeping the same everywhere.
--
function settings.set(it, value, read, write)
  read = read or fs.read
  write = write or fs.write

  if not it or not it.file or not it.key then return false, "not stored" end

  local saved = read(it.file)

  if type(saved) ~= "table" then saved = {} end

  if value == it.default then value = nil end

  if saved[it.key] == value then return true end

  saved[it.key] = value

  return write(it.file, saved)
end

--
-- The name of a choice's current value, for a control that shows it.
--
function settings.name_of(it, value)
  for _, c in ipairs(it.choices or {}) do
    if c[1] == value then return c[2] end
  end

  return tostring(value)
end

return settings
