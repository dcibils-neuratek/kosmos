-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The LLM Kit's GPU kernels (`user/lib/llmgpu.lua`, `docs/xavier.md` X7
-- G1) held to the processor's arithmetic **to the bit**: Q4_0 rows times
-- Q8_0 inputs, compiled by `kc.lua`, run in the emulator of the execution
-- units (`build/host/eu_run`) in SIMD8 and SIMD16, and every sum compared
-- with `llm_dot_q4_0`'s, worked out here in single precision rounded at
-- each step as C rounds it.
--
-- The rows are random, with scales of every kind a file can hold -
-- ordinary, tiny, subnormal, nought and negative - and the grid's last
-- thread only part full, so the lanes past the last row are seen to write
-- nothing.
--
--   lua tools/test_llmgpu.lua build/host/eu_run [--control]
--
-- `--control` runs the kernel with its product taken in another order,
-- `(s * sw) * xs` - close, and not the processor's. (A scale made with
-- `exp2` would be the obvious control, and the emulator's `exp2` is exact
-- for a whole power, so it does not bite here; the M700's need not be.)

local kc = dofile("user/lib/kc.lua")
local llmgpu = dofile("user/lib/llmgpu.lua")
local RUN = assert(arg[1], "the emulator's runner, build/host/eu_run")
local CONTROL = arg[2] == "--control"
local OUT = RUN .. ".llmgpu.out"
local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

local function hex(s) return (s:gsub(".", function(c) return ("%02x"):format(c:byte()) end)) end
local function unhex(h) return (h:gsub("%x%x", function(x) return string.char(tonumber(x, 16)) end)) end
local function f32(x) return (string.unpack("<f", string.pack("<f", x))) end

-- A half-precision float's value, as llm_f16_to_f32 gives it.
local function half(h)
  local sign = (h >> 15) & 1
  local e = (h >> 10) & 31
  local m = h & 1023
  local v

  if e == 0 then v = m * 2.0 ^ -24
  elseif e == 31 then v = m == 0 and math.huge or (0 / 0)
  else v = (1024 + m) * 2.0 ^ (e - 25) end

  return sign == 1 and -v or v
end

-- Deterministic, so a failure is the same failure next time.
local seed = 12345
local function rnd(n)
  seed = (seed * 1103515245 + 12345) % 2147483648
  return seed % n
end

-- Scales of every kind: mostly the ordinary sizes of a real model's,
-- and some tiny, subnormal, nought or negative.
local function a_scale()
  local k = rnd(10)

  if k == 0 then return rnd(1024) end                       -- subnormal, or nought
  if k == 1 then return 0x8000 | rnd(1024) end              -- negative subnormal
  if k == 2 then return 0x0400 + rnd(0x400) end             -- the smallest normal ones
  if k == 3 then return 0x8000 | (0x2000 + rnd(0x1800)) end -- negative
  return 0x2000 + rnd(0x1800)                               -- about 2^-7 to 2^-2
end

local function case(name, rows, blocks, inputs, w_at)
  -- The weight: w_at bytes before it, then the rows' blocks of 18, laid
  -- out in tiles as `llmgpu.tile` says - the last tile's missing rows
  -- nought.
  local wv, row_bytes = {}, {}

  for r = 1, rows do
    wv[r] = {}
    local bytes = {}

    for b = 1, blocks do
      local h = a_scale()
      local q = {}

      for j = 1, 16 do q[j] = rnd(256) end

      wv[r][b] = { scale = half(h), q = q }
      bytes[#bytes + 1] = string.pack("<I2", h) .. string.char(table.unpack(q))
    end

    row_bytes[r] = table.concat(bytes)
  end

  local tiles = (rows + llmgpu.TILE - 1) // llmgpu.TILE
  local cells = {}

  for i = 1, tiles * (blocks // 2) * 144 do cells[i] = "\0\0\0\0" end

  for r = 1, rows do
    for p = 0, blocks // 2 - 1 do
      for j = 0, 8 do
        cells[llmgpu.tile(r - 1, p, j, blocks) // 4 + 1] = row_bytes[r]:sub(p * 36 + j * 4 + 1, p * 36 + j * 4 + 4)
      end
    end
  end

  local w = { string.rep("\0", w_at), table.concat(cells) }

  -- The inputs: 32 signed values a block, and a float scale a block.
  local xq, xm, xv = {}, {}, {}

  for t = 1, inputs do
    xv[t] = {}

    for b = 1, blocks do
      local vals = {}

      for i = 1, 32 do vals[i] = rnd(255) - 127 end

      local s = f32((rnd(2000) + 1) / 2000 * 0.05)
      xv[t][b] = { scale = s, v = vals }
      xq[#xq + 1] = string.pack(("<" .. ("b"):rep(32)), table.unpack(vals))
      local total = 0
      for i = 1, 32 do total = total + vals[i] end
      xv[t][b].sum = total
    end
  end

  -- Each pair's record: the two scales, then the two sums.
  for t = 1, inputs do
    for b = 1, blocks, 2 do
      local p, q = xv[t][b], xv[t][b + 1]
      xm[#xm + 1] = string.pack("<ffi4i4", p.scale, q.scale, p.sum, q.sum)
    end
  end

  -- What the processor makes: llm_dot_q4_0, block after block.
  local want = {}

  for t = 1, inputs do
    for r = 1, rows do
      local sum = 0.0

      for b = 1, blocks do
        local wb, xb, s = wv[r][b], xv[t][b], 0

        for j = 1, 16 do
          s = s + ((wb.q[j] & 15) - 8) * xb.v[j] + ((wb.q[j] >> 4) - 8) * xb.v[j + 16]
        end

        sum = f32(sum + f32(s * f32(wb.scale * xb.scale)))
      end

      want[(t - 1) * rows + r] = sum
    end
  end

  local src = llmgpu.q4_0

  if CONTROL then
    -- The product taken in another order, `(s * sw) * xs` - each step
    -- rounded, so close to the processor's and not its.
    local function swap(text, from, to)
      local a, b = text:find(from, 1, true)
      assert(a, "the control's line is not in the kernel: " .. from)
      return text:sub(1, a - 1) .. to .. text:sub(b + 1)
    end

    src = swap(src, "float s = sum + float(block(q0, q1, q2, q3, xa, lanev(m, 2))) * (h0 * asfloat(lanev(m, 0)));",
                    "float s = sum + (float(block(q0, q1, q2, q3, xa, lanev(m, 2))) * h0) * asfloat(lanev(m, 0));")
    src = swap(src, "return s + float(block(e0, e1, e2, e3, xb, lanev(m, 3))) * (h1 * asfloat(lanev(m, 1)));",
                    "return s + (float(block(e0, e1, e2, e3, xb, lanev(m, 3))) * h1) * asfloat(lanev(m, 1));")
  end

  for _, simd in ipairs { 8, 16 } do
    local code, info = kc.assemble(src, { simd = simd })

    if not code then
      check(false, ("%s, SIMD%d: does not compile: %s"):format(name, simd, tostring(info and info.why or info)))
    else
      local constants = kc.pack(info, { rows = rows, blocks = blocks, w_at = w_at, inputs = inputs })
      local groups = (rows + simd - 1) // simd
      local wb, xqb, xmb = table.concat(w), table.concat(xq), table.concat(xm)
      -- `out` a little larger than the sums, so a lane past the last row
      -- that wrote would be seen.
      local out_bytes = (inputs * rows + simd) * 4
      local cmd = ("%s %d %d 1 %s %s %d:%s %d:%s %d:%s %d"):format(RUN, simd, groups, hex(constants),
                    hex(code), #wb, hex(wb), #xqb, hex(xqb), #xmb, hex(xmb), out_bytes)
      os.execute(cmd .. " > " .. OUT .. " 2>&1")
      local f = io.open(OUT)
      local said = f and f:read("a") or "error: the runner said nothing"
      if f then f:close() end

      if said:match("^error") then
        check(false, ("%s, SIMD%d: %s"):format(name, simd, said:gsub("%s+$", "")))
      else
        local lines = {}
        for line in said:gmatch("[^\n]+") do lines[#lines + 1] = line end
        local got = unhex(lines[4] or "")
        local wrong, first = 0, nil

        for i = 1, inputs * rows do
          local g = string.unpack("<f", got, (i - 1) * 4 + 1)

          if string.pack("<f", g) ~= string.pack("<f", want[i]) then
            wrong = wrong + 1
            first = first or ("sum %d is %.9g, the processor's %.9g"):format(i - 1, g, want[i])
          end
        end

        local past = got:sub(inputs * rows * 4 + 1)
        check(wrong == 0, ("%s, SIMD%d: %d of %d sums not the processor's to the bit; %s")
                          :format(name, simd, wrong, inputs * rows, tostring(first)))
        check(past == string.rep("\0", #past), ("%s, SIMD%d: a lane past the last row wrote"):format(name, simd))
      end
    end
  end
end

case("one input, rows a whole thread", 16, 2, 1, 0)
case("three inputs, a part-full last thread, the weight 36 bytes in", 21, 6, 3, 36)
case("phi-2's narrowest, 80 blocks", 9, 80, 2, 0)
case("six inputs, a second pass of four part full", 32, 4, 6, 0)

if #fails > 0 then
  for _, f in ipairs(fails) do print("FAIL: " .. f) end
  print(("FAIL: %d of %d checks"):format(#fails, checks))
  os.exit(1)
end

print(("PASS: %d checks - Q4_0 rows times Q8_0 inputs on the GPU, in the emulator, the processor's "
       .. "sums to the bit in SIMD8 and SIMD16"):format(checks))
