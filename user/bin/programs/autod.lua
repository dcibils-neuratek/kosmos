-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
-- autod: the automations in /Home/Automations, each run on its trigger
-- (`docs/automations.md` A3). Started by the Deskbar at login when there is
-- one to run, as `maild` is, and there whether the window is open or not.
--
--   autod                  run them, for as long as Kosmos is up
--
-- **Asleep between triggers.** It wakes at the nearest time an automation
-- is due, every five seconds to look at the folders and drives it was
-- asked to watch - a listing each - when `maild` pulses that mail arrived
-- (`mailarrived.lua`), and when somebody asks it something at
-- `/Running/autod`:
--
--   { type = "run", name = }       run it now, by hand; answered at once,
--                                  and run after the answer
--   { type = "status" }            each automation: on, its trigger, when
--                                  it next runs, its last run
--   { type = "reload" }            read /Home/Automations again now
--
-- **Never in the middle of somebody else's wait**: a run is queued and the
-- answer goes first, and between a run's steps it answers whoever asked
-- meanwhile - a press that waits on an application must not hold the
-- Automations window that asked for the run.
--
-- **A failure is said once**: a banner the first time an automation fails,
-- none while it goes on failing, and its history has every run.


local automations = use("/Kosmos/Libraries/automations.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local notify_ok, notify = pcall(use, "/Kosmos/Libraries/notify.lua")

local LOOK_SECONDS = 5

local tick_hz = (sys.info() or {}).tick_hz or 250
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

local known = {}          -- name -> { a = automation, next_at, last, failing, seen, drives }
local queue = {}          -- { name =, found =, trigger = }
local said_listing = nil
local running = nil       -- { name, step }: what is running now, for the window

local function now_epoch()
  local n = clock.now()
  return n and n.epoch or 0
end

-- A folder's names now, as a set.
local function listing(path)
  local set = {}

  for _, n in ipairs(fs.list(path) or {}) do
    set[tostring(type(n) == "table" and n.name or n)] = true
  end

  return set
end

local function queued(name, found, trigger)
  for _, q in ipairs(queue) do
    if q.name == name then return false end
  end

  queue[#queue + 1] = { name = name, found = found or {}, trigger = trigger }
  print(("autod: %s queued, %s"):format(name, trigger))
  return true
end

--
-- /Home/Automations read again: each automation's state kept across the
-- reading when it is still there - its last run, what its folder held - so
-- a reload is not a trigger.
--
local function reload(starting)
  local names = automations.list()
  local fresh = {}

  for _, name in ipairs(names) do
    local a, why = automations.read(name)

    if not a then
      if not (known[name] and known[name].bad == why) then print("autod: " .. tostring(why)) end
      fresh[name] = { bad = why }
    else
      local k = known[name] or {}

      k.a, k.bad = a, nil

      if a.when.kind == "folder" and not k.seen then k.seen = listing(a.when.path) end
      if a.when.kind == "drive" and not k.drives then k.drives = listing("/Drives") end

      k.next_at = automations.next_time(a.when, clock.now(), k.last)
      fresh[name] = k

      if starting and a.when.kind == "start" and a.on ~= false then
        queued(name, {}, "Kosmos started")
      end
    end
  end

  known = fresh

  local line = table.concat(names, ", ")

  if line ~= said_listing then
    print(("autod: %d automation%s - %s"):format(#names, #names == 1 and "" or "s", line))
    said_listing = line
  end
end

-- **Mail, told by pulse** (`mailarrived.lua`, `docs/pulses.md` P3). While
-- an automation waits for mail, `autod` is subscribed to `maild`: `maild`
-- pulses this endpoint when mail comes, and `autod` then asks what came
-- after the newest number it has seen. Nothing is read on a clock. The
-- subscription is renewed on the five-second look - one small call, only
-- while a mail automation exists - so a `maild` started later, or again,
-- learns of `autod`; its numbers start again with it, which the start
-- number it answers says.
local arrived = use("/Kosmos/Libraries/mailarrived.lua")
local mail_started, mail_seen = nil, 0
local mail_due = false
local autod_started = sys.ticks()       -- mail that came before this is not news

local function has(text, words)
  return words == nil or words == "" or tostring(text):lower():find(tostring(words):lower(), 1, true) ~= nil
end

local function wants_mail()
  for _, k in pairs(known) do
    if k.a and k.a.on ~= false and k.a.when.kind == "mail" then return true end
  end
  return false
end

local control                                   -- this program's endpoint, below

local function subscribe()
  if not control or not wants_mail() then return end

  local started = arrived.subscribe(control, "autod")

  if started and started ~= mail_started then
    -- A maild new to this autod: everything it kept is looked at, and what
    -- came since this autod started is news - a mail can arrive in the
    -- seconds between maild starting and this subscribing.
    mail_started, mail_seen = started, 0
    mail_due = true
    print("autod: told of mail by maild")
  elseif not started then
    mail_started = nil
  end
end

local function take_mail()
  mail_due = false

  local list, newest, started = arrived.since(mail_seen)

  if not list then return end

  if started ~= mail_started then
    -- A maild started again since: subscribed afresh on the next look.
    mail_started = nil
    return
  end

  mail_seen = newest

  for _, m in ipairs(list) do
    -- Mail that came before this autod started is not news to it.
    local fresh = (tonumber(m.at) or 0) >= autod_started

    for name, k in pairs(known) do
      local a = k.a

      if fresh and a and a.on ~= false and a.when.kind == "mail" and has(m.from, a.when.from)
         and has(m.subject, a.when.subject) then
        queued(name, { sender = m.from, subject = m.subject, message = m.path },
               "mail from " .. tostring(m.from))
      end
    end
  end
end

-- Folders and drives, looked at: a name that was not there is a trigger -
-- and mail's subscription renewed.
local function look()
  subscribe()

  for name, k in pairs(known) do
    local a = k.a

    if a and a.on ~= false and a.when.kind == "folder" then
      local now = listing(a.when.path)

      for n in pairs(now) do
        if not k.seen[n] then
          queued(name, { file = a.when.path:gsub("/$", "") .. "/" .. n }, "a file appeared: " .. n)
        end
      end

      k.seen = now
    elseif a and a.on ~= false and a.when.kind == "drive" then
      local now = listing("/Drives")

      for n in pairs(now) do
        if not k.drives[n] and (a.when.name == nil or n == a.when.name) then
          queued(name, { drive = "/Drives/" .. n }, "a drive arrived: " .. n)
        end
      end

      k.drives = now
    end
  end
end

-- What is due by the clock.
local function due()
  local t = now_epoch()

  for name, k in pairs(known) do
    if k.a and k.a.on ~= false and k.next_at and k.next_at <= t then
      queued(name, {}, k.a.when.kind == "every" and "its interval" or "its time")
      k.last = t
      k.next_at = automations.next_time(k.a.when, clock.now(), k.last)
    end
  end
end

-- How long to sleep, in ticks: until the nearest time, and no longer than
-- the look at folders.
local function sleep_ticks()
  local t, soonest = now_epoch(), LOOK_SECONDS

  for _, k in pairs(known) do
    if k.next_at then soonest = math.min(soonest, math.max(0, k.next_at - t)) end
  end

  return math.max(1, math.floor(soonest * tick_hz))
end

local serve

-- One automation run, kept, and its failure said once.
local function run_one(q)
  local k = known[q.name]
  local a = k and k.a

  if not a then
    print(("autod: %s is not an automation any more"):format(q.name))
    return
  end

  print(("autod: %s runs, %s"):format(q.name, q.trigger))

  local run = automations.run(a, q.found, function(i, step)
    print(("autod: %s step %d %s - %s%s"):format(q.name, i, step.said, step.ok and "ok" or "failed",
          step.why and (": " .. step.why) or ""))
    serve(false)
  end, q.trigger, function(i)
    running = { name = q.name, step = i }
  end)

  running = nil

  automations.keep(q.name, run)
  k.last_run = run

  if run.ok then
    print(("autod: %s %s in %d ms%s"):format(q.name, run.stopped and "stopped" or "done", run.ms,
          run.stopped and (" - " .. run.why) or ""))
    k.failing = false
  else
    print(("autod: %s failed - %s"):format(q.name, run.why))

    if not k.failing and notify_ok then
      notify.post{ title = q.name .. " failed", body = run.why }
    end

    k.failing = true
  end
end

control = sys.endpoint()

if control then fs.send("/Running", { type = "register", name = "autod" }, control) end

local function answer(req)
  if req.type == "run" then
    local name = tostring(req.name or "")

    if not (known[name] and known[name].a) then
      return { ok = false, error = name .. " is not an automation here" }
    end

    queued(name, {}, "by hand")
    return { ok = true }
  elseif req.type == "status" then
    local out = {}

    for name, k in pairs(known) do
      out[#out + 1] = { name = name, on = k.a and k.a.on ~= false, kind = k.a and k.a.when.kind,
                        next_at = k.next_at, bad = k.bad, failing = k.failing,
                        last_ok = k.last_run and k.last_run.ok, last_why = k.last_run and k.last_run.why }
    end

    table.sort(out, function(x, y) return x.name < y.name end)
    return { ok = true, automations = out, running = running }
  elseif req.type == "reload" then
    reload(false)
    return { ok = true }
  end

  return { ok = false, error = "autod answers run, status and reload" }
end

-- Whoever asked, answered; `wait` ticks for the first, or none.
function serve(wait)
  if not control then
    if wait then sys.sleep(wait) end
    return
  end

  local req, who = sys.receive(control, not wait, wait or nil)

  while req do
    if type(req) == "table" and req.pulse then
      -- News, from nobody to answer: mail arrived.
      if req.code == arrived.CODE then mail_due = true end
    else
      pcall(sys.reply, who, answer(type(req) == "table" and req or {}))
    end

    req, who = sys.receive(control, true)
  end
end

print("autod: watching " .. automations.DIR)
reload(true)
subscribe()

local looked_at = sys.ticks()

while true do
  while #queue > 0 do run_one(table.remove(queue, 1)) end

  serve(sleep_ticks())

  if mail_due then take_mail() end

  if (sys.ticks() - looked_at) / counter_hz >= LOOK_SECONDS then
    looked_at = sys.ticks()
    reload(false)
    look()
  end

  due()
end
