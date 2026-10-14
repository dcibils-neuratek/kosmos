-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **What mail arrived, kept for whoever looks** (`docs/automations.md`, a
-- mail arriving as a trigger).
--
--   local arrived = use("/Kosmos/Libraries/mailarrived.lua")
--   arrived.add{ { account =, from =, subject =, path = }, ... }   maild
--   arrived.since(seq)   -> the ones after `seq`, oldest first, and the newest seq
--   arrived.newest()     -> the newest seq, 0 when none
--
-- **Written, not called**: `maild` adds to a file and `autod` reads it on
-- its five-second look, as it looks at folders. A call from one to the
-- other would have `maild` wait while `autod` is inside a step - Diego, 14
-- October 2026, agreeing this over the call: "Your recommendations are ok".
-- When programs have threads of their own, a call can replace it.
--
-- The last fifty, each with its number; a reader keeps the newest number
-- it has seen and asks for what came after. In /Temporary: what arrived
-- before this start of Kosmos is not news to anybody.

local arrived = {}

arrived.PATH = "/Temporary/maild/arrived"
arrived.KEPT = 50

local function read()
  local t = fs.read(arrived.PATH)

  if type(t) ~= "table" then return { seq = 0, list = {} } end
  t.seq, t.list = tonumber(t.seq) or 0, type(t.list) == "table" and t.list or {}
  return t
end

function arrived.add(messages)
  if not messages or #messages == 0 then return true end

  local t = read()

  for _, m in ipairs(messages) do
    t.seq = t.seq + 1
    t.list[#t.list + 1] = { seq = t.seq, account = tostring(m.account or ""),
                            from = tostring(m.from or ""), subject = tostring(m.subject or ""),
                            path = m.path and tostring(m.path) or nil }
  end

  while #t.list > arrived.KEPT do table.remove(t.list, 1) end

  pcall(function() use("/Kosmos/Libraries/files.lua").make_folder("/Temporary/maild") end)
  return fs.write(arrived.PATH, t)
end

function arrived.since(seq)
  local t = read()
  local out = {}

  for _, m in ipairs(t.list) do
    if m.seq > (seq or 0) then out[#out + 1] = m end
  end

  return out, t.seq
end

function arrived.newest()
  return read().seq
end

return arrived
