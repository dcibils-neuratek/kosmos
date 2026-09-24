-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Deskbar's menu, read off the disk.
--
-- `/home/Deskbar` holds a folder per section, each holding launchers, and a
-- folder inside one of those is a submenu. This turns that into the shape
-- the menu draws, and it is the whole of the rule: **a thing appears in the
-- menu because a launcher file exists.** Nothing here invents an item, so
-- there is no path by which a program with no launcher can appear.
--
-- Here rather than in `deskbar.lua` for `iconlayout.lua`'s reason, which is
-- the same reason: it is the part with the decisions in it - what counts,
-- what order things come in, how deep a folder may go - and a decision is
-- worth testing on the build machine, where a test costs no boot. The
-- Deskbar is then the part that draws, which is the part a test could not
-- have checked anyway.
--
-- `store` is anything answering `list(path)` and `getattr(path)`. In the
-- Deskbar that is `fs`; in `tools/test_deskbarmenu.lua` it is a table. The
-- indirection exists for the test and is one line in the caller.

local menu = {}

--
-- One folder's rows: its subfolders, then its launchers, each sorted.
--
-- **Submenus first**, which is what the Deskbar did when sections lived in
-- program headers and the reason has not changed: a submenu is a heavier
-- thing to open than an item is to click, so the few that need one should
-- not be hunted for among the many that do not.
--
-- **Only `kind == "launcher"` counts.** Anything else somebody keeps in
-- these folders - a note, a picture, a folder of their own - is their
-- business, and a menu that listed it would be guessing at what they meant.
--
-- `depth` is a guard rather than a feature. A directory tree can contain a
-- cycle the moment anything can link, and a menu that recurses for ever is
-- a desktop that does not come up; twelve is far past any menu worth
-- having.
--
--
-- `exists`, when given, answers whether a launcher's program is still
-- there: a launcher to a program `/bin` no longer has is not shown. It was
-- - Appearance's, on every machine seeded before it folded into
-- Preferences (`roadmap.md` 5zp) - and it opened nothing. The file stays;
-- it is the person's, and a program can come back.
--
function menu.read(store, path, depth, exists)
  depth = depth or 12

  local folders, launchers = {}, {}

  for _, name in ipairs(store.list(path) or {}) do
    local full = path .. "/" .. name
    local attrs = store.getattr(full) or {}

    if attrs.kind == "directory" then
      if depth > 0 then folders[#folders + 1] = { name = name, path = full } end
    elseif attrs.kind == "launcher"
           and (not exists or exists(tostring(attrs.program or ""))) then
      launchers[#launchers + 1] = {
        name = name,
        path = full,
        program = tostring(attrs.program or ""),
        args = tostring(attrs.args or ""),
        icon = attrs.icon,
      }
    end
  end

  local function by_name(a, b) return a.name < b.name end

  table.sort(folders, by_name)
  table.sort(launchers, by_name)

  local items = {}

  for _, folder in ipairs(folders) do
    items[#items + 1] = {
      name = folder.name,
      path = folder.path,
      folder = true,
      items = menu.read(store, folder.path, depth - 1, exists),
    }
  end

  for _, one in ipairs(launchers) do items[#items + 1] = one end

  return items
end

--
-- The sections: every folder directly under the root, in the only order a
-- directory has, which is its own.
--
-- The four used to be named in `deskbar.lua` in a chosen order -
-- applications, system, preferences, demos. A folder cannot express that,
-- and the trade was taken deliberately: a name somebody can change is worth
-- more than an order they cannot see. Renaming a folder reorders the menu,
-- which is a thing a person can discover; editing a list in a source file
-- is not.
--
-- A file directly under the root is not a section. A launcher belongs in
-- one, and one loose in the root has nowhere to appear - so it does not,
-- rather than appearing in a section it was never put in.
--
function menu.sections(store, root, exists)
  local names = {}

  for _, name in ipairs(store.list(root) or {}) do
    local attrs = store.getattr(root .. "/" .. name) or {}

    if attrs.kind == "directory" then names[#names + 1] = name end
  end

  table.sort(names)

  local out = {}

  for _, name in ipairs(names) do
    out[#out + 1] = {
      name = name,
      path = root .. "/" .. name,
      items = menu.read(store, root .. "/" .. name, nil, exists),
    }
  end

  return out
end

--
-- **Which applications a launcher names**, anywhere under `root`: the short
-- name of each, `/bin/doom.lua` and `doom` alike - the two ways a launcher
-- has recorded one.
--
function menu.programs_in(store, root, depth)
  depth = depth or 12

  local found = {}

  for _, name in ipairs(store.list(root) or {}) do
    local full = root .. "/" .. name
    local attrs = store.getattr(full) or {}

    if attrs.kind == "directory" and depth > 0 then
      for short in pairs(menu.programs_in(store, full, depth - 1)) do
        found[short] = true
      end
    elseif attrs.kind == "launcher" then
      local program = tostring(attrs.program or "")
      local short = program:match("^/bin/([^/]+)%.lua$")

      if not short and not program:find("/", 1, true) then
        short = (program:gsub("%.lua$", ""))
      end

      if short and short ~= "" then found[short] = true end
    end
  end

  return found
end

--
-- **What to add to the menu**: the applications `/bin` declares that the
-- menu has never been given.
--
-- The menu was made once, the first time `/home/Deskbar` did not exist,
-- and never again - so every application that arrived after that had no
-- launcher, and on a `/home` older than Preferences there was no
-- Preferences in the menu at all (Diego, 24 September: "where is the
-- preferences app in the menu? please add it").
--
-- `seeded` is the record of what the menu has been given, kept beside it;
-- an application in it whose launcher is gone was taken out by a person,
-- and stays out. Without a record - a menu made before there was one - the
-- launchers themselves are the record: `present`, what they name.
--
function menu.missing(launchable, seeded, present)
  local out = {}
  local known = seeded or present or {}

  for _, short in ipairs(launchable) do
    if not known[short] then out[#out + 1] = short end
  end

  return out
end

return menu
