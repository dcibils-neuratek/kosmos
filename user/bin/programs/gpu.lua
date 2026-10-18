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

--   gpu engine [n]      the blitter's first work: the GPU woken, a ring
--                       given to its blit engine, one command in it - a
--                       number written to memory - and whether it came
--                       (`docs/m700-2d.md` step 5)
--
-- The test refuses while the window manager's arrow is on the plane, which
-- on the M700 is always once the desktop is up (step 3b).

local gpu = use("/Kosmos/Libraries/gpu.lua")

local words = use("/Kosmos/Libraries/files.lua").words(args, 2)
local seconds = math.tointeger(tonumber(words[2] or "")) or 10

if words[1] and words[1] ~= "cursor" and words[1] ~= "engine" then
  print("gpu: gpu [cursor [seconds] | engine]")
  return
end

if words[1] == "engine" then
  -- `gpu engine 300`: that many in a row, past the ring's 256 and round.
  local times = math.tointeger(tonumber(words[2] or "")) or 1
  local r, why
  local done, fastest, slowest, total = 0, math.huge, 0, 0

  for _ = 1, math.max(1, times) do
    r, why = gpu.engine_test()
    if not r then break end
    done = done + 1
    fastest, slowest = math.min(fastest, r.elapsed_us), math.max(slowest, r.elapsed_us)
    total = total + r.elapsed_us
  end

  if r and times <= 1 then
    print(("gpu: the blitter wrote %08x where it was told, %d us after its tail moved"):format(r.value, r.elapsed_us))
  elseif done > 0 then
    print(("gpu: %d of %d written, the last %08x; %d us fastest, %d slowest, %d on average")
          :format(done, times, gpu.last_engine.value, fastest, slowest, total // done))
  end

  if not r then
    print("gpu: " .. why)
  end

  if r or (r == nil and why) then
    local e = gpu.last_engine
    if e then
      print(("gpu: ring head %08x, tail %08x, ctl %08x, mode %08x, errors %08x")
            :format(e.head, e.tail, e.ctl, e.mode, e.esr))
    end
  end

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
