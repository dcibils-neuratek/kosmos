/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A row of 0xAARRGGBB into a viewer's pixel format. `pack.h` says what for.
 *
 * **Eight pixels at a time**, in GCC's own vector types as `gfx.c`'s blend
 * is written - one implementation, NEON on AArch64 and SSE2 on x86-64, the
 * compiler choosing the instructions - and the scalar loop for what is left
 * of a row. Diego, 29 September: "Make sure we use simd vector arithmetic
 * when possible in all you code". A VNC frame at 1920x1080 is two million
 * of these.
 *
 * **Measured on this Mac's cores** (`tools/test_pack.c` prints it): a frame
 * in the surface's own format 0.39 ms against 5.8 a pixel at a time, 15x;
 * 565 and big-endian 1.5 ms against 3.4, about 2x, the three multiplies of
 * the rounding being the work there. Eight lanes rather than four for a
 * quarter more on the common case; sixteen gave a few per cent beyond.
 *
 * **The rounding is the scalar one to the bit.** A channel becomes
 * `(c * max + 127) / 255`, which is `round(c * max / 255)` - 255 is odd, so
 * there is never a tie to break. A vector unit has no divide, so the lanes
 * take the old identity for dividing by 255, `(t + (t >> 8)) >> 8` with
 * `t = x + 128`, which is exact while `x` fits sixteen bits: so the vector
 * path is taken when every channel's largest value is 255 or less - all the
 * formats a viewer actually asks for - and a ten-bit channel goes a pixel
 * at a time. `tools/test_pack.c` checks the identity for every `c` and
 * `max` it is used for, and the rows against the scalar loop.
 *
 * And the format a surface already has - 32 bits, little-endian, red at 16
 * - is each row copied with its alpha byte cleared, which is what most
 * viewers ask for. Cleared rather than passed through, because the byte a
 * format does not use is zero in the scalar reference, and the two paths
 * are one answer.
 */

#include <stddef.h>
#include <string.h>

#include "pack.h"

/*
 * The lanes load and store through `__builtin_memcpy` of a vector's size,
 * which GCC makes one instruction. Plain `memcpy` was a call: under
 * `-ffreestanding` GCC may not take `memcpy` to be the C library's, so on
 * ARM every sixteen bytes went through the libc's loop - which a test on
 * the Mac, whose compiler inlines it, could not show (`roadmap.md` 6zz h).
 */
typedef uint32_t u32x8 __attribute__((vector_size(32)));
typedef uint16_t u16x8 __attribute__((vector_size(16)));
typedef uint8_t  u8x8  __attribute__((vector_size(8)));

static inline uint32_t scale(uint32_t c, uint32_t max)
{
    return (c * max + 127u) / 255u;
}

uint32_t gfx_pack_pixel(uint32_t pixel, const struct gfx_pack_format *f)
{
    return scale((pixel >> 16) & 0xFFu, f->rmax) << f->rshift
         | scale((pixel >> 8) & 0xFFu, f->gmax) << f->gshift
         | scale(pixel & 0xFFu, f->bmax) << f->bshift;
}

/* One value, `bpp / 8` bytes of it, in the byte order asked for. */
static inline uint8_t *store(uint8_t *o, uint32_t v, unsigned bpp, bool big)
{
    if (bpp == 8) {
        *o = (uint8_t)v;
        return o + 1;
    }

    if (bpp == 16) {
        if (big) { o[0] = (uint8_t)(v >> 8); o[1] = (uint8_t)v; }
        else     { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); }
        return o + 2;
    }

    if (big) {
        o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16);
        o[2] = (uint8_t)(v >> 8);  o[3] = (uint8_t)v;
    } else {
        o[0] = (uint8_t)v;         o[1] = (uint8_t)(v >> 8);
        o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24);
    }

    return o + 4;
}

void gfx_pack_row_scalar(const uint32_t *src, uint8_t *out, unsigned width,
                         const struct gfx_pack_format *f)
{
    unsigned i;

    for (i = 0; i < width; i++) {
        out = store(out, gfx_pack_pixel(src[i], f), f->bpp, f->big);
    }
}

void gfx_pack_row(const uint32_t *src, uint8_t *out, unsigned width,
                  const struct gfx_pack_format *f)
{
    unsigned i = 0;

    if (f->bpp == 32 && !f->big && f->rmax == 255 && f->gmax == 255
        && f->bmax == 255 && f->rshift == 16 && f->gshift == 8
        && f->bshift == 0) {
        for (; i + 8 <= width; i += 8) {
            u32x8 p;

            __builtin_memcpy(&p, src + i, sizeof(p));
            p &= 0xFFFFFFu;
            __builtin_memcpy(out + i * 4u, &p, sizeof(p));
        }

        gfx_pack_row_scalar(src + i, out + i * 4u, width - i, f);
        return;
    }

    if (f->rmax <= 255 && f->gmax <= 255 && f->bmax <= 255) {
        size_t per = f->bpp / 8u;

        for (; i + 8 <= width; i += 8) {
            u32x8 p, r, g, b, v;

            __builtin_memcpy(&p, src + i, sizeof(p));

            /* `round(c * max / 255)`, exact while `c * max` fits 16 bits.
             * Written out here rather than as a function: a 32-byte vector
             * returned from one is an ABI x86-64 has only with AVX. */
            r = ((p >> 16) & 0xFFu) * f->rmax + 128u;
            g = ((p >> 8) & 0xFFu) * f->gmax + 128u;
            b = (p & 0xFFu) * f->bmax + 128u;
            r = (r + (r >> 8)) >> 8;
            g = (g + (g >> 8)) >> 8;
            b = (b + (b >> 8)) >> 8;

            v = r << f->rshift | g << f->gshift | b << f->bshift;

            if (f->bpp == 32) {
                if (f->big) {
                    v = (v >> 24) | ((v >> 8) & 0xFF00u)
                      | ((v << 8) & 0xFF0000u) | (v << 24);
                }

                __builtin_memcpy(out + i * per, &v, sizeof(v));
            } else if (f->bpp == 16) {
                u16x8 n;

                if (f->big) {
                    v = ((v >> 8) & 0xFFu) | ((v & 0xFFu) << 8);
                }

                n = __builtin_convertvector(v, u16x8);
                __builtin_memcpy(out + i * per, &n, sizeof(n));
            } else {
                u8x8 n = __builtin_convertvector(v, u8x8);

                __builtin_memcpy(out + i * per, &n, sizeof(n));
            }
        }

        out += i * per;
    }

    gfx_pack_row_scalar(src + i, out, width - i, f);
}
