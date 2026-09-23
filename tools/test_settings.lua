--  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The settings list, on this machine.
--
-- `user/lib/settings.lua` is what Preferences draws itself from: every
-- setting the system has, which file it lives in and what kind of thing it
-- is. It is a table and two functions over it, so all of it can be checked
-- here rather than by opening a window and looking.
--
-- **The read-modify-write is the part worth testing hardest.** Two places
-- share `/home/.appearance` and two share `/home/.tracker`, so a write that
-- rebuilt the file from what one process happened to know would silently
-- drop the other's keys. That is not a crash; it is a setting that comes
-- back wrong an hour later, which is the kind of bug a window test would
-- never see.
--

package.path = "user/lib/?.lua;" .. package.path

-- `settings.lua` names `fs.read` as its default, and there is no namespace
-- here. Every call below passes its own reader and writer, so the global is
-- only needed for the file to load.
fs = { read = function() return nil end, write = function() return true end }

local settings = dofile("user/lib/settings.lua")

local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

--
-- Every item names a category that exists, and every category has items.
--
local known = {}

for _, c in ipairs(settings.CATEGORIES) do known[c.id] = true end

for _, it in ipairs(settings.ITEMS) do
  check(known[it.category],
        ("%s/%s names category %q, which is not in CATEGORIES")
        :format(it.group, it.label, tostring(it.category)))
  check(type(it.group) == "string" and it.group ~= "",
        ("an item in %s has no group"):format(it.category))
  check(type(it.kind) == "string",
        ("%s/%s has no kind"):format(it.group, tostring(it.label)))
end

for _, c in ipairs(settings.CATEGORIES) do
  check(#settings.groups(c.id) > 0,
        ("category %q has no settings in it, so the sidebar would show an "
         .. "empty page"):format(c.id))
end

--
-- A choice's default is one of its choices. A default that is not on the
-- list is a control that opens showing a value it cannot go back to.
--
for _, it in ipairs(settings.ITEMS) do
  if it.kind == "choice" and it.choices then
    local found = false

    for _, ch in ipairs(it.choices) do
      if ch[1] == it.default then found = true end
    end

    check(found, ("%s/%s defaults to %s, which is not one of its choices")
                 :format(it.group, it.label, tostring(it.default)))
  end
end

--
-- Anything stored names both a file and a key.
--
for _, it in ipairs(settings.ITEMS) do
  if it.file or it.key then
    check(it.file and it.key,
          ("%s/%s names a %s and not the other")
          :format(it.group, it.label, it.file and "file" or "key"))
  end
end

--
-- `groups` keeps the order of ITEMS, and divides by group.
--
local g = settings.groups("appearance")

check(g[1] and g[1].name == "Look",
      "the first group of Appearance is not Look, so groups() lost the order")
check(#g >= 3, "Appearance should have Look, Size and Icons at least")

--
-- get() falls back to the default, and reads what is there.
--
local look = settings.ITEMS[1]

check(settings.get(look, function() return nil end) == "plex",
      "a setting with no file on disk did not come back as its default")

check(settings.get(look, function() return { palette = "studio" } end)
      == "studio", "a setting on disk did not come back")

check(settings.get(look, function() return "not a table" end) == "plex",
      "a settings file holding something that is not a table was not "
      .. "ignored - a damaged file should read as the default")

--
-- **set() keeps what it did not touch.** The whole reason for read, change,
-- write.
--
local wrote
local file = { palette = "classic", wallpaper = "nebula" }

settings.set(look, "studio",
             function() return file end,
             function(_, t) wrote = t; return true end)

check(wrote and wrote.palette == "studio", "set() did not change the key")
check(wrote and wrote.wallpaper == "nebula",
      "set() dropped `wallpaper`, which another place in the system wrote. "
      .. "Read, change, write - never compose the file from what this "
      .. "process knows")

--
-- The default is stored as nothing, so a place that never chose follows a
-- default that changes.
--
wrote = nil
settings.set(look, "plex",
             function() return { palette = "studio" } end,
             function(_, t) wrote = t; return true end)

check(wrote and wrote.palette == nil,
      "setting a value back to the default stored it, so this place would "
      .. "freeze today's default instead of following tomorrow's")

--
-- Two settings may share a file and a key on purpose - Scale is on two
-- pages - and both must be the same item's worth of information.
--
local scales = {}

for _, it in ipairs(settings.ITEMS) do
  if it.file == settings.APPEARANCE and it.key == "scale" then
    scales[#scales + 1] = it
  end
end

check(#scales == 2, "Scale should appear on both Appearance and Displays")

if #scales == 2 then
  check(scales[1].default == scales[2].default
        and #scales[1].choices == #scales[2].choices,
        "the two Scale rows disagree about their choices or default, so the "
        .. "same setting would look different depending on where it is "
        .. "opened")
end

--
-- **The Theme setting offers every look that ships, and no other.**
--
-- Two lists of the same thing in two files, which is a list that drifts -
-- and it did, the day it was written: Endeavour went into `themes.lua` and
-- not into `settings.lua`, so Preferences showed four looks by name and the
-- fifth as the raw word `endeavour`. Nothing was broken; it just looked
-- like somebody had not finished.
--
local themes = dofile("user/lib/themes.lua")
local theme_item

for _, it in ipairs(settings.ITEMS) do
  if it.label == "Theme" then theme_item = it end
end

check(theme_item ~= nil, "there is no Theme setting at all")

if theme_item then
  local offered = {}

  for _, c in ipairs(theme_item.choices or {}) do offered[c[1]] = c[2] end

  for _, name in ipairs(themes.order) do
    check(offered[name] ~= nil,
          ("the look %q ships and Preferences does not offer it, so it "
           .. "would show as its own raw name"):format(name))
  end

  check(#(theme_item.choices or {}) == #themes.order,
        ("Preferences offers %d looks and %d ship")
        :format(#(theme_item.choices or {}), #themes.order))
end

--
-- name_of turns a value into what the control shows.
--
check(settings.name_of(look, "plexnight") == "Plex Night",
      "name_of did not find a value's name")
check(settings.name_of(look, "nonsense") == "nonsense",
      "name_of should fall back to the value itself rather than nothing")

if #fails > 0 then
  print(("FAIL: %d of %d checks on the settings list:"):format(#fails, checks))
  for _, f in ipairs(fails) do print("  " .. f) end
  os.exit(1)
end

print(("PASS: %d checks on the settings list, on this machine (every item "
       .. "in a category that exists, every choice's default on its own "
       .. "list, and a write that keeps what another program put in the "
       .. "file)."):format(checks))
