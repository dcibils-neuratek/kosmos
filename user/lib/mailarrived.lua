-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **What mail arrived, told to whoever subscribed** (`docs/automations.md`, a
-- mail arriving as a trigger; `docs/pulses.md` P3).
--
-- In `maild`:
--   arrived.add{ { account =, from =, subject =, path = }, ... }   kept, and told
--   arrived.answer(req, cap)    `subscribe` and `arrived` requests, or nil
-- In a subscriber (`autod`):
--   arrived.subscribe(ep, name) -> true and maild's start number, or nil and why
--   arrived.since(seq)          -> what came after `seq`, oldest first, and the newest seq
--   arrived.CODE                 the pulse's code: mail arrived
--
-- **Told, not looked for.** A subscriber hands `maild` its endpoint once;
-- `maild` keeps the last fifty arrivals, numbered, and when mail comes it
-- **pulses** each subscriber (`sys.pulse`): sixteen bytes on the
-- subscriber's endpoint, and `maild` goes on - it never waits on anybody.
-- The subscriber, when it is ready, asks what came after the newest number
-- it has seen: *control by message*, the pulse saying only that something
-- happened. Two arrivals before it looks are one pulse counted twice.
--
-- It was a file in `/Temporary/maild` that `autod` read every five seconds,
-- because Nebula's IPC had no way to tell without waiting (0.11.226); Diego
-- then: "Go ahead with files And then we will design pulses". Pulses came
-- in 0.11.227, and this is the one file that changed.
--
-- What arrived before `maild` started is not news; the numbers start again
-- with it, and a subscriber learns that from the start number `subscribe`
-- answers. Each arrival carries `at`, the counter when it came, so a
-- subscriber that started after `maild` - `autod` and `maild` are started
-- together at login, in either order - takes what came since *it* started
-- and not before.

local arrived = {}

arrived.CODE = 1
arrived.KEPT = 50

--------------------------------------------------------------------------
-- maild's half.
--------------------------------------------------------------------------

local kept, seq = {}, 0
local subscribers = {}      -- name -> capability to the subscriber's endpoint
local started = sys.ticks()

-- Kept, numbered, and each subscriber pulsed once for the lot. One that is
-- gone - its endpoint destroyed - is let go.
function arrived.add(messages)
  if not messages or #messages == 0 then return end

  for _, m in ipairs(messages) do
    seq = seq + 1
    kept[#kept + 1] = { seq = seq, at = sys.ticks(), account = tostring(m.account or ""),
                        from = tostring(m.from or ""), subject = tostring(m.subject or ""),
                        path = m.path and tostring(m.path) or nil }
  end

  while #kept > arrived.KEPT do table.remove(kept, 1) end

  for name, cap in pairs(subscribers) do
    local ok, why = sys.pulse(cap, arrived.CODE, seq)

    if not ok and tostring(why):find("no such capability", 1, true) then
      subscribers[name] = nil
      pcall(sys.release, cap)
    end
  end
end

local function after(n)
  local out = {}

  for _, m in ipairs(kept) do
    if m.seq > (n or 0) then out[#out + 1] = m end
  end

  return out
end

-- A request this answers - `subscribe`, with the subscriber's endpoint
-- passed beside it, or `arrived` - or nil for one it does not.
function arrived.answer(req, cap)
  if req.type == "subscribe" then
    local name = tostring(req.name or "")

    if name == "" or not cap or cap < 0 then
      return { ok = false, why = "a subscription is a name and an endpoint passed with it" }
    end

    -- The same subscriber again - started over - replaces its old door.
    if subscribers[name] and subscribers[name] ~= cap then pcall(sys.release, subscribers[name]) end

    subscribers[name] = cap
    return { ok = true, newest = seq, started = started }
  end

  if req.type == "arrived" then
    return { ok = true, list = after(tonumber(req.after) or 0), newest = seq, started = started }
  end
end

--------------------------------------------------------------------------
-- A subscriber's half.
--------------------------------------------------------------------------

local function ask(req, pass)
  local ok, reply = pcall(fs.send, "/Running/maild", req, pass)

  if ok and type(reply) == "table" and reply.ok then return reply end
  return nil, ok and type(reply) == "table" and reply.why or "maild is not running"
end

function arrived.subscribe(ep, name)
  local r, why = ask({ type = "subscribe", name = tostring(name) }, ep)

  if not r then return nil, why end
  return r.started, r.newest
end

function arrived.since(n)
  local r, why = ask{ type = "arrived", after = n or 0 }

  if not r then return nil, why end
  return r.list or {}, r.newest or 0, r.started
end

return arrived
