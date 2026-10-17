-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- gpu: the GPU's driver, asked (`docs/m700-2d.md`).
--
--   gpu                 say the display engine again, in the log
--   gpu cursor          the cursor plane tried: a test picture in the middle
--   gpu cursor 20       of the screen for that many seconds, then off
--
-- The cursor test is the first write Kosmos makes to the M700's display
-- engine. It is seen on the machine's own monitor and not by VNC, which
-- shows what the window manager composed and not what the display adds.

local REQUEST, REPLY = "<I4I4", "<I4I4I4I4I4I4I4I4"
local OP_LOOK, OP_CURSOR_TEST = 1, 2
local SAYS = {
  [1] = "no Intel GPU this driver knows",
  [2] = "a request the driver does not know",
  [3] = "no pipe running to show it on",
  [4] = "no contiguous pages for the picture",
  [5] = "the GTT slots chosen could not be written",
  [6] = "written, and read back different",
}

local words = use("/Kosmos/Libraries/files.lua").words(args, 2)
local op = words[1] == "cursor" and OP_CURSOR_TEST or OP_LOOK
local seconds = math.tointeger(tonumber(words[2] or "")) or 10

if words[1] and words[1] ~= "cursor" then
  print("gpu: gpu [cursor [seconds]]")
  return
end

if op == OP_CURSOR_TEST then
  print(("gpu: a test cursor in the middle of the screen for %d seconds - look at the monitor"):format(seconds))
end

local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, op, seconds), nil, "gpu")

if not reply then
  print("gpu: " .. tostring(why))
  return
end

local err, cntr, base, pos, buf, wm0, before, after = string.unpack(REPLY, reply)

if err ~= 0 then
  print("gpu: " .. (SAYS[err] or ("error " .. err)))
end

if op == OP_CURSOR_TEST and err ~= 1 and err ~= 3 and err ~= 4 then
  print(("gpu: read back - control %08x, base %08x, position %08x, buffer %08x, watermark %08x")
        :format(cntr, base, pos, buf, wm0))
  print(("gpu: underrun before %s, after %s"):format(before ~= 0 and "yes" or "no",
                                                    after ~= 0 and "yes" or "no"))
elseif op == OP_LOOK and err == 0 then
  print("gpu: said in the log - `log intelgpu`")
end
