-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Devices
-- kosmos: name About This Machine
-- kosmos: section system
-- What this machine turned out to be.
--
-- **Every device, driven or not, as a card** (`docs/machine.md` M3, drawn
-- in `docs/machine.html`; Diego, 18 October 2026: "lets redesign the about
-- this machine app to show all hardware components detected, driven and
-- not driven in a visual and nice way"). The machine in a header with
-- three counts; every device a card in its group - drawn as what it is,
-- named, placed, and a pill for whether Kosmos drives it and with what;
-- By connection the same devices as a tree from the processor down; a
-- filter for what has no driver. The list is `devices.lua`'s, the one
-- `lsdev` prints, asked again every two seconds so a stick plugged in
-- shows.
--
--   machine          the cards
--   machine tree     By connection first
--   machine report   the full report first
--
-- **The full report stays**, behind a button: the text this window was
-- until 0.11.278, which is what a bug report wants. It is below, and
-- printed when there is no window manager.
--
-- The boot log answers this question once, in twelve stages, on a screen
-- that is gone by the time anybody wants it. `lsdev` at the prompt
-- answers it again in a listing. This is the same answer as a window you
-- can leave open, which is what BeOS had and called it that.
--
-- **Every line here is read, never assumed.** The nodes are the ones every
-- other program reads - /Devices/cpu, /Devices/memory, /Devices/screen, /Devices/keyboard,
-- /Devices/clock, /Devices/kernel - plus `sys.disk`, `sys.net` and `sys.info` for
-- the three devices that answer through their own servers rather than
-- through /Devices. Nothing is hardcoded, and where the machine cannot be
-- asked, the last section says so by name rather than leaving a gap.
--
-- That last part is the point of a window like this. A hardware inventory
-- that quietly omits what it does not know is worse than none: it reads
-- as "this machine has no such thing" when it means "nobody asked". So
-- the memory's speed, the graphics accelerator and the USB tree are
-- listed as unanswerable, with the reason.

local ui = use("/Kosmos/Libraries/ui.lua")
local hardware = use("/Kosmos/Libraries/hardware.lua")
local devices = use("/Kosmos/Libraries/devices.lua")

local START = use("/Kosmos/Libraries/files.lua").words(args, 2)[1]
local W, H = 980, 760

-- The report alone opens at the size it always had; the cards at what the
-- screen has room for, as Preferences sizes itself.
do
  local sc = fs.read("/Devices/screen") or {}

  if START == "report" then
    W, H = 700, 720
  elseif sc.width and sc.height then
    W = math.max(700, math.min(W, sc.width - 120))
    H = math.max(560, math.min(H, sc.height - 120))
  end
end

--------------------------------------------------------------------------
-- Reading the machine.
--
-- All of it up front and none of it again: this window is a description of
-- what was found, not a monitor. `sysmon` is the one that samples.
--------------------------------------------------------------------------

local b      = sys.build()
local cpu    = fs.read("/Devices/cpu")      or {}
local mem    = fs.read("/Devices/memory")   or {}
local screen = fs.read("/Devices/screen")
local keyb   = fs.read("/Devices/keyboard")
local clock  = fs.read("/Devices/clock")    or {}
local kern   = fs.read("/Devices/kernel")   or {}
local timer  = fs.read("/Devices/timer")    or {}
local info   = sys.info() or {}
local disk   = sys.disk()
local net    = sys.net()
local sb     = fs.read("/Home/.super")

--
-- **Plain text, in a column, and that is the whole change.**
--
-- This was `ui.text` blocks: a widget that wraps prose by splitting on
-- `%S+` and rejoining with single spaces, which reads well for a
-- paragraph and destroys every run of padding. So the labels could not be
-- aligned and the report was a picture of text rather than text.
--
-- One string of lines instead, shown in a read-only editor with its
-- gutter off. The editor is monospace and does not reflow, so a column is
-- a column - and the same string is what this program prints when there
-- is no window manager to open one, which is the only way to get it off
-- the machine as text, there being no clipboard in this system yet.
--
local lines = {}
local COL = 18                          -- where every value starts

local function out(text)
  lines[#lines + 1] = text or ""
end

local function head(text)
  if #lines > 0 then out("") end
  out(text)
  out(("-"):rep(#text))
end

-- A label in its own column, which the editor keeps and `ui.text` could
-- not. Long values wrap under the column rather than past the window.
local function row(label, value)
  local text = tostring(value)
  local left = ("%-" .. COL .. "s"):format(label)

  if #left + #text <= 78 then
    out(left .. text)
    return
  end

  -- Wrapped by hand, because nothing here is going to do it and a line
  -- that runs off the right edge is a line nobody reads.
  local room = 78 - COL
  local first = true

  for word in text:gmatch("%S+") do
    local cur = lines[#lines]

    if first or #cur + 1 + #word > COL + room then
      out((" "):rep(COL) .. word)
      if first then lines[#lines] = left .. word; first = false end
    else
      lines[#lines] = cur .. " " .. word
    end
  end
end

-- Present and absent are different sentences, and a machine that has no
-- card should say so rather than showing an empty value.
local function absent(what, why)
  row(what, why)
end

-- 1,234,567, as every count is written (`text.grouped`).
local commas = use("/Kosmos/Libraries/text.lua").grouped

--------------------------------------------------------------------------

--
-- **The machine's name as its firmware wrote it, then what the image was
-- built for.** Two facts, and this header ran them together as one for as
-- long as the second was the only one there was - so on a ThinkPad it opened
-- with "QEMU q35 x86-64".
--
local named = hardware.name(info)

out(named or b.platform or "this machine")
out((b.kernel or "Nebula") .. " " .. (b.version or "?") ..
    "   " .. (b.build or "?") .. "   " .. (b.date or "?"))

if named then
  out(("named by %s; the image was built for %s")
      :format(info.machine_source or "the firmware", b.platform or "?"))
else
  out(("not named by the firmware: %s")
      :format(info.machine_source or "no reason given"))
end

--------------------------------------------------------------------------
head("Processor")

-- `implementer`/`part`/`revision` are filled by whichever decoder in the
-- devices server matched `cpu_arch` - ARM's out of MIDR_EL1, x86's out of
-- CPUID. Both use the same three field names precisely so this does not
-- have to know which machine it is on.
if cpu.implementer then
  row("Model", ("%s %s %s"):format(cpu.implementer, cpu.part or "",
                                   cpu.revision or ""))
else
  row("Model", cpu.arch or "unknown")
end

row("Architecture", cpu.arch or "unknown")
--
-- Three numbers, because they are three questions, and `sysinfo` has the
-- long version: what the firmware says the machine has, how many processors
-- are running kernel code, and how many are given new threads. One number
-- when they agree, which on a machine given no boot option they do. A gap
-- between the first two is a processor that never arrived; between the last
-- two, `opt/kosmos/smp` asking for fewer.
--
-- It said "(SMP is being built)" beside any gap, long after it was built.
--
local present = info.cpus_present or 1
local online  = info.cpus_online or present
local in_use  = info.cpus or 1

if present == online and online == in_use then
  row("Cores", tostring(present))
else
  row("Cores", ("%d present, %d running the kernel, %d given threads")
               :format(present, online, in_use))
end

if cpu.counter_hz and cpu.counter_hz > 0 then
  row("Counter", ("%d MHz"):format(cpu.counter_hz // 1000000))
end
if cpu.cache_line then row("Cache line", cpu.cache_line .. " bytes") end
if cpu.pa_bits   then row("Addresses",  cpu.pa_bits .. "-bit physical") end
if cpu.el        then row("Privilege",  "the kernel runs at level " .. cpu.el) end

-- The feature bits, as one line of what is there rather than a column of
-- yes and no. Absent is said once at the end instead of six times.
local has, hasnt = {}, {}
for _, f in ipairs{ { "fp", "FP" }, { "simd", "SIMD" }, { "aes", "AES" },
                    { "sha1", "SHA1" }, { "sha2", "SHA2" },
                    { "crc32", "CRC32" }, { "atomics", "atomics" } } do
  local key, name = f[1], f[2]
  if cpu[key] ~= nil then
    if cpu[key] == 1 or cpu[key] == true then has[#has + 1] = name
    else hasnt[#hasnt + 1] = name end
  end
end
if #has > 0    then row("Has", table.concat(has, " ")) end
if #hasnt > 0  then row("Lacks", table.concat(hasnt, " ")) end

--------------------------------------------------------------------------
head("Memory")

row("Installed", (mem.total_mb or 0) .. " MB")
row("Free", (mem.free_mb or 0) .. " MB")
row("Pages", commas(mem.pages_total) .. " of " ..
             ((mem.page_size or 4096) // 1024) .. " KB")
if mem.base then row("RAM starts at", ("0x%x"):format(mem.base)) end

--------------------------------------------------------------------------
head("Display")

if screen then
  row("Resolution", ("%d x %d"):format(screen.width or 0, screen.height or 0))
  row("Colour", "32-bit XRGB")
  -- The pitch is worth showing precisely because it is not width * 4 here,
  -- and every drawing bug this system has had came from assuming it was.
  row("Bytes a row", ("%d  (%d x 4 is %d)"):format(
        screen.pitch or 0, screen.width or 0, (screen.width or 0) * 4))
  -- Where the pixels come from, as the board tells its own boot log. This
  -- said "ramfb" on every machine, and on the ThinkPad the screen is the
  -- loader's.
  row("Framebuffer", ((info.screen_source or "") ~= "") and info.screen_source
                     or "linear, and the board did not say from where")
else
  absent("Display", "none attached; this machine is serial-only")
end

--------------------------------------------------------------------------
head("Input")

if keyb then
  row("Keyboard", "present")
else
  absent("Keyboard", "none; input comes over the serial line")
end

-- The pointer is deliberately not here, and is in the last section
-- instead. `sys.pointer` answers nil both when the board has none and
-- when this process was not granted it - and a window running under the
-- window manager is never granted it, because the manager holds the
-- console. So the answer from here is always nil and would always read
-- "none", on a machine with a tablet attached. That is the exact failure
-- this window exists to avoid.

--------------------------------------------------------------------------
head("Storage")

if disk then
  row("Disk", ("%s sectors of %d bytes  (%d MB)"):format(
        commas(disk.sectors), disk.sector_size or 512,
        (disk.bytes or 0) // (1024 * 1024)))

  if sb and sb.present and sb.formatted then
    row("Filesystem", ("kfs version %d, %s blocks of %d bytes"):format(
          sb.version or 1, commas(sb.blocks), sb.block_size or 4096))
    row("Free", sb.free_blocks and (commas(sb.free_blocks) .. " blocks")
                or ("unknown; " .. tostring(sb.free_why)))
    row("Journal", "at block " .. tostring(sb.journal_at))
  elseif sb and sb.present then
    row("Filesystem", "none (" .. tostring(sb.why) .. ")")
  end
else
  absent("Disk", "none attached; /Home is in memory and will not survive")
end

--------------------------------------------------------------------------
head("Network")

--
-- **Two questions, and only one of them this program may ask.**
--
-- `sys.net` answers nil both when the board has no card and when the
-- caller was not granted one, and an ordinary application is never
-- granted it - so asking it alone reports "no card" on a machine with a
-- card in it, which is exactly the lie this window exists to avoid. It
-- said precisely that on the first run, next to a boot log that had found
-- the card.
--
-- `sysinfo` carries `net_mtu` and is not gated on a grant, because it is
-- the kernel describing the machine rather than handing anything over. So
-- *whether there is a card* comes from there, and the hardware address -
-- which is the part you need the grant for - is shown only when this
-- program actually holds it.
--
-- **And which card, from the bus rather than from the name of the one driver
-- this system has.** It said "virtio-net" whenever a card was driven, which
-- was true only because nothing else can be; on a machine with an Ethernet
-- controller and no driver for it, the branch below said "no card".
--
local driven, undriven = hardware.network(sys.bus())

if (info.net_mtu or 0) > 0 then
  row("Card", driven[1] and (driven[1].name .. " at " .. driven[1].place)
              or "one the bus did not list")
  row("MTU", tostring(info.net_mtu))

  if net then
    -- Six raw bytes, because formatting them is the caller's business and
    -- not the driver's.
    local mac = {}
    for i = 1, #(net.mac or "") do
      mac[#mac + 1] = ("%02x"):format(net.mac:byte(i))
    end
    row("Hardware address", table.concat(mac, ":"))
  else
    row("Hardware address", "not shown; this program was not granted the card")
  end

  row("Stack", "TCP/IP, in a process (see the Network preference)")
elseif undriven[1] then
  for _, card in ipairs(undriven) do
    row("Card", card.name .. " at " .. card.place .. ", with no driver")
  end
else
  absent("Network", "no card; this machine is on its own")
end

--------------------------------------------------------------------------
head("Audio")

--
-- **Which controller, from the bus**, for the reason the Network section
-- gives: this said "virtio-sound" whenever there was sound, which was true
-- only while nothing else could make any. A controller a driver took and
-- got no sound out of is a third answer, and the ThinkPad's is that one.
--
local sound, silent = hardware.audio(sys.bus())

if (info.audio_rate or 0) > 0 then
  row("Device", sound[1] and (sound[1].name .. " at " .. sound[1].place)
                or "one the bus did not list")
  row("Format", ("%d Hz, %d channel%s, 16-bit"):format(
        info.audio_rate, info.audio_channels or 2,
        (info.audio_channels == 1) and "" or "s"))
  row("Period", ("%d bytes, %d of them"):format(
        info.audio_period or 0, info.audio_periods or 0))
elseif sound[1] then
  row("Device", sound[1].name .. " at " .. sound[1].place
                .. ", driven, and nothing playing through it")
elseif silent[1] then
  for _, card in ipairs(silent) do
    row("Device", card.name .. " at " .. card.place .. ", with no driver")
  end
else
  absent("Audio", "no device; this machine is silent")
end

--------------------------------------------------------------------------
head("Graphics")

local devs, counts = devices.read()

do
  local any = false

  for _, d in ipairs(devs) do
    if d.group == "Graphics" then
      any = true
      row(d.driven and "Driven" or "No driver", d.name .. " at " .. (d.place or "?")
          .. (d.driver and (", by " .. d.driver) or "")
          .. (d.detail and ("; " .. d.detail) or ""))
    end
  end

  if not any then absent("GPU", "none on the bus") end
end

--------------------------------------------------------------------------
head("USB")

do
  local any = false

  for _, d in ipairs(devs) do
    if d.id:match("^usb:") then
      any = true
      row(d.driven and "Driven" or "No driver", d.name .. ", " .. (d.place or "?")
          .. (d.detail and ("; " .. d.detail) or ""))
    end
  end

  if not any then absent("USB devices", "nothing plugged in, or no USB driver") end
end

--------------------------------------------------------------------------
head("Clock and timer")

row("Tick", (timer.hz or info.tick_hz or 0) .. " Hz, the scheduler's")
if clock.epoch and clock.epoch > 0 then
  row("Wall clock", "set, from the board's RTC (UTC)")
else
  absent("Wall clock", "not set; there is no battery-backed clock here")
end

--------------------------------------------------------------------------
head("Kernel")

row("Threads", (kern.threads or 0) .. " of " .. (kern.threads_max or 0))
row("Processes", (kern.processes or 0) .. " of " .. (kern.processes_max or 0))
row("Endpoints", (kern.endpoints or 0) .. " of " .. (kern.endpoints_max or 0))
row("Address spaces", (kern.spaces or 0) .. " of " .. (kern.spaces_max or 0))
out("")
out("  Pools, because the kernel keeps its objects in no heap: each grows")
out("  by a slab when it is full and never gives one back, up to a")
out("  ceiling the machine's memory sets.")

--------------------------------------------------------------------------
head("On the bus")

--
-- **The section that answers a question none of the others can.**
--
-- Every row above reports presence, and presence cannot tell a machine
-- with no sound card from one whose card nothing drives - both simply
-- have no Audio section. This is what the board's own enumeration found,
-- driven or not, so a device with no driver is a line rather than a
-- silence.
--
-- The names are here and not in the kernel, which decodes none of these
-- numbers: the same division `cpu_raw` draws, and for the same reason -
-- turning 0x1af4:0x1041 into "virtio-net" is a table, and a table that
-- lives in a driver is a driver deciding how somebody else prints.
--
-- The tables are `/Kosmos/Libraries/hardware.lua`'s, so that a card this listing names is
-- called the same thing by `neofetch` and the Network preference.
local VENDORS = hardware.VENDORS
local CLASSES = hardware.CLASSES
local VIRTIO  = hardware.VIRTIO

local bus, found = sys.bus()

found = found or (bus and #bus) or 0

if bus and #bus > 0 then
  local undriven = 0

  --
  -- **A column each: where, whether anything drives it, and what it is.**
  -- The status trailed the name, and on the ThinkPad the names ran to the
  -- edge. The class code goes out whole - class, subclass and interface -
  -- because what those numbers mean is a table this project's references do
  -- not have, and a number is better than a name guessed at.
  --
  for _, d in ipairs(bus) do
    local name

    if d.class == 0 then
      -- A device-tree window: the type is the whole identity.
      name = VIRTIO[d.device] or ("virtio type " .. d.device)
    else
      name = ("%s %s %06x (%04x:%04x)"):format(hardware.vendor(d),
                                              CLASSES[d.class >> 16] or "device",
                                              d.class, d.vendor, d.device)
    end

    if not d.claimed then undriven = undriven + 1 end

    out(("%-9s %-10s %s"):format(hardware.place(d),
                                 d.claimed and "driven" or "no driver", name))
  end

  out("")
  out(("  %d device%s found, %d driven, %d without a driver."):format(
      found, (found == 1) and "" or "s", #bus - undriven, undriven))

  if found > #bus then
    out(("  The first %d are listed; the kernel's answer has room for no"
         .. " more."):format(#bus))
  end

  if undriven > 0 then
    out("  A device with no driver is not a fault. It is hardware this")
    out("  system has not been taught.")
  end
else
  absent("Bus", "this board reports no enumerable bus")
end

--------------------------------------------------------------------------
head("Heat")

if info.package_c then
  row("Temperature", ("%d °C, the package; it slows to cool at %d °C"):format(
                     info.package_c, info.limit_c or 0))
else
  absent("Temperature", "this processor's sensor is not one the kernel reads")
end

--------------------------------------------------------------------------
head("What this machine cannot be asked")

out("  Listed rather than left out, because a blank line reads as")
out("  \"there is none\" when it means \"nobody asked\".")
out("")

absent("Memory speed", "SMBIOS is read for the machine's name and nothing " ..
                       "else; the firmware knows and is not asked")
absent("Slots and DIMMs", "the same - a count of modules is in that table, " ..
                          "unread")
absent("Pointer", "sys.pointer answers nil both for a board with none and " ..
                  "for a program not granted one, and a window never is - " ..
                  "the window manager holds the console. sysinfo carries " ..
                  "has_keyboard and no has_pointer to ask instead")

--------------------------------------------------------------------------
-- Where it goes.
--
-- **The report, the same string either way**: printed when there is no
-- window manager - how the text leaves a machine at a prompt with a serial
-- line attached - and the window's Full report otherwise, a read-only
-- editor over it, monospace so the columns survive, and selectable, so
-- Control-C sends it to somebody else.
--------------------------------------------------------------------------

local report = table.concat(lines, "\n")
local theme, L = ui.theme, ui.layout

local win = ui.window{ title = "This Machine", w = W, h = H, x = 100, y = 60, header = true }

if not win then
  print(report)
  return
end

local counter_hz = cpu.counter_hz or 62500000

--------------------------------------------------------------------------
-- The state: the devices, how they are shown, and which.
--------------------------------------------------------------------------

local by_tree = START == "tree"
local showing_report = START == "report"
local FILTERS = { "All", "Driven", "No driver" }
local filter = 1

local function wanted(d)
  return filter == 1 or (filter == 2 and d.driven) or (filter == 3 and not d.driven)
end

--------------------------------------------------------------------------
-- The header: the window's name, and the report behind a button.
--------------------------------------------------------------------------

local CARD_H, PIC, GAP = 118, 44, 10
local HERO_H, BAR_H = 104, 48
local DRIVEN_C, NONE_C, BUSY_C = 0xff4fb878, 0xff8b93a3, 0xff4f9fe6

local cards, editor, hint, view_seg, filter_seg
local toggle

local report_button = ui.button{ text = showing_report and "Devices" or "Full report",
                                 on_click = function() toggle() end }

win:add(ui.header{ x = 0, y = 0, w = W, title = "This Machine",
                   sub = (b.kernel or "Nebula") .. " " .. (b.version or "?"),
                   right = { report_button }, title_bar = true })

--------------------------------------------------------------------------
-- The machine in a header: what it is, and three counts.
--------------------------------------------------------------------------

local hero = ui.view{ x = 0, y = L.head, w = W, h = HERO_H,
                      follow = { left = true, right = true, top = true } }

-- A computer, drawn as the cards draw a device: a screen on its stand.
local function computer(g, x, y, s)
  g:fill_round(x, y + s // 10, s, s * 6 // 10, 0xff3c4556, 6)
  g:fill_round(x + 4, y + s // 10 + 4, s - 8, s * 6 // 10 - 8, 0xff1f3554, 3)
  g:fill(x + s * 4 // 10, y + s * 7 // 10, s * 2 // 10, s // 8, 0xff6b768a)
  g:fill_round(x + s * 3 // 10, y + s * 8 // 10, s * 4 // 10, s // 12, 0xff8e98aa, 2)
end

function hero:draw(g)
  local x = L.page_side
  local s = 64

  g:fill(0, 0, self.w, self.h, theme.raised)
  g:fill(0, self.h - 1, self.w, 1, theme.line_soft)
  computer(g, x, (self.h - s) // 2, s)

  local tx = x + s + 18
  local function trim(t) return (tostring(t or "")):match("^%s*(.-)%s*$") end
  local maker, product, version = trim(info.machine_vendor), trim(info.machine_product),
                                  trim(info.machine_version)

  --
  -- **The model is the product, except on a Lenovo**, which writes its
  -- machine type in the product - "10J0S1CK00" - and the name a person
  -- knows it by in the version: "ThinkCentre M700". QEMU's version is its
  -- machine type's, "pc-q35-11.1", and goes with the facts.
  --
  local model, other = product, version

  if maker:upper() == "LENOVO" and version ~= "" then model, other = version, product end
  if model == "" then model, other = b.platform or "This machine", "" end

  local facts = ("%sKosmos %s, %s on %s%s"):format(other ~= "" and (other .. " \u{b7} ") or "",
                 b.version or "?", b.kernel or "Nebula", cpu.arch or "?",
                 named and (" \u{b7} named by " .. (info.machine_source or "the firmware")) or "")
  local counts_w = 3 * 104
  local room = self.w - tx - counts_w - L.page_side - 12
  local y = (self.h - gfx.height("label") - gfx.height(ui.sized("title", 24)) - gfx.height()) // 2

  if maker ~= "" then g:text(tx, y, ui.fitted(maker:upper(), room, "label"), theme.text_dim, theme.raised, "label") end
  y = y + gfx.height("label")
  g:text(tx, y, ui.fitted(model, room, ui.sized("title", 24)), theme.text, theme.raised, ui.sized("title", 24))
  y = y + gfx.height(ui.sized("title", 24))
  g:text(tx, y, ui.fitted(facts, room, "ui"), theme.text_dim, theme.raised, "ui")

  -- The three counts against the right.
  local cx = self.w - L.page_side - counts_w + 8
  local big = ui.sized("title", 22)

  for _, c in ipairs { { counts.total, "devices", theme.text }, { counts.driven, "driven", DRIVEN_C },
                       { counts.undriven, "no driver yet", NONE_C } } do
    local bw, bh = 96, gfx.height(big) + gfx.height("label") + 14
    local by = (self.h - bh) // 2

    g:fill_round(cx, by, bw, bh, theme.window, 9)
    g:frame_round(cx, by, bw, bh, theme.line_soft, 9)

    local n = tostring(c[1])

    g:text(cx + (bw - gfx.measure(n, big)) // 2, by + 6, n, c[3], theme.window, big)
    g:text(cx + (bw - gfx.measure(c[2], "label")) // 2, by + 6 + gfx.height(big), c[2],
           theme.text_dim, theme.window, "label")
    cx = cx + 104
  end
end

win:add(hero)

--------------------------------------------------------------------------
-- The bar: By kind or By connection, and All, Driven or No driver.
--------------------------------------------------------------------------

local bar = ui.view{ x = 0, y = L.head + HERO_H, w = W, h = BAR_H,
                     follow = { left = true, right = true, top = true } }

function bar:draw(g)
  g:fill(0, self.h - 1, self.w, 1, theme.line_soft)
end

win:add(bar)

local said = nil

local function say()
  -- What is shown, said when it changes, for `tools/run_machine.py`.
  local parts, group, n = {}, nil, 0

  for _, d in ipairs(devs) do
    if wanted(d) then
      if d.group ~= group then
        if group then parts[#parts + 1] = ("%s %d"):format(group, n) end
        group, n = d.group, 0
      end
      n = n + 1
    end
  end

  if group then parts[#parts + 1] = ("%s %d"):format(group, n) end

  local line = ("machine: %s, %s - %d devices, %d driven, %d with no driver; %s"):format(
               showing_report and "the report" or (by_tree and "by connection" or "by kind"),
               FILTERS[filter]:lower(), counts.total, counts.driven, counts.undriven,
               table.concat(parts, ", "))

  if line ~= said then
    said = line
    print(line)
  end
end

view_seg = ui.segments{ x = L.page_side, y = L.head + HERO_H + 10,
                        items = { "By kind", "By connection" }, on = by_tree and 2 or 1,
                        on_change = function(_, i)
                          by_tree = (i == 2)
                          cards.top = 0
                          say()
                          win:paint()
                        end }

filter_seg = ui.segments{ x = L.page_side + 250, y = L.head + HERO_H + 10,
                          items = FILTERS, on = filter,
                          on_change = function(_, i)
                            filter = i
                            cards.top = 0
                            say()
                            win:paint()
                          end }

win:add(view_seg)
win:add(filter_seg)

--------------------------------------------------------------------------
-- The devices: cards by kind, or a tree by connection.
--------------------------------------------------------------------------

local TOP = L.head + HERO_H + BAR_H

cards = ui.view{ x = 0, y = TOP, w = W, h = H - TOP,
                 follow = { left = true, right = true, top = true, bottom = true } }
cards.top = 0

local small = ui.sized("label", 13)
local kind_face = ui.sized("label", 12, "semibold")

-- A card is as tall as what is in it: the kind, the name, the place, the
-- pill and the line of what it is doing.
CARD_H = 12 + gfx.height(kind_face) + gfx.height("label") + gfx.height(small) + 6
         + gfx.height(small) + 4 + 8 + gfx.height("ui") + 12

-- A pill: words in a rounded rule, in their colour.
local function pill(g, x, y, text, colour, ground)
  local w = gfx.measure(text, small) + 16
  local h = gfx.height(small) + 4

  g:frame_round(x, y, w, h, colour, h // 2)
  g:text(x + 8, y + 2, text, colour, ground, small)

  return w
end

local function state_pill(g, x, y, d, ground)
  local w = pill(g, x, y, d.driven and ("Driven" .. (d.driver and (" by " .. d.driver) or ""))
                          or "Detected, no driver",
                 d.driven and DRIVEN_C or NONE_C, ground)

  if d.busy then w = w + 6 + pill(g, x + w + 6, y, d.busy, BUSY_C, ground) end

  return w
end

local function card(g, x, y, w, d)
  local ground = d.driven and theme.raised or theme.window

  g:fill_round(x, y, w, CARD_H, ground, L.card_r)
  g:frame_round(x, y, w, CARD_H, theme.line_soft, L.card_r)

  ui.device_picture(g, x + 14, y + 14, PIC, d.driven and d.picture or nil, d.label, d.maker)

  local tx, room = x + 14 + PIC + 12, w - (14 + PIC + 12) - 14
  local ty = y + 12

  g:text(tx, ty, d.group:upper(), theme.text_dim, ground, kind_face)
  ty = ty + gfx.height(kind_face)
  g:text(tx, ty, ui.fitted(d.name, room, "label"), d.driven and theme.text or theme.text_dim, ground, "label")
  ty = ty + gfx.height("label")
  g:text(tx, ty, ui.fitted(d.place or "", room, small), theme.text_dim, ground, small)
  ty = ty + gfx.height(small) + 6
  state_pill(g, tx, ty, d, ground)
  ty = ty + gfx.height(small) + 10

  if d.detail then
    g:text(tx, ty, ui.fitted(d.detail, room, "ui"), theme.text_dim, ground, "ui")
  end
end

function cards:draw(g)
  local x0 = L.page_side
  local inner = self.w - 2 * L.page_side
  local y = 14 - self.top

  if by_tree then
    local roots, children = devices.tree(devs)
    local row_h = 34

    -- Whether a device or anything under it passes the filter: a parent
    -- stays to hold a child that does.
    local function any(d)
      if wanted(d) then return true end
      for _, c in ipairs(children[d.id] or {}) do if any(c) then return true end end
      return false
    end

    local function node(d, depth)
      if not any(d) then return end

      local x = x0 + depth * 30
      local ground = theme.window

      if depth > 0 then
        g:fill(x - 18, y + row_h // 2, 14, 1, theme.line_soft)
      end

      ui.device_picture(g, x, y + 3, 28, d.driven and d.picture or nil, d.label, d.maker)

      local tx = x + 38
      local name = d.name

      g:text(tx, y + (row_h - gfx.height("label")) // 2, name,
             d.driven and theme.text or theme.text_dim, ground, "label")

      local px = tx + gfx.measure(name, "label") + 10

      g:text(px, y + (row_h - gfx.height(small)) // 2, ui.fitted(d.place or "", math.max(0, x0 + inner - 300 - px), small),
             theme.text_dim, ground, small)

      local right = x0 + inner
      local pw = gfx.measure(d.driven and ("Driven" .. (d.driver and (" by " .. d.driver) or "")) or "Detected, no driver", small) + 16
                 + (d.busy and (gfx.measure(d.busy, small) + 22) or 0)

      state_pill(g, right - pw, y + (row_h - gfx.height(small) - 4) // 2, d, ground)

      local top_y = y
      local middle = y + row_h // 2
      y = y + row_h

      for _, v in ipairs(d.volumes or {}) do
        g:text(tx + 8, y, v, theme.text_dim, ground, small)
        y = y + gfx.height(small) + 4
      end

      local kids = children[d.id] or {}

      if #kids > 0 then
        local last

        for _, c in ipairs(kids) do last = node(c, depth + 1) or last end

        -- The rule down the children's left, to the middle of the last.
        if last then
          g:fill(x + 14, top_y + row_h - 4, 1, math.max(0, last - (top_y + row_h - 4)), theme.line_soft)
        end
      end

      return middle
    end

    for _, r in ipairs(roots) do node(r, 0) end
  else
    --
    -- **Groups side by side where they fit**: a group of one or two takes
    -- the columns after the last one's on the same row, its name over its
    -- own first card, so a machine of many small groups is not a column of
    -- single cards down the left. A group longer than the row starts one
    -- and wraps.
    --
    local cols = math.max(1, (inner + GAP) // (280 + GAP))
    local cw = (inner - (cols - 1) * GAP) // cols
    local head_h = gfx.height(kind_face) + 8
    local band = head_h + CARD_H + 16
    local groups, order = {}, {}

    for _, d in ipairs(devs) do
      if wanted(d) then
        if not groups[d.group] then
          groups[d.group] = {}
          order[#order + 1] = d.group
        end
        table.insert(groups[d.group], d)
      end
    end

    local col, any = 0, false

    for _, name in ipairs(order) do
      local list = groups[name]

      if col > 0 and col + #list > cols then
        col = 0
        y = y + band
      end

      g:text(x0 + col * (cw + GAP), y, name:upper(), theme.text_dim, theme.window, kind_face)

      for _, d in ipairs(list) do
        if col == cols then
          col = 0
          y = y + band
        end

        card(g, x0 + col * (cw + GAP), y + head_h, cw, d)
        col = col + 1
        any = true
      end
    end

    if any then y = y + band end

    local group = any or nil

    if not group then
      g:text(x0, y, filter == 3 and "Every device on this machine has a driver."
                    or "Nothing to show.", theme.text_dim, theme.window, "ui")
    end
  end

  self.content_h = y + self.top + L.page_foot

  -- Where the two choices are on the screen, said once, for the suite's
  -- clicks (`tools/run_machine.py`), as Shortcuts says its links.
  if not self.placed and win.origin_x then
    self.placed = true
    for _, sg in ipairs { view_seg, filter_seg } do
      print(("machine: %s at %d,%d %dx%d"):format(table.concat(sg.items, " | "),
            win.origin_x + sg.x, win.origin_y + sg.y, sg.w, sg.h))
    end
  end
end

function cards:wheel(n)
  local most = math.max(0, (self.content_h or 0) - self.h)

  self.top = math.max(0, math.min(most, self.top - n * 3 * 40))
  return true
end

win:add(cards)

--------------------------------------------------------------------------
-- The full report, behind its button.
--
-- **Pinned to all four edges**, like the Log window, so a bigger window is
-- a bigger report. Read-only and selectable: drag to select, Control-C.
--------------------------------------------------------------------------

editor = ui.editor{ x = 8, y = L.head + 8, w = W - 32, h = H - L.head - 74,
                    text = report, gutter = false, read_only = true,
                    follow = { left = true, right = true, top = true, bottom = true } }

-- Said rather than left to be discovered. There are no modifier keys on
-- this machine, so the clipboard lives behind the window manager's prefix,
-- and a prefix nobody mentions is a feature nobody has.
hint = ui.label{ x = 10, y = H - 60,
                 text = "Drag to select, or Control-A for all.   Control-C copies it.",
                 color = "text_dim", follow = { left = true, bottom = true } }

win:add(editor)
win:add(hint)

local function show()
  for _, v in ipairs { hero, bar, view_seg, filter_seg, cards } do v.hidden = showing_report end
  editor.hidden, hint.hidden = not showing_report, not showing_report
  report_button.text = showing_report and "Devices" or "Full report"
end

toggle = function()
  showing_report = not showing_report
  show()
  say()
  win:paint()
end

show()

--------------------------------------------------------------------------
-- Live: the list asked again every two seconds, so a stick plugged in
-- appears and one pulled out goes.
--------------------------------------------------------------------------

local watcher = ui.view{ x = 0, y = 0, w = 0, h = 0 }

local function signature(list)
  local out = {}

  for _, d in ipairs(list) do
    out[#out + 1] = d.id .. (d.driven and "+" or "-") .. (d.detail or "") .. (d.busy or "")
  end

  return table.concat(out, "|")
end

local was = signature(devs)

function watcher:tick()
  local now, n = devices.read()
  local sig = signature(now)

  if sig ~= was then
    was, devs, counts = sig, now, n
    say()
    win:paint()
  end
end

win:add(watcher)
win.tick_every = counter_hz * 2

say()
win:run()
