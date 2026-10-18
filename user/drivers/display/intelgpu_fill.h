/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The first program Kosmos runs on the GPU's execution units**
 * (`docs/m700-2d.md` 7b): each thread group fills 16 bytes of a row of
 * surface 0 with the colour in the first byte of its constants - the
 * thread group's X times 16 the byte, its Y the row - by a media block
 * write, and ends its thread.
 *
 * **The words are intel-gpu-tools' own**, `gen9_gpgpu_kernel` in
 * `lib/gpgpu_fill.c` (igt-gpu-tools 2.6, MIT, Copyright (c) 2014 Intel
 * Corporation; assembled from `lib/i915/shaders/gpgpu/gpgpu_fill.gxa`):
 * run on Skylake by its own tests, so the first try here has the batch
 * around it as its only unknown. As Mesa's `gentool disasm -p skl -v`
 * reads them (built on the Mac for this, `testing.md` 18.681):
 *
 *           mov (4|M0)                r1.0<1>:ub    r1.0<0;1,0>:ub
 *           mul (1|M0)                r2.0<1>:ud    r0.1<0;1,0>:ud    0x00000010:ud
 *           mov (1|M0)                r2.1<1>:ud    r0.6<0;1,0>:ud
 *           mov (8|M0)                r4.0<1>:ud    r0.0<8;8,1>:ud
 *           mov (2|M0)                r4.0<1>:ud    r2.0<2;2,1>:ud
 *           mov (1|M0)                r4.2<1>:ud    0x0000000f:ud
 *           mov (16|M0)               r5.0<1>:ud    r1.0<0;1,0>:ud
 *           send.hdc1 (16|M0)         acc0    r4             a0.0        0x060A8000
 *           mov (8|M0)                r112.0<1>:ud  r0.0<8;8,1>:ud
 *           send.ts (16|M0)           null    r112           a0.0        0x02000010   {EOT}
 *
 * r0 is the thread's header (r0.1 its group's X, r0.6 its Y), r1 the
 * constants, r4 and r5 the write's header and data, r112 r0 again for the
 * end of the thread.
 */
#ifndef KOSMOS_INTELGPU_FILL_H
#define KOSMOS_INTELGPU_FILL_H

#include <stdint.h>

static const uint32_t intelgpu_fill_kernel[][4] = {
    { 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
    { 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
    { 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
    { 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
    { 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
    { 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
    { 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
    { 0x0c800031, 0x24000a40, 0x06000080, 0x060a8000 },
    { 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
    { 0x07800031, 0x20000a40, 0x06000e00, 0x82000010 },
};

#endif
