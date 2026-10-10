-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What is on the network, by name: `/Discovery`, `mdnsd`
-- (`docs/sharing.md` N11c, `user/include/mdnsproto.h`).
--
--   local mdns = use("/Kosmos/Libraries/mdns.lua")
--   mdns.browse("_smb._tcp")            -> how many are known; asks again
--   mdns.instances("_smb._tcp")         -> { { name, host, port, address }, ... }
--   mdns.resolve("studio.local", ticks) -> four bytes, or nil and why
--
-- `browse` is asked and answered at once from what `mdnsd` keeps, and sets
-- it asking the network for the next ten seconds; a window that lists
-- servers browses on its own clock and shows what has come, never waiting
-- in a repaint (`CLAUDE.md`, nothing on the desktop waits on a server).
--

local M = {}

local REQUEST = "<I4I4I4I4c256"
local REPLY = "<I4I4I2I2c4c64c256"
local OP = { browse = 1, instance = 2, resolve = 3 }
local ERRORS = {
  [1] = "the discovery server did not understand that",
  [2] = "that is not a service type or a .local name",
  [3] = "no such instance",
  [4] = "nobody answered for that name",
  [5] = "this machine has no network yet",
  [6] = "the discovery server has no room for that",
}

-- A string up to its first zero byte.
local function cut(s) return (s:gsub("%z.*$", "")) end

local function ask(op, name, index, ticks)
  local bytes = string.pack(REQUEST, op, index or 0, ticks or 0, 0,
                            name .. string.rep("\0", 256 - #name))
  local ok, reply, why = pcall(fs.raw, "/Discovery", bytes, nil, "mdns")

  if not ok then return nil, tostring(reply) end
  if type(reply) ~= "string" or #reply < 336 then return nil, tostring(why or reply) end

  local err, count, port, has, address, instance, host = string.unpack(REPLY, reply)

  if err ~= 0 then return nil, ERRORS[err] or ("error " .. err) end

  return { count = count, port = port, address = has ~= 0 and address or nil,
           name = cut(instance), host = cut(host) }
end

function M.browse(type_)
  if #type_ > 255 then return nil, ERRORS[2] end

  local r, why = ask(OP.browse, type_)

  return r and r.count, why
end

-- Every instance known of a type, an item a message.
function M.instances(type_)
  local list, i = {}, 0

  while true do
    local r = ask(OP.instance, type_, i)

    if not r then break end

    list[#list + 1] = { name = r.name, host = r.host ~= "" and r.host or nil,
                        port = r.port ~= 0 and r.port or nil, address = r.address }
    i = i + 1
  end

  return list
end

function M.resolve(name, ticks)
  if #name > 255 then return nil, ERRORS[2] end

  local r, why = ask(OP.resolve, name, 0, ticks or 0)

  return r and r.address, why
end

return M
