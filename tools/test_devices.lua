-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `user/lib/devices.lua` on the host (`docs/machine.md` M2): the M700's
-- bus and USB ports, as its drivers answered on 18 October 2026, handed
-- in as tables, and the one list This Machine draws held to it - every
-- device in its group, driven or not, by whom, and hung from what By
-- connection.

fs = { read = function() return nil end }
sys = {}
use = function(path)
  return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/")))
end

local devices = dofile("user/lib/devices.lua")
local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

local function at(b, d, f) return (b << 8) | (d << 3) | f end

-- The M700's bus: what `sys.bus()` answers there, abridged.
local bus = {
  { where = at(0, 0, 0),    vendor = 0x8086, device = 0x191f, class = 0x060000, claimed = false },
  { where = at(0, 2, 0),    vendor = 0x8086, device = 0x1912, class = 0x030000, claimed = true },
  { where = at(0, 0x14, 0), vendor = 0x8086, device = 0xa12f, class = 0x0c0330, claimed = true },
  { where = at(0, 0x16, 0), vendor = 0x8086, device = 0xa13a, class = 0x078000, claimed = false },
  { where = at(0, 0x17, 0), vendor = 0x8086, device = 0xa102, class = 0x010601, claimed = true },
  { where = at(0, 0x1f, 0), vendor = 0x8086, device = 0xa146, class = 0x060100, claimed = false },
  { where = at(0, 0x1f, 3), vendor = 0x8086, device = 0xa170, class = 0x040300, claimed = true },
  { where = at(0, 0x1f, 6), vendor = 0x8086, device = 0x15b8, class = 0x020000, claimed = true },
}

-- Its USB ports, as `lsusb` printed them - and a stranger nothing reads.
local xhci = at(0, 0x14, 0)
local usb = {
  { controller = 0, port = 3, pci = xhci, kind = "mouse", driven = true,
    vendor = 0x1532, product = 0x0043, name = "Razer DeathAdder Chroma",
    class = 0, subclass = 0, protocol = 0 },
  { controller = 0, port = 5, pci = xhci, kind = "keyboard", driven = true,
    vendor = 0x25a7, product = 0xfa70, name = "2.4G Wireless Receiver" },
  { controller = 0, port = 18, pci = xhci, kind = "stick", driven = true,
    vendor = 0x0951, product = 0x1666, name = "DataTraveler 3.0" },
  { controller = 0, port = 7, pci = xhci, kind = "none", driven = false,
    vendor = 0x0403, product = 0x6001, name = "FT232R USB UART",
    class = 0xff, subclass = 0xff, protocol = 0xff },
}

local list, counts = devices.gather {
  cpu = { name = "Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz", implementer = "GenuineIntel",
          cores = 8, cores_present = 8 },
  memory = { total_mb = 8049, free_mb = 7821 },
  bus = bus, usb = usb,
  gpu = { model = "Intel HD Graphics 530", detail = "page flips" },
  drives = { { kind = "USB stick", volumes = { { name = "KOSMOS" }, { name = "Home" },
                                               { name = "Sandboxes" } } } },
  net = { address = "192.168.1.28" },
}

local by = {}
for _, d in ipairs(list) do by[d.id] = d end

local function id_of(where) return "pci:" .. where end

-- Every source made a card: the processor, memory, eight on the bus, four
-- on the ports.
check(counts.total == 14, ("%d devices, not 14"):format(counts.total))
check(counts.driven == 10 and counts.undriven == 4,
      ("%d driven and %d not, not 10 and 4"):format(counts.driven, counts.undriven))

-- In its group, driven by whom.
local function holds(id, group, driver, why)
  local d = by[id]

  check(d and d.group == group and d.driver == driver,
        ("%s: %s, %s by %s - not %s by %s"):format(why, id,
         d and d.group or "absent", d and tostring(d.driver) or "-", group,
         tostring(driver)))
end

holds("cpu", "Processor", "Nebula", "the processor")
check(by.cpu.name == "Intel Core i7-6700" and by.cpu.maker == "Intel",
      ("the processor was not called as CPU Monitor calls it: %q by %q"):format(by.cpu.name,
       tostring(by.cpu.maker)))
holds("memory", "Memory", "Nebula", "the memory")
holds(id_of(at(0, 2, 0)), "Graphics", "intelgpu", "the GPU")
holds(id_of(xhci), "USB", "xhci", "the USB controller")
holds(id_of(at(0, 0x17, 0)), "Storage", "ahci", "the SATA controller")
holds(id_of(at(0, 0x1f, 3)), "Audio", "hda", "the sound")
holds(id_of(at(0, 0x1f, 6)), "Network", "e1000", "the network card")
holds("usb:0:3", "USB", "xhci", "the mouse")
holds("usb:0:5", "USB", "xhci", "the keyboard")
holds("usb:0:18", "Storage", "xhci", "the stick")
holds("usb:0:7", "USB", nil, "the UART nothing reads")
holds(id_of(at(0, 0x16, 0)), "Other", nil, "the management engine")

-- Named as the system knows them, and a stranger by its class.
check(by[id_of(at(0, 2, 0))].name == "Intel HD Graphics 530",
      "the GPU was not named by its driver: " .. tostring(by[id_of(at(0, 2, 0))].name))
check(by[id_of(at(0, 0x1f, 6))].name == "Intel Ethernet I219-V",
      "the network card was not named: " .. tostring(by[id_of(at(0, 0x1f, 6))].name))
check(by[id_of(at(0, 0x1f, 6))].detail == "192.168.1.28",
      "the network card's line was not its address")
check(by[id_of(at(0, 0x16, 0))].detail == "class 07 80 00" and not by[id_of(at(0, 0x16, 0))].picture,
      "the management engine was not a dim chip with its class")
check(by[id_of(at(0, 0x16, 0))].label == "ME",
      "the management engine's chip did not say ME")
check(by["usb:0:7"].detail == "class ff/ff/ff" and not by["usb:0:7"].driven,
      "the UART was not listed undriven with its class")
check(by["usb:0:18"].busy == "in use" and by["usb:0:18"].detail == "KOSMOS, Home and Sandboxes on it",
      "the stick did not say what is on it: " .. tostring(by["usb:0:18"].detail))
check(by["usb:0:3"].picture == "mouse" and by["usb:0:5"].picture == "keyboard"
      and by["usb:0:18"].picture == "stick",
      "a USB device was not drawn as what it is")

-- In group order: the processor first, Other last.
check(list[1].id == "cpu", "the processor was not first")
check(list[#list].group == "Other", "Other was not last")

-- By connection: the processor at the root, the bus behind the host
-- bridge, the ports under their controller.
local roots, children = devices.tree(list)
local hb = id_of(at(0, 0, 0))

check(#roots == 1 and roots[1].id == "cpu", "the tree had more than the processor at its root")
check(by[hb].parent == "cpu", "the host bridge did not hang from the processor")
check(by[id_of(at(0, 2, 0))].parent == hb, "the GPU did not hang from the host bridge")
check(#(children[id_of(xhci)] or {}) == 4, "the controller did not hold its four ports")

local ports = {}
for _, c in ipairs(children[id_of(xhci)] or {}) do ports[#ports + 1] = c.id end
check(table.concat(ports, " ") == "usb:0:3 usb:0:5 usb:0:18 usb:0:7",
      "the ports were not in the order the driver listed them: " .. table.concat(ports, " "))

-- The M700's GPU: its driver answers, though the bus says no interrupt
-- was claimed for it.
local unclaimed = {}
for i, d in ipairs(bus) do unclaimed[i] = d end
unclaimed[2] = { where = at(0, 2, 0), vendor = 0x8086, device = 0x1912, class = 0x030000, claimed = false }

local again = devices.gather { bus = unclaimed, gpu = { model = "Intel HD Graphics 530" } }
local gpu_card
for _, d in ipairs(again) do if d.id == id_of(at(0, 2, 0)) then gpu_card = d end end

check(gpu_card and gpu_card.driven and gpu_card.driver == "intelgpu"
      and gpu_card.name == "Intel HD Graphics 530",
      "a GPU its driver answers for was not driven by intelgpu")
check(by["memory"].parent == "cpu", "the memory did not sit beside the processor")

-- A machine that answers nothing still has a processor, and nothing else.
local bare = devices.gather {}

check(#bare == 1 and bare[1].id == "cpu", "an empty machine was not one processor")

if #fails > 0 then
  print(("FAIL: %d of %d devices checks"):format(#fails, checks))
  for _, why in ipairs(fails) do print("  " .. why) end
  os.exit(1)
end

print(("PASS: %d devices checks"):format(checks))
