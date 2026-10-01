-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The browser's cache: replies kept, and used again for as long as their
-- server said they may be (`roadmap.md` 6zz k).
--
--   local httpcache = use("/Kosmos/Libraries/httpcache.lua")
--   local cache = httpcache.open{ dir = "/Home/Cache/Browser" }
--   local reply, why, how = http.get(address, { cache = cache })
--
-- Diego, 30 September 2026: "we should add a browser cache feature as
-- well", "as all browser rely on this for performance reasons". Every
-- Reload fetched everything again, Back read the page from the network
-- once more, and a site's pictures were fetched anew on every page of it.
--
-- **What a reply says of keeping it** is HTTP's caching (RFC 9111), the
-- part a browser for one person needs:
--
--   - `Cache-Control: max-age`, or `Expires` against the reply's own `Date`,
--     says how long it may be used as it is; `Age` is what it had already
--     spent on its way. With neither, a tenth of how long ago it was last
--     changed, a day at most - what browsers do with `Last-Modified` alone.
--   - Once that runs out, it is asked for again with its validators -
--     `If-None-Match` its `ETag`, `If-Modified-Since` its `Last-Modified` -
--     and a `304 Not Modified`, a few hundred bytes, means the kept one is
--     still right, and how much longer it may be used.
--   - `no-store` is never kept; `no-cache` is kept and always asked about.
--   - `Vary` is ignored but for `*`. A reply that varies by a field of the
--     request is one of several, chosen by that field - and this browser
--     sends the same fields every time: the same agent and `Accept`, the
--     same encoding, no cookies, no credentials. So the next request would
--     choose the one kept. It said "anything but the encoding is not kept"
--     at first, and Wikipedia, which varies by `Cookie`, was never kept.
--   - Only a 200, only what came by GET, and nothing taken "anyway" over a
--     certificate that did not check out - kept, it would come back later
--     looking like any other.
--
-- **Where it is kept**: `/Home/Cache/Browser`, Diego's choice on 1 October -
-- a file a reply, named by its address's hash and holding the reply as
-- `http.get` returns it, head as it came and body as it reads. What the
-- cache knows of it - the address, until when it is fresh, its
-- validators, when it was last used - are the file's attributes, so
-- Tracker shows them and nothing else has to be kept in step. The last
-- few used are held in memory as well.
--
-- **Bounded**, by the disk it is on: an eighth of it, 256 MB at most, and
-- the least lately used go first when a new one would not fit.
--
-- `cache.date`, `cache.headers` and `cache.policy` touch nothing, so
-- `tools/test_httpcache.lua` runs them on the Mac; the store uses `fs`,
-- which the test gives it in memory.

local cache = {}

local DAY = 86400

--------------------------------------------------------------------------
-- HTTP's dates: "Sun, 06 Nov 1994 08:49:37 GMT", which servers send, and
-- the two older forms a reader must still take (RFC 9110 5.6.7).
--------------------------------------------------------------------------

local MONTHS = { jan = 1, feb = 2, mar = 3, apr = 4, may = 5, jun = 6,
                 jul = 7, aug = 8, sep = 9, oct = 10, nov = 11, dec = 12 }

-- Days from 1 January 1970 to a date: Howard Hinnant's `days_from_civil`.
local function days(y, m, d)
  y = m <= 2 and y - 1 or y

  local era = (y >= 0 and y or y - 399) // 400
  local yoe = y - era * 400
  local doy = (153 * (m + (m > 2 and -3 or 9)) + 2) // 5 + d - 1
  local doe = yoe * 365 + yoe // 4 - yoe // 100 + doy

  return era * 146097 + doe - 719468
end

-- Seconds since 1970 for an HTTP date, or nil for one that is not.
function cache.date(text)
  if type(text) ~= "string" then return nil end

  local d, mon, y, hh, mm, ss =
    text:match("^%s*%a+,%s*(%d%d?)%s+(%a%a%a)%s+(%d%d%d%d)%s+(%d%d):(%d%d):(%d%d)")

  if not d then
    d, mon, y, hh, mm, ss =
      text:match("^%s*%a+,%s*(%d%d)%-(%a%a%a)%-(%d%d)%s+(%d%d):(%d%d):(%d%d)")

    if y then
      y = tonumber(y)
      y = y < 70 and 2000 + y or 1900 + y
    end
  end

  if not d then
    mon, d, hh, mm, ss, y =
      text:match("^%s*%a+%s+(%a%a%a)%s+(%d%d?)%s+(%d%d):(%d%d):(%d%d)%s+(%d%d%d%d)")
  end

  local month = mon and MONTHS[mon:lower()]

  if not month then return nil end

  return days(tonumber(y), month, tonumber(d)) * DAY
         + tonumber(hh) * 3600 + tonumber(mm) * 60 + tonumber(ss)
end

--------------------------------------------------------------------------
-- What a reply says of keeping it.
--------------------------------------------------------------------------

-- A head's fields, by their names in lower case; one said twice is joined
-- with a comma, as HTTP says it may be. Values keep their case - an ETag
-- is compared exactly.
function cache.headers(head)
  local out = {}

  for name, value in tostring(head or ""):gmatch("\r?\n([^:\r\n]+):[ \t]*([^\r\n]*)") do
    name = name:lower()
    value = value:gsub("[ \t]+$", "")
    out[name] = out[name] and (out[name] .. ", " .. value) or value
  end

  return out
end

-- `Cache-Control`'s directives: { ["max-age"] = "600", ["no-store"] = true }.
local function directives(text)
  local out = {}

  for part in tostring(text or ""):gmatch("[^,]+") do
    local name, value = part:match("^%s*([%w%-]+)%s*=%s*\"?([^\",]*)\"?%s*$")

    if name then
      out[name:lower()] = value
    else
      name = part:match("^%s*([%w%-]+)%s*$")

      if name then out[name:lower()] = true end
    end
  end

  return out
end

--
-- From a reply's fields, received at `now`: how long it may be used and
-- what to ask with after - `{ fresh_until, etag, last_modified }` - or nil
-- and why it is not to be kept.
--
local function policy_of(h, now)
  local cc = directives(h["cache-control"])

  if cc["no-store"] then return nil, "the server said not to keep it" end

  for token in tostring(h["vary"] or ""):gmatch("[^,%s]+") do
    if token == "*" then
      return nil, "it varies by everything, so no request is answered by it"
    end
  end

  local date = cache.date(h["date"]) or now
  local age = math.max(0, tonumber(h["age"]) or 0)
  local lifetime = 0

  if cc["max-age"] then
    lifetime = math.max(0, math.tointeger(tonumber(cc["max-age"]) or 0) or 0)
  elseif h["expires"] then
    local expires = cache.date(h["expires"])

    lifetime = expires and math.max(0, expires - date) or 0
  elseif h["last-modified"] then
    local changed = cache.date(h["last-modified"])

    lifetime = changed and math.min(DAY, math.max(0, (date - changed) // 10)) or 0
  end

  if cc["no-cache"] then lifetime = 0 end

  local etag, changed = h["etag"], h["last-modified"]

  if lifetime <= age and not etag and not changed then
    return nil, "it may not be used again and has nothing to ask about it by"
  end

  return { fresh_until = now + lifetime - age, etag = etag,
           last_modified = changed }
end

-- A reply's status and head, received at `now`: its policy, or nil and why.
function cache.policy(status, head, now)
  if status ~= 200 then return nil, ("it is a %d, not a 200"):format(status) end

  return policy_of(cache.headers(head), now)
end

--
-- A `304`'s head, over the head of the reply it says is still right: the
-- 304's fields win where it has them, which is how a server says how much
-- longer the kept one may be used.
--
function cache.refreshed(kept_head, head304, now)
  local h = cache.headers(kept_head)

  for name, value in pairs(cache.headers(head304)) do h[name] = value end

  return policy_of(h, now)
end

--------------------------------------------------------------------------
-- The store.
--------------------------------------------------------------------------

-- A file's name for an address: FNV-1a, 64 bits, as sixteen hex digits.
-- Two addresses could meet in one; each file says whose it is.
local function name_of(url)
  local h = -3750763034362895579               -- 0xcbf29ce484222325

  for i = 1, #url do
    h = (h ~ url:byte(i)) * 1099511628211      -- 0x100000001b3, wrapping
  end

  return ("%08x%08x"):format((h >> 32) & 0xffffffff, h & 0xffffffff)
end

cache.name_of = name_of

local Store = {}
Store.__index = Store

-- The disk's size, from its superblock, or nil for a /Home in memory.
local function disk_bytes(dir)
  local top = dir:match("^(/[^/]+)") or "/Home"
  local sb = fs.read(top .. "/.super")

  if type(sb) == "table" and sb.blocks and sb.block_size then
    return sb.blocks * sb.block_size
  end

  return nil
end

--
-- `opts.dir`, the folder; `opts.most`, its bound in bytes, else an eighth
-- of the disk it is on and 256 MB at most - a sixteenth of 64 MB for a
-- /Home in memory; `opts.memory`, what is also held in memory, 8 MB.
--
function cache.open(opts)
  opts = opts or {}

  local dir = opts.dir or "/Home/Cache/Browser"
  local whole = disk_bytes(dir)
  local self = setmetatable({
    dir = dir,
    most = opts.most or math.min(256 * 1024 * 1024,
                                 whole and whole // 8 or 4 * 1024 * 1024),
    memory_most = opts.memory or 8 * 1024 * 1024,
    held = {},               -- address -> entry, the last few used
    held_bytes = 0,
    order = {},              -- addresses held, oldest first
    total = nil,             -- the folder's bytes, once counted
    made = false,
  }, Store)

  return self
end

-- The folder, made the first time something is kept in it - and its
-- parents, `/Home/Cache` the first time anything is.
function Store:make()
  if self.made then return true end

  local at = ""

  for part in self.dir:gmatch("[^/]+") do
    at = at .. "/" .. part

    if not fs.getattr(at) then fs.send(at, { type = "mkdir" }) end
  end

  self.made = fs.getattr(self.dir) ~= nil
  return self.made
end

local function hold(self, url, entry)
  local had = self.held[url]

  if had then
    self.held_bytes = self.held_bytes - #had.reply

    for i, u in ipairs(self.order) do
      if u == url then table.remove(self.order, i) break end
    end
  end

  if #entry.reply > self.memory_most // 4 then
    self.held[url] = nil
    return
  end

  self.held[url] = entry
  self.held_bytes = self.held_bytes + #entry.reply
  self.order[#self.order + 1] = url

  while self.held_bytes > self.memory_most and #self.order > 1 do
    local oldest = table.remove(self.order, 1)

    self.held_bytes = self.held_bytes - #self.held[oldest].reply
    self.held[oldest] = nil
  end
end

--
-- What is kept for `url`, at `now`: `{ reply, fresh, fresh_until, etag,
-- last_modified, secure }`, or nil. `fresh` says it may be used as it is;
-- otherwise it is to be asked about with its validators.
--
function Store:lookup(url, now)
  local entry = self.held[url]

  if not entry then
    local path = self.dir .. "/" .. name_of(url)
    local a = fs.getattr(path)

    if type(a) ~= "table" or a.address ~= url then return nil end

    local reply = fs.read(path)

    if type(reply) ~= "string" then return nil end

    entry = { reply = reply, fresh_until = tonumber(a.fresh_until) or 0,
              etag = a.etag ~= "" and a.etag or nil,
              last_modified = a.last_modified ~= "" and a.last_modified or nil,
              secure = a.secure == "yes" }
    hold(self, url, entry)
  end

  entry.fresh = now < entry.fresh_until
  return entry
end

-- The folder's bytes, counted once a session and kept in step after.
function Store:count()
  if self.total then return self.total end

  local total = 0

  for _, name in ipairs(fs.list(self.dir) or {}) do
    local a = fs.getattr(self.dir .. "/" .. name)

    total = total + (type(a) == "table" and tonumber(a.size) or 0)
  end

  self.total = total
  return total
end

-- Room for `bytes` more: the least lately used let go until it fits.
function Store:room(bytes)
  if bytes > self.most then return false end

  local total = self:count()

  if total + bytes <= self.most then return true end

  local all = {}

  for _, name in ipairs(fs.list(self.dir) or {}) do
    local path = self.dir .. "/" .. name
    local a = fs.getattr(path)

    if type(a) == "table" then
      all[#all + 1] = { path = path, url = a.address,
                        used = tonumber(a.used) or 0,
                        size = tonumber(a.size) or 0 }
    end
  end

  table.sort(all, function(x, y) return x.used < y.used end)

  for _, e in ipairs(all) do
    if total + bytes <= self.most then break end

    fs.send(e.path, { type = "delete" })
    total = total - e.size

    if e.url then self.held[e.url] = nil end
  end

  self.total = total
  return total + bytes <= self.most
end

--
-- A reply fetched for `url` and received at `now`, kept if what it says of
-- itself allows: `how` is `http.get`'s - nothing taken over a certificate
-- that did not check out. Returns whether it was kept, and why not.
--
function Store:store(url, reply, how, now)
  if how and how.scheme == "https" and how.secure ~= true then
    return false, "it came over a certificate that did not check out"
  end

  local status = tonumber(reply:match("^HTTP/%d%.%d%s+(%d%d%d)")) or 0
  local head = reply:match("^(.-)\r\n\r\n") or ""
  local p, why = cache.policy(status, head, now)

  if not p then return false, why end

  if not self:make() or not self:room(#reply) then
    return false, "there is no room for it"
  end

  local path = self.dir .. "/" .. name_of(url)
  local old = fs.getattr(path)

  if not fs.write(path, reply) then return false, "it could not be written" end

  fs.setattr(path, { address = url, fresh_until = tostring(p.fresh_until),
                     etag = p.etag or "", last_modified = p.last_modified or "",
                     secure = (how and how.secure) and "yes" or "no",
                     used = tostring(now) })

  self.total = (self.total or 0) + #reply
               - (type(old) == "table" and tonumber(old.size) or 0)
  hold(self, url, { reply = reply, fresh_until = p.fresh_until,
                    etag = p.etag, last_modified = p.last_modified,
                    secure = how and how.secure == true })
  return true
end

--
-- A kept reply the server said is still right (`304`), at `now`: how much
-- longer it may be used, from the 304's head over its own.
--
function Store:revalidated(url, entry, head304, now)
  local head = entry.reply:match("^(.-)\r\n\r\n") or ""
  local p = cache.refreshed(head, head304, now)

  if not p then return end

  entry.fresh_until = p.fresh_until
  entry.etag = p.etag or entry.etag
  entry.last_modified = p.last_modified or entry.last_modified

  fs.setattr(self.dir .. "/" .. name_of(url),
             { fresh_until = tostring(p.fresh_until), etag = entry.etag or "",
               last_modified = entry.last_modified or "",
               used = tostring(now) })
end

-- Its use noted, so the least lately used is what goes first.
function Store:used(url, now)
  fs.setattr(self.dir .. "/" .. name_of(url), { used = tostring(now) })
end

-- Everything kept, let go: the folder emptied, and memory with it.
function Store:empty()
  for _, name in ipairs(fs.list(self.dir) or {}) do
    fs.send(self.dir .. "/" .. name, { type = "delete" })
  end

  self.held, self.held_bytes, self.order, self.total = {}, 0, {}, 0
end

return cache
