/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A kernel run in the emulator from the command line (`eu_emu.h`), for the
 * tests that drive it from Lua - `kc.lua`'s, which compile a program, run
 * it here and read its buffers back (`docs/cafesa3d-gpu.md` R3).
 *
 *   eu_run SIMD GX GY CONSTANTS-HEX KERNEL-HEX BYTES [BYTES ...]
 *
 * Each BYTES is a buffer of that many bytes, bound as bytes in the order
 * given: all nought, or `N:HEX` to start with those bytes; `tN:HEX` binds
 * it as a table, which the sampler reads (`testing.md` 18.705). On the way out,
 * a line a buffer of its bytes in hex; or "error: why at N" and a status
 * of 1.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eu_emu.h"
#include "../user/include/gpuproto.h"

static int nibble(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static size_t unhex(const char *hex, uint8_t *out, size_t room)
{
    size_t n = strlen(hex) / 2, i;

    for (i = 0; i < n && i < room; i++) {
        out[i] = (uint8_t)(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    }

    return i;
}

int main(int argc, char **argv)
{
    static uint8_t constants[GPU_RUN_CONSTANTS];
    static uint32_t kernel[65536 / 4];
    struct eu_buffer buffers[GPU_RUN_BUFFERS];
    struct eu_run r;
    struct eu_stats st;
    const char *why;
    uint32_t at = 0;
    int i, n;

    if (argc < 7 || argc - 6 > (int)GPU_RUN_BUFFERS) {
        fprintf(stderr, "usage: eu_run SIMD GX GY CONSTANTS-HEX KERNEL-HEX BYTES [BYTES ...]\n");
        return 2;
    }

    memset(&r, 0, sizeof r);
    r.simd = (uint32_t)atoi(argv[1]);
    r.groups_x = (uint32_t)atoi(argv[2]);
    r.groups_y = (uint32_t)atoi(argv[3]);
    r.nconstants = (uint32_t)unhex(strcmp(argv[4], "-") == 0 ? "" : argv[4], constants, sizeof constants);
    r.nconstants = (r.nconstants + 31u) & ~31u;
    r.constants = constants;
    r.bytes = (uint32_t)unhex(argv[5], (uint8_t *)kernel, sizeof kernel);
    r.kernel = kernel;
    r.most_steps = 50000000u;

    n = argc - 6;

    for (i = 0; i < n; i++) {
        char *rest;
        const char *arg = argv[6 + i];
        bool table = arg[0] == 't';
        uint32_t size = (uint32_t)strtoul(arg + table, &rest, 10);
        uint32_t rows = (size + 4095u) / 4096u;

        buffers[i].bytes = calloc(rows ? rows : 1, 4096);

        if (*rest == ':') {
            (void)unhex(rest + 1, buffers[i].bytes, size);
        }

        buffers[i].width = 1024;
        buffers[i].height = rows ? rows : 1;
        buffers[i].pitch = 4096;
        buffers[i].view = table ? GPU_VIEW_TABLE : GPU_VIEW_BYTES;
    }

    r.buffers = buffers;
    r.nbuffers = (uint32_t)n;

    if ((why = eu_emulate(&r, &st, &at)) != NULL) {
        printf("error: %s at %u\n", why, at);
        return 1;
    }

    for (i = 0; i < n; i++) {
        const char *arg = argv[6 + i];
        uint32_t size = (uint32_t)strtoul(arg + (arg[0] == 't'), NULL, 10), k;

        for (k = 0; k < size; k++) {
            printf("%02x", buffers[i].bytes[k]);
        }

        printf("\n");
    }

    return 0;
}
