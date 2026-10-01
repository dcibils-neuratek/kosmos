-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The browser's cache, on the Mac (`user/lib/httpcache.lua`, `roadmap.md`
-- 6zz k): HTTP's dates in all three of their forms, what a reply says of
-- keeping it - `max-age`, `Age`, `Expires` against `Date`, a tenth of the
-- time since `Last-Modified`, `no-store`, `no-cache`, `Vary: *` - a 304's
-- head over a kept one, and the store over an `fs` kept in memory here:
-- kept and found, found again by a cache that starts with nothing held,
-- refused over a certificate that did not check out, the least lately used
-- let go first, and emptied.

local cache = assert(loadfile("user/lib/httpcache.lua"))()

local failures, checks = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failures = failures + 1
    print(("not ok %d - %s"):format(checks, what))
  end
end

--------------------------------------------------------------------------
-- Dates.
--------------------------------------------------------------------------

check(cache.date("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777,
      "an IMF-fixdate, which servers send")
check(cache.date("Sunday, 06-Nov-94 08:49:37 GMT") == 784111777,
      "RFC 850's, two-digit year and all")
check(cache.date("Sun Nov  6 08:49:37 1994") == 784111777, "asctime's")
check(cache.date("Thu, 01 Oct 2026 12:00:00 GMT") == 1790856000, "this year")
check(cache.date("Tue, 29 Feb 2000 23:59:59 GMT") == 951868799,
      "a leap day")
check(cache.date("Thu, 01 Jan 1970 00:00:00 GMT") == 0, "the epoch itself")
check(cache.date("0") == nil and cache.date("tomorrow") == nil
      and cache.date(nil) == nil, "what is not a date is nil")

--------------------------------------------------------------------------
-- What a reply says of keeping it.
--------------------------------------------------------------------------

local NOW = 1790856000
local DATE = "Date: Thu, 01 Oct 2026 12:00:00 GMT"

local function head(...)
  return "HTTP/1.1 200 OK\r\n" .. table.concat({ ... }, "\r\n")
end

do
  local h = cache.headers(head("Content-Type: text/html", "cache-control: a",
                               "Cache-Control: b", "ETag: \"AbC\""))

  check(h["content-type"] == "text/html" and h["cache-control"] == "a, b"
        and h.etag == "\"AbC\"",
        "fields by name in lower case, repeats joined, values as they were")
end

do
  local p = cache.policy(200, head(DATE, "Cache-Control: max-age=600"), NOW)

  check(p and p.fresh_until == NOW + 600, "max-age is how long it is fresh")
end

do
  local p = cache.policy(200, head(DATE, "Cache-Control: public, max-age=600",
                                   "Age: 100"), NOW)

  check(p and p.fresh_until == NOW + 500,
        "Age is what it spent on its way, taken off")
end

do
  local p = cache.policy(200, head("Date: Thu, 01 Oct 2026 11:00:00 GMT",
                                   "Expires: Thu, 01 Oct 2026 12:00:00 GMT"),
                         NOW)

  check(p and p.fresh_until == NOW + 3600,
        "Expires against the reply's own Date, not this machine's clock")
end

do
  local p = cache.policy(200, head(DATE, "Expires: 0",
                                   "Last-Modified: Wed, 30 Sep 2026 12:00:00 GMT"),
                         NOW)

  check(p and p.fresh_until == NOW and p.last_modified,
        "an Expires that is no date means stale now, its validator kept")
end

do
  local ten = cache.policy(200, head(DATE,
                           "Last-Modified: Mon, 21 Sep 2026 12:00:00 GMT"), NOW)
  local two = cache.policy(200, head(DATE,
                           "Last-Modified: Tue, 29 Sep 2026 12:00:00 GMT"), NOW)

  check(ten and ten.fresh_until == NOW + 86400,
        "a tenth of ten days since it changed is a day, and a day is most")
  check(two and two.fresh_until == NOW + 17280,
        "a tenth of two days is 17280 seconds")
end

check(cache.policy(200, head(DATE, "Cache-Control: no-store, max-age=600"),
                   NOW) == nil, "no-store is never kept")

do
  local p = cache.policy(200, head(DATE, "Cache-Control: no-cache, max-age=600",
                                   "ETag: \"v1\""), NOW)

  check(p and p.fresh_until == NOW and p.etag == "\"v1\"",
        "no-cache is kept, and always asked about")
end

check(cache.policy(200, head(DATE, "Content-Type: text/html"), NOW) == nil,
      "nothing that says how long, and nothing to ask by: not kept")
check(cache.policy(200, head(DATE, "Cache-Control: max-age=60",
                             "Vary: Accept-Encoding"), NOW) ~= nil,
      "varying by the encoding asked for is kept - this always asks the same")
check(cache.policy(200, head(DATE, "Cache-Control: max-age=60",
                             "Vary: Accept-Encoding, Cookie"), NOW) ~= nil,
      "nor by a cookie, which this browser never sends - as Wikipedia does")
check(cache.policy(200, head(DATE, "Cache-Control: max-age=60", "Vary: *"),
                   NOW) == nil, "nor by everything")
check(cache.policy(404, head(DATE, "Cache-Control: max-age=60"), NOW) == nil,
      "only a 200")

do
  local p = cache.refreshed(head(DATE, "Cache-Control: max-age=60",
                                 "ETag: \"a\""),
                            "HTTP/1.1 304 Not Modified\r\n" .. DATE
                            .. "\r\nCache-Control: max-age=3600", NOW)

  check(p and p.fresh_until == NOW + 3600 and p.etag == "\"a\"",
        "a 304's head over the kept one: its own freshness, the kept ETag")
end

--------------------------------------------------------------------------
-- The store, over an fs in memory.
--------------------------------------------------------------------------

local files, dirs = {}, { ["/Home"] = true }

fs = {}

function fs.read(path)
  if path == "/Home/.super" then return { blocks = 4096, block_size = 4096 } end

  return files[path] and files[path].data or nil
end

function fs.write(path, data)
  files[path] = files[path] or { attrs = {} }
  files[path].data = data
  return true
end

function fs.getattr(path)
  if dirs[path] then return { kind = "directory", size = 0 } end

  local f = files[path]

  if not f then return nil end

  local out = { kind = "file", size = #f.data }

  for k, v in pairs(f.attrs) do out[k] = v end

  return out
end

function fs.setattr(path, t)
  local f = files[path]

  if not f then return nil end

  for k, v in pairs(t) do f.attrs[k] = v end

  return true
end

function fs.list(dir)
  local out = {}

  for path in pairs(files) do
    local name = path:match("^" .. dir:gsub("%p", "%%%0") .. "/([^/]+)$")

    if name then out[#out + 1] = name end
  end

  table.sort(out)
  return out
end

function fs.send(path, msg)
  if msg.type == "mkdir" then
    dirs[path] = true
  elseif msg.type == "delete" then
    files[path] = nil
  end

  return true
end

local DIR = "/Home/Cache/Browser"

local function reply(age, body, ...)
  return head(DATE, "Cache-Control: max-age=" .. age, ...) .. "\r\n\r\n" .. body
end

do
  local c = cache.open{ dir = DIR }
  local r = reply(600, "<p>kept</p>", "ETag: \"k1\"")

  check(c:store("http://example.com/a", r, { scheme = "http" }, NOW),
        "a 200 that may be kept is kept")
  check(dirs["/Home/Cache"] and dirs[DIR], "/Home/Cache/Browser made, its parent too")

  local hit = c:lookup("http://example.com/a", NOW + 10)

  check(hit and hit.fresh and hit.reply == r, "found, fresh, the reply as it came")

  local stale = c:lookup("http://example.com/a", NOW + 700)

  check(stale and not stale.fresh and stale.etag == "\"k1\"",
        "and stale once its time is past, with its ETag to ask by")

  local again = cache.open{ dir = DIR }:lookup("http://example.com/a", NOW + 10)

  check(again and again.fresh and again.reply == r,
        "a cache that starts with nothing held finds it on the disk")
  check(c:lookup("http://example.com/b", NOW) == nil, "what was never kept is not found")
end

do
  local c = cache.open{ dir = DIR }

  check(not c:store("https://example.com/x", reply(600, "x"),
                    { scheme = "https", secure = false }, NOW),
        "nothing taken over a certificate that did not check out")
  check(c:store("https://example.com/y", reply(600, "y"),
                { scheme = "https", secure = true }, NOW)
        and c:lookup("https://example.com/y", NOW).secure,
        "a secure one is kept, and remembered as secure")
end

do
  -- Two addresses whose file is one: the file says whose it is.
  local path = DIR .. "/" .. cache.name_of("http://example.com/a")

  files[path].attrs.address = "http://example.com/someone-else"
  check(cache.open{ dir = DIR }:lookup("http://example.com/a", NOW) == nil,
        "a file whose address is another's is not this one's")
  files[path].attrs.address = "http://example.com/a"
end

do
  local c = cache.open{ dir = DIR }
  local kept = c:lookup("http://example.com/a", NOW + 700)

  c:revalidated("http://example.com/a", kept,
                "HTTP/1.1 304 Not Modified\r\n" .. DATE
                .. "\r\nCache-Control: max-age=3600", NOW + 700)

  local now = cache.open{ dir = DIR }:lookup("http://example.com/a", NOW + 800)

  check(now and now.fresh and now.fresh_until == NOW + 700 + 3600,
        "a 304 makes it fresh again for as long as it says, on the disk too")
end

do
  for path in pairs(files) do files[path] = nil end

  local c = cache.open{ dir = DIR, most = 1000 }
  local body = ("x"):rep(300)

  c:store("http://example.com/1", reply(600, body), { scheme = "http" }, NOW)
  c:store("http://example.com/2", reply(600, body), { scheme = "http" }, NOW + 1)
  c:used("http://example.com/1", NOW + 2)
  c:store("http://example.com/3", reply(600, body), { scheme = "http" }, NOW + 3)

  local fresh = cache.open{ dir = DIR, most = 1000 }

  check(fresh:lookup("http://example.com/2", NOW + 4) == nil,
        "full, the least lately used goes first")
  check(fresh:lookup("http://example.com/1", NOW + 4)
        and fresh:lookup("http://example.com/3", NOW + 4),
        "and the one used since stays, beside the new one")
  check(not c:store("http://example.com/big", reply(600, ("y"):rep(2000)),
                    { scheme = "http" }, NOW + 5),
        "a reply larger than the whole cache is not kept")

  c:empty()
  check(#fs.list(DIR) == 0 and c:lookup("http://example.com/1", NOW) == nil,
        "emptied: nothing on the disk and nothing held")
end

if failures == 0 then
  print(("PASS: %d checks on the browser's cache, on this machine."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the browser's cache."):format(failures, checks))
os.exit(1)
