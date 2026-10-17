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

-- The test refuses while the window manager's arrow is on the plane, which
-- on the M700 is always once the desktop is up (step 3b).

local gpu = use("/Kosmos/Libraries/gpu.lua")

local words = use("/Kosmos/Libraries/files.lua").words(args, 2)
local seconds = math.tointeger(tonumber(words[2] or "")) or 10

if words[1] and words[1] ~= "cursor" then
  print("gpu: gpu [cursor [seconds]]")
  return
end

if words[1] ~= "cursor" then
  local ok, why = gpu.look()
  print(ok and "gpu: said in the log - `log intelgpu`" or ("gpu: " .. why))
  return
end

print(("gpu: a test cursor in the middle of the screen for %d seconds - look at the monitor"):format(seconds))

local r, why, code = gpu.cursor_test(seconds)

if not r then
  print("gpu: " .. why)
  if code ~= 6 then return end
end

if r then
  print(("gpu: read back - control %08x, base %08x, position %08x, buffer %08x, watermark %08x")
        :format(r.curcntr, r.curbase, r.curpos, r.buf_cfg, r.wm0))
  print(("gpu: underrun before %s, after %s"):format(r.underrun_before and "yes" or "no",
                                                    r.underrun_after and "yes" or "no"))
end
