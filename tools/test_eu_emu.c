/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The emulator of the execution units held to what the kernels must do
 * (`docs/cafesa3d-gpu.md` R2): the kernels in the file `tools/eu_kernels.lua`
 * writes, run as the M700 runs them and held to the processor's answer -
 *
 *   - intel-gpu-tools' fill: 256 bytes of the colour, none past them;
 *   - GPU Fractal's Mandelbrot against `fractal.c`'s arithmetic, every
 *     pixel of an 800 by 600 view;
 *   - lesson 15's pattern against its formula;
 *   - the structured branches: lanes that take each side of an `if` and
 *     leave a `while` at their own count, held to the same loop in C.
 *
 *   test_eu_emu build/host/eu_kernels.txt
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eu_emu.h"
#include "../user/include/gpuproto.h"
#include "../user/drivers/display/intelgpu_fill.h"
#include "../user/bin/apps/fractal/fractal_core.h"

static int checks, failed;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

static int nibble(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* The kernel named `want` in the file, into `words`: its bytes, or 0. */
static uint32_t kernel(const char *path, const char *want, uint32_t *words, size_t room)
{
    static char line[200000];
    FILE *f = fopen(path, "r");
    uint32_t got = 0;

    if (f == NULL) {
        return 0;
    }

    while (fgets(line, sizeof line, f) != NULL) {
        unsigned why, at;
        char name[128];
        int skip;

        if (sscanf(line, "%u %u %127s %n", &why, &at, name, &skip) == 3 && strcmp(name, want) == 0) {
            const char *hex = line + skip;
            size_t n = strcspn(hex, "\r\n") / 2, i;

            for (i = 0; i < n && i < room; i++) {
                ((unsigned char *)words)[i] = (unsigned char)(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
            }

            got = (uint32_t)n;
            break;
        }
    }

    fclose(f);
    return got;
}

static uint32_t words[16384];

int main(int argc, char **argv)
{
    char what[256];
    struct eu_stats st;
    uint32_t at;
    const char *why;

    if (argc < 2) {
        printf("FAIL: no kernels file - tools/eu_kernels.lua writes it\n");
        return 1;
    }

    /* intel-gpu-tools' fill, a 64 by 4 surface of bytes (16 by 4 pixels at 64). */
    {
        static uint8_t page[4096];
        struct eu_buffer b = { page, 16, 4, 64, GPU_VIEW_R8 };
        uint8_t constants[32] = { 0x5a };
        struct eu_run r = { &intelgpu_fill_kernel[0][0], sizeof intelgpu_fill_kernel, 16, 4, 4,
                            &b, 1, constants, 32, 0 };
        int right = 0, past = 0, i;

        why = eu_emulate(&r, &st, &at);
        snprintf(what, sizeof what, "the fill did not run: %s at %u", why ? why : "", at);
        check(why == NULL, what);

        for (i = 0; i < 256; i++) { right += page[i] == 0x5a; }
        for (i = 256; i < 4096; i++) { past += page[i] != 0; }

        snprintf(what, sizeof what, "the fill wrote %d of 256 bytes, %d past them", right, past);
        check(right == 256 && past == 0, what);
        check(st.threads == 16, "the fill was not sixteen threads");
    }

    /* GPU Fractal's Mandelbrot, against fractal.c's arithmetic. */
    {
        enum { W = 800, H = 600 };
        static uint8_t pixels[W * H * 4];
        struct eu_buffer b = { pixels, W, H, W * 4, GPU_VIEW_BGRA };
        uint8_t constants[32] = { 0 };
        float x0 = -0.6f - W / 2 * 0.004f, dx = 0.004f, y0 = 0.0f - H / 2 * 0.004f, dy = 0.004f;
        int32_t most = 128, differ = 0, x, y;
        uint32_t n = kernel(argv[1], "gpu-fractal", words, sizeof words);
        struct eu_run r = { words, n, 16, W / 16, H, &b, 1, constants, 32, 0 };

        memcpy(constants, &x0, 4);
        memcpy(constants + 4, &dx, 4);
        memcpy(constants + 8, &y0, 4);
        memcpy(constants + 12, &dy, 4);
        memcpy(constants + 16, &most, 4);

        check(n == 47 * 16, "GPU Fractal's kernel is not in the file, 47 instructions");
        why = eu_emulate(&r, &st, &at);
        snprintf(what, sizeof what, "GPU Fractal's kernel did not run: %s at %u", why ? why : "", at);
        check(why == NULL, what);

        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                uint32_t got, want = fractal_colour(fractal_escape(x0 + (float)x * dx, y0 + (float)y * dy, most), most);

                memcpy(&got, &pixels[((size_t)y * W + (size_t)x) * 4], 4);
                differ += got != want;
            }
        }

        /* The kernel's mad rounds once where fractal.c rounds twice: a
         * pixel on the set's edge now and then, and never more. */
        snprintf(what, sizeof what, "GPU Fractal's picture differs from fractal.c's in %d of %d pixels",
                 differ, W * H);
        check(differ <= W * H / 1000, what);
        printf("eu_emu: GPU Fractal, %d of %d pixels as fractal.c's, %llu instructions in %llu threads\n",
               W * H - differ, W * H, (unsigned long long)st.instructions, (unsigned long long)st.threads);
    }

    /* Lesson 15's pattern, against its formula. */
    {
        enum { W = 640, H = 400 };
        static uint8_t pixels[W * H * 4];
        struct eu_buffer b = { pixels, W, H, W * 4, GPU_VIEW_BGRA };
        uint8_t constants[32] = { 0 };
        int32_t frame = 37, differ = 0, x, y;
        uint32_t n = kernel(argv[1], "lesson-15", words, sizeof words);
        struct eu_run r = { words, n, 16, W / 16, H, &b, 1, constants, 32, 0 };

        memcpy(constants, &frame, 4);
        why = eu_emulate(&r, &st, &at);
        snprintf(what, sizeof what, "lesson 15's kernel did not run: %s at %u", why ? why : "", at);
        check(n > 0 && why == NULL, what);

        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                uint32_t got, want = 0xff000000u | ((uint32_t)((x + frame) & 0xff) << 16)
                                   | ((uint32_t)((y + frame) & 0xff) << 8) | (uint32_t)((x ^ y) & 0xff);

                memcpy(&got, &pixels[((size_t)y * W + (size_t)x) * 4], 4);
                differ += got != want;
            }
        }

        snprintf(what, sizeof what, "lesson 15's pattern differs from its formula in %d pixels", differ);
        check(differ == 0, what);
    }

    /* The structured branches: each lane its own count. */
    {
        static uint8_t out[4096];
        struct eu_buffer b = { out, 1024, 1, 4096, GPU_VIEW_BYTES };
        uint8_t constants[32] = { 0 };
        uint32_t n = kernel(argv[1], "branches", words, sizeof words), lane;
        struct eu_run r = { words, n, 16, 1, 1, &b, 1, constants, 32, 0 };
        int differ = 0;

        why = eu_emulate(&r, &st, &at);
        snprintf(what, sizeof what, "the branches did not run: %s at %u", why ? why : "", at);
        check(n > 0 && why == NULL, what);

        for (lane = 0; lane < 16; lane++) {
            /* As `branches` in eu_kernels.lua: count up until the count
             * passes the lane, odd lanes stopping a step early by `break`;
             * then ten times it, plus one on an odd lane and two on an even. */
            int32_t count = 0, want, got;

            for (;;) {
                count++;
                if ((lane & 1) && count + 1 > (int32_t)lane) { break; }
                if (!(count <= (int32_t)lane)) { break; }
            }

            want = count * 10 + ((lane & 1) ? 1 : 2);
            memcpy(&got, &out[lane * 4], 4);
            differ += got != want;

            if (got != want && differ <= 3) {
                snprintf(what, sizeof what, "lane %u: %d, not %d", lane, got, want);
                check(0, what);
            }
        }

        check(differ == 0, "the branches' lanes did not each keep their own count");
    }

    if (failed) {
        printf("FAIL: %d of %d emulator checks\n", failed, checks);
        return 1;
    }

    printf("PASS: %d emulator checks (the fill, GPU Fractal held to fractal.c, lesson 15's pattern, "
           "and lanes through if, else, while and break)\n", checks);
    return 0;
}
