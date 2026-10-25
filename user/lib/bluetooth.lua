-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Bluetooth, as a program sees it (`bluetooth.md` B4).
--
--   local bt = use("/Kosmos/Libraries/bluetooth.lua")
--   local s = bt.state()          -- the adapter: present, maker, version,
--                                 -- address, what it can do, scanning
--   bt.scan(10000)                -- look for ten seconds more: a lease,
--                                 -- renewed while results are wanted
--   for _, d in ipairs(bt.devices()) do ... end   -- nearest first
--   bt.stop()
--
-- `/Devices/bluetooth` speaks a declared struct (`btproto.h`), and this is
-- the one place in Lua that knows its shape - the Bluetooth app, `bt`,
-- This Machine and the devices indicator all go through it. Only a program
-- that declares `kosmos: needs bluetooth` has `/Devices/bluetooth` at all;
-- for any other, `bt.state()` is nil and says why.
--
-- **Asked, never waited on.** The host stack answers from what it has
-- heard; a scan's results arrive as the radio hears them, so a program
-- asks again on its own clock rather than waiting for one.
--
-- A device is a table:
--
--   address    "02:4B:53:00:03:80", as a person reads it
--   name       what it calls itself, or "" until it has said
--   kind       "keyboard", "mouse", "keyboard and mouse", "headphones",
--              "game controller", "phone", "computer", "input device",
--              "other"
--   transport  "classic" or "le"
--   rssi       dBm, -40 near and -90 far, or nil when it did not say
--   heard_ms   how long ago it was last heard

local bluetooth = {}

-- struct bt_request: op, arg, reserved[2]
local REQUEST = "<I4I4I4I4"

-- struct bt_reply's header: error, present, bus, scanning, flags, port,
-- version, reserved, manufacturer, acl_length, acl_packets, reserved,
-- address, reserved, devices, first, count, maker
local HEADER = "<I4BBBBI2BBI2I2I2I2c6c2I4I4I4c24"
local HEADER_SIZE = 64

-- struct bt_device: address, transport, kind, rssi, flags, appearance, cod,
-- heard_ms, name
local DEVICE = "<c6BBbBI2I4I4c52"
local DEVICE_SIZE = 72
local REPLY_SIZE = HEADER_SIZE + 24 * DEVICE_SIZE

local OP = { state = 1, scan = 2, stop = 3, list = 4 }

local ERRORS = {
  [1] = "the Bluetooth stack did not understand that",
  [2] = "there is no Bluetooth adapter",
  [3] = "the Bluetooth adapter is not ready",
}

-- `gap_decode.h`'s GAP_KIND_*: the words are the reader's to choose, as
-- every protocol here has it.
local KINDS = {
  [0] = "other", "keyboard", "mouse", "keyboard and mouse", "headphones",
  "game controller", "phone", "computer", "input device",
}

-- Assigned Numbers' HCI versions, by number.
local VERSIONS = {
  [0] = "1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1", "4.2",
  "5.0", "5.1", "5.2", "5.3", "5.4", "6.0",
}

local BUSES = { [1] = "usb", [2] = "simulated" }

local function cstring(s)
  return (s:match("^[^%z]*"))
end

-- An address as a person reads it: most significant byte first.
local function address(raw)
  local out = {}

  for i = 6, 1, -1 do out[#out + 1] = ("%02X"):format(raw:byte(i)) end

  return table.concat(out, ":")
end

local function ask(op, arg)
  local ok, reply, why = pcall(fs.raw, "/Devices/bluetooth",
                               string.pack(REQUEST, op, arg or 0, 0, 0),
                               nil, "bluetooth")

  if not ok or not reply then
    return nil, tostring(ok and why or reply)
  end

  if #reply < REPLY_SIZE then
    return nil, "the Bluetooth stack sent a reply of the wrong size"
  end

  local err = string.unpack("<I4", reply)

  if err ~= 0 then
    return nil, ERRORS[err] or ("Bluetooth error " .. tostring(err))
  end

  return reply
end

local function state_of(reply)
  local _, present, bus, scanning, flags, port, version, _, manufacturer,
        acl_length, acl_packets, _, raw, _, devices, first, count, maker =
    string.unpack(HEADER, reply)

  return {
    present = present ~= 0,
    bus = BUSES[bus] or "other",
    port = port,
    scanning = scanning ~= 0,
    classic = flags & 1 ~= 0,
    le = flags & 2 ~= 0,
    ssp = flags & 4 ~= 0,
    version = VERSIONS[version] or ("version " .. version),
    manufacturer = manufacturer,
    maker = cstring(maker) ~= "" and cstring(maker)
            or ("company " .. manufacturer),
    acl_length = acl_length,
    acl_packets = acl_packets,
    address = address(raw),
    devices = devices,
    first = first,
    count = count,
  }
end

-- The adapter, and how many devices it has heard.
function bluetooth.state()
  local reply, why = ask(OP.state)

  if not reply then return nil, why end

  return state_of(reply)
end

-- Look for `ms` more milliseconds: 10 seconds unless said, 30 at most.
function bluetooth.scan(ms)
  local reply, why = ask(OP.scan, ms or 10000)

  if not reply then return nil, why end

  return state_of(reply)
end

function bluetooth.stop()
  local reply, why = ask(OP.stop)

  if not reply then return nil, why end

  return state_of(reply)
end

-- Every device heard, nearest first.
function bluetooth.devices()
  local out, first = {}, 0

  repeat
    local reply, why = ask(OP.list, first)

    if not reply then return out, why end

    local s = state_of(reply)

    for i = 0, s.count - 1 do
      local raw, transport, kind, rssi, _, appearance, cod, heard_ms, name =
        string.unpack(DEVICE, reply, HEADER_SIZE + i * DEVICE_SIZE + 1)

      out[#out + 1] = {
        address = address(raw),
        name = cstring(name),
        kind = KINDS[kind] or "other",
        transport = transport == 1 and "classic" or "le",
        rssi = rssi ~= 127 and rssi or nil,
        appearance = appearance,
        cod = cod,
        heard_ms = heard_ms,
      }
    end

    first = first + s.count
  until s.count == 0 or first >= s.devices

  return out
end

return bluetooth
