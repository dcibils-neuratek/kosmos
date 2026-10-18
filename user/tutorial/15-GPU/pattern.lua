-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name GPU Pattern
--
-- Lesson 15 of the IDE's tutorial, finished: a moving pattern, every one of
-- its 640 by 400 pixels computed by a kernel on the GPU - a thread for
-- sixteen pixels of a row, a lane a pixel - sixty times a second. The
-- processor decides which frame; the GPU computes it.

local ui = use("/Kosmos/Libraries/ui.lua")
local compute = use("/Kosmos/Kits/compute")

local W, H = 640, 400
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- The kernel: the work for one pixel, written once. r0 is the thread's
-- header - r0.1 its group across, r0.6 its row - and r1.0 the frame's
-- number, from the run's constants.
local KERNEL = [[
// Which pixel: the lanes 0 to 15, and the group's first pixel.
mov (8|M0) r10.0<1>:uw 0x76543210:v
add (8|M0) r10.8<1>:uw r10.0<8;8,1>:uw 8:uw
mul (1|M0) r2.0<1>:ud r0.1<0;1,0>:ud 0x10:ud
add (16|M0) r12.0<1>:ud r10.0<8;8,1>:uw r2.0<0;1,0>:ud
mov (16|M0) r14.0<1>:ud r0.6<0;1,0>:ud

// Its colour: red from x and the frame, green from y and the frame,
// blue from x xor y - a byte each.
add (16|M0) r16.0<1>:ud r12.0<8;8,1>:ud r1.0<0;1,0>:ud
and (16|M0) r16.0<1>:ud r16.0<8;8,1>:ud 0xff:ud
shl (16|M0) r16.0<1>:ud r16.0<8;8,1>:ud 0x10:ud
add (16|M0) r18.0<1>:ud r14.0<8;8,1>:ud r1.0<0;1,0>:ud
and (16|M0) r18.0<1>:ud r18.0<8;8,1>:ud 0xff:ud
shl (16|M0) r18.0<1>:ud r18.0<8;8,1>:ud 0x8:ud
xor (16|M0) r20.0<1>:ud r12.0<8;8,1>:ud r14.0<8;8,1>:ud
and (16|M0) r20.0<1>:ud r20.0<8;8,1>:ud 0xff:ud
or (16|M0) r22.0<1>:ud r16.0<8;8,1>:ud r18.0<8;8,1>:ud
or (16|M0) r22.0<1>:ud r22.0<8;8,1>:ud r20.0<8;8,1>:ud
or (16|M0) r22.0<1>:ud r22.0<8;8,1>:ud 0xff000000:ud

// Written to the picture, buffer 0: sixteen pixels, 64 bytes, one row.
mov (8|M0) r4.0<1>:ud r0.0<8;8,1>:ud
shl (1|M0) r4.0<1>:ud r0.1<0;1,0>:ud 0x6:ud
mov (1|M0) r4.1<1>:ud r0.6<0;1,0>:ud
mov (1|M0) r4.2<1>:ud 0x3f:ud
mov (16|M0) r5.0<1>:ud r22.0<8;8,1>:ud
send.hdc1 (16|M0) acc0 r4:d 0x00000000 0x060A8000

// And the thread ends.
mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud
send.ts (16|M0) null r112:d 0x00000000 0x02000010 {EOT}
]]

local win = ui.window{ title = "GPU Pattern", w = W, h = H + 40, x = 200, y = 120 }
local said = ui.label{ x = 12, y = H + 10, w = W - 24, text = "" }

local kernel, picture, why
local ok, none = compute.available()

if ok then
  kernel, why = compute.kernel(KERNEL)
  if kernel then picture, why = compute.picture(W, H) end
else
  why = none
end

if picture then ui.lend_picture("pattern", picture:surface()) end

local view = ui.view{ x = 0, y = 0, w = W, h = H }

function view:draw(g)
  g:fill(0, 0, W, H, 0xff202020)

  if picture then
    g:lent(0, 0, W, H, "pattern", W, H)
  else
    g:text(20, H // 2, "No Intel GPU here: " .. tostring(why), 0xffe0e0e0)
  end
end

win:add(view)
win:add(said)

-- The clock: a frame asked for when the last is done - never waited for.
local frame, ticket = 0, nil
local clock = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function clock:tick()
  if not picture then return end

  if ticket and ticket:done() then
    local ns = ticket:time()

    said.text = ns and ("frame %d: %.2f ms on the GPU"):format(frame, ns / 1e6) or ""
    ticket = nil
    win:paint()
  end

  if not ticket then
    frame = frame + 1
    ticket = compute.run{ kernel = kernel, grid = { W // 16, H },
                          buffers = { picture },
                          constants = string.pack("<i4", frame) }
  end
end

win:add(clock)
win.tick_every = counter_hz // 60

print("pattern: " .. (picture and "on the GPU" or ("no Intel GPU here - " .. tostring(why))))
win:run()
