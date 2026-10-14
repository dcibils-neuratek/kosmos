-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The documents an application opened last, newest first - and where each
-- was left.
--
--   local recentdocs = use("/Kosmos/Libraries/recentdocs.lua")
--   local store = prefs.open("pdfview")
--   local list = recentdocs.parse(store.recent)
--
--   list = recentdocs.add(list, "/Home/odyssey.pdf", { page = 12 })
--   store:set("recent", recentdocs.format(list))
--   recentdocs.find(list, "/Home/odyssey.pdf").page     -- 12
--   recentdocs.items(list, open)                        -- File, Open recent
--
-- **One door for every application's recent documents** (`CLAUDE.md`, kits
-- supply): the Text Editor kept a list of its own, and the PDF Viewer was
-- about to write a second (`docs/pdfview.md` step 5). An entry is the path
-- and whatever the application wants to find again when it opens the
-- document - the PDF Viewer's page and zoom - kept in its settings by the
-- settings kit (`prefs.lua`), as text a person can read.
--
-- **Where a document was left lives in its entry**, so it is bounded by the
-- list: a document that falls off the end is forgotten whole, and no file
-- grows with every document ever opened.
--
-- Pure: no `sys`, no `fs`, so `tools/test_recentdocs.lua` holds it on the Mac.

local recentdocs = {}

recentdocs.MOST = 8

-- What the settings kit read, as entries `{ path = ..., ... }`, newest
-- first and each path once. A plain string is a path - the Text Editor's
-- list before 14 October 2026, kept rather than lost. Anything else, or a
-- path that is not one, is passed over.
function recentdocs.parse(t, most)
  local out, seen = {}, {}

  most = most or recentdocs.MOST

  for _, e in ipairs(type(t) == "table" and t or {}) do
    local entry = type(e) == "string" and { path = e }
                  or (type(e) == "table" and e or nil)
    local path = entry and entry.path

    if type(path) == "string" and path:match("^/[^%c]*$") and not seen[path]
       and #out < most then
      seen[path] = true

      local copy = {}
      for k, v in pairs(entry) do copy[k] = v end
      out[#out + 1] = copy
    end
  end

  return out
end

-- The entries as the settings kit keeps them: the same tables.
function recentdocs.format(list)
  return recentdocs.parse(list, math.huge)
end

-- The entry for `path`, or nil.
function recentdocs.find(list, path)
  for _, e in ipairs(list or {}) do
    if e.path == path then return e end
  end
end

-- The list with `path` at the top: once, so opening it again moves it up,
-- carrying what its entry held with `fields` over it, and no more than
-- `most`. A new list; the one given is not changed.
function recentdocs.add(list, path, fields, most)
  most = most or recentdocs.MOST

  local entry = { path = path }
  local was = recentdocs.find(list, path)

  for k, v in pairs(was or {}) do entry[k] = v end
  for k, v in pairs(fields or {}) do entry[k] = v end
  entry.path = path

  local out = { entry }

  for _, e in ipairs(list or {}) do
    if #out >= most then break end
    if e.path ~= path then out[#out + 1] = e end
  end

  return out
end

-- A File menu's Open recent: each document by its name, its folder beside
-- it, `open(path)` when chosen - or one item saying there is nothing yet.
function recentdocs.items(list, open)
  local items = {}

  for _, e in ipairs(list or {}) do
    local name = e.path:match("([^/]+)$") or e.path
    local folder = e.path:match("^(.*)/[^/]*$")

    items[#items + 1] = { text = name, hint = (folder == "" and "/" or folder),
                          on_choose = function() open(e.path) end }
  end

  if #items == 0 then items[1] = { text = "Nothing yet", disabled = true } end

  return items
end

return recentdocs
