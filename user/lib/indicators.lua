-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What the machine is doing, for a bar to show: the sound, the network,
-- the battery, the devices on the USB ports, how busy the processor is and
-- how much memory is used, the clock, who is reaching this machine, and
-- whether the notifications have news.
--
--   local indicators = use("/Kosmos/Libraries/indicators.lua").new("deskbar")
--   indicators.listen()            -- on the bar's own second
--   indicators.heard.level         -- what a repaint draws from
--
-- **It was the Deskbar's, and moved rather than copied** (`docs/pewter.md`
-- P3): Pewter's menu bar shows the same things, and a second copy of how
-- to read the battery is a second thing to be wrong. Each bar draws in its
-- own manner; what is read, and how, is here once.
--
-- **What the bar last heard, which is all it draws from** (`testing.md`
-- 18.345; Diego, 2 October: "all that needs to be programmed async so it
-- does not wait or hang waiting for network or anything").
--
-- The draw asked five servers on every repaint - the clock, the sound, the
-- network, the battery, the processor and memory - so a repaint was only
-- as quick as the slowest of them. On 2 October the slowest was the network
-- stack, held four seconds and more at a time by a card that would not
-- send, and the bar's first picture came 23 seconds late, its menu with it:
-- a click is a repaint too. Now the questions are asked on the bar's own
-- second (`listen`, from its tick), a repaint draws what was heard, and an
-- indicator nobody has answered yet is a gap - the honest picture of "not
-- yet", as the meters already were.
--
-- Asking is still a call, and a server that never answered would still hold
-- the tick; that is what a call with a deadline is for (`roadmap.md`), and
-- a server that answers at once is the rule the network stack's card driver
-- was brought back to the same day.
--
-- **Any of these may be unanswerable, and that is not an error.** A
-- capability this process was not granted means the device is not in its
-- namespace at all - `/Devices/audio` is not denied, it is absent - so the
-- honest answer is "no indicator" rather than a picture of silence or a
-- crossed-out aerial. Each is wrapped in `pcall`, because a library that
-- reaches a server it cannot see raises rather than returning nil, and a
-- bar that stops drawing because the sound card is missing would be a
-- worse failure than no speaker.

local clock = use("/Kosmos/Libraries/clock.lua")
local notify = use("/Kosmos/Libraries/notify.lua")
local netprogram = use("/Kosmos/Libraries/netprogram.lua")

local M = {}

local audio_lib = nil

do
  local ok, got = pcall(use, "/Kosmos/Libraries/audio.lua")

  audio_lib = ok and got or nil
end

-- The master gain, 0 to 256, and whether it is muted; nil when there is
-- no audio to ask about.
local function volume_now()
  if not audio_lib then return nil end

  local ok, stats = pcall(audio_lib.stats)

  if not ok or not stats or not stats.master then return nil end

  return stats.master, stats.master_muted
end

--
-- **The devices on the USB ports**, from the USB driver's list (`usb.lua`,
-- `roadmap.md` 5q; Diego: "a usb top bar indicator that upon clicking
-- shows all things connected and detected on the usb ports"): how many,
-- and how many of them Kosmos drives - the icon counts every one, as he
-- chose, and This Machine says which is which. nil where no USB driver
-- answers, and then nothing is drawn.
--
local have_usb, usb = pcall(use, "/Kosmos/Libraries/usb.lua")

local function usb_now()
  if not have_usb then return nil end

  local ok, list = pcall(usb.devices)

  if not ok or type(list) ~= "table" then return nil end

  local driven = 0

  for _, d in ipairs(list) do if d.driven then driven = driven + 1 end end

  return { count = #list, driven = driven }
end

-- The battery, from `/Devices/battery`, or nil when the machine reads none -
-- the devices server lists that node only when the board has a reading.
local function battery_now()
  local ok, b = pcall(fs.read, "/Devices/battery")

  if not ok or type(b) ~= "table" or b.present ~= 1 or not b.percent then
    return nil
  end

  return b
end

--
-- A card, not merely a stack: the stack answers `net_info` with no card to
-- drive, and says so in `card`. This read only that the stack answered, so
-- the picture was drawn on every PC, card or not - found on 27 September
-- by the x86 suite's first machine with `-nic none`, once "offline" was a
-- thing the bar says (`roadmap.md` 6zl).
--
-- `fs.net_info("/Network")` is what `network.lua` asks, and it is the only
-- thing that answers: there is no `/Devices/net`. The network is reached
-- through the capability rather than a device file, which is why `needs
-- network` is the whole of the access check.
--
local function network_now()
  local ok, info = pcall(fs.net_info, "/Network")

  if not ok or type(info) ~= "table" or not info.card then return nil end

  return info
end

local function memory_now()
  local ok, m = pcall(fs.read, "/Devices/memory")

  if not ok or type(m) ~= "table" or not m.total_mb then return nil end

  return m.total_mb - m.free_mb, m.total_mb
end

-- The battery's line glyph: the bolt while it charges, else how full.
function M.battery_glyph(bat)
  return bat.state == "charging" and "battery-charging"
         or (bat.percent >= 80 and "battery-full")
         or (bat.percent >= 30 and "battery-medium")
         or "battery-low"
end

--
-- One bar's indicators: `who` is the name it says things under. Each has
-- its own `heard`, and its own last reading of the processor - busy is a
-- *difference* between two readings, so the first pass has nothing to say
-- and reports nothing rather than nought, which would be a bar claiming an
-- idle machine before it had looked.
--
function M.new(who)
  local self = { heard = {}, battery_glyph = M.battery_glyph }
  local heard = self.heard
  local last_idle, last_busy = nil, nil

  -- How busy the machine is, as a percentage: `monitor.lua`'s reading,
  -- from `/Devices/kernel`'s idle and busy ticks.
  local function load_now()
    local ok, k = pcall(fs.read, "/Devices/kernel")

    if not ok or type(k) ~= "table" then return nil end

    local pct = nil

    if last_idle then
      local di = (k.idle_ticks or 0) - last_idle
      local db = (k.busy_ticks or 0) - last_busy

      if di + db > 0 then pct = (db * 100) // (di + db) end
    end

    last_idle, last_busy = k.idle_ticks, k.busy_ticks

    return pct
  end

  --
  -- **The notifications' history, from the clock** (`roadmap.md`,
  -- *Notifications*, step 2): a press on the clock opens it - the
  -- `notifications` program's panel - and a dot beside the date says
  -- something has arrived since it was last opened. What has arrived is the
  -- server's newest number, asked on the bar's second with the rest; what
  -- was looked at is the newest when the panel was last opened, or when
  -- this bar started, so a bar starting again does not call old news new.
  --
  local looked = nil

  function self.open_history()
    looked = heard.newest or looked
    fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/notifications.lua",
                             args = "panel" })
    print(who .. ": the notifications' history asked for")
  end

  function self.news()
    return heard.newest and looked and heard.newest > looked
  end

  function self.listen()
    heard.at = sys.ticks()
    heard.newest = notify.newest()
    looked = looked or heard.newest
    heard.now = clock.now()
    heard.level, heard.muted = volume_now()
    heard.network = network_now() and "wired" or "offline"
    heard.battery = battery_now()
    heard.usb = usb_now()
    heard.busy = load_now()
    heard.used, heard.total = memory_now()

    -- Who is reaching this machine: VNC's viewers and Telnet's sessions,
    -- from what each server publishes for the Servers window (`netprogram`).
    local vnc, telnet = netprogram.status("vncd"), netprogram.status("telnetd")

    heard.vnc = vnc and #(vnc.viewers or {}) or nil
    heard.telnet = telnet and #(telnet.sessions or {}) or nil
  end

  return self
end

return M
