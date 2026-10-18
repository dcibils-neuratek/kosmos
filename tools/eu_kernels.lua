-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Kernels for the GPU driver's check (`docs/compute.md` C2, decision D2),
-- assembled by `user/lib/eu.lua` and written one a line - what the check
-- must answer, at which instruction, a name, and the bytes in hex - for
-- `tools/test_intelgpu_kernel.c` to hold `intelgpu_kernel.c` to.
--
--   lua tools/eu_kernels.lua build/host/eu_kernels.txt

local eu = dofile("user/lib/eu.lua")
local out = assert(io.open(assert(arg[1], "where to write the kernels"), "w"))

-- GPU_KERNEL_*, as gpuproto.h numbers them.
local OK, SIZE, COMPACT, DEBUG, OPCODE, INDIRECT, REGISTER, UNIT, MESSAGE, DESC, BRANCH, END, EOT =
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12

local function hex(s)
  return (s:gsub(".", function(c) return ("%02x"):format(c:byte()) end))
end

local count = 0

local function case(name, why, at, code)
  out:write(("%d %d %s %s\n"):format(why, at, name, hex(code)))
  count = count + 1
end

local function kernel(name, why, at, text)
  local code, err = eu.assemble(text)

  assert(code, name .. ": " .. tostring(err))
  case(name, why, at, code)
  return code
end

-- An instruction's bits changed after assembly: what eu.lua will not write,
-- and a hostile program would.
local function poke(code, index, lo, hi, v)
  local w = { string.unpack("<I4I4I4I4", code, index * 16 + 1) }

  for bit = lo, hi do
    local word, at = bit // 32 + 1, bit % 32
    w[word] = (w[word] & ~(1 << at)) | (((v >> (bit - lo)) & 1) << at)
  end

  return code:sub(1, index * 16) .. string.pack("<I4I4I4I4", w[1], w[2], w[3], w[4])
         .. code:sub(index * 16 + 17)
end

local END_THREAD = [[
mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud
send.ts (16|M0) null r112 0x00000000 0x02000010 {EOT}
]]

-- What may run: arithmetic, a loop, a write to buffer 2, the end.
local good = kernel("mandelbrot-shaped", OK, 0, [[
mov (16|M0) r10.0<1>:f r1.0<0;1,0>:f
loop:
mul (16|M0) r20.0<1>:f r10.0<8;8,1>:f r10.0<8;8,1>:f
add (16|M0) r10.0<1>:f r20.0<8;8,1>:f r1.1<0;1,0>:f
cmp (16|M0) (lt)f0.0 null<1>:f r20.0<8;8,1>:f 0x40800000:f
(f0.0.any16h) jmpi (1) loop
mov (8|M0) r4.0<1>:ud r0.0<8;8,1>:ud
send.hdc1 (16|M0) acc0 r4 0x00000000 0x060A8002
]] .. END_THREAD)

-- A branch to the very first instruction, and one to the last.
kernel("branch-to-first-and-last", OK, 0, [[
top:
add (16|M0) r10.0<1>:d r10.0<8;8,1>:d 1:d
(f0.0) jmpi (1) top
(f0.0) jmpi (1) last
mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud
last:
send.ts (16|M0) null r112 0x00000000 0x02000010 {EOT}
]])

-- The data port, every kind that names a buffer: dword scattered read on
-- the data cache, an untyped write on its second port, a constant read.
kernel("data-port-kinds", OK, 0, [[
send.hdc0 (16|M0) r20 r4 0x00000000 0x0410C001
send.hdc1 (16|M0) null r4 0x00000000 0x0A026003
send.hdc_ro (16|M0) r20 r4 0x00000000 0x0410C004
]] .. END_THREAD)

-- Refused: memory it was not handed.
kernel("stateless", MESSAGE, 0, "send.hdc1 (16|M0) null r4 0x00000000 0x0A0260FF\n" .. END_THREAD)
kernel("shared-local", MESSAGE, 0, "send.hdc1 (16|M0) null r4 0x00000000 0x0A0260FE\n" .. END_THREAD)
kernel("past-eight-buffers", MESSAGE, 0, "send.hdc1 (16|M0) null r4 0x00000000 0x0A026008\n" .. END_THREAD)
kernel("a64-write", MESSAGE, 0, "send.hdc1 (16|M0) null r4 0x00000000 0x0A064000\n" .. END_THREAD)
kernel("scratch", MESSAGE, 0, "send.hdc0 (16|M0) null r4 0x00000000 0x0A040000\n" .. END_THREAD)
kernel("sampler", UNIT, 0, "send.smpl (16|M0) r20 r4 0x00000000 0x04000000\n" .. END_THREAD)
kernel("spawner-not-ending", MESSAGE, 0, "send.ts (16|M0) null r112 0x00000000 0x02000010\n" .. END_THREAD)
kernel("end-from-r20", EOT, 0, "send.ts (16|M0) null r20 0x00000000 0x02000010 {EOT}\n" .. END_THREAD)
kernel("predicated-end", EOT, 1, "mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud\n"
       .. "(f0.0) send.ts (16|M0) null r112 0x00000000 0x02000010 {EOT}\n" .. END_THREAD)

-- Refused: the kernel left.
kernel("no-end", END, 1, "mov (8|M0) r10.0<1>:ud r0.0<8;8,1>:ud\nadd (16|M0) r10.0<1>:d r10.0<8;8,1>:d 1:d\n")
kernel("branch-before", BRANCH, 0, "jmpi (1) -32:d\n" .. END_THREAD)
kernel("branch-past", BRANCH, 0, "jmpi (1) 32:d\n" .. END_THREAD)
kernel("branch-misaligned", BRANCH, 0, "jmpi (1) 8:d\n" .. END_THREAD)

-- The bits eu.lua does not write, put in by hand.
local plain = kernel("plain", OK, 0, "add (16|M0) r10.0<1>:d r11.0<8;8,1>:d 1:d\n" .. END_THREAD)

case("compacted", COMPACT, 0, poke(plain, 0, 29, 29, 1))
case("breakpoint", DEBUG, 1, poke(plain, 1, 30, 30, 1))
case("wait", OPCODE, 0, poke(plain, 0, 0, 6, 48))
case("movi", OPCODE, 0, poke(plain, 0, 0, 6, 3))
case("split-send", OPCODE, 0, poke(plain, 0, 0, 6, 51))
case("dst-indirect", INDIRECT, 0, poke(plain, 0, 63, 63, 1))
case("src0-indirect", INDIRECT, 0, poke(plain, 0, 79, 79, 1))
case("dst-a0", REGISTER, 0, poke(poke(plain, 0, 35, 36, 0), 0, 53, 60, 0x10))
case("dst-cr0", REGISTER, 0, poke(poke(plain, 0, 35, 36, 0), 0, 53, 60, 0x80))
case("src-file-2", REGISTER, 0, poke(plain, 0, 41, 42, 2))
case("descriptor-in-a-register", DESC, 6, poke(good, 6, 89, 90, 1))
case("branch-through-a-register", BRANCH, 4, poke(good, 4, 89, 90, 1))
case("not-whole", SIZE, 0, plain:sub(1, 24))

out:close()
print(("eu_kernels: %d kernels"):format(count))
