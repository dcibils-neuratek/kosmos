-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The browser's history, on the Mac (`user/lib/history.lua`, `roadmap.md`
-- 6zz d4): a day's name, a page recorded at the top of its day and only
-- once in it, its folder made, the days newest first, what was open lately
-- across them each page once, searched without regard to case in titles and
-- addresses, a hand-edited file read for what it still says, and the days
-- before the oldest kept let go - or all of them.

local history = assert(loadfile("user/lib/history.lua"))()

local failures, checks = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failures = failures + 1
    print(("not ok %d - %s"):format(checks, what))
  end
end

--------------------------------------------------------------------------
-- An fs in memory, which copies a table in and out as the serialiser does:
-- what is read is never the table that was written.
--------------------------------------------------------------------------

local files, dirs = {}, { ["/Home"] = true }

local function copy(v)
  if type(v) ~= "table" then return v end

  local out = {}

  for k, x in pairs(v) do out[k] = copy(x) end

  return out
end

fs = {}

function fs.getattr(path)
  if dirs[path] then return { kind = "directory" } end
  if files[path] ~= nil then return { kind = "file" } end
  return nil
end

function fs.read(path) return copy(files[path]) end

function fs.write(path, data)
  files[path] = copy(data)
  return true
end

function fs.list(dir)
  local out = {}
  local pat = "^" .. dir:gsub("%p", "%%%0") .. "/([^/]+)$"

  for path in pairs(files) do
    local name = path:match(pat)

    if name then out[#out + 1] = name end
  end

  table.sort(out)
  return out
end

function fs.send(path, msg)
  if msg.type == "mkdir" then dirs[path] = true
  elseif msg.type == "delete" then files[path] = nil end

  return true
end

local DIR = history.DIR

local function at(y, m, d, h, mi)
  return { year = y, month = m, day = d, hour = h, min = mi,
           epoch = 1790812800 + ((d - 1) * 24 + h) * 3600 + mi * 60 }
end

--------------------------------------------------------------------------

check(history.day_of({ year = 2026, month = 10, day = 1 }) == "2026-10-01",
      "a day is named by its date")

check(history.record("https://en.wikipedia.org/wiki/Dam", "Dam - Wikipedia",
                     at(2026, 9, 29, 21, 47))
      and dirs["/Home/Preferences"] and dirs["/Home/Preferences/browser"]
      and dirs[DIR], "recorded, its folder and the ones above it made")

history.record("https://www.lua.org/manual/5.4/", "Lua 5.4\nReference Manual",
               at(2026, 9, 30, 22, 10))
history.record("https://news.ycombinator.com/", "Hacker News", at(2026, 10, 1, 12, 40))
history.record("https://en.wikipedia.org/wiki/Microkernel", "Microkernel - Wikipedia",
               at(2026, 10, 1, 12, 58))
history.record("https://news.ycombinator.com/", "Hacker News", at(2026, 10, 1, 13, 21))

do
  local today = history.day("2026-10-01")

  check(#today == 2 and today[1].address == "https://news.ycombinator.com/"
        and today[1].time == "13:21" and today[2].time == "12:58",
        "a page shown again moves to the top of its day, once, with its new time")
  check(history.day("2026-09-30")[1].title == "Lua 5.4 Reference Manual",
        "a title kept on one line")
end

do
  local days = history.days()

  check(table.concat(days, ",") == "2026-10-01,2026-09-30,2026-09-29",
        "the days newest first: " .. table.concat(days, ","))
end

do
  history.record("https://en.wikipedia.org/wiki/Dam", "Dam - Wikipedia",
                 at(2026, 10, 1, 14, 2))

  local l = history.lately(3)

  check(#l == 3 and l[1].address == "https://en.wikipedia.org/wiki/Dam"
        and l[1].day == "2026-10-01" and l[2].address == "https://news.ycombinator.com/"
        and l[3].address == "https://en.wikipedia.org/wiki/Microkernel",
        "lately: the newest pages, each once, across days")

  local all = history.lately(10)

  check(#all == 4 and all[4].address == "https://www.lua.org/manual/5.4/",
        "and the older days after, a page seen today not again from yesterday: " .. #all)
end

do
  local found = history.search("WIKIPEDIA")

  check(#found == 2 and found[1].day == "2026-10-01" and #found[1].pages == 2
        and found[2].day == "2026-09-29" and #found[2].pages == 1,
        "searched without regard to case, by day")

  local by_address = history.search("ycombinator")

  check(#by_address == 1 and by_address[1].pages[1].title == "Hacker News",
        "an address searched as well as a title")
  check(#history.search("") == 3, "nothing typed is every day")
  check(#history.search("nothing like it") == 0, "and a word on no page is none")
end

do
  -- A file edited by hand: a line with no address, and one that is not a table.
  files[DIR .. "/2026-09-28"] = { { title = "no address" }, "a string",
                                  { address = "http://kept/", title = 7 } }

  local d = history.day("2026-09-28")

  check(#d == 1 and d[1].address == "http://kept/" and d[1].title == "7",
        "a hand-edited day read for what it still says")
  check(#history.day("2026-01-01") == 0, "and a day with no file as nothing")
end

do
  local gone = history.prune("2026-09-30")

  check(gone == 2 and table.concat(history.days(), ",") == "2026-10-01,2026-09-30",
        "the days before the oldest kept let go: " .. gone)
  check(history.clear() == 2 and #history.days() == 0, "and Clear, all of them")
end

if failures == 0 then
  print(("PASS: %d checks on the browser's history, on this machine."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the browser's history."):format(failures, checks))
os.exit(1)
