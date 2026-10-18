-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Kernels the system's own checks run on the GPU, as text for `eu.lua`
-- (`docs/compute.md` C2): one copy, which `gpu kernel` hands the driver and
-- `tools/test_eu.lua` holds to the bytes the M700 has run.
--
--   local kernels = use("/Kosmos/Libraries/eukernels.lua")
--   local code, lines = eu.assemble(kernels.fill)
--
-- **fill** is intel-gpu-tools' own (`gen9_gpgpu_kernel` in
-- `lib/gpgpu_fill.c`, igt-gpu-tools 2.6, MIT, Copyright (c) 2014 Intel
-- Corporation) as Mesa's `gentool` reads `intelgpu_fill.h`, with the
-- sends' payload types written out: each thread group fills 16 bytes of a
-- row of buffer 0 with the first byte of its constants - its X times 16 the
-- byte, its Y the row - by a media block write, and ends its thread.
--
-- **wild** is the same with its write sent stateless (binding-table entry
-- 255) - anywhere in the GPU's memory - which the driver must refuse at its
-- eighth line.

local kernels = {}

kernels.fill = [[
mov (4|M0) r1.0<1>:ub r1.0<0;1,0>:ub
mul (1|M0) r2.0<1>:ud r0.1<0;1,0>:ud 0x00000010:ud
mov (1|M0) r2.1<1>:ud r0.6<0;1,0>:ud
mov (8|M0) r4.0<1>:ud r0.0<8;8,1>:ud
mov (2|M0) r4.0<1>:ud r2.0<2;2,1>:ud
mov (1|M0) r4.2<1>:ud 0x0000000f:ud
mov (16|M0) r5.0<1>:ud r1.0<0;1,0>:ud
send.hdc1 (16|M0) acc0 r4:d 0x00000000 0x060A8000
mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud
send.ts (16|M0) null r112:d 0x00000000 0x02000010 {EOT}
]]

kernels.wild = kernels.fill:gsub("0x060A8000", "0x060A80FF")

return kernels
