-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The LLM Kit's GPU kernels as C (`user/kits/llm/llm_gpu.c`, `docs/xavier.md`
-- X7): `user/lib/llmgpu.lua` compiled by `kc.lua` when the kit is built, so
-- the server - which is C, and runs no Lua - hands the driver the bytes the
-- host tests ran. The constants' and buffers' places are written out with
-- them, so the C cannot fill them in another order than the kernel reads.
--
--   build/host/lua tools/llmgpu_header.lua OUT.h

local kc = dofile("user/lib/kc.lua")
local llmgpu = dofile("user/lib/llmgpu.lua")
local SIMD = 16

local code, info = kc.assemble(llmgpu.q4_0, { simd = SIMD })
assert(code, info and info.why or tostring(info))

local out = { "/* Made by tools/llmgpu_header.lua from user/lib/llmgpu.lua; not to be edited. */",
              "#ifndef KOSMOS_LLMGPU_KERNELS_H", "#define KOSMOS_LLMGPU_KERNELS_H", "",
              "#include <stdint.h>", "", ("#define LLMGPU_Q4_0_SIMD %du"):format(SIMD) }

for i, c in ipairs(info.constants) do
  -- The C fills an array of ints, so each must be one, in its place.
  assert(c.t == "d" and c.off == (i - 1) * 4, "constant " .. c.name .. " is not an int at " .. (i - 1) * 4)
  out[#out + 1] = ("#define LLMGPU_Q4_0_%s %d"):format(c.name:upper(), i - 1)
end

for i, b in ipairs(info.buffers) do
  out[#out + 1] = ("#define LLMGPU_Q4_0_%s %d"):format((type(b) == "table" and b.name or b):upper(), i - 1)
end

out[#out + 1] = ""
out[#out + 1] = ("static const uint8_t llmgpu_q4_0[%d] = {"):format(#code)

for i = 1, #code, 16 do
  local row = {}
  for j = i, math.min(i + 15, #code) do row[#row + 1] = ("0x%02x"):format(code:byte(j)) end
  out[#out + 1] = "  " .. table.concat(row, ", ") .. ","
end

out[#out + 1] = "};"
out[#out + 1] = ""
out[#out + 1] = "#endif"

local f = assert(io.open(assert(arg[1], "usage: llmgpu_header.lua OUT.h"), "w"))
f:write(table.concat(out, "\n"), "\n")
f:close()
