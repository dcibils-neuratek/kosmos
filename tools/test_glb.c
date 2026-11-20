/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's binary glTF reader, on the Mac (`user/kits/3d/k3d_glb.c`,
 * `docs/flight.md` F5), against the Cessna 172 Flight flies:
 *
 *     test_glb assets/models/cessna172/cessna_172.glb
 *
 * What it must come to was read from the file a second way, by a Python
 * reading of its JSON with no code in common (its accessors summed, the
 * nodes multiplied out, each vertex given to its heaviest joint): 77317
 * triangles, 28 textures, the movables' triangles and pivots below, and
 * a box 8.15 by 2.70 by 10.92 metres - a Cessna 172 is 8.28 long, 2.72
 * tall and 11.0 across, so the file is in metres. Every part is then held
 * to what a renderer will assume of it: indices inside their vertices,
 * finite positions, unit normals, a texture that is a PNG.
 *
 * Then the same file damaged: cut at many lengths, its JSON's bytes
 * changed one at a time. Each must be refused or read without reaching
 * past the bytes - which the AddressSanitizer build of this test is the
 * judge of. And a file small enough to know every number of (`tiny`).
 */

#include "k3d_glb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

static int near(float a, float b, float within)
{
    return fabsf(a - b) <= within;
}

static const char *const NAMES[] = {
    "elevator_0", "flaps_1", "aileron-right_2", "aileron-left_3", "rudder_4", "prop_5",
};
#define NNAMES 6

/* The Python reading's, per movable: triangles, pivot. */
static const struct {
    unsigned triangles;
    float pivot[3];
} EXPECT[NNAMES] = {
    { 2528, { -4.769f, 1.271f, 0.000f } },
    { 384,  { -1.071f, 1.849f, 0.000f } },
    { 192,  { -0.852f, 1.981f, 3.950f } },
    { 192,  { -0.855f, 1.953f, -3.951f } },
    { 976,  { -5.077f, 1.854f, 0.000f } },
    { 3518, { 2.335f, 1.280f, -0.001f } },
};

static void cessna(const uint8_t *bytes, size_t n)
{
    struct k3d_glb g;
    const char *why;

    clock_t t0 = clock();
    bool ok = k3d_glb_read(bytes, n, NAMES, NNAMES, &g, &why);

    printf("read in %.1f ms\n", (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
    if (!ok) {
        printf("  FAIL the Cessna refused: %s\n", why);
        fails++;
        return;
    }

    printf("parts %u, triangles %u, textures %u, skipped %u\n", g.nparts, g.triangles, g.ntextures,
           g.skipped);
    printf("box (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f): %.3f long (x), %.3f tall (y), %.3f across (z)\n",
           g.lo[0], g.lo[1], g.lo[2], g.hi[0], g.hi[1], g.hi[2], g.hi[0] - g.lo[0], g.hi[1] - g.lo[1],
           g.hi[2] - g.lo[2]);

    check(g.triangles == 77317, "77317 triangles, as the file's accessors sum to");
    check(g.ntextures == 28, "28 textures");
    check(g.skipped == 0, "nothing skipped");
    check(near(g.lo[0], -5.816f, 0.002f) && near(g.hi[0], 2.335f, 0.002f), "nose to tail, x");
    check(near(g.lo[1], -0.020f, 0.002f) && near(g.hi[1], 2.682f, 0.002f), "wheels to fin, y");
    check(near(g.lo[2], -5.459f, 0.002f) && near(g.hi[2], 5.465f, 0.002f), "wing tip to tip, z");

    uint32_t airframe = 0;

    for (uint32_t i = 0; i < g.nparts; i++) {
        const struct k3d_glb_part *p = &g.parts[i];
        int ok = p->nindices % 3 == 0 && p->movable >= -1 && p->movable < NNAMES &&
                 p->texture >= -1 && p->texture < (int)g.ntextures;

        for (uint32_t k = 0; ok && k < p->nindices; k++) ok = p->indices[k] < p->nvertices;
        for (uint32_t v = 0; ok && v < p->nvertices; v++) {
            const float *q = p->vertices[v].position, *nn = p->vertices[v].normal;
            float l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);

            ok = isfinite(q[0]) && isfinite(q[1]) && isfinite(q[2]) && near(l, 1, 1e-3f) &&
                 isfinite(p->vertices[v].uv[0]) && isfinite(p->vertices[v].uv[1]);
        }
        if (p->movable < 0) airframe += p->nindices / 3;
        printf("  part %2u: mesh %2d material %2d texture %2d movable %2d  %6u vertices %6u triangles"
               "  colour (%.2f %.2f %.2f %.2f)\n", i, p->mesh, p->material, p->texture, p->movable,
               p->nvertices, p->nindices / 3, p->colour[0], p->colour[1], p->colour[2], p->colour[3]);
        check(ok, "a part a renderer can take as it is");
    }
    check(airframe == 69527, "69527 triangles of airframe");

    for (unsigned m = 0; m < NNAMES; m++) {
        const struct k3d_glb_movable *mv = &g.movables[m];
        char what[96];

        printf("%-16s node %2d  %5u triangles  pivot (%.3f, %.3f, %.3f)\n", NAMES[m], mv->node,
               mv->triangles, mv->pivot[0], mv->pivot[1], mv->pivot[2]);
        for (int k = 0; k < 3; k++) {
            printf("    its %c (%6.3f, %6.3f, %6.3f)  extent %.3f m\n", "XYZ"[k], mv->axes[k][0],
                   mv->axes[k][1], mv->axes[k][2], mv->extent[k]);
        }
        printf("    hinge: its %c (%6.3f, %6.3f, %6.3f)\n", mv->hinge_axis, mv->hinge[0], mv->hinge[1],
               mv->hinge[2]);
        snprintf(what, sizeof what, "%s: its triangles", NAMES[m]);
        check(mv->triangles == EXPECT[m].triangles, what);
        snprintf(what, sizeof what, "%s: its pivot", NAMES[m]);
        check(near(mv->pivot[0], EXPECT[m].pivot[0], 0.002f) && near(mv->pivot[1], EXPECT[m].pivot[1], 0.002f) &&
              near(mv->pivot[2], EXPECT[m].pivot[2], 0.002f), what);
    }

    /* The hinges, as an aircraft has them: the elevator and the flaps
     * across the span, the ailerons nearly so, the rudder upright and
     * raked back. The propeller's axis is named rather than found: its
     * node's Z, which must be the line of flight. */
    check(g.movables[0].hinge[2] > 0.999f, "the elevator hinges across the span");
    check(g.movables[1].hinge[2] > 0.999f, "the flaps hinge across the span");
    check(g.movables[2].hinge[2] > 0.99f && g.movables[3].hinge[2] > 0.99f, "the ailerons nearly so");
    check(g.movables[4].hinge[1] > 0.9f && g.movables[4].hinge[0] < -0.3f, "the rudder upright, raked back");
    check(g.movables[5].axes[2][0] < -0.999f, "the propeller's Z along the line of flight");

    int pngs = 1;

    for (uint32_t t = 0; t < g.ntextures; t++) {
        const struct k3d_glb_texture *x = &g.textures[t];

        pngs = pngs && x->length > 8 && x->offset + x->length <= n &&
               memcmp(bytes + x->offset, "\x89PNG\r\n\x1a\n", 8) == 0 && strcmp(x->mime, "image/png") == 0;
    }
    check(pngs, "every texture a PNG inside the file");
    printf("texture 0: %zu bytes at %zu, %s\n", g.textures[0].length, g.textures[0].offset, g.textures[0].mime);
    k3d_glb_free(&g);
}

/* The file damaged: refused or read, never past its bytes. */
static void damaged(const uint8_t *bytes, size_t n)
{
    struct k3d_glb g;
    const char *why;
    uint32_t json = (uint32_t)bytes[12] | (uint32_t)bytes[13] << 8 | (uint32_t)bytes[14] << 16 |
                    (uint32_t)bytes[15] << 24;
    uint8_t *copy = malloc(n);
    unsigned refused = 0, read = 0;

    /* Cut short, the length in its header left as it was. */
    for (size_t cut = 0; cut < 20 + json + 64; cut += cut < 64 ? 1 : 97) {
        if (k3d_glb_read(bytes, cut, NAMES, NNAMES, &g, &why)) read++, k3d_glb_free(&g);
        else refused++;
    }
    check(read == 0, "a file cut short is refused");

    /* Cut short with its length made to agree: the BIN chunk's size now
     * runs past the end. */
    memcpy(copy, bytes, n);
    copy[8] = (uint8_t)((20 + json + 100) & 0xff), copy[9] = (uint8_t)((20 + json + 100) >> 8);
    copy[10] = copy[11] = 0;
    check(!k3d_glb_read(copy, 20 + json + 100, NAMES, NNAMES, &g, &why), "a chunk past the end is refused");

    /* Every byte of the JSON changed in turn, one in 29 of them. */
    memcpy(copy, bytes, n);
    read = refused = 0;
    for (uint32_t k = 0; k < json; k += 29) {
        static const char swaps[] = "9\"{}[],:-0";

        copy[20 + k] = (uint8_t)swaps[k % 10];
        if (k3d_glb_read(copy, n, NAMES, NNAMES, &g, &why)) {
            read++;
            for (uint32_t i = 0; i < g.nparts; i++) {
                for (uint32_t x = 0; x < g.parts[i].nindices; x++) {
                    if (g.parts[i].indices[x] >= g.parts[i].nvertices) fails++;
                }
            }
            k3d_glb_free(&g);
        } else {
            refused++;
        }
        copy[20 + k] = bytes[20 + k];
    }
    printf("JSON changed at %u places: %u read, %u refused\n", read + refused, read, refused);
    check(read + refused > 0, "the damaged JSON was tried");

    free(copy);
}

/* A file made here, small enough to know every number of: one node,
 * moved by (1, 2, 3) and named as the propeller, whose mesh has two
 * triangles in two primitives over three points with no normals - the
 * second's last index past them. The first is read, its normal worked
 * out from its face; the second is skipped. */
static void tiny(void)
{
    static const char text[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"translation\":[1,2,3],\"name\":\"prop_5\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1},"
        "{\"attributes\":{\"POSITION\":0},\"indices\":2}]}],"
        "\"buffers\":[{\"byteLength\":48}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"byteOffset\":6,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}";
    static const float points[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    static const uint16_t indices[6] = { 0, 1, 2, 0, 1, 7 };
    uint8_t file[1024];
    uint32_t jl = (uint32_t)(sizeof text - 1 + 3) & ~3u, at = 12, total;

    total = 12 + 8 + jl + 8 + 48;
    memcpy(file, "glTF\2\0\0\0", 8);
    memcpy(file + 8, &total, 4);
    memcpy(file + at, &jl, 4), memcpy(file + at + 4, "JSON", 4);
    memset(file + at + 8, ' ', jl), memcpy(file + at + 8, text, sizeof text - 1);
    at += 8 + jl;

    uint32_t bl = 48;

    memcpy(file + at, &bl, 4), memcpy(file + at + 4, "BIN\0", 4);
    memcpy(file + at + 8, points, 36), memcpy(file + at + 8 + 36, indices, 12);

    struct k3d_glb g;
    const char *why;

    if (!k3d_glb_read(file, total, NAMES, NNAMES, &g, &why)) {
        printf("  FAIL the small file refused: %s\n", why);
        fails++;
        return;
    }
    check(g.nparts == 1 && g.triangles == 1 && g.skipped == 1, "small: one part, one skipped");
    if (g.nparts == 1) {
        const struct k3d_glb_part *p = &g.parts[0];

        check(p->movable == 5 && p->texture == -1 && p->colour[3] == 1, "small: the propeller's, untextured");
        check(p->matrix[12] == 1 && p->matrix[13] == 2 && p->matrix[14] == 3 && p->matrix[0] == 1,
              "small: its node's matrix");
        check(p->vertices[0].normal[2] == 1, "small: its normal from its face");
    }
    check(g.lo[0] == 1 && g.lo[1] == 2 && g.hi[0] == 2 && g.hi[1] == 3 && g.hi[2] == 3, "small: its box");
    check(g.movables[5].node == 0 && g.movables[5].pivot[1] == 2 && g.movables[0].node == -1,
          "small: the propeller found, the elevator not");
    k3d_glb_free(&g);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test_glb cessna_172.glb\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");

    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);

    long n = ftell(f);
    uint8_t *bytes = malloc((size_t)n);

    fseek(f, 0, SEEK_SET);
    if (!bytes || fread(bytes, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "could not read %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    cessna(bytes, (size_t)n);
    damaged(bytes, (size_t)n);
    tiny();
    free(bytes);

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
