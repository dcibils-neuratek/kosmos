-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: needs network
-- What this machine is on the network, and how to change it.
--
--   wm network
--
-- The card, the addresses, and a way to try them. `Appearance` is the model:
-- a settings window that writes a file and tells the thing that cares, so
-- the change takes effect now *and* survives a reboot.
--
-- **The device list is one row long and that is not a placeholder.** This
-- machine has one network card, `hal_net_init` takes the first one it finds,
-- and a list that offered a choice between one thing would be an interface
-- pretending to a generality the system does not have. It shows what was
-- found; when there are two cards, choosing between them is a real feature
-- with a real question behind it - which one is the default route - and it
-- can be built then.

local ui = use("/lib/ui.lua")
local theme = ui.theme

local W, H = 460, 420
local BAR_H = gfx.font.h + 8

local SETTINGS = "/home/.network"

local win, err = ui.window{ title = "Network", w = W, h = H, x = 140, y = 90 }

if not win then
  print("network: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- Addresses, between four bytes and four numbers.
--
-- The protocol carries bytes in the order they go on the wire; a person
-- writes them with dots. The conversion lives here for the reason `ping`
-- gives: text is a presentation, and a stack that parsed dotted quads would
-- be deciding how somebody else writes an address.
--------------------------------------------------------------------------

local function to_bytes(text)
  local a, b, c, d = tostring(text or ""):match("^%s*(%d+)%.(%d+)%.(%d+)%.(%d+)%s*$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end

  return string.char(a, b, c, d)
end

local function dotted(bytes)
  if type(bytes) ~= "string" or #bytes ~= 4 then return "" end

  return ("%d.%d.%d.%d"):format(bytes:byte(1, 4))
end

local function mac_text(bytes)
  if type(bytes) ~= "string" or #bytes ~= 6 then return "unknown" end

  local out = {}

  for i = 1, 6 do out[i] = ("%02x"):format(bytes:byte(i)) end

  return table.concat(out, ":")
end

--------------------------------------------------------------------------

local info = fs.net_info("/net")
local status = ui.label{ x = 12, y = H - 30, w = W - 24, text = "",
                         follow = { "left", "right", "bottom" } }

--
-- The card, as facts rather than as a control.
--
-- Every line here is something the machine found out rather than something
-- a person chose, so none of it is editable. A settings window that lets you
-- type a MAC address is a settings window that lies about what it can do.
--
-- Below its own label rather than on top of it. The label sits at
-- `BAR_H` and is a line of text tall, so the box starts a line lower;
-- twelve put the two in the same place and the word "Device" was drawn
-- behind the frame.
local device = ui.view{ x = 12, y = 22 + BAR_H, w = W - 24, h = 74,
                        follow = { "left", "right", "top" } }

function device:draw(g)
  g:sunken(0, 0, self.w, self.h, "sunken")

  local y = 6

  if not info or not info.card then
    g:text(8, y, "no network card found", theme.bad, theme.sunken)
    g:text(8, y + gfx.font.h + 4,
           "nothing below will do anything", theme.text_dim, theme.sunken)
    return
  end

  g:text(8, y, "virtio-net", theme.text, theme.sunken)
  y = y + gfx.font.h + 4
  g:text(8, y, "hardware address  " .. mac_text(info.mac),
         theme.text_dim, theme.sunken)
  y = y + gfx.font.h + 4
  g:text(8, y, ("MTU               %d bytes"):format(info.mtu or 0),
         theme.text_dim, theme.sunken)
end

win:add(ui.label{ x = 12, y = BAR_H, w = 200, text = "Device",
                  color = "text_dim" })
win:add(device)

--------------------------------------------------------------------------
-- The settings.
--------------------------------------------------------------------------

local saved = fs.read(SETTINGS)

if type(saved) ~= "table" then saved = {} end

local function field_at(y, label, value)
  win:add(ui.label{ x = 12, y = y + 4, w = 90, text = label,
                    color = "text_dim" })

  local f = ui.field{ x = 110, y = y, w = 180, text = value or "" }

  win:add(f)

  return f
end

local top = 120 + BAR_H

win:add(ui.label{ x = 12, y = top - 18, w = 200, text = "Addresses",
                  color = "text_dim" })

local address = field_at(top, "address",
                         dotted(info and info.address) ~= ""
                         and dotted(info.address) or (saved.address or ""))
local netmask = field_at(top + 30, "netmask",
                         dotted(info and info.netmask) ~= ""
                         and dotted(info.netmask) or (saved.netmask or ""))
local gateway = field_at(top + 60, "gateway",
                         dotted(info and info.gateway) ~= ""
                         and dotted(info.gateway) or (saved.gateway or ""))

--
-- DNS, and it is worth being straight about it.
--
-- **Nothing on this machine resolves a name yet.** There is no resolver, so
-- this setting is remembered and used by nothing: `ping` and `fetch` take
-- four numbers and say so. It is here because a network settings window
-- without a DNS field is a window somebody will look for one in - and
-- because the value is what a resolver will need on the day there is one.
--
-- Saying that in the interface rather than only in this comment is the
-- point. A field that quietly does nothing is worse than no field.
--
local dns = field_at(top + 90, "DNS", saved.dns or "10.0.2.3")

-- Short enough to fit, which the first version was not: it read "nothing
-- resolves names yet; this is remembered only" and the window cut it at
-- "this is remem". A caveat that does not fit is a caveat nobody reads.
win:add(ui.label{ x = 110, y = top + 114, w = W - 130,
                  text = "no resolver yet - remembered only",
                  color = "line" })

--------------------------------------------------------------------------

local function collect()
  local a, m, g = to_bytes(address.text), to_bytes(netmask.text),
                  to_bytes(gateway.text)

  if not a then return nil, "the address is not four numbers and three dots" end
  if not m then return nil, "the netmask is not four numbers and three dots" end
  if not g then return nil, "the gateway is not four numbers and three dots" end

  return a, m, g
end

--
-- Apply, and then save.
--
-- In that order, and it matters: a setting that could not be applied should
-- not be the one the machine boots with next time. `Appearance` writes its
-- file the same way round and for the same reason.
--
local function apply(and_save)
  local a, m, g = collect()

  if not a then
    status.text = m
    return
  end

  local ok, why = fs.net_configure("/net", a, m, g)

  if not ok then
    status.text = "the stack refused it: " .. tostring(why)
    return
  end

  info = fs.net_info("/net")

  if not and_save then
    status.text = "applied, until the next reboot"
    return
  end

  local put, werr = fs.write(SETTINGS, {
    address = address.text:match("^%s*(.-)%s*$"),
    netmask = netmask.text:match("^%s*(.-)%s*$"),
    gateway = gateway.text:match("^%s*(.-)%s*$"),
    dns     = dns.text:match("^%s*(.-)%s*$"),
  })

  status.text = put and "applied, and saved for next time"
                or ("applied, but not saved: " .. tostring(werr))
end

local row = top + 140

win:add(ui.button{ x = 12, y = row, w = 70, h = 24, text = "Apply",
                   on_click = function() apply(false) end })
win:add(ui.button{ x = 90, y = row, w = 60, h = 24, text = "Save",
                   on_click = function() apply(true) end })

--
-- And a way to find out whether any of it worked.
--
-- One echo to the gateway, which is the first thing that can be wrong and
-- the only address the machine is certain to have been told about. A
-- settings window with no way to test the setting sends people to a prompt.
--
win:add(ui.button{
  x = 160, y = row, w = 110, h = 24, text = "Test gateway",
  on_click = function()
    local a = to_bytes(gateway.text)

    if not a then
      status.text = "the gateway is not an address"
      return
    end

    local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
    local reply, why = fs.ping("/net", a, 1, "kosmos network settings test")

    if reply then
      local us = reply.ticks * 1000000 // hz

      status.text = ("the gateway answered in %d.%03d ms")
                    :format(us // 1000, us % 1000)
    else
      status.text = "no answer from the gateway (" .. tostring(why) .. ")"
    end
  end,
})

win:add(status)

if not info or not info.card then
  status.text = "there is no card; these settings will not take effect"
end

win:run()
