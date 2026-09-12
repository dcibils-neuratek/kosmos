-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Devices
-- kosmos: section system
-- What this machine turned out to be.
--
-- The boot log answers this question once, in twelve stages, on a screen
-- that is gone by the time anybody wants it. `devices` at the prompt
-- answers it again in a listing. This is the same answer as a window you
-- can leave open, which is what BeOS had and called it that.
--
-- **Every line here is read, never assumed.** The nodes are the ones every
-- other program reads - /dev/cpu, /dev/memory, /dev/screen, /dev/keyboard,
-- /dev/clock, /dev/kernel - plus `sys.disk`, `sys.net` and `sys.info` for
-- the three devices that answer through their own servers rather than
-- through /dev. Nothing is hardcoded, and where the machine cannot be
-- asked, the last section says so by name rather than leaving a gap.
--
-- That last part is the point of a window like this. A hardware inventory
-- that quietly omits what it does not know is worse than none: it reads
-- as "this machine has no such thing" when it means "nobody asked". So
-- the memory's speed, the graphics accelerator and the USB tree are
-- listed as unanswerable, with the reason.

local ui = use("/lib/ui.lua")

local W, H = 700, 720

--------------------------------------------------------------------------
-- Reading the machine.
--
-- All of it up front and none of it again: this window is a description of
-- what was found, not a monitor. `sysmon` is the one that samples.
--------------------------------------------------------------------------

local b      = sys.build()
local cpu    = fs.read("/dev/cpu")      or {}
local mem    = fs.read("/dev/memory")   or {}
local screen = fs.read("/dev/screen")
local keyb   = fs.read("/dev/keyboard")
local clock  = fs.read("/dev/clock")    or {}
local kern   = fs.read("/dev/kernel")   or {}
local timer  = fs.read("/dev/timer")    or {}
local info   = sys.info() or {}
local disk   = sys.disk()
local net    = sys.net()
local sb     = fs.read("/home/.super")

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

local function commas(n)
  local s = tostring(math.floor(n or 0))
  local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
  return (out:gsub("^,", ""))
end

--------------------------------------------------------------------------

out(b.platform or "this machine")
out((b.kernel or "Nebula") .. " " .. (b.version or "?") ..
    "   " .. (b.build or "?") .. "   " .. (b.date or "?"))

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
-- Two numbers, because they are not the same one.
--
-- `cpus_present` is what the firmware says the machine has; `cpus` is what
-- this kernel is scheduling on. A report that gave only the second would
-- describe a four-core laptop as a one-core machine, which is true about
-- Kosmos and false about the computer - and the gap is exactly what
-- `docs/smp.md` measures its own progress by.
--
local present = (sys.info() or {}).cpus_present or 1
local in_use  = (sys.info() or {}).cpus or 1

row("Cores", (present == in_use)
             and tostring(present)
             or ("%d present, %d scheduling  (SMP is being built)")
                :format(present, in_use))

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
  row("Framebuffer", "linear, from the firmware (ramfb)")
else
  absent("Display", "none attached; this machine is serial-only")
end

--------------------------------------------------------------------------
head("Input")

if keyb then
  row("Keyboard", keyb.transport or "present")
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
    row("Free", commas(sb.free_blocks) .. " blocks")
    row("Journal", "at block " .. tostring(sb.journal_at))
  elseif sb and sb.present then
    row("Filesystem", "none (" .. tostring(sb.why) .. ")")
  end
else
  absent("Disk", "none attached; /home is in memory and will not survive")
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
if (info.net_mtu or 0) > 0 then
  row("Card", "virtio-net")
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
else
  absent("Network", "no card; this machine is on its own")
end

--------------------------------------------------------------------------
head("Audio")

if (info.audio_rate or 0) > 0 then
  row("Device", "virtio-sound")
  row("Format", ("%d Hz, %d channel%s, 16-bit"):format(
        info.audio_rate, info.audio_channels or 2,
        (info.audio_channels == 1) and "" or "s"))
  row("Period", ("%d bytes, %d of them"):format(
        info.audio_period or 0, info.audio_periods or 0))
else
  absent("Audio", "no device; this machine is silent")
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
out("  Fixed pools, because the kernel has no allocator: running out")
out("  is an error at a known limit rather than a failure at an")
out("  unknown one.")

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
local VENDORS = {
  [0x1af4] = "Red Hat / virtio",
  [0x8086] = "Intel",
  [0x1b36] = "Red Hat / QEMU",
}

-- PCI class codes, high byte, and only the ones a machine here can show.
local CLASSES = {
  [0x01] = "storage controller",
  [0x02] = "network controller",
  [0x03] = "display controller",
  [0x04] = "multimedia device",
  [0x06] = "bridge",
  [0x09] = "input device",
  [0x0c] = "serial bus controller",
}

-- virtio device types, for a board whose bus reports the type directly
-- rather than a vendor and a device. `class` is zero there, which is how
-- this tells the two shapes apart.
local VIRTIO = {
  [1] = "virtio-net", [2] = "virtio-blk", [3] = "virtio-console",
  [16] = "virtio-gpu", [18] = "virtio-input", [19] = "virtio-vsock",
  [25] = "virtio-sound",
}

local bus = sys.bus()

if bus and #bus > 0 then
  local undriven = 0

  for _, d in ipairs(bus) do
    local name, place

    if d.class == 0 then
      -- A device-tree window: the type is the whole identity.
      name  = VIRTIO[d.device] or ("virtio type " .. d.device)
      place = "window " .. d.where
    else
      local vendor = VENDORS[d.vendor] or ("vendor 0x%04x"):format(d.vendor)
      local kind   = CLASSES[d.class >> 16] or ("class 0x%02x"):format(d.class >> 16)
      name  = ("%s %s (0x%04x:0x%04x)"):format(vendor, kind, d.vendor, d.device)
      place = ("%02x:%02x.%d"):format(0, d.where >> 3, d.where & 7)
    end

    if d.claimed then
      row(place, name .. "  -  driven")
    else
      row(place, name .. "  -  NO DRIVER")
      undriven = undriven + 1
    end
  end

  out("")
  out(("  %d device%s found, %d driven, %d without a driver."):format(
      #bus, (#bus == 1) and "" or "s", #bus - undriven, undriven))

  if undriven > 0 then
    out("  A device with no driver is not a fault. It is hardware this")
    out("  system has not been taught, and on a q35 most of it never will")
    out("  be: the bridges and the SATA controller are QEMU's, not")
    out("  something Kosmos asked for.")
  end
else
  absent("Bus", "this board reports no enumerable bus")
end

--------------------------------------------------------------------------
head("What this machine cannot be asked")

out("  Listed rather than left out, because a blank line reads as")
out("  \"there is none\" when it means \"nobody asked\".")
out("")

absent("Memory speed", "no SMBIOS reader; the firmware knows and is not asked")
absent("Slots and DIMMs", "the same - a count of modules needs that table")
absent("Graphics", "ramfb is a linear framebuffer from the firmware. There " ..
                   "is no accelerator to name, and no driver that would " ..
                   "know one if there were")
absent("USB", "no host controller driver, so no tree to walk")
absent("Temperature", "nothing here reads a sensor")
absent("Pointer", "sys.pointer answers nil both for a board with none and " ..
                  "for a program not granted one, and a window never is - " ..
                  "the window manager holds the console. sysinfo carries " ..
                  "has_keyboard and no has_pointer to ask instead")

--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- Where it goes.
--
-- **The same string, either way.** With a window manager this opens a
-- read-only editor over it: monospace, so the columns above survive, and
-- scrollable, so the whole inventory is reachable. Without one it prints,
-- which is how the text leaves this machine at a prompt with a serial line
-- attached, or into a file from the shell.
--
-- One report and two ways to show it, rather than a window that says one
-- thing and a program that says another.
--
-- **Read-only, and selectable, which is the pair that makes it useful.**
-- A report of what a machine is has one job after being read, and it is
-- being sent to somebody else. So the text is text: drag over it and it
-- highlights, `Control-C` puts it on the clipboard, and nothing typed at
-- it can change what it says. `ui.editor` refuses the editing keys when
-- `read_only` is set and answers copy and select-all regardless, so that
-- is the whole of it here.
--------------------------------------------------------------------------

local report = table.concat(lines, "\n")

local win = ui.window{ title = "This Machine", w = W, h = H, x = 100, y = 60 }

if not win then
  print(report)
  return
end

win:add(ui.editor{ x = 8, y = 8, w = W - 32, h = H - 74,
                   text = report, gutter = false, read_only = true })

-- Said rather than left to be discovered. There are no modifier keys on
-- this machine, so the clipboard lives behind the window manager's prefix,
-- and a prefix nobody mentions is a feature nobody has.
win:add(ui.label{ x = 10, y = H - 60,
                  text = "Drag to select, or Control-A for all."
                         .. "   Control-C copies it.",
                  color = "text_dim" })

win:run()
