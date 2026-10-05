-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The browser's history, on the disk (`roadmap.md` 6zz d4, `docs/browser.html`).
--
-- **Where**: "in `/Home/Preferences/browser`", as the drawing says - a folder
-- there, the browser's own, with a file a day in `history/` named by its
-- date: `history/2026-10-01`. Every other settings file in Kosmos is one
-- table at `/Home/Preferences/<application>`, and history is not one: thirty
-- days of it, kept in one table, would be written whole every time a page is
-- shown. A day's file is the size of a day, the sidebar shows history by day
-- anyway, and letting a day go is deleting a file.
--
-- **A day's file is a Lua table**, written with `fs.write` and read back with
-- `fs.read` through the system's serialiser, as every settings file is: a
-- list of `{ address, title, time, epoch }`, newest first, each page once - a
-- page shown again that day moves to the top with its new time.
--
-- Pure: `fs` is the global every process has, and the host test hands it one
-- kept in memory (`tools/test_history.lua`). The time comes from whoever
-- calls - a table as `clock.lua` makes one - so nothing here reads a clock.

local history = {}

-- Its folder made, and the ones above it, by the file library.
local files = use("/Kosmos/Libraries/files.lua")

-- In the browser's folder, where the settings kit keeps it (`prefs.lua`).
history.DIR = use("/Kosmos/Libraries/prefs.lua").path("browser/history")

-- A day's name: its date, which sorts as the days do.
function history.day_of(t)
  return ("%04d-%02d-%02d"):format(t.year, t.month, t.day)
end

local function is_day(name)
  return name:match("^%d%d%d%d%-%d%d%-%d%d$") ~= nil
end

-- The days there are, newest first.
function history.days(dir)
  local out = {}

  for _, name in ipairs(fs.list(dir or history.DIR) or {}) do
    if is_day(name) then out[#out + 1] = name end
  end

  table.sort(out, function(a, b) return a > b end)
  return out
end

-- A day's pages, newest first; nothing for a day with none.
function history.day(name, dir)
  local got = fs.read((dir or history.DIR) .. "/" .. name)

  if type(got) ~= "table" then return {} end

  local out = {}

  -- Only what has the shape: a file somebody edited by hand is read for
  -- what it still says rather than trusted whole.
  for _, e in ipairs(got) do
    if type(e) == "table" and type(e.address) == "string" and e.address ~= "" then
      out[#out + 1] = { address = e.address, title = tostring(e.title or ""),
                        time = tostring(e.time or ""), epoch = tonumber(e.epoch) }
    end
  end

  return out
end

--
-- **A page shown**, at `now` - a time as `clock.at` gives one. At the top
-- of its day, and only once in it. A title is kept on one line.
--
function history.record(address, title, now, dir)
  dir = dir or history.DIR

  if type(address) ~= "string" or address == "" or type(now) ~= "table" then
    return false
  end

  if not files.make_folder(dir) then return false end

  local name = history.day_of(now)
  local list = history.day(name, dir)

  for i = #list, 1, -1 do
    if list[i].address == address then table.remove(list, i) end
  end

  table.insert(list, 1, {
    address = address,
    title = tostring(title or ""):gsub("%c", " "),
    time = ("%02d:%02d"):format(now.hour or 0, now.min or 0),
    epoch = now.epoch,
  })

  return fs.write(dir .. "/" .. name, list) and true or false
end

--
-- **The newest `n` pages**, each once, across the days: what a new tab
-- offers as what was open lately. Each says its day as well.
--
function history.lately(n, dir)
  local out, seen = {}, {}

  for _, day in ipairs(history.days(dir)) do
    for _, e in ipairs(history.day(day, dir)) do
      if not seen[e.address] then
        seen[e.address] = true
        e.day = day
        out[#out + 1] = e

        if #out >= n then return out end
      end
    end
  end

  return out
end

--
-- **Searched as typed** (`docs/browser.html`): the days with a page whose
-- title or address has `text` in it, without regard to case, each with
-- those pages - `{ day = , pages = { ... } }`, newest first. Nothing typed
-- is every day whole.
--
function history.search(text, dir)
  local want = tostring(text or ""):lower()
  local out = {}

  for _, day in ipairs(history.days(dir)) do
    local pages = {}

    for _, e in ipairs(history.day(day, dir)) do
      if want == "" or e.title:lower():find(want, 1, true)
         or e.address:lower():find(want, 1, true) then
        pages[#pages + 1] = e
      end
    end

    if #pages > 0 then out[#out + 1] = { day = day, pages = pages } end
  end

  return out
end

--
-- **The days before `oldest` let go** - thirty days, until Settings says
-- otherwise (d5). How many went.
--
function history.prune(oldest, dir)
  dir = dir or history.DIR

  local gone = 0

  for _, day in ipairs(history.days(dir)) do
    if day < oldest then
      fs.send(dir .. "/" .. day, { type = "delete" })
      gone = gone + 1
    end
  end

  return gone
end

-- Every day let go: Settings' Clear history (d5).
function history.clear(dir)
  return history.prune("9999-99-99", dir)
end

return history
