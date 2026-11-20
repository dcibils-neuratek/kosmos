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
--   gpu engine render [n]   the same on the render engine (step 7a)
--   gpu el [n]          the video engine on execlists: one context, a page
--                       table of its own, a number written where only it
--                       maps (`docs/gpu3d.md` G6b)
--   gpu el2 [n]         two contexts submitted together, the same GPU
--                       address in each page table on a page of its own,
--                       each holding only its own number (G6c)
--   gpu elshared        its control: two contexts given one page table,
--                       which the check must call a failure
--   gpu contexts        programs' own contexts (G6e): two, each with a
--                       buffer bound at the same address, each storing its
--                       own number there and keeping its own register
--                       across the other's run, told by pulses - on the
--                       GPU's engines, or the stand-in where there is none
--   gpu contexts hang   a program's batch that jumps to itself, stopped -
--                       by the driver's watch on the GPU, by the stand-in's
--                       count of commands - and the context working after
--   gpu compute [colour]    the first program on the execution units: a
--                       64 by 4 surface filled with a byte (step 7b)
--   gpu state           the render engine's starting state, run once (7c)
--   gpu render [blend]  a 64 by 64 texture drawn by the 3D pipeline, or
--                       blended over a ground by its alpha (7c)
--   gpu draw            a part of a 301x203 picture blended onto 800x600
--                       at (123, 77), every pixel held (7c)
--   gpu read [words]    a kernel that reads a buffer into another, checked
--   gpu kc FILE [n]     a kernel in kc from a file, its first words said
--   gpu runs [steps]    a list of runs in one submission, held and timed
--   gpu kernel [colour] a program's own kernel (`compute.md` C2): the fill
--                       written as text, assembled here by eu.lua, checked
--                       and kept by the driver, run over a buffer this
--                       program imported, every byte held - and the same
--                       kernel sending stateless, which must be refused
--
-- The test refuses while the window manager's arrow is on the plane, which
-- on the M700 is always once the desktop is up (step 3b).

local gpu = use("/Kosmos/Libraries/gpu.lua")

local words = use("/Kosmos/Libraries/files.lua").words(args, 3)
local seconds = math.tointeger(tonumber(words[2] or "")) or 10

if words[1] and words[1] ~= "cursor" and words[1] ~= "engine" and words[1] ~= "compute" and words[1] ~= "state" and words[1] ~= "render" and words[1] ~= "draw" and words[1] ~= "kernel" and words[1] ~= "read" and words[1] ~= "kc" and words[1] ~= "runs" and words[1] ~= "el" and words[1] ~= "el2" and words[1] ~= "elshared" and words[1] ~= "hang" and words[1] ~= "contexts" then
  print("gpu: gpu [cursor [seconds] | engine [render] [n] | el [n] | el2 [n] | hang [render] | contexts | compute [colour] | kernel [colour]]")
  return
end

if words[1] == "draw" and words[2] == "big" then
  local r, why = gpu.draw_test(true)
  local c = gpu.last_draw

  print(r and ("gpu: a 1720x1440 copy, %d right, %d us"):format(r.right, r.elapsed_us)
          or ("gpu: %s - %d right, %d never written, %d wrong"):format(tostring(why), c and c.right or 0,
                                                                       c and c.exact or 0, c and c.outside or 0))
  return
end

if words[1] == "draw" then
  local r, why = gpu.draw_test()
  local c = gpu.last_draw

  print(r and ("gpu: %d of 480000 pixels pass, %d exactly, %d outside changed, %d us")
               :format(r.right, r.exact, r.outside, r.elapsed_us)
          or ("gpu: " .. tostring(why)))

  if c and not r then
    print(("gpu: %d of 480000 pass, %d exact, %d outside changed; errors %08x")
          :format(c.right, c.exact, c.outside, c.esr))
  end

  return
end

if words[1] == "render" then
  local blend = words[2] == "blend"
  local r, why = gpu.render_test(blend)
  local c = gpu.last_render

  if r and blend then
    print(("gpu: the 3D pipeline blended %d of 4096 pixels within one of the compositor's, %d exactly, %d us")
          :format(r.right, r.exact, r.elapsed_us))
  elseif r then
    print(("gpu: the 3D pipeline drew %d of 4096 pixels as the texture has them, %d us"):format(r.right, r.elapsed_us))
  else
    print("gpu: " .. tostring(why))
  end

  if c then
    print(("gpu: %d of 4096 right, %d exact, %d bytes written past; ring head %08x, tail %08x, errors %08x")
          :format(c.right, c.exact, c.past, c.head, c.tail, c.esr))
  end

  return
end

if words[1] == "state" then
  local r, why = gpu.render_state()
  local st = gpu.last_state

  print(r and ("gpu: the render engine's starting state ran, %d us"):format(r.elapsed_us)
          or ("gpu: " .. tostring(why)))

  if st then
    print(("gpu: ring head %08x, tail %08x, errors %08x"):format(st.head, st.tail, st.esr))
  end

  return
end

if words[1] == "kernel" then
  local compute = use("/Kosmos/Kits/compute")
  local kernels = use("/Kosmos/Libraries/eukernels.lua")
  local colour = (tonumber(words[2] or "") or 0x5a) & 0xff

  -- A mistake is said by its line before any GPU is asked: the kit
  -- assembles in this process, by eu.lua.
  local _, mistake = compute.kernel("mov (16|M0) r10.0<1>:f r11.0<8;8,1>:f\nfrobnicate (16) r1 r2\n")

  print("gpu: a kernel with a mistake: " .. tostring(mistake))

  local ok, why = compute.available()

  if not ok then print("gpu: " .. tostring(why)) return end

  -- What must be refused, first: the check is the point of the verb.
  local wild, wwhy, code = compute.kernel(kernels.wild)

  if wild then
    print("gpu: the stateless kernel was kept - it should have been refused")
    wild:forget()
  elseif code == 13 then
    print("gpu: the stateless kernel refused, " .. tostring(wwhy))
  else
    print("gpu: " .. tostring(wwhy))
    return
  end

  local k, kwhy = compute.kernel(kernels.fill)

  if not k then print("gpu: the fill kernel was not kept: " .. tostring(kwhy)) return end

  -- The fill's 64 by 4 bytes, in a picture 16 pixels - 64 bytes - wide and
  -- 64 rows high, seen as R8: its first 256 bytes are the fill's four rows
  -- and the 3840 after them what was written past. (A buffer of bytes is
  -- one row a page wide, and the fill's rows 1 to 3 fell outside it.)
  local b, bwhy = compute.picture(16, 64)

  if not b then print("gpu: " .. tostring(bwhy)) k:forget() return end

  local t0 = sys.ticks()
  local t, rwhy = compute.run{ kernel = k, simd = 16, grid = { 4, 4 },
                               buffers = { { b, "r8" } }, constants = string.char(colour) }
  local done, dwhy = false, rwhy

  if t then done, dwhy = t:wait(1000) end

  local t1 = sys.ticks()
  local bytes = b:read(0, 4096)
  local right, past = 0, 0

  for i = 1, 256 do right = right + (bytes:byte(i) == colour and 1 or 0) end
  for i = 257, 4096 do past = past + (bytes:byte(i) ~= 0 and 1 or 0) end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz

  if not done then
    print("gpu: the run did not finish: " .. tostring(dwhy))
  else
    local ns = t:time()

    print(("gpu: kernel ran: %d of 256 bytes %02x, %d written past; %s on the GPU, %s here")
          :format(right, colour, past, ns and ("%d us"):format(ns // 1000) or "time unknown",
                  hz and ("%d us"):format((t1 - t0) * 1000000 // hz) or "time unknown"))
  end

  print((done and right == 256 and past == 0) and "gpu: kernel run held" or "gpu: kernel run NOT held")

  b:free()
  k:forget()
  return
end

-- `gpu read`: a kernel that reads a buffer - every word of one written
-- into another, plus one - and the time it took. Every kernel the M700 had
-- run before Cafesa3D's tracer only wrote; the tracer reads everything,
-- and on the M700 drew black, slowly (`testing.md` 18.700).
if words[1] == "read" then
  local compute = use("/Kosmos/Kits/compute")
  local kc = use("/Kosmos/Libraries/kc.lua")
  local ok, why = compute.available()

  if not ok then print("gpu: " .. tostring(why)) return end

  local n = tonumber(words[2] or "") or 16384
  local text, info = kc.compile([[
const int n;
buffer from, to;

void main() {
  int i = group_x * simd + lane;
  if (i < n) {
    store(to, i * 4, loadi(from, i * 4) + 1);
  }
}
]], { simd = 8 })

  if not text then print("gpu: the copy did not compile: " .. tostring(info)) return end

  local k, kwhy = compute.kernel(text)

  if not k then print("gpu: the copy was not kept: " .. tostring(kwhy)) return end

  local from, fwhy = compute.buffer(n * 4)
  local to, twhy = compute.buffer(n * 4)

  if not (from and to) then print("gpu: " .. tostring(fwhy or twhy)) k:forget() return end

  -- Words new to every run, so a cache that kept an earlier run's lines
  -- at the same addresses shows as words wrong.
  local seed = sys.ticks() & 0xffffff
  local words_in = {}

  for i = 0, n - 1 do words_in[#words_in + 1] = string.pack("<i4", i * 7 + seed) end

  from:write(0, table.concat(words_in))
  to:write(0, string.rep("\0", n * 4))

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz
  local t0 = sys.ticks()
  local t, rwhy = compute.run{ kernel = k, simd = 8, grid = { (n + 7) // 8, 1 },
                               buffers = { from, to }, constants = kc.pack(info, { n = n }) }
  local done, dwhy = false, rwhy

  if t then done, dwhy = t:wait(5000) end

  local t1 = sys.ticks()

  if not done then
    print("gpu: the copy did not finish: " .. tostring(dwhy))
  else
    local back, right, zero, first_wrong = to:read(0, n * 4), 0, 0, nil

    for i = 0, n - 1 do
      local v = string.unpack("<i4", back, i * 4 + 1)

      if v == i * 7 + seed + 1 then
        right = right + 1
      else
        if v == 0 then zero = zero + 1 end
        first_wrong = first_wrong or ("word %d is %d, not %d"):format(i, v, i * 7 + seed + 1)
      end
    end

    local ns = t:time()

    print(("gpu: read %d of %d words right, %d nought%s; %s on the GPU, %s here")
          :format(right, n, zero, first_wrong and ("; " .. first_wrong) or "",
                  ns and ("%d us"):format(ns // 1000) or "time unknown",
                  hz and ("%d us"):format((t1 - t0) * 1000000 // hz) or "time unknown"))
    print(right == n and "gpu: read held" or "gpu: read NOT held")
  end

  from:free()
  to:free()
  k:forget()
  return
end

-- `gpu runs [steps]`: a list of runs in one submission (R8 W2) - each
-- step doubling every word and adding its own number, so the answer is
-- right only if every step saw the last one's writes, in order - and the
-- same steps as separate runs, timed against it.
if words[1] == "runs" then
  local compute = use("/Kosmos/Kits/compute")
  local kc = use("/Kosmos/Libraries/kc.lua")
  local ok, why = compute.available()

  if not ok then print("gpu: " .. tostring(why)) return end

  local nsteps = math.min(32, tonumber(words[2] or "") or 16)
  local n = 16384
  local text, info = kc.compile([[
const int n, step;
buffer out;

void main() {
  int i = group_x * simd + lane;
  if (i < n) {
    store(out, i * 4, loadi(out, i * 4) * 2 + step);
  }
}
]], { simd = 8 })

  if not text then print("gpu: " .. tostring(info)) return end

  local k, kwhy = compute.kernel(text)

  if not k then print("gpu: " .. tostring(kwhy)) return end

  local out = compute.buffer(n * 4)
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
  local want = 0

  -- As the GPU's 32-bit words wrap, for a list long enough to.
  for s = 1, nsteps do want = (want * 2 + s) & 0xffffffff end
  want = want >= 0x80000000 and want - 0x100000000 or want

  local function check(how)
    local back, right = out:read(0, n * 4), 0

    for i = 0, n - 1 do
      if string.unpack("<i4", back, i * 4 + 1) == want then right = right + 1 end
    end

    return right
  end

  -- In one list.
  out:write(0, string.rep("\0", n * 4))
  local steps = {}

  for s = 1, nsteps do
    steps[s] = { kernel = k, simd = 8, grid = { n // 8, 1 }, step = s }
  end

  local t0 = sys.ticks()
  local t, rwhy = compute.runs{ buffers = { out }, constants = kc.pack(info, { n = n, step = 0 }),
                                step_at = 4, steps = steps }
  local done = t and t:wait(5000)
  local t1 = sys.ticks()
  local listed = done and check() or 0
  local gpu_us = t and t:time() and t:time() // 1000 or -1

  -- And as separate runs, each waited for.
  out:write(0, string.rep("\0", n * 4))
  local t2 = sys.ticks()

  for s = 1, nsteps do
    local r = compute.run{ kernel = k, simd = 8, grid = { n // 8, 1 }, buffers = { out },
                           constants = kc.pack(info, { n = n, step = s }) }
    if r then r:wait(5000) end
  end

  local t3 = sys.ticks()
  local separate = check()

  print(("gpu: %d steps in one list: %d of %d words right, %d us on the GPU, %d us here; "
         .. "as %d runs: %d right, %d us here"):format(nsteps, listed, n, gpu_us,
         (t1 - t0) * 1000000 // hz, nsteps, separate, (t3 - t2) * 1000000 // hz))
  if not t then print("gpu: " .. tostring(rwhy)) end
  print((listed == n and separate == n) and "gpu: runs held" or "gpu: runs NOT held")
  out:free()
  k:forget()
  return
end

-- `gpu kc FILE [n]`: a kernel in Kosmos's kernel language, from a file,
-- run over `n` lanes with one buffer, `out`, of `n` words and the constant
-- `n` - and its first words said as integers and as floats; or assembly,
-- from a file ending .eu, with `n` in r1.0. For finding
-- which instruction the GPU takes otherwise than the emulator does.
if words[1] == "kc" then
  local compute = use("/Kosmos/Kits/compute")
  local kc = use("/Kosmos/Libraries/kc.lua")
  local ok, why = compute.available()

  if not ok then print("gpu: " .. tostring(why)) return end

  local source = words[2] and fs.read(words[2])

  if type(source) ~= "string" then print("gpu: no kernel at " .. tostring(words[2])) return end

  local n = tonumber(words[3] or "") or 16
  local text, info

  -- A file ending .eu is assembly already, its one constant `n` in r1.0.
  if words[2]:match("%.eu$") then
    text, info = source, { constants = { { name = "n", t = "i" } } }
  else
    text, info = kc.compile(source, { simd = 8 })
  end

  if not text then print("gpu: " .. tostring(info)) return end

  local k, kwhy = compute.kernel(text)

  if not k then print("gpu: not kept: " .. tostring(kwhy)) return end

  -- A buffer for each the kernel declares, as it declares it - bytes, or a
  -- table the sampler reads - each n words of 0xee; the first plain one is
  -- what is said.
  local bound, all, out = {}, {}, nil

  for i, view in ipairs(info.views or { "bytes" }) do
    local b = compute.buffer(math.max(4096, n * 4))

    b:write(0, string.rep("\xee", n * 4))
    all[#all + 1] = b
    bound[i] = view == "table" and { b, "table" } or b
    if view ~= "table" and not out then out = b end
  end

  out = out or all[1]

  local t = compute.run{ kernel = k, simd = 8, grid = { (n + 7) // 8, 1 }, buffers = bound,
                         constants = kc.pack(info, { n = n }) }
  local done, dwhy = false, "not run"

  if t then done, dwhy = t:wait(5000) end

  if not done then
    print("gpu: did not finish: " .. tostring(dwhy))
  else
    local back = out:read(0, n * 4)

    for i = 0, math.min(n, 16) - 1 do
      local v = string.unpack("<i4", back, i * 4 + 1)
      local f = string.unpack("<f", back, i * 4 + 1)

      print(("gpu: %2d  %08x  %d  %.6g"):format(i, v & 0xffffffff, v, f))
    end

    print(("gpu: %d instructions, %d us on the GPU"):format(k.instructions, (t:time() or 0) // 1000))
  end

  for _, b in ipairs(all) do b:free() end
  k:forget()
  return
end

if words[1] == "compute" then
  local colour = tonumber(words[2] or "") or 0x5a
  local r, why = gpu.compute_test(colour)
  local c = gpu.last_compute

  if r then
    print(("gpu: the execution units filled %d of 256 bytes with %02x, %d us after the tail moved")
          :format(r.right, colour & 0xff, r.elapsed_us))
  else
    print("gpu: " .. tostring(why))
  end

  if c then
    print(("gpu: %d of 256 right, %d written past; ring head %08x, tail %08x, errors %08x")
          :format(c.right, c.past, c.head, c.tail, c.esr))
  end

  return
end

if words[1] == "contexts" and words[2] == "hang" then
  local ARB, BB_START, STORE, BB_END = 0x05 << 23, 0x31 << 23 | 1 << 8 | 1, 0x20 << 23 | 2, 0x0a << 23
  local BUF, BATCH = 0x100000000, 0x200000
  local ep = sys.endpoint()
  local c, why = gpu.context("render")

  if not c then print("gpu: " .. tostring(why)); return end

  local buf, bat = sys.memory(1), sys.memory(1)

  assert(c:bind(BUF, buf) and c:bind(BATCH, bat))
  sys.region_write(bat, 0, string.pack("<I4I4I4I4", ARB, BB_START, BATCH & 0xffffffff, BATCH >> 32))

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
  local t0 = sys.ticks()

  assert(c:exec(BATCH, 16, 1))
  assert(c:wait(1, ep, 1))

  local m = sys.receive(ep, false, 2500)
  local ms = (sys.ticks() - t0) * 1000 // hz
  local st = c:state() or {}

  -- And after: a store, which must land.
  sys.region_write(bat, 0, string.pack("<I4I4I4I4I4", STORE, BUF & 0xffffffff, BUF >> 32, 0x4b6f7349, BB_END))
  assert(c:exec(BATCH, 20, 2))
  assert(c:wait(2, ep, 2))

  local after = sys.receive(ep, false, 2500)
  local value = sys.region_load32(buf, 0)
  local right = m and m.pulse and after and after.pulse and st.hangs == 1 and value == 0x4b6f7349

  print(("gpu: contexts hang %s: stopped after %d ms, %d hang%s, %d fault%s; then it wrote %08x")
        :format(right and "right" or "wrong", ms, st.hangs or -1, st.hangs == 1 and "" or "s",
                st.faults or -1, st.faults == 1 and "" or "s", value))
  c:forget()
  return
end

if words[1] == "contexts" then
  -- Commands, by hand from `intel_gpu_commands.h`: MI_INSTR(op, length).
  local LRI1, STORE, SRM, BB_END = 0x22 << 23 | 1, 0x20 << 23 | 2, 0x24 << 23 | 2, 0x0a << 23
  local GPR0 = 0x2600                       -- the render engine's CS_GPR(0)
  local BUF, BATCH = 0x100000000, 0x200000  -- the same addresses in both contexts
  local ep = sys.endpoint()
  local c, bufs, bats = {}, {}, {}
  local mine = { 0x11111111, 0x22222222 }
  local regs = { 0xa0a0a0a0, 0xb0b0b0b0 }
  local failed, pulses = {}, 0

  local function fail(why) failed[#failed + 1] = why end

  for i = 1, 2 do
    local why

    c[i], why = gpu.context("render")
    if not c[i] then print("gpu: " .. tostring(why)); return end

    bufs[i], bats[i] = sys.memory(1), sys.memory(1)

    local ok, err = c[i]:bind(BUF, bufs[i])
    if ok then ok, err = c[i]:bind(BATCH, bats[i]) end
    if not ok then print("gpu: context " .. c[i].id .. ": " .. tostring(err)); return end
  end

  -- What a context must be refused: a binding over its own, a value that
  -- does not move on.
  local refused = 0

  if not c[1]:bind(BUF, bufs[2]) then refused = refused + 1 else fail("a binding over another was taken") end

  -- Each run's batch, written, given, and waited for by a pulse.
  local function run(i, words_, signal)
    sys.region_write(bats[i], 0, string.pack("<" .. ("I4"):rep(#words_), table.unpack(words_)))

    local v, why = c[i]:exec(BATCH, #words_ * 4, signal)
    if not v then fail(("context %d's batch %d: %s"):format(c[i].id, signal, tostring(why))); return end

    local ok, err = c[i]:wait(signal, ep, i)
    if not ok then fail("a wait: " .. tostring(err)) end
  end

  local function told(n)
    for _ = 1, n do
      local m = sys.receive(ep, false, 1000)

      if m and m.pulse then pulses = pulses + 1 else fail("a pulse did not come") end
    end
  end

  local t0 = sys.ticks()

  -- The register read before it is set: a new context begins from the
  -- engine's default state, never from the other's, which ran just before.
  for i = 1, 2 do
    run(i, { SRM, GPR0, (BUF + 8) & 0xffffffff, BUF >> 32,
             LRI1, GPR0, regs[i], STORE, BUF & 0xffffffff, BUF >> 32, mine[i], BB_END }, 1)
  end
  told(2)

  for i = 1, 2 do
    run(i, { SRM, GPR0, (BUF + 4) & 0xffffffff, BUF >> 32, BB_END, 0 }, 2)
  end
  told(2)

  local elapsed = sys.ticks() - t0

  if c[1]:exec(BATCH, 20, 2) then fail("a value that does not move on was taken") else refused = refused + 1 end

  local got = {}
  local first = { sys.region_load32(bufs[1], 8), sys.region_load32(bufs[2], 8) }

  if first[2] == regs[1] or first[1] ~= first[2] then
    fail(("the second context began with %08x, the first with %08x: not both the engine's default")
         :format(first[2], first[1]))
  end

  for i = 1, 2 do
    local stored, kept = sys.region_load32(bufs[i], 0), sys.region_load32(bufs[i], 4)
    local st = c[i]:state() or {}

    got[i] = ("%08x/%08x"):format(stored, kept)
    if stored ~= mine[i] then fail(("context %d's page holds %08x, not %08x"):format(c[i].id, stored, mine[i])) end
    if kept ~= regs[i] then fail(("context %d's register came back %08x, not %08x"):format(c[i].id, kept, regs[i])) end
    if c[i]:reached() ~= 2 then fail(("context %d's fence page says %d"):format(c[i].id, c[i]:reached())) end
    if (st.faults or 1) ~= 0 or (st.hangs or 1) ~= 0 then
      fail(("context %d: %s faults, %s hangs, the last refused %08x"):format(c[i].id, tostring(st.faults),
           tostring(st.hangs), st.fault_word or 0))
    end
  end

  for i = 1, 2 do
    local ok, why = c[i]:forget()
    if not ok then fail("forget: " .. tostring(why)) end
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1

  print(("gpu: contexts %s: %d and %d, one address, pages %s and %s, %d of 4 pulses, %d of 2 refused, %d us; each began with %08x")
        :format(#failed == 0 and "right" or "wrong", c[1].id, c[2].id, got[1], got[2], pulses, refused,
                elapsed * 1000000 // hz, first[2]))

  for _, why in ipairs(failed) do print("gpu: contexts: " .. why) end
  return
end

if words[1] == "hang" then
  local which = words[2] == "render" and "render" or nil
  local r, why = gpu.hang_test(which)
  local h = gpu.last_hang
  local name = which and "the render engine" or "the blitter"

  if r then
    print(("gpu: %s spun until the driver reset it, %d ms after it was given; reset %d time%s since boot; then it wrote %08x")
          :format(name, r.reset_ms, r.resets, r.resets == 1 and "" or "s", r.value))
  else
    print("gpu: " .. name .. ": " .. tostring(why)
          .. (h and (" - %d ms waited, %d resets since boot, the store read %08x"):format(h.reset_ms, h.resets, h.value) or ""))
  end
  if h then print(("gpu: mode %08x, errors %08x"):format(h.mode, h.esr)) end
  return
end

if words[1] == "el" or words[1] == "el2" or words[1] == "elshared" then
  local contexts = words[1] == "el2" and 2 or words[1] == "elshared" and 3 or 1
  local times = math.tointeger(tonumber(words[2] or "")) or 1
  local r, why
  local done, fastest, slowest = 0, math.huge, 0

  for _ = 1, math.max(1, times) do
    r, why = gpu.execlist_test(contexts)
    if not r then break end
    done = done + 1
    fastest, slowest = math.min(fastest, r.elapsed_us), math.max(slowest, r.elapsed_us)
  end

  local e = gpu.last_execlist
  local held = {}
  for i, v in ipairs(e and e.values or {}) do held[i] = ("%08x"):format(v) end
  if r then
    print(("gpu: the video engine, %d context%s each at the same GPU address, %d of %d runs; %d us fastest, %d slowest; the pages hold %s")
          :format(math.min(contexts, 2), contexts > 1 and "s" or "", done, times, fastest, slowest, table.concat(held, " and ")))
  else
    print("gpu: " .. tostring(why) .. (done > 0 and (" after %d runs"):format(done) or "")
          .. (#held > 0 and ("; the pages hold " .. table.concat(held, " and ")) or ""))
  end
  if e then
    print(("gpu: %d status events, bits %04x; execlist status %08x, mode %08x, errors %08x")
          :format(e.events, e.seen, e.status, e.mode, e.esr))
  end
  return
end

if words[1] == "engine" then
  -- `gpu engine 300`: that many in a row, past the ring's 256 and round.
  local render = words[2] == "render"
  local times = math.tointeger(tonumber(words[render and 3 or 2] or "")) or 1
  local who = render and "the render engine" or "the blitter"
  local r, why
  local done, fastest, slowest, total = 0, math.huge, 0, 0

  for _ = 1, math.max(1, times) do
    r, why = gpu.engine_test(render and "render" or nil)
    if not r then break end
    done = done + 1
    fastest, slowest = math.min(fastest, r.elapsed_us), math.max(slowest, r.elapsed_us)
    total = total + r.elapsed_us
  end

  if r and times <= 1 then
    print(("gpu: %s wrote %08x where it was told, %d us after its tail moved"):format(who, r.value, r.elapsed_us))
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
