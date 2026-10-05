-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Network
-- kosmos: name Network
-- kosmos: section preferences
-- kosmos: needs network
-- What this machine is on the network, and how to change it.
--
--   wm network
--
-- The card, the addresses, and a way to try them: a settings window that
-- writes a file and tells the thing that cares, so the change takes effect
-- now *and* survives a reboot.
--
-- **The device list is one row long and that is not a placeholder.** This
-- machine has one network card, `hal_net_init` takes the first one it finds,
-- and a list that offered a choice between one thing would be an interface
-- pretending to a generality the system does not have. It shows what was
-- found; when there are two cards, choosing between them is a real feature
-- with a real question behind it - which one is the default route - and it
-- can be built then.

local ui = use("/Kosmos/Libraries/ui.lua")
local hardware = use("/Kosmos/Libraries/hardware.lua")
local theme = ui.theme

local W, H = 500, 490

local prefs = use("/Kosmos/Libraries/prefs.lua")

local win, err = ui.window{ title = "Network", w = W, h = H, x = 140, y = 90,
                            header = true }

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

local info = fs.net_info("/Network")

-- Which card, from the bus. See `/Kosmos/Libraries/hardware.lua` for why it is not the
-- name of the driver.
local driven, undriven = hardware.network(sys.bus())
local L = ui.layout

--------------------------------------------------------------------------
-- The window, as `docs/apps.html` draws it (`roadmap.md` 5zp): the card's
-- name in the header with Apply as the verb, Save and the gateway test
-- behind the dots, and two cards - what the machine found, and what a
-- person chooses. It was a sunken box and four fields at positions picked
-- one by one, and a caveat under the last field in a colour meant for
-- borders.
--------------------------------------------------------------------------

--
-- The card, as facts rather than as a control.
--
-- Every line here is something the machine found out rather than something
-- a person chose, so none of it is editable. A settings window that lets you
-- type a MAC address is a settings window that lies about what it can do.
--
local function device_row()
  if not info or not info.card then
    -- A controller found and not driven is not "no card", and on a laptop
    -- with an Ethernet port that is the case this row is most likely to say.
    return { label = undriven[1]
                     and ("No driver for " .. undriven[1].name)
                     or "No network card found",
             note = undriven[1] and ("at " .. undriven[1].place
                                     .. ", so nothing below will do anything")
                    or "nothing below will do anything" }
  end

  return { label = driven[1] and driven[1].name or "A card the bus did not list",
           note = ("%s · %s · MTU %d")
                  :format(driven[1] and driven[1].place or "?",
                          mac_text(info.mac), info.mtu or 0) }
end

--------------------------------------------------------------------------
-- The settings.
--------------------------------------------------------------------------

local saved = prefs.read("network")

local function field(value)
  return ui.field{ w = 190, text = value or "" }
end

local address = field(dotted(info and info.address) ~= ""
                      and dotted(info.address) or (saved.address or ""))
local netmask = field(dotted(info and info.netmask) ~= ""
                      and dotted(info.netmask) or (saved.netmask or ""))
local gateway = field(dotted(info and info.gateway) ~= ""
                      and dotted(info.gateway) or (saved.gateway or ""))

--
-- **The name server**: the stack resolves names through it (`NET_OP_RESOLVE`),
-- and init applies the saved one at boot. Apply hands it to the stack with
-- the rest - until 0.11 it did not, and Apply switched name resolution off
-- until the next boot, under a note that said there was no resolver.
--
local dns = field(saved.dns or "10.0.2.3")

local apply                      -- the verb, below

local more = ui.iconbutton{ icon = "more" }

local header = ui.header{
  x = 0, y = 0, w = W, title = "Network",
  sub = driven[1] and driven[1].name or "no card",
  right = { ui.button{ text = "Apply", go = true,
                       on_click = function() apply(false) end },
            more },
  title_bar = true,
}

local cards = ui.cards{
  x = 0, y = L.head, w = W, h = H - L.head,
  groups = {
    { name = "Device", rows = { device_row() } },
    { name = "Addresses", rows = {
        { label = "Address", control = address },
        { label = "Netmask", control = netmask },
        { label = "Gateway", control = gateway },
        { label = "DNS", control = dns } } },
  },
}

--
-- What the last thing done said, under the cards where the drawings put a
-- page's note: 10 below the last card, in the dim `ui` face.
--
local status = ui.label{ x = L.page_side + 3,
                         y = L.head + cards.content_h + 10,
                         w = W - 2 * L.page_side - 3, text = "",
                         color = "text_dim", role = "ui" }

win:add(header)
win:add(cards)

--------------------------------------------------------------------------

local function collect()
  local a, m, g = to_bytes(address.text), to_bytes(netmask.text),
                  to_bytes(gateway.text)
  local d = to_bytes(dns.text)

  if not a then return nil, "the address is not four numbers and three dots" end
  if not m then return nil, "the netmask is not four numbers and three dots" end
  if not g then return nil, "the gateway is not four numbers and three dots" end
  if not d then return nil, "the name server is not four numbers and three dots" end

  return a, m, g, d
end

--
-- Apply, and then save.
--
-- In that order, and it matters: a setting that could not be applied should
-- not be the one the machine boots with next time.
--
function apply(and_save)
  local a, m, g, d = collect()

  if not a then
    status.text = m
    return
  end

  local ok, why = fs.net_configure("/Network", a, m, g, d)

  if not ok then
    status.text = "the stack refused it: " .. tostring(why)
    return
  end

  info = fs.net_info("/Network")

  if not and_save then
    status.text = "applied, until the next reboot"
    return
  end

  local put, werr = prefs.write("network", {
    address = address.text:match("^%s*(.-)%s*$"),
    netmask = netmask.text:match("^%s*(.-)%s*$"),
    gateway = gateway.text:match("^%s*(.-)%s*$"),
    dns     = dns.text:match("^%s*(.-)%s*$"),
  })

  status.text = put and "applied, and saved for next time"
                or ("applied, but not saved: " .. tostring(werr))
end

--
-- And a way to find out whether any of it worked.
--
-- One echo to the gateway, which is the first thing that can be wrong and
-- the only address the machine is certain to have been told about. A
-- settings window with no way to test the setting sends people to a prompt.
--
local function test_gateway()
  local a = to_bytes(gateway.text)

  if not a then
    status.text = "the gateway is not an address"
    return
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local reply, why, said = fs.ping("/Network", a, 1, "kosmos network settings test")

  if reply then
    local us = reply.ticks * 1000000 // hz

    status.text = ("the gateway answered in %d.%03d ms")
                  :format(us // 1000, us % 1000)
  else
    status.text = "no answer from the gateway: " .. (said or tostring(why))
  end
end

more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, {
    { text = "Save", on_choose = function() apply(true) end },
    { text = "Test gateway", on_choose = test_gateway },
  })
end

win:add(status)

if not info or not info.card then
  status.text = "there is no card; these settings will not take effect"
end

win:run()
