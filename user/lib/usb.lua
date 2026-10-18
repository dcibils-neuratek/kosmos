-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Every device on every USB port, from Lua: `/Devices/usb`, the USB driver
-- (`docs/machine.md` M1). The one door to it - This Machine, `lsusb` and
-- the Deskbar's devices indicator read it here.
--
-- **The layout below is `usbproto.h` written a second time**, asserted
-- when this loads, as `gpu.lua` and `backlight.lua` write theirs.
--
-- `usb.devices()` returns every device - its controller and port, its
-- speed, the kind the driver made of it and whether it reads it, the class
-- it reported, its vendor and product numbers and its name - or nil and a
-- sentence. A machine with no USB driver answers nil.

local usb = {}

local OP_LIST = 1
local REQUEST = "<I4I4"
local HEAD = "<I4I4I4I4"
local DEVICE = "<I1I1I1I1I1I1I1I1I2I2I4c32"

assert(#string.pack(DEVICE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "") == 48,
       "usb: the device layout does not match usbproto.h")

usb.KINDS = { [0] = "none", "keyboard", "mouse", "game controller", "stick", "network",
              "camera", "MIDI", "HID device", "storage" }

usb.SPEEDS = { "full speed", "low speed", "high speed", "SuperSpeed", "SuperSpeed+" }

function usb.devices()
  local out, first = {}, 0

  repeat
    local reply, why = fs.raw("/Devices/usb", string.pack(REQUEST, OP_LIST, first), nil, "usb")

    if not reply then return nil, tostring(why) end
    if #reply < 16 then return nil, "the USB driver sent a reply of the wrong size" end

    local err, total, count = string.unpack(HEAD, reply)

    if err ~= 0 then return nil, "the USB driver did not understand that" end

    for i = 1, math.min(count, (#reply - 16) // 48) do
      local ctl, port, speed, kind, driven, class, subclass, protocol, vendor, product, pci, name =
        string.unpack(DEVICE, reply, 16 + (i - 1) * 48 + 1)

      out[#out + 1] = {
        controller = ctl, port = port, speed = speed, kind = usb.KINDS[kind] or ("kind " .. kind),
        kind_code = kind, driven = driven ~= 0, class = class, subclass = subclass,
        protocol = protocol, vendor = vendor, product = product, pci = pci,
        name = (name:match("^[^%z]*") or ""),
      }
    end

    first = first + count
  until count == 0 or first >= total

  return out
end

return usb
