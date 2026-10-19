/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `compute_core.c` held to `gpuproto.h`, on the Mac (`docs/compute.md`
 * C3): the shapes buffers are given in the GPU's table, a run's message
 * byte for byte where the driver reads it, and each thing refused before
 * it is sent.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../user/kits/compute/compute_core.h"

static int checks, failed;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

static uint32_t word(const void *base, size_t at)
{
    uint32_t w;

    memcpy(&w, (const char *)base + at, 4);
    return w;
}

int main(void)
{
    static struct gpu_run q;
    struct compute_shape s;
    struct compute_binding b[9];
    unsigned char consts[300];
    unsigned i;

    /* Bytes: rows of a page. */
    check(compute_bytes_shape(1, &s) == NULL && s.width == 1024 && s.pitch == 4096 && s.height == 1
          && s.pages == 1, "one byte is not one row of a page");
    check(compute_bytes_shape(4097, &s) == NULL && s.height == 2 && s.pages == 2,
          "4097 bytes are not two rows");
    check(compute_bytes_shape(64u << 20, &s) == NULL && s.height == 16384,
          "64 MB is not 16384 rows");
    check(compute_bytes_shape((64u << 20) + 1, &s) != NULL, "past 64 MB was not refused");
    check(compute_bytes_shape(0, &s) != NULL, "no bytes was not refused");

    /* Pictures: whole 64-byte lines; what the import takes. */
    check(compute_picture_shape(1024, 768, &s) == NULL && s.pitch == 4096 && s.pages == 768,
          "1024 by 768 is not 4096 a row in 768 pages");
    check(compute_picture_shape(301, 203, &s) == NULL && s.pitch == 1216 && s.width == 301
          && s.pages == (1216u * 203u + 4095u) / 4096u, "301 by 203 is not 1216 a row");
    check(compute_picture_shape(8176, 2, &s) == NULL && s.pitch == 32768 - 64,
          "8176 wide, the widest, was not taken");
    check(compute_picture_shape(8177, 2, &s) != NULL, "8177 wide, a pitch of 32768, was not refused");
    check(compute_picture_shape(16, 16385, &s) != NULL, "16385 high was not refused");
    check(compute_picture_shape(0, 4, &s) != NULL, "no pixels was not refused");

    check(compute_view("bytes") == GPU_VIEW_BYTES && compute_view("bgra") == GPU_VIEW_BGRA
          && compute_view("r8") == GPU_VIEW_R8 && compute_view("r32f") == GPU_VIEW_R32F
          && compute_view("table") == GPU_VIEW_TABLE && compute_view("rgba") == -1, "the views' names are not gpuproto.h's");

    /* A run, where the driver reads each field. */
    for (i = 0; i < sizeof consts; i++) {
        consts[i] = (unsigned char)(i + 1);
    }

    b[0].id = 7; b[0].view = GPU_VIEW_BGRA;
    b[1].id = 9; b[1].view = GPU_VIEW_R32F;

    check(compute_pack_run(&q, 3, 16, 64, 768, b, 2, consts, 12) == NULL, "a good run was refused");
    check(word(&q, 0) == GPU_OP_RUN && word(&q, 4) == 3 && word(&q, 8) == 16 && word(&q, 12) == 64
          && word(&q, 16) == 768 && word(&q, 20) == 2, "the run's six words are not where gpuproto.h says");
    check(word(&q, 24) == 7 && word(&q, 28) == GPU_VIEW_BGRA && word(&q, 32) == 9
          && word(&q, 36) == GPU_VIEW_R32F && word(&q, 40) == 0, "the buffers are not in their pairs");
    check(word(&q, 88) == 32, "twelve bytes of constants were not padded to a register of 32");
    check(memcmp((const char *)&q + 92, consts, 12) == 0 && ((const char *)&q)[92 + 12] == 0,
          "the constants are not after their count, zeros after");
    check(offsetof(struct gpu_run, constants) == 92 && sizeof q == 348,
          "struct gpu_run is not 92 bytes and the constants");

    check(compute_pack_run(&q, 3, 8, 1, 1, NULL, 0, NULL, 0) == NULL && word(&q, 88) == 0,
          "a run with nothing bound and no constants was refused");
    check(compute_pack_run(&q, 3, 16, 1, 1, NULL, 0, consts, 256) == NULL && word(&q, 88) == 256,
          "256 bytes of constants were refused");

    /* Refused before it is sent. */
    check(compute_pack_run(&q, 0, 16, 1, 1, NULL, 0, NULL, 0) != NULL, "no kernel was not refused");
    check(compute_pack_run(&q, 3, 32, 1, 1, NULL, 0, NULL, 0) != NULL, "SIMD32 was not refused");
    check(compute_pack_run(&q, 3, 16, 0, 1, NULL, 0, NULL, 0) != NULL, "an empty grid was not refused");
    check(compute_pack_run(&q, 3, 16, 65536, 1, NULL, 0, NULL, 0) != NULL, "65536 across was not refused");
    check(compute_pack_run(&q, 3, 16, 1, 1, b, 9, NULL, 0) != NULL, "nine buffers were not refused");
    check(compute_pack_run(&q, 3, 16, 1, 1, NULL, 0, consts, 257) != NULL,
          "257 bytes of constants were not refused");
    b[0].id = 0;
    check(compute_pack_run(&q, 3, 16, 1, 1, b, 1, NULL, 0) != NULL, "buffer 0, the screen, was not refused");
    b[0].id = 7; b[0].view = GPU_VIEW_TABLE + 1u;
    check(compute_pack_run(&q, 3, 16, 1, 1, b, 1, NULL, 0) != NULL, "a view past TABLE was not refused");
    b[0].view = GPU_VIEW_TABLE;
    check(compute_pack_run(&q, 3, 16, 1, 1, b, 1, NULL, 0) == NULL, "a table was refused");

    /* A list of runs (R8 W2): the steps where the driver reads them - each
     * five words from byte 340 - and what is refused. */
    {
        static struct gpu_runs qs;
        struct compute_step st[GPU_RUN_STEPS + 1];
        uint32_t k;

        for (k = 0; k <= GPU_RUN_STEPS; k++) {
            st[k].kernel = 3; st[k].simd = 8; st[k].groups_x = 80; st[k].groups_y = 360;
            st[k].step = k;
        }
        st[1].kernel = 4; st[1].simd = 16;

        check(compute_pack_runs(&qs, b, 1, consts, 40, 36, st, 2) == NULL, "a good list was refused");
        check(word(&qs, 0) == GPU_OP_RUNS && word(&qs, 4) == 1 && word(&qs, 72) == 64
              && word(&qs, 76) == 36 && word(&qs, 336) == 2
              && word(&qs, 340) == 3 && word(&qs, 344) == 8 && word(&qs, 348) == 80
              && word(&qs, 352) == 360 && word(&qs, 356) == 0
              && word(&qs, 360) == 4 && word(&qs, 364) == 16 && word(&qs, 376) == 1,
              "a list's fields are not where the driver reads them");
        check(compute_pack_runs(&qs, b, 1, consts, 40, 36, st, 0) != NULL, "a list of no steps was taken");
        check(compute_pack_runs(&qs, b, 1, consts, 40, 36, st, GPU_RUN_STEPS + 1) != NULL,
              "a list of 33 steps was taken");
        check(compute_pack_runs(&qs, b, 1, consts, 40, 64, st, 2) != NULL,
              "a step's number past the constants was taken");
        check(compute_pack_runs(&qs, b, 1, consts, 40, 37, st, 2) != NULL,
              "a step's number not on a word was taken");
        st[0].simd = 4;
        check(compute_pack_runs(&qs, b, 1, consts, 40, 36, st, 2) != NULL, "a step of 4 lanes was taken");
    }

    if (failed) {
        printf("FAIL: %d of %d compute checks\n", failed, checks);
        return 1;
    }

    printf("PASS: %d compute checks (buffers' shapes, a run's message where the driver reads it, "
           "what is refused before it is sent)\n", checks);
    return 0;
}
