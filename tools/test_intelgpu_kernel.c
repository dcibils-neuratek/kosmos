/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `intelgpu_kernel.c` held to kernels, on the Mac (`docs/compute.md` C2,
 * decision D2): what may run and each thing that must not, assembled by
 * `eu.lua` into the file `tools/eu_kernels.lua` writes - and the kernel the
 * M700 already runs, intel-gpu-tools' fill, which must pass and name
 * binding-table entry 0 and nothing above it.
 *
 *   test_intelgpu_kernel build/host/eu_kernels.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/include/gpuproto.h"
#include "../user/drivers/display/intelgpu_kernel.h"
#include "../user/drivers/display/intelgpu_fill.h"

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

int main(int argc, char **argv)
{
    static uint32_t words[(IGPU_KERNEL_MAX_BYTES + 64) / 4];   /* room for one past the most */
    static char line[2 * (IGPU_KERNEL_MAX_BYTES + 64) + 256];
    struct igpu_kernel_verdict v;
    char what[512];
    int kernels = 0, refused = 0;
    FILE *f;

    /* intel-gpu-tools' fill: runs, and names entry 0. */
    memcpy(words, intelgpu_fill_kernel, sizeof intelgpu_fill_kernel);
    check(igpu_kernel_check(words, sizeof intelgpu_fill_kernel, &v) == GPU_KERNEL_OK,
          "intel-gpu-tools' fill kernel was refused");
    check(v.bti_max == 0, "the fill kernel did not name binding-table entry 0 as its highest");

    if (argc < 2 || (f = fopen(argv[1], "r")) == NULL) {
        printf("FAIL: no kernels file - tools/eu_kernels.lua writes it\n");
        return 1;
    }

    while (fgets(line, sizeof line, f) != NULL) {
        unsigned why, at;
        char name[128], *hex;
        size_t n, i;
        int skip;

        if (sscanf(line, "%u %u %127s %n", &why, &at, name, &skip) != 3) {
            continue;
        }

        hex = line + skip;
        n = strcspn(hex, "\r\n") / 2;
        memset(words, 0, sizeof words);

        for (i = 0; i < n && i < sizeof words; i++) {
            ((unsigned char *)words)[i] = (unsigned char)(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
        }

        kernels++;
        refused += why != GPU_KERNEL_OK;
        (void)igpu_kernel_check(words, (uint32_t)n, &v);
        snprintf(what, sizeof what, "%s: answered %u at %u, not %u at %u", name, v.why, v.at, why, at);
        check(v.why == why && v.at == at, what);
    }

    fclose(f);
    check(kernels >= 30, "fewer than thirty kernels in the file");

    if (failed) {
        printf("FAIL: %d of %d kernel checks\n", failed, checks);
        return 1;
    }

    printf("PASS: %d kernel checks (the fill kernel, %d kernels of which %d refused, each where and why)\n",
           checks, kernels, refused);
    return 0;
}
