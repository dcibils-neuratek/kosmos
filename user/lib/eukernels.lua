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
-- **wild** is the fill with its write sent stateless (binding-table entry
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

--
-- **mandelbrot** is GPU Fractal's (`docs/compute.md` C4), and the IDE's
-- lesson reads it: every one of its lines but the branches and the sends
-- assembles as Mesa's `gentool` makes it, and `fractal.c` computes what it
-- computes, step for step, on the processor.
--
kernels.mandelbrot = [[
// GPU Fractal's kernel: a thread a group, sixteen pixels of a row, a lane
// each. r0 is the thread's header - r0.1 its group across, r0.6 down - and
// r1 the constants: r1.0 the left edge's x, r1.1 a pixel's width, r1.2 the
// top's y, r1.3 a pixel's height, r1.4 the most iterations.

// Which pixel: the group's first, and the lanes 0 to 15 beside it.
mov (8|M0) r10.0<1>:uw 0x76543210:v
add (8|M0) r10.8<1>:uw r10.0<8;8,1>:uw 8:uw
mul (1|M0) r2.0<1>:ud r0.1<0;1,0>:ud 0x00000010:ud
add (16|M0) r12.0<1>:ud r10.0<8;8,1>:uw r2.0<0;1,0>:ud
mov (16|M0) r14.0<1>:f r12.0<8;8,1>:ud
mov (16|M0) r18.0<1>:f r0.6<0;1,0>:ud

// c, the point: x and y from the corner and a pixel's size.
mad (16|M0) r16.0<0>:f r1.0<0;0>.r:f r14.0<0;0>:f r1.1<0>.r:f {Align16}
mad (16|M0) r20.0<0>:f r1.2<0;0>.r:f r18.0<0;0>:f r1.3<0>.r:f {Align16}

// z = 0, the count n = 0, the iteration k = 0.
mov (16|M0) r22.0<1>:f 0x00000000:f
mov (16|M0) r24.0<1>:f 0x00000000:f
mov (16|M0) r26.0<1>:d 0:d
mov (1|M0) r8.0<1>:d 0:d

// z = z^2 + c, counted for the lanes still inside the circle of radius 2.
loop:
mul (16|M0) r30.0<1>:f r22.0<8;8,1>:f r22.0<8;8,1>:f
mul (16|M0) r32.0<1>:f r24.0<8;8,1>:f r24.0<8;8,1>:f
add (16|M0) r34.0<1>:f r30.0<8;8,1>:f r32.0<8;8,1>:f
cmp (16|M0) (lt)f0.0 null<1>:f r34.0<8;8,1>:f 0x40800000:f
(f0.0) add (16|M0) r26.0<1>:d r26.0<8;8,1>:d 1:d
mul (16|M0) r36.0<1>:f r22.0<8;8,1>:f r24.0<8;8,1>:f
add (16|M0) r36.0<1>:f r36.0<8;8,1>:f r36.0<8;8,1>:f
add (16|M0) r24.0<1>:f r36.0<8;8,1>:f r20.0<8;8,1>:f
add (16|M0) r38.0<1>:f r30.0<8;8,1>:f -r32.0<8;8,1>:f
add (16|M0) r22.0<1>:f r38.0<8;8,1>:f r16.0<8;8,1>:f
add (1|M0) r8.0<1>:d r8.0<0;1,0>:d 1:d
cmp (1|M0) (ge)f1.0 null<1>:d r8.0<0;1,0>:d r1.4<0;1,0>:d
(f1.0) jmpi (1) done
(f0.0.any16h) jmpi (1) loop
done:

// The colour, from how quickly it left: red, green and blue each a
// multiple of n, a byte; black inside.
mul (16|M0) r42.0<1>:d r26.0<8;8,1>:d 9:d
and (16|M0) r42.0<1>:ud r42.0<8;8,1>:ud 0x000000ff:ud
shl (16|M0) r42.0<1>:ud r42.0<8;8,1>:ud 0x00000010:ud
mul (16|M0) r44.0<1>:d r26.0<8;8,1>:d 4:d
and (16|M0) r44.0<1>:ud r44.0<8;8,1>:ud 0x000000ff:ud
shl (16|M0) r44.0<1>:ud r44.0<8;8,1>:ud 0x00000008:ud
mul (16|M0) r46.0<1>:d r26.0<8;8,1>:d 15:d
and (16|M0) r46.0<1>:ud r46.0<8;8,1>:ud 0x000000ff:ud
or (16|M0) r40.0<1>:ud r42.0<8;8,1>:ud r44.0<8;8,1>:ud
or (16|M0) r40.0<1>:ud r40.0<8;8,1>:ud r46.0<8;8,1>:ud
or (16|M0) r40.0<1>:ud r40.0<8;8,1>:ud 0xff000000:ud
cmp (16|M0) (ge)f0.0 null<1>:d r26.0<8;8,1>:d r1.4<0;1,0>:d
(f0.0) mov (16|M0) r40.0<1>:ud 0xff000000:ud

// Written to the picture, buffer 0: a block 64 bytes wide and a row high
// at the group's place, by the data port's media block write.
mov (8|M0) r4.0<1>:ud r0.0<8;8,1>:ud
shl (1|M0) r4.0<1>:ud r0.1<0;1,0>:ud 0x00000006:ud
mov (1|M0) r4.1<1>:ud r0.6<0;1,0>:ud
mov (1|M0) r4.2<1>:ud 0x0000003f:ud
mov (16|M0) r5.0<1>:ud r40.0<8;8,1>:ud
send.hdc1 (16|M0) acc0 r4:d 0x00000000 0x060A8000
mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud
send.ts (16|M0) null r112:d 0x00000000 0x02000010 {EOT}
]]

return kernels
