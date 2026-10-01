-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- **A menu longer than the screen** (`roadmap.md` 6zz j6), for the kit's
-- menus, which are as tall as their items: a browser's `<select>` of every
-- country there is would run off the bottom, and what is past the edge
-- cannot be chosen.
--
-- `longmenu.grouped(items, fit)` -> the items as they are when `fit` of
-- them fit, and otherwise grouped into submenus of `fit` each, in order,
-- each called by its first and last item - "Afghanistan - Burundi" - and
-- grouped again while there are more groups than fit. No list is too long
-- to choose from, the deepest is as shallow as it can be, and nothing of the
-- kit's menus changes: a submenu is what they already have.
--
-- Pure: an item is any table with `text`; what the groups add is `text`,
-- `submenu`, and `first` and `last` for the names of groups of groups.
--

local longmenu = {}

-- The words of a name, not the whole of a long option.
local function short(text)
  text = tostring(text or "")

  if #text <= 24 then return text end

  -- At a character's start, never inside one's UTF-8.
  local cut = 24

  while cut > 1 and (text:byte(cut + 1) or 0) & 0xC0 == 0x80 do cut = cut - 1 end

  return text:sub(1, cut) .. "..."
end

function longmenu.grouped(items, fit)
  fit = math.max(2, math.floor(fit or 2))

  if #items <= fit then return items end

  local groups = {}

  for at = 1, #items, fit do
    local part = {}

    for i = at, math.min(at + fit - 1, #items) do part[#part + 1] = items[i] end

    local first = part[1].first or part[1].text
    local last = part[#part].last or part[#part].text

    groups[#groups + 1] = { text = short(first) .. " - " .. short(last),
                            first = first, last = last, submenu = part }
  end

  return longmenu.grouped(groups, fit)
end

return longmenu
