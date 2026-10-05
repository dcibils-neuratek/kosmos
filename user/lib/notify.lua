-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Notifications, from Lua: `/Notifications`, the notification server.
--
--   local notify = use("/Kosmos/Libraries/notify.lua")
--   notify.post{ title = "Render finished",
--                body = "Kitchen.c3d, 1920 by 1080, in 4 min 12 s.",
--                alert = false,            -- true: it stays until closed
--                open = "/Home/Renders" }  -- what a press on it opens
--
-- **Who posted is not said here**: the server asks the kernel which process
-- the post came from and the file it runs (`notifyproto.h`), so there is
-- no field for it and no way to post as another application.
--
-- **The layout below is `notifyproto.h` written a second time**, as
-- `backlight.lua` and `audio.lua` write theirs, and asserted when this
-- loads. A text longer than its field is cut at a character, never inside
-- one.
--
-- What shows notifications walks them with `next`, one a call, asking each
-- time for what came after the last number it saw; `when` turns the
-- server's counter into how long ago, the one place that conversion is made.

local notify = {}

local OP_POST, OP_NEXT, OP_REMOVE, OP_CLEAR, OP_KEEP = 1, 2, 3, 4, 5
local ALERT = 1

local TITLE, BODY, OPEN = 64, 256, 128

local REQUEST = "<I4I4I4I4c64c256c128"
local HEAD    = "<I4I4I4I4"
local ENTRY   = "<I4I4I8I4I4c16c128c64c256c128"

assert(#string.pack(REQUEST, 0, 0, 0, 0, "", "", "") == 464,
       "notify: the request layout does not match notifyproto.h")
assert(#string.pack(HEAD, 0, 0, 0, 0) + #string.pack(ENTRY, 0, 0, 0, 0, 0, "", "", "", "", "") == 632,
       "notify: the reply layout does not match notifyproto.h")

local ERRORS = {
  [1] = "the notification server did not understand that",
  [2] = "a notification needs a title",
  [3] = "the notification server could not tell who sent it",
  [4] = "the notification server has no room",
}

-- `s` cut to fit under `n` bytes, at a character: before the one whose
-- bytes reach the `n`th, which `utf8.offset` finds by stepping back to
-- where it starts.
local function fit(s, n)
  s = tostring(s or "")
  if #s < n then return s end

  return s:sub(1, utf8.offset(s, 0, n) - 1)
end

local function text(s)
  return (s:gsub("%z.*$", ""))
end

local function request(op, flags, id, count, title, body, open)
  local packed = string.pack(REQUEST, op, flags or 0, id or 0, count or 0,
                             fit(title, TITLE), fit(body, BODY), fit(open, OPEN))
  local reply, why = fs.raw("/Notifications", packed, nil, "notify")

  if not reply then return nil, tostring(why) end
  if #reply < 632 then return nil, "the notification server sent a reply of the wrong size" end

  local err, count_, newest, held, at = string.unpack(HEAD, reply)

  if err ~= 0 then
    return nil, ERRORS[err] or ("notification error " .. tostring(err))
  end

  local entry = nil

  if count_ > 0 or op == OP_POST then
    local id_, flags_, counter, sender, _, name, from, title_, body_, open_ =
      string.unpack(ENTRY, reply, at)

    entry = {
      id = id_, alert = (flags_ & ALERT) ~= 0, at_counter = counter,
      sender = sender, name = text(name), from = text(from),
      title = text(title_), body = text(body_), open = text(open_),
    }
  end

  return { entry = entry, newest = newest, held = held, found = count_ > 0 }
end

-- Says something. Returns its number, or nil and why.
function notify.post(t)
  t = t or {}

  local got, why = request(OP_POST, t.alert and ALERT or 0, 0, 0,
                           t.title, t.body, t.open)

  if not got then return nil, why end

  return got.entry.id
end

-- The oldest kept after `after` (0 for the first), or nil; and the newest
-- number given and how many are kept, either way.
function notify.next(after)
  local got, why = request(OP_NEXT, 0, after or 0, 0)

  if not got then return nil, why end

  return got.found and got.entry or nil, got.newest, got.held
end

-- Every one kept after `after`, oldest first.
function notify.all(after)
  local out, at = {}, after or 0

  while true do
    local e = notify.next(at)
    if not e then break end
    out[#out + 1] = e
    at = e.id
  end

  return out
end

-- The newest number given so far: what a reader starts from to show only
-- what comes after it.
function notify.newest()
  local _, newest = notify.next(0xFFFFFFFF)
  return newest or 0
end

function notify.remove(id) return request(OP_REMOVE, 0, id, 0) ~= nil end
function notify.clear() return request(OP_CLEAR, 0, 0, 0) ~= nil end

-- How many the server keeps; 0 for as many as it can.
function notify.keep(n) return request(OP_KEEP, 0, 0, math.max(0, math.floor(n or 0))) ~= nil end

--
-- **How many seconds ago `entry` arrived**, from the server's counter: the
-- one function that turns that number into time, with the counter's
-- frequency from `/Devices/cpu` - the two clocks of `CLAUDE.md`, and a
-- number that crossed a boundary named for the one it is.
--
local counter_hz

function notify.age(entry)
  counter_hz = counter_hz or (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

  return math.max(0, (sys.ticks() - (entry.at_counter or 0)) / counter_hz)
end

--
-- Who said it, for a person: an application by its own name - a file in
-- `/Kosmos/Apps` or `/Home/Apps`, its stem with a capital - and something
-- built into the system by what it calls itself, capitalised the same way.
-- `names` may map a file to the name its header declares (`kosmos: name`),
-- which is what the Deskbar's menu already knows.
--
-- The system's own programs, which speak for the system rather than as
-- themselves: an application the window manager ended is the system's news.
local SYSTEM = { ["/Kosmos/Programs/wm.lua"] = true }

-- And the processes built into the image, by what a person calls them.
local BUILT_IN = { xhci = "USB", drives = "Drives", diskfs = "Disk", net = "Network",
                   audio = "Sound", e1000 = "Network" }

function notify.who(entry, names)
  if SYSTEM[entry.from] then return "System" end

  if entry.from ~= "" then
    if names and names[entry.from] then return names[entry.from] end

    local stem = entry.from:match("([^/]+)%.lua$") or entry.from:match("([^/]+)$") or entry.from
    return stem:sub(1, 1):upper() .. stem:sub(2)
  end

  local n = BUILT_IN[entry.name] or (entry.name ~= "" and entry.name) or "System"
  return n:sub(1, 1):upper() .. n:sub(2)
end

-- What the history and the Preferences call one sender: the file it runs,
-- or what a process built into the image calls itself.
function notify.key(entry)
  return entry.from ~= "" and entry.from or ("system:" .. entry.name)
end

return notify
