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
function menu.read(store, path, depth)
  depth = depth or 12

  local folders, launchers = {}, {}

  for _, name in ipairs(store.list(path) or {}) do
    local full = path .. "/" .. name
    local attrs = store.getattr(full) or {}

    if attrs.kind == "directory" then
      if depth > 0 then folders[#folders + 1] = { name = name, path = full } end
    elseif attrs.kind == "launcher" then
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
      items = menu.read(store, folder.path, depth - 1),
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
function menu.sections(store, root)
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
      items = menu.read(store, root .. "/" .. name),
    }
  end

  return out
end

return menu
