-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Every device this machine has, driven or not, in one list (`docs/
-- machine.md` M2). The one door: This Machine, `devices` at the prompt and
-- the Deskbar's devices indicator (`roadmap.md` 5q) read the machine here,
-- so a device is called the same thing, in the same group, by all three.
--
--   local devices = use("/Kosmos/Libraries/devices.lua")
--
--   devices.read()          the machine now: { list, counts }
--   devices.gather(src)     the same from sources handed in - what the
--                           host test feeds, and what `read` calls
--
-- **A library and not a server**: it is run, not asked (`glossary.md`),
-- and every source it reads is a door of its own - `sys.bus()` for what is
-- on the PCI bus and whether a driver claimed it, the USB driver's list
-- (`usb.lua`), `/Devices/cpu` and `/Devices/memory`, the GPU's driver
-- (`gpu.lua`), the drives (`drivelist.lua`), the battery. What this adds is
-- the one shape:
--
--   { id, group, picture, label, name, maker, place, driven, driver,
--     detail, busy, parent }
--
-- `group` is where By kind puts it, `parent` the id of what it hangs from
-- By connection, `picture` which picture the kit draws (`label` the word on a
-- plain chip, for a device with no picture of its own), and `detail` one
-- line of what it is doing. `driver` is a name only where Kosmos has one
-- to give; a device a driver claimed under a name this file does not know
-- is driven with no name rather than a guessed one.
--
-- **Nothing is invented**, as `hardware.lua` says of itself. A device with
-- no driver keeps its class code, and the tree hangs every PCI device from
-- the host bridge because the bus enumeration does not say which bridge a
-- device sits behind - where the bus says nothing more, the tree does not
-- guess.

local devices = {}

local function lib(path)
  local ok, m = pcall(use, path)

  return ok and type(m) == "table" and m or nil
end

local hardware = use("/Kosmos/Libraries/hardware.lua")

-- The groups By kind shows, in this order.
devices.GROUPS = { "Processor", "Graphics", "Memory", "Storage", "Network",
                   "Audio", "USB", "Input", "Power", "Other" }

--------------------------------------------------------------------------
-- PCI: what a class code is, and who drives it.
--------------------------------------------------------------------------

--
-- By class and subclass (the class code's top two bytes), then by class
-- alone: the group, the word for it and, for a device with no picture, the
-- short word on its chip. Only classes a machine here has shown.
--
local SUBCLASSES = {
  [0x0101] = { "Storage", "IDE controller", "IDE" },
  [0x0106] = { "Storage", "SATA controller", "SATA", "ssd" },
  [0x0108] = { "Storage", "NVMe controller", "NVMe", "ssd" },
  [0x0200] = { "Network", "Ethernet controller", "LAN", "port" },
  [0x0280] = { "Network", "network controller", "WLAN", "port" },
  [0x0300] = { "Graphics", "VGA controller", "GPU", "gpu" },
  [0x0380] = { "Graphics", "display controller", "GPU", "gpu" },
  [0x0401] = { "Audio", "audio device", "SND", "speaker" },
  [0x0403] = { "Audio", "HD Audio controller", "HDA", "speaker" },
  [0x0580] = { "Other", "memory controller", "PMC" },
  [0x0600] = { "Other", "host bridge", "HB" },
  [0x0601] = { "Other", "ISA bridge", "LPC" },
  [0x0604] = { "Other", "PCI bridge", "PCI" },
  [0x0780] = { "Other", "communication controller", "ME" },
  [0x0880] = { "Other", "system peripheral", "SYS" },
  [0x0c03] = { "USB", "USB controller", "USB", "usb" },
  [0x0c05] = { "Other", "SMBus controller", "SMB" },
  [0x1180] = { "Other", "signal processing controller", "DSP" },
}

local CLASSES = {
  [0x01] = { "Storage", "storage controller", "STO", "ssd" },
  [0x02] = { "Network", "network controller", "NET", "port" },
  [0x03] = { "Graphics", "display controller", "GPU", "gpu" },
  [0x04] = { "Audio", "multimedia device", "MM", "speaker" },
  [0x09] = { "Input", "input device", "IN" },
}

-- virtio's device types on a board of device-tree windows, where the bus
-- gives a type rather than a class.
local VIRTIO = {
  [1]  = { "Network", "LAN", "port" },
  [2]  = { "Storage", "BLK", "ssd" },
  [4]  = { "Other", "RNG" },
  [16] = { "Graphics", "GPU", "gpu" },
  [18] = { "Input", "IN" },
  [25] = { "Audio", "SND", "speaker" },
}

--
-- Who drives a claimed device, by name - the drivers in this tree, matched
-- on what they take: `xhci.c` the xHCI class, `e1000.c` Intel's network
-- class, `intelgpu.c` Intel's display class, the HDA driver class 0403h,
-- `ahci.c` class 010601h, and a virtio device its own driver.
--
function devices.driver_of(d)
  if not d.claimed then return nil end

  local class = d.class or 0

  if class == 0 then return hardware.VIRTIO[d.device] end
  if d.vendor == 0x1af4 then
    return hardware.VIRTIO[hardware.virtio_type(d.device or 0) or -1] or "virtio"
  end
  if class == 0x0c0330 then return "xhci" end
  if class == 0x010601 then return "ahci" end
  if (class >> 8) == 0x0403 then return "hda" end
  if (class >> 16) == 0x02 and d.vendor == 0x8086 then return "e1000" end
  if (class >> 16) == 0x03 and d.vendor == 0x8086 then return "intelgpu" end

  return nil
end

local function pci_place(where)
  return ("%02x:%02x.%d"):format(where >> 8, (where >> 3) & 0x1f, where & 7)
end

local function class_words(class)
  return ("class %02x %02x %02x"):format(class >> 16, (class >> 8) & 0xff,
                                         class & 0xff)
end

--------------------------------------------------------------------------
-- Gathering.
--------------------------------------------------------------------------

local function mb_words(mb)
  if mb >= 1024 then return ("%.0f GB"):format(mb / 1024) end

  return ("%d MB"):format(mb)
end

--
-- Every source into one list. `src` holds what each door answered, any of
-- it missing: { info, cpu, memory, bus, usb, gpu, drives, battery, net }.
-- Returns the list, in group order and within a group as found, and the
-- counts { total, driven, undriven }.
--
function devices.gather(src)
  local list, by_id = {}, {}

  local function add(d)
    d.driven = d.driven and true or false
    list[#list + 1] = d
    by_id[d.id] = d

    return d
  end

  local cpu, mem, info = src.cpu or {}, src.memory or {}, src.info or {}

  -- The processor is the root of the tree, and driven by Nebula itself.
  local model = cpu.name
    or cpu.implementer and (("%s %s"):format(cpu.implementer, cpu.part or ""))
    or cpu.arch or "Processor"
  local present = cpu.cores_present or cpu.cores or info.cpus_present or 1

  -- CPUID's vendor strings, said as the makers' names - which is also what
  -- the chip's mark is keyed on.
  local maker = cpu.implementer or cpu.vendor
  maker = ({ GenuineIntel = "Intel", AuthenticAMD = "AMD" })[maker] or maker

  add { id = "cpu", group = "Processor", picture = "cpu", label = "CPU",
        name = model, maker = maker,
        place = cpu.arch, driven = true, driver = "Nebula",
        detail = ("%d processor%s"):format(present, present == 1 and "" or "s") }

  if mem.total_mb then
    add { id = "memory", group = "Memory", picture = "memory", label = "RAM",
          name = mb_words(mem.total_mb), place = "installed",
          driven = true, driver = "Nebula", parent = "cpu",
          detail = ("%d MB used of %d MB"):format(
                   mem.total_mb - (mem.free_mb or 0), mem.total_mb) }
  end

  -- The PCI bus, every device on it.
  local root = "cpu"

  for _, d in ipairs(src.bus or {}) do
    if (d.class or 0) >> 8 == 0x0600 then root = "pci:" .. d.where break end
  end

  local gpu = src.gpu

  for _, d in ipairs(src.bus or {}) do
    local class = d.class or 0
    local id = "pci:" .. d.where
    local group, word, label, picture

    if class == 0 then
      local v = VIRTIO[d.device] or { "Other", "VIO" }

      group, label, picture = v[1], v[2], v[3]
      word = hardware.VIRTIO[d.device] or ("virtio type " .. d.device)
    else
      local s = SUBCLASSES[class >> 8] or CLASSES[class >> 16]
             or { "Other", "device", "DEV" }

      group, word, label, picture = s[1], s[2], s[3], s[4]
    end

    local maker = class ~= 0 and hardware.vendor(d) or nil
    local name = maker and (maker .. " " .. word) or word
    local detail

    -- The names this system knows better than a class's word.
    local known = (hardware.NETWORK_NAMES[d.vendor] or {})[d.device]

    if known then name = maker .. " " .. known end

    -- A virtio device by its type, which says more than its PCI class:
    -- QEMU's random number source is class 00ff, "unclassified".
    local vt = d.vendor == 0x1af4 and hardware.virtio_type(d.device) or nil

    if vt and hardware.VIRTIO[vt] then
      local v = VIRTIO[vt]

      name = hardware.VIRTIO[vt]
      if v then group, label, picture = v[1], v[2], v[3] end
    end

    if group == "Graphics" and gpu and gpu.model and d.claimed then
      name = gpu.model
      detail = gpu.detail
    end

    if not d.claimed then
      detail = class ~= 0 and class_words(class) or nil
      picture = nil                  -- a dim chip with its short word
    end

    add { id = id, group = group, picture = picture, label = label, name = name,
          maker = maker, driven = d.claimed, driver = devices.driver_of(d),
          place = class ~= 0
                  and ("PCI %s · %04x:%04x"):format(pci_place(d.where),
                                                    d.vendor, d.device)
                  or ("window " .. tostring(d.where)),
          detail = detail,
          parent = (id == root) and "cpu" or root }
  end

  -- The network's address, on the card that carries it.
  if src.net and src.net.address then
    for _, d in ipairs(list) do
      if d.group == "Network" and d.driven then
        d.detail, d.busy = src.net.address, "in use"
        break
      end
    end
  end

  -- USB: every device on every port, hung from its controller.
  local USB_GROUP = { keyboard = "USB", mouse = "USB", ["game controller"] = "USB",
                      stick = "Storage", storage = "Storage", network = "Network" }
  local USB_PICTURE = { keyboard = "keyboard", mouse = "mouse", stick = "stick",
                     storage = "stick", network = "port" }

  local sticks = {}

  for _, u in ipairs(src.usb or {}) do
    local controller = "pci:" .. tostring(u.pci)
    local name = (u.name ~= "" and u.name)
                 or ("%04x:%04x"):format(u.vendor or 0, u.product or 0)
    local d = add {
      id = ("usb:%d:%d"):format(u.controller or 0, u.port or 0),
      group = u.driven and USB_GROUP[u.kind] or "USB",
      picture = u.driven and USB_PICTURE[u.kind] or nil,
      label = u.driven and (u.kind or ""):upper():sub(1, 4) or "USB",
      name = name, driven = u.driven, driver = u.driven and "xhci" or nil,
      place = ("USB port %d · %04x:%04x"):format(u.port or 0, u.vendor or 0,
                                                 u.product or 0),
      detail = u.driven and (u.kind == "keyboard" and "the keyboard"
                             or u.kind == "mouse" and "the pointer" or nil)
               or ("class %02x/%02x/%02x"):format(u.class or 0, u.subclass or 0,
                                                  u.protocol or 0),
      parent = by_id[controller] and controller or root,
    }

    if u.kind == "stick" and u.driven then sticks[#sticks + 1] = d end
  end

  -- The drives: a stick's volumes on its card, the machine's own disk a
  -- card of its own.
  local s = 0

  for _, drive in ipairs(src.drives or {}) do
    local vols = {}

    for _, v in ipairs(drive.volumes or {}) do vols[#vols + 1] = v.name end

    if drive.kind == "USB stick" then
      s = s + 1
      local d = sticks[s]

      if d then
        d.volumes = vols
        if #vols > 0 then
          d.detail, d.busy = table.concat(vols, " and ") .. " on it", "in use"
        end
      end
    end
  end

  -- The battery, where there is one.
  if src.battery and src.battery.present == 1 and src.battery.percent then
    add { id = "battery", group = "Power", picture = "plug", label = "BAT",
          name = "Battery", driven = true, driver = "acpi", parent = "cpu",
          detail = ("%d%%%s"):format(src.battery.percent,
                                     src.battery.charging and ", charging" or "") }
  end

  -- In group order, stable within a group.
  local rank = {}

  for i, g in ipairs(devices.GROUPS) do rank[g] = i end
  for i, d in ipairs(list) do d.found = i end

  table.sort(list, function(a, b)
    local ra, rb = rank[a.group] or 99, rank[b.group] or 99

    if ra ~= rb then return ra < rb end

    return a.found < b.found
  end)

  local counts = { total = #list, driven = 0, undriven = 0 }

  for _, d in ipairs(list) do
    if d.driven then counts.driven = counts.driven + 1
    else counts.undriven = counts.undriven + 1 end
  end

  return list, counts
end

--
-- The children of each device, By connection: { [id] = { child, ... } },
-- and the roots - the processor, alone, unless something has no parent.
--
function devices.tree(list)
  local children, roots, known = {}, {}, {}

  for _, d in ipairs(list) do known[d.id] = true end

  for _, d in ipairs(list) do
    if d.parent and known[d.parent] and d.parent ~= d.id then
      children[d.parent] = children[d.parent] or {}
      table.insert(children[d.parent], d)
    else
      roots[#roots + 1] = d
    end
  end

  return roots, children
end

--------------------------------------------------------------------------
-- The machine now.
--------------------------------------------------------------------------

function devices.read()
  local src = {
    info = sys.info and sys.info() or nil,
    cpu = fs.read("/Devices/cpu"),
    memory = fs.read("/Devices/memory"),
    battery = fs.read("/Devices/battery"),
    bus = sys.bus and sys.bus() or nil,
  }

  local usb = lib("/Kosmos/Libraries/usb.lua")
  if usb then src.usb = usb.devices() end

  local gpu = lib("/Kosmos/Libraries/gpu.lua")
  local stats = gpu and gpu.stats() or nil

  if stats then
    src.gpu = { model = stats.model,
                detail = gpu.desktop_words(stats):gsub("^.- %- ", "") }
  end

  local drivelist = lib("/Kosmos/Libraries/drivelist.lua")
  if drivelist then src.drives = drivelist.drives() end

  local ipv4 = lib("/Kosmos/Libraries/ipv4.lua")

  if fs.net_info and ipv4 then
    local ok, n = pcall(fs.net_info, "/Network")

    if ok and type(n) == "table" and n.card and ipv4.given(n.address) then
      src.net = { address = ipv4.text(n.address) }
    end
  end

  return devices.gather(src)
end

return devices
