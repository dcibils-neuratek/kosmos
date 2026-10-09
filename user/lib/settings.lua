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
--
-- `icon` names a line icon the mockup draws beside the category
-- (`assets/icons/line/`, `tools/lineicons.py`).
--
settings.CATEGORIES = {
  { id = "appearance", name = "Appearance",  icon = "appearance" },
  { id = "displays",   name = "Displays",    icon = "display" },
  --
  -- **Notifications** (`roadmap.md`, *Notifications*, step 3): Do Not
  -- Disturb and how long and how many, from `ITEMS`; and a switch for
  -- every application that has said something, which are not here - they
  -- are whoever posted (`settings.notifiers`), so a new one brings its own.
  --
  { id = "notifications", name = "Notifications", icon = "bell",
    from_notifications = true },
  { id = "sound",      name = "Sound",       icon = "sound" },
  { id = "power",      name = "Power",       icon = "power", gap_after = true },
  { id = "network",    name = "Network",     icon = "network" },
  { id = "keyboard",   name = "Keyboard",    icon = "keyboard" },
  { id = "mouse",      name = "Mouse",       icon = "mouse" },
  { id = "startup",    name = "Startup",     icon = "startup" },
  --
  -- **File types** (`roadmap.md` 6z): what opens what, whose rows are not
  -- here - every type an application says it opens, from the applications
  -- themselves (`filetypes.page`), so a new one brings its own.
  --
  { id = "filetypes",  name = "File types",  icon = "document",
    from_applications = true },
  { id = "datetime",   name = "Date & Time", icon = "datetime",
    gap_after = true },
  { id = "system",     name = "System",      icon = "system" },
}

--
-- The settings the rows live in, named once so a typo is a missing value
-- rather than a second file nobody reads - as the settings kit names them
-- (`prefs.lua`: "appearance" is `/Home/Preferences/appearance`), which is
-- what reads and writes them for this list as for every application.
--
settings.APPEARANCE = "appearance"
settings.TRACKER    = "tracker"
settings.CLOCK      = "clock"
settings.STARTUP    = "startup"
settings.POWER      = "power"
settings.KEYBOARD   = "keyboard"
settings.MOUSE      = "mouse"

--
-- **The Mouse page's two scales** (`roadmap.md` 6zi, `docs/preferences.html`):
-- each a slider of steps, the value kept being the step's own number - the
-- board's units per count, a span in milliseconds - so the file says what it
-- does without the slider beside it. The pointer's middle is today's 32; the
-- double click's slow end is the second it has always been, because under
-- QEMU a quick double click arrives three quarters of a second apart by the
-- machine's clock (`ui.md` 16.8c), and its middle half a second, as macOS and
-- Windows have it.
--
settings.POINTER_SPEEDS = { 8, 12, 16, 20, 26, 32, 40, 48, 60, 72, 90 }
settings.DOUBLE_CLICK_MS = { 1000, 800, 650, 500, 400, 300, 200 }
settings.NOTIFICATIONS = "notifications"

-- The settings kit's whole-file read and write, by name: what `get` and
-- `set` use unless a test hands them its own.
local function kit_read(name)
  return (use("/Kosmos/Libraries/prefs.lua").read(name))
end

local function kit_write(name, t)
  return use("/Kosmos/Libraries/prefs.lua").write(name, t)
end

--
-- One setting.
--
--   category  which page it appears on
--   group     the heading above it, and the card it shares
--   label     what it is called
--   note      one line under the label, or nil for none
--   kind      "choice" | "switch" | "stepper" | "fact" | "open" | and the
--             ones only one row has, each named for what it is: "volume",
--             "mute", "brightness", "startup"
--   fact      for a fact: which one, as Preferences reads it
--   program   for an `open`: what the button starts
--   file/key  where it lives; nil for something read from elsewhere
--   keep_default  written even when it equals the default
--   clears    other keys in the same file that this choice makes stale
--   choices   for a choice: { { value, name }, ... }
--   default   what it is when the file does not say
--
-- **Every row does something or says something true** (24 September,
-- Diego: "go thrpugh all the settings options and make sure they do
-- something useful, look good, stick to the design guidelines of kosmos").
-- Until then five rows were placeholders - a volume and a mute wired to
-- nothing, a power button and a Super key whose choice nothing read, an
-- address that said "Not yet", a startup list printed as `table:`, a time
-- zone printed as a number of minutes - and three said "Needs a restart"
-- about options set on a boot line. What a boot line sets is shown now as
-- what the machine *is* running, a `fact`, with the option in the note.
--
local function item(t) return t end

settings.ITEMS = {
  --------------------------------------------------------------- appearance
  item{ category = "appearance", group = "Look",
        label = "Theme", note = "The colours and faces every window uses",
        kind = "choice", file = settings.APPEARANCE, key = "palette",
        -- `themes.order[1]`, which `test_settings.lua` holds this to.
        default = "endeavour",
        --
        -- **The looks that ship, and it has to be all of them.** Endeavour
        -- arrived in `themes.lua` and not here, so the dropdown showed the
        -- four it knew and the fifth as the raw word `endeavour` - which is
        -- what `name_of` falls back to and exactly what it should have
        -- looked like. `tools/test_settings.lua` holds this list to
        -- `themes.order` now, because a list of the same thing in two files
        -- is a list that drifts.
        --
        choices = { { "endeavour", "Endeavour" }, { "plex", "Plex" },
                    { "plexnight", "Plex Night" }, { "classic", "Classic" },
                    { "studio", "Studio" }, { "night", "Night" },
                    { "aurora", "Aurora" }, { "amethyst", "Amethyst" },
                    { "ember", "Ember" }, { "sakura", "Sakura" },
                    { "meadow", "Meadow" } },
        --
        -- **Written by name even when it is the default**, which no other
        -- row is: the file is what a desktop starting up reads, and a
        -- machine on Plex should say so rather than be on Plex because
        -- nothing was said. And choosing a look forgets the faces an older
        -- panel may have written beside it, because the look brings its own.
        --
        keep_default = true, clears = { "fonts" } },

  --
  -- **Where the bar is** (`roadmap.md`, a dock at the bottom; Diego, 3
  -- October: "1. two" - the bar's place and the look are two settings - and
  -- "make it an option but defaults to floating"). Told to the Deskbar as it
  -- changes, which starts itself again in its new place (`preferences.lua`).
  --
  item{ category = "appearance", group = "Look",
        label = "The bar", note = "Across the top, or a dock at the foot of the screen",
        kind = "choice", file = settings.APPEARANCE, key = "bar",
        default = "top",
        choices = { { "top", "Top" }, { "dock", "Bottom, centred" } } },

  item{ category = "appearance", group = "Look",
        label = "The dock", note = "Floating above the edge, or the whole width",
        kind = "choice", file = settings.APPEARANCE, key = "dock",
        default = "floating",
        choices = { { "floating", "Floating" }, { "whole", "Whole width" } } },

  --
  -- **How much shows through the dock** (Diego, 4 October: "a slider from
  -- 100% to 0% for the dock bar", 25% unless said); its icons stay as they
  -- are. Told to the Deskbar as it moves (`preferences.lua`).
  --
  item{ category = "appearance", group = "Look",
        label = "The dock's transparency",
        note = "How much of what is behind shows through; its icons stay as they are",
        kind = "percent", file = settings.APPEARANCE, key = "dock_transparency",
        default = 25 },

  --
  -- **The dock's size** (Diego, 7 October: "can we add a preferences setting
  -- for it?", "so we can resize as needed"): its height and its icons
  -- together (`dock.SIZES`). Told to the Deskbar, which starts itself again
  -- at it, as it does for where the bar is.
  --
  item{ category = "appearance", group = "Look",
        label = "The dock's size",
        note = "How tall the dock is, and its icons with it",
        kind = "choice", file = settings.APPEARANCE, key = "dock_size",
        default = "medium",
        choices = { { "small", "Small" }, { "medium", "Medium" },
                    { "large", "Large" }, { "larger", "Larger" } } },

  item{ category = "appearance", group = "Look",
        label = "Wallpaper", note = "Carried in the image, or a picture in /Home",
        kind = "choice", file = settings.APPEARANCE, key = "wallpaper",
        default = "", choices = nil },   -- filled at run time from the image

  item{ category = "appearance", group = "Look",
        label = "Wallpaper size", note = "Filling the screen, its middle kept, or as it is in the middle",
        kind = "choice", file = settings.APPEARANCE, key = "wallpaper_fit",
        default = "fill",
        choices = { { "fill", "Fill the screen" }, { "centre", "Centred" } } },

  item{ category = "appearance", group = "Size",
        label = "Scale",
        note = "Everything larger, for a small screen at a high resolution",
        kind = "choice", file = settings.APPEARANCE, key = "scale",
        default = 100,
        choices = { { 100, "100%" }, { 125, "125%" }, { 150, "150%" },
                    { 200, "200%" } } },

  --
  -- **Two the window manager draws rather than any window.** They live in
  -- `/Home/Preferences/appearance` like the look, and like the look, the scale and the
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

  --
  -- **Focus follows the pointer** (`roadmap.md`, agreed 7 October; Diego:
  -- "once you hover an app with the mouse for 1 second (configurable in
  -- preferences) the window becomes active (as if you clicked it)"). Off
  -- until it is turned on, and the delay from a quarter of a second to
  -- three, a second by default. Told to the window manager at once
  -- (`handlers.keys`), which acts on it in its pointer pass.
  --
  item{ category = "appearance", group = "Windows",
        label = "Focus follows the pointer",
        note = "A window the pointer rests on comes to the front",
        kind = "switch", file = settings.APPEARANCE, key = "focus_follows",
        default = false },

  item{ category = "appearance", group = "Windows",
        label = "After resting for", note = "How long before it comes forward",
        kind = "choice", file = settings.APPEARANCE, key = "focus_delay_ms",
        default = 1000,
        -- Diego, 8 October: "use these options: 0ms, 100ms, 250ms, 500ms,
        -- 1 sec, 1.5 seconds, 2 seconds".
        choices = { { 0, "0 ms" }, { 100, "100 ms" }, { 250, "250 ms" }, { 500, "500 ms" },
                    { 1000, "1 second" }, { 1500, "1.5 seconds" },
                    { 2000, "2 seconds" } } },

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
        note = "The largest the firmware offers, or video=WxH at boot",
        kind = "fact", fact = "resolution" },

  item{ category = "displays", group = "Screen",
        label = "Scale", note = "The same setting Appearance has, here too",
        kind = "choice", file = settings.APPEARANCE, key = "scale",
        default = 100,
        choices = { { 100, "100%" }, { 125, "125%" }, { 150, "150%" },
                    { 200, "200%" } } },

  item{ category = "displays", group = "Screen",
        label = "Brightness", kind = "brightness" },

  ------------------------------------------------------------ notifications
  --
  -- What `notifications` reads when something arrives (`docs/
  -- notifications.html`, agreed 3 October): the switch, five seconds,
  -- two hundred kept. How many are kept is told to the server as it
  -- changes (`preferences.lua`); the rest are read where they are used.
  --
  item{ category = "notifications", group = "Notifications",
        label = "Do Not Disturb",
        note = "Silences every banner and alert; the history still keeps them",
        kind = "switch", file = settings.NOTIFICATIONS, key = "dnd",
        default = false },

  item{ category = "notifications", group = "Notifications",
        label = "A banner stays", note = "How long one shows before it goes by itself",
        kind = "choice", file = settings.NOTIFICATIONS, key = "seconds",
        default = 5,
        choices = { { 3, "3 seconds" }, { 5, "5 seconds" }, { 10, "10 seconds" } } },

  item{ category = "notifications", group = "Notifications",
        label = "Keep in the history", note = "The oldest go past it",
        kind = "choice", file = settings.NOTIFICATIONS, key = "keep",
        default = 200,
        choices = { { 50, "50" }, { 200, "200" }, { 0, "All" } } },

  -------------------------------------------------------------------- sound
  item{ category = "sound", group = "Output",
        label = "Device", kind = "fact", fact = "sound" },
  item{ category = "sound", group = "Output",
        label = "Volume", kind = "volume" },
  item{ category = "sound", group = "Output",
        label = "Mute", note = "Everything, until it is switched back",
        kind = "mute" },

  -------------------------------------------------------------------- power
  item{ category = "power", group = "Power button",
        label = "When it is pressed", kind = "choice",
        file = settings.POWER, key = "button", default = "off",
        choices = { { "off", "Shut down" }, { "menu", "Open the menu" },
                    { "nothing", "Do nothing" } } },

  ------------------------------------------------------------------ network
  item{ category = "network", group = "Connection",
        label = "Card", kind = "fact", fact = "net_card" },
  item{ category = "network", group = "Connection",
        label = "Address", kind = "fact", fact = "net_address" },
  item{ category = "network", group = "Connection",
        label = "Gateway", kind = "fact", fact = "net_gateway" },
  item{ category = "network", group = "Connection",
        label = "Addresses by hand", note = "In the Network window",
        kind = "open", program = "network" },

  ----------------------------------------------------------------- keyboard
  item{ category = "keyboard", group = "The Super key",
        label = "Pressed alone", kind = "choice",
        file = settings.KEYBOARD, key = "super", default = "menu",
        choices = { { "menu", "Opens the menu" },
                    { "nothing", "Does nothing" } } },
  item{ category = "keyboard", group = "Shortcuts",
        label = "Every shortcut",
        note = "Each key the desktop keeps, and what it does",
        kind = "open", program = "shortcuts" },

  ------------------------------------------------------------------- mouse
  item{ category = "mouse", group = "Pointer", label = "Speed",
        note = "How far the arrow goes for a move of a mouse, a TrackPoint or a touchpad",
        kind = "pointer_speed", file = settings.MOUSE, key = "speed", default = 32 },
  item{ category = "mouse", group = "Double click", label = "Speed",
        note = "How soon the second click has to come to make one double click",
        kind = "double_click", file = settings.MOUSE, key = "double_click_ms",
        default = 1000 },
  item{ category = "mouse", group = "Double click", label = "Try it",
        note = "Double-click the folder; it opens only at the speed chosen",
        kind = "try_double_click" },

  ------------------------------------------------------------------ startup
  item{ category = "startup", group = "Open when the desktop starts",
        label = "", kind = "startup", file = settings.STARTUP, key = "items" },

  ---------------------------------------------------------------- date/time
  item{ category = "datetime", group = "Clock",
        label = "Now", kind = "fact", fact = "now" },
  item{ category = "datetime", group = "Clock",
        label = "Time zone",
        note = "The board keeps UTC; this is the offset from it",
        kind = "stepper", file = settings.CLOCK, key = "offset", default = 0,
        choices = nil },   -- filled at run time from `clock.OFFSETS`

  ------------------------------------------------------------------- system
  item{ category = "system", group = "Processors",
        label = "In use", note = "opt/kosmos/smp=N at boot narrows it",
        kind = "fact", fact = "processors" },

  item{ category = "system", group = "About", label = "Kosmos",
        kind = "fact", fact = "version" },
  item{ category = "system", group = "About", label = "Machine",
        kind = "fact", fact = "machine" },
  item{ category = "system", group = "About", label = "Memory",
        kind = "fact", fact = "memory" },
  item{ category = "system", group = "About",
        label = "Licences", note = "What is in this image and under what terms",
        kind = "open", program = "about" },
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
  read = read or kit_read

  -- A row that knows where it is kept - an application's notifications,
  -- which are a key inside a table (`settings.notifiers`).
  if it and it.get then return it.get(read) end

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
-- process happens to know.** Two places share `/Home/Preferences/appearance` and two
-- share `/Home/Preferences/tracker`, so a write that rebuilt the table would drop
-- whatever the other one had put there - which is the mistake `iconsize.lua`
-- documents having avoided for the same reason.
--
-- **The default is stored as nothing.** A setting nobody chose follows a
-- default that may change rather than freezing the one that was in force the
-- day somebody opened the window, which is `textsize.lua`'s rule and worth
-- keeping the same everywhere.
--
function settings.set(it, value, read, write)
  read = read or kit_read
  write = write or kit_write

  -- A row that knows how it is kept - a file type's choice, made for every
  -- spelling of the type (`filetypes.page`).
  if it and it.set then return it.set(value) end

  if not it or not it.file or not it.key then return false, "not stored" end

  local saved = read(it.file)

  if type(saved) ~= "table" then saved = {} end

  if value == it.default and not it.keep_default then value = nil end

  --
  -- **`clears`: keys this choice makes stale.** A look is a whole - its
  -- colours and its faces - so choosing one has to forget any faces an
  -- older panel wrote into the same file, or a restart brings them back
  -- over the look. The Appearance panel did this by writing a fresh table;
  -- a read-modify-write keeps everything, so the row says what it replaces.
  --
  local stale = false

  for _, k in ipairs(it.clears or {}) do
    if saved[k] ~= nil then saved[k] = nil stale = true end
  end

  if saved[it.key] == value and not stale then return true end

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

--
-- **The wallpapers there are to choose from**, as `{ value, name }` pairs
-- for the Wallpaper row: none, then the pictures in `/Home`, then the
-- photographs the image carries.
--
-- The value is what `/Home/Preferences/appearance` keeps and what the window manager
-- is sent - a path in `/Home`, or `wallpaper/<file>` in the image - and ""
-- for the look's own desk. The name is what a person reads.
--
-- A carried photograph is named after the photographer:
-- `alexander-slattery-LI748t0BK8w.jpg` is Alexander Slattery, the words
-- before Unsplash's eleven-character photo id, each capitalised unless it
-- has a digit in it, which is how a username like `v2osk` is written. This
-- lived in the Appearance panel until 0.10.148, which is where the list was
-- drawn; it is here because Preferences draws it now and a rule written
-- twice is two rules.
--
local UNSPLASH_ID = string.rep("[%w_%-]", 11)

function settings.photographer(name)
  local who = name:match("^wallpaper/(.+)%-" .. UNSPLASH_ID .. "%.jpg$")

  if not who then return nil end

  local words = {}

  for word in who:gmatch("[^%-]+") do
    words[#words + 1] = word:find("%d") and word
                        or (word:sub(1, 1):upper() .. word:sub(2))
  end

  return table.concat(words, " ")
end

function settings.wallpapers()
  local out, seen = { { "", "None" } }, {}

  for _, name in ipairs(fs.list("/Home") or {}) do
    local suffix = name:lower():match("%.([%a]+)$")

    if suffix == "png" or suffix == "jpg" or suffix == "jpeg" then
      out[#out + 1] = { "/Home/" .. name, name }
      seen[name] = true
    end
  end

  for _, name in ipairs(sys.asset() or {}) do
    local who = settings.photographer(name)

    if who and not seen[who] then
      out[#out + 1] = { name, who }
      seen[who] = true
    end
  end

  return out
end

--
-- **Each application that has said something**, as a group of switches for
-- Preferences' Notifications page: newest first, each named as the history
-- names it and noted with the last thing it said, on until it is turned off.
-- `entries` are the server's (`notify.all`), `who` and `key` notify.lua's
-- own; kept as `off[key] = true` in `/Home/Preferences/notifications`, which
-- is what `notifications` reads.
--
-- `read` and `write` are passed in so the host can hold this without a
-- filesystem, as `settings.set` is.
--
function settings.notifiers(entries, who, key, read, write)
  read = read or kit_read
  write = write or kit_write

  local items, seen = {}, {}

  local function file()
    local saved = read(settings.NOTIFICATIONS)

    return type(saved) == "table" and saved or {}
  end

  for i = #(entries or {}), 1, -1 do
    local e = entries[i]
    local k = key(e)

    if not seen[k] then
      seen[k] = true

      items[#items + 1] = {
        category = "notifications", group = "Applications",
        label = who(e), note = "Last: " .. tostring(e.title),
        kind = "switch", sender = k,
        get = function()
          local off = file().off
          return not (type(off) == "table" and off[k] == true)
        end,
        set = function(on)
          local saved = file()

          if type(saved.off) ~= "table" then saved.off = {} end

          saved.off[k] = (not on) and true or nil
          if next(saved.off) == nil then saved.off = nil end

          return write(settings.NOTIFICATIONS, saved)
        end,
      }
    end
  end

  return { name = "Applications", items = items }
end

return settings
