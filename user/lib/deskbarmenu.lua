-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Deskbar's menu, read off the disk.
--
-- A folder per section, each holding launchers, and a folder inside one of
-- those is a submenu. This turns that into the shape the menu draws, and it
-- is the whole of the rule: **a thing appears in the menu because a
-- launcher file exists.** Nothing here invents an item, so there is no path
-- by which a program with no launcher can appear.
--
-- **Two such trees, merged** (`roadmap.md` 6zd): `/Kosmos/Deskbar`, the menu
-- as it ships - laid out from each application's header by the store that
-- serves them, so it cannot go stale - and `/Home/Deskbar`, holding only
-- what the person made. A section in both is one submenu; an item in both
-- is the person's; and a note of theirs, `kind = "hidden"`, under an item's
-- name takes the shipped one out (`menu.merge`).
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
--
-- **What a row says, and the order rows come in**: an application's name
-- for a person when it declares one (`kosmos: name`, `title` here), else
-- the launcher's own name - and sorted by what is shown, ignoring case, so
-- About This Machine sits under A rather than where `machine` would.
--
function menu.shown(item)
  return tostring(item.title or item.name or "")
end

local function by_shown(a, b)
  local x, y = menu.shown(a):lower(), menu.shown(b):lower()

  if x ~= y then return x < y end

  return tostring(a.name) < tostring(b.name)
end

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
        title = attrs.title,
      }
    elseif attrs.kind == "hidden" then
      -- A person's note that a shipped item is not wanted: it takes that
      -- item out in `menu.merge`, and is never a row itself.
      launchers[#launchers + 1] = { name = name, path = full, hidden = true }
    end
  end

  table.sort(folders, by_shown)
  table.sort(launchers, by_shown)

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
-- more than an order they cannot see. **Diego took it back on 3 October
-- 2026** - "i want my order" - when the menu became five folders:
-- `merge_sections` puts the five in `SECTION_ORDER` first, and only a
-- person's own folders still follow their names.
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
-- **The two trees as one**: `shipped` and `home` are one folder's rows each,
-- as `menu.read` gives them, and what comes back is the rows the menu
-- shows.
--
-- By name, whatever its case, since a person who writes `quake` means
-- Quake: an item only in one is itself; a folder in both is one folder,
-- merged the same way; anything else in both is the person's - changed by
-- them, so theirs wins; and a person's hidden note takes the shipped item
-- of its name out and shows nothing. In `read`'s order: submenus first,
-- then launchers, each by name.
--
local function in_order(items)
  local folders, launchers = {}, {}

  for _, item in ipairs(items) do
    if item.folder then folders[#folders + 1] = item
    else launchers[#launchers + 1] = item end
  end

  table.sort(folders, by_shown)
  table.sort(launchers, by_shown)

  for _, one in ipairs(launchers) do folders[#folders + 1] = one end

  return folders
end

function menu.merge(shipped, home)
  local mine, taken, out = {}, {}, {}

  for _, item in ipairs(home or {}) do mine[item.name:lower()] = item end

  for _, item in ipairs(shipped or {}) do
    local theirs = mine[item.name:lower()]

    if not theirs then
      out[#out + 1] = item
    else
      taken[theirs] = true

      if theirs.hidden then
        -- Taken out by the person, and the note is not a row.
      elseif theirs.folder and item.folder then
        out[#out + 1] = { name = theirs.name, path = theirs.path,
                          folder = true,
                          items = menu.merge(item.items, theirs.items) }
      else
        out[#out + 1] = theirs
      end
    end
  end

  for _, item in ipairs(home or {}) do
    if not taken[item] and not item.hidden then
      if item.folder then
        -- A folder only the person has can still hold notes, which are
        -- never rows.
        out[#out + 1] = { name = item.name, path = item.path, folder = true,
                          items = menu.merge(nil, item.items) }
      else
        out[#out + 1] = item
      end
    end
  end

  return in_order(out)
end

--
-- **The applications installed in `/Home/Apps`** (`docs/elf.md` step 5), as
-- sections to merge with the menu that ships: each filed by its header's
-- `section` - Doom under Demos - as `binfs` files the shipped ones, the
-- first letter a capital and a group after a slash a submenu, and named by
-- its folder. "The Deskbar lists the folders in /Home/Apps, so there is
-- nothing to unregister": deleting the folder takes it out of the menu.
--
-- `apps` is `filetypes.installed`'s list; `declared` is
-- `filetypes.declared`, so this reads nothing itself.
--
function menu.installed(apps, declared)
  local by, order = {}, {}

  for _, app in ipairs(apps or {}) do
    local said = declared(app.source, "section") or "applications"

    if said:lower() ~= "none" and said ~= "" then
      local where, group = said:match("^([^/]+)/(.+)$")

      where = where or said

      local name = where:sub(1, 1):upper() .. where:sub(2)
      local section = by[name]

      if not section then
        section = { name = name, items = {} }
        by[name] = section
        order[#order + 1] = section
      end

      local item = { name = app.folder, program = app.program, args = "",
                     icon = declared(app.source, "icon"),
                     title = declared(app.source, "name") }
      local into = section.items

      if group then
        local sub = nil

        for _, it in ipairs(section.items) do
          if it.folder and it.name == group then sub = it end
        end

        if not sub then
          sub = { name = group, folder = true, items = {} }
          section.items[#section.items + 1] = sub
        end

        into = sub.items
      end

      into[#into + 1] = item
    end
  end

  return order
end

-- The same for the sections, which are the two roots' folders.
--
-- **The order the sections come in: Diego's** (3 October 2026, "i want my
-- order"): Applications, System, Development, Demos, Preferences, and any
-- folder a person made after them, A to Z. `menu.sections` says why the
-- order was a folder's name until then, and what changed.
--
menu.SECTION_ORDER = { "Applications", "System", "Development", "Demos", "Preferences" }

local section_rank = {}

for i, name in ipairs(menu.SECTION_ORDER) do section_rank[name:lower()] = i end

function menu.merge_sections(shipped, home)
  local function as_folders(sections)
    local out = {}

    for i, s in ipairs(sections or {}) do
      out[i] = { name = s.name, path = s.path, folder = true, items = s.items }
    end

    return out
  end

  local out = {}

  for _, f in ipairs(menu.merge(as_folders(shipped), as_folders(home))) do
    out[#out + 1] = { name = f.name, path = f.path, items = f.items }
  end

  table.sort(out, function(a, b)
    local x = section_rank[a.name:lower()] or #menu.SECTION_ORDER + 1
    local y = section_rank[b.name:lower()] or #menu.SECTION_ORDER + 1

    if x ~= y then return x < y end

    return a.name:lower() < b.name:lower()
  end)

  return out
end

--
-- The short name of the application a launcher starts - `/Kosmos/Apps/
-- doom.lua`, `/bin/doom.lua` from before 27 September, and `doom`, the
-- three ways a launcher has recorded one - or nil for anything else.
--
local function short_of(program)
  local short = program:match("^/[Kk][Oo][Ss][Mm][Oo][Ss]/[Aa][Pp][Pp][Ss]/([^/]+)%.lua$")
                or program:match("^/[Bb][Ii][Nn]/([^/]+)%.lua$")

  if not short and program ~= "" and not program:find("/", 1, true) then
    short = (program:gsub("%.lua$", ""))
  end

  return short
end

--
-- **What the seed left in a person's menu, to go to the Trash once**
-- (`roadmap.md` 6zd). Until 28 September the Deskbar copied a launcher for
-- every application into `/Home/Deskbar`, recording which in `.seeded`;
-- the shipped menu makes them copies of what is already there, and a menu
-- with everything twice. Diego: "Send to the trash all seeded".
--
-- `seeded` is that record, a set of short names. A launcher starting one
-- of them is the seed's, changed or not - one a person changed can be
-- dragged back out of the Trash. A folder holding nothing but the seed's
-- goes whole, so the Trash keeps the menu's shape; a folder with anything
-- of the person's in it stays, and only the seed's go from inside it.
-- The root itself never goes: it is where the person's own menu lives.
--
local function sweep(store, path, seeded, depth)
  local all, any, moves = true, false, {}

  for _, name in ipairs(store.list(path) or {}) do
    local full = path .. "/" .. name
    local attrs = store.getattr(full) or {}

    if attrs.kind == "launcher"
       and seeded[short_of(tostring(attrs.program or "")) or ""] then
      any = true
      moves[#moves + 1] = full
    elseif attrs.kind == "directory" and depth > 0 then
      local whole, some, inside = sweep(store, full, seeded, depth - 1)

      if whole then
        any = true
        moves[#moves + 1] = full
      else
        all = false
        any = any or some

        for _, p in ipairs(inside) do moves[#moves + 1] = p end
      end
    else
      all = false
    end
  end

  return all and any, any, moves
end

function menu.seed_leftovers(store, root, seeded)
  local _, _, moves = sweep(store, root, seeded or {}, 12)

  return moves
end

return menu
