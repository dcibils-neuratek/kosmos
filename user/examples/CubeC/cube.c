/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * cube.c - the rotating cube of Cube in Lua (cube3d, `g3d.lua`), with all of
 * it in C: the matrices, the perspective divide, back-face culling, the
 * painter's sort, and the triangle fill itself, into a window of its own
 * (the Window Kit, `kosmos_window.h`).
 *
 * The same cube, the same camera and the same colours as the Lua one, so the
 * two can be run side by side and their time a frame compared: the number in
 * the corner is how long drawing one frame takes, waiting not counted.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "kosmos_kit.h"
#include "kosmos_window.h"

#define W 400
#define H 320

/* The smaller and the larger of two, as numbers in float. */
static float smaller(float a, float b) { return a < b ? a : b; }
static float larger(float a, float b) { return a > b ? a : b; }
#define BG 0xff101828u

/* A 4x4 matrix as g3d writes one: a row vector times it, translation last. */
typedef struct { float m[16]; } mat4;

static mat4 multiply(mat4 a, mat4 b)
{
    mat4 r;

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i * 4 + j] = a.m[i * 4 + 0] * b.m[0 * 4 + j] + a.m[i * 4 + 1] * b.m[1 * 4 + j]
                           + a.m[i * 4 + 2] * b.m[2 * 4 + j] + a.m[i * 4 + 3] * b.m[3 * 4 + j];
        }
    }

    return r;
}

static mat4 rotation_x(float a)
{
    float c = (float)cos(a), s = (float)sin(a);
    mat4 r = {{ 1, 0, 0, 0,  0, c, s, 0,  0, -s, c, 0,  0, 0, 0, 1 }};

    return r;
}

static mat4 rotation_y(float a)
{
    float c = (float)cos(a), s = (float)sin(a);
    mat4 r = {{ c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  0, 0, 0, 1 }};

    return r;
}

/* The camera at `eye` looking at the origin, up along y. */
static mat4 look_at(float ex, float ey, float ez)
{
    float fx = -ex, fy = -ey, fz = -ez;
    float fl = (float)sqrt(fx * fx + fy * fy + fz * fz);

    fx /= fl; fy /= fl; fz /= fl;

    float rx = 1 * fz - 0 * fy, ry = 0 * fx - 0 * fz, rz = 0 * fy - 1 * fx;
    float rl = (float)sqrt(rx * rx + ry * ry + rz * rz);

    rx /= rl; ry /= rl; rz /= rl;

    float ux = fy * rz - fz * ry, uy = fz * rx - fx * rz, uz = fx * ry - fy * rx;
    mat4 r = {{ rx, ux, fx, 0,  ry, uy, fy, 0,  rz, uz, fz, 0,
                -(rx * ex + ry * ey + rz * ez), -(ux * ex + uy * ey + uz * ez),
                -(fx * ex + fy * ey + fz * ez), 1 }};

    return r;
}

static mat4 perspective(float fov, float aspect, float near, float far)
{
    float f = 1.0f / (float)tan(fov / 2), range = far - near;
    mat4 r = {{ f / aspect, 0, 0, 0,  0, f, 0, 0,  0, 0, far / range, 1,
                0, 0, -(far * near) / range, 0 }};

    return r;
}

/* The cube: eight corners, twelve triangles, a colour each. */
static float vertices[8][3];
static int faces[12][3] = {
    { 0, 1, 2 }, { 0, 2, 3 },  { 4, 5, 6 }, { 4, 6, 7 },
    { 0, 1, 5 }, { 0, 5, 4 },  { 3, 2, 6 }, { 3, 6, 7 },
    { 0, 3, 7 }, { 0, 7, 4 },  { 1, 2, 6 }, { 1, 6, 5 },
};
static const uint32_t colours[12] = {
    0xff3a5f8f, 0xff3a5f8f, 0xff5a7fbf, 0xff5a7fbf, 0xff2a4f7f, 0xff2a4f7f,
    0xff6a8fcf, 0xff6a8fcf, 0xff4a6f9f, 0xff4a6f9f, 0xff7a9fdf, 0xff7a9fdf,
};

static void make_cube(float size)
{
    float h = size / 2;
    static const int corner[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 },
    };

    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 3; k++) vertices[i][k] = corner[i][k] * h;
    }

    /* Every triangle wound the same way, outwards - as `g3d.orient` does. */
    for (int f = 0; f < 12; f++) {
        float *a = vertices[faces[f][0]], *b = vertices[faces[f][1]], *c = vertices[faces[f][2]];
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0] };
        float m[3] = { (a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3 };

        if (n[0] * m[0] + n[1] * m[1] + n[2] * m[2] > 0) {
            int t = faces[f][1];

            faces[f][1] = faces[f][2];
            faces[f][2] = t;
        }
    }
}

/* One triangle filled: every pixel of its box whose centre is inside it. */
static void triangle(struct kw_surface s, float ax, float ay, float bx, float by,
                     float cx, float cy, uint32_t colour)
{
    int x0 = (int)larger(0, (float)floor(smaller(ax, smaller(bx, cx))));
    int x1 = (int)smaller((float)s.width - 1, (float)ceil(larger(ax, larger(bx, cx))));
    int y0 = (int)larger(0, (float)floor(smaller(ay, smaller(by, cy))));
    int y1 = (int)smaller((float)s.height - 1, (float)ceil(larger(ay, larger(by, cy))));
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);

    if (area == 0) return;

    for (int y = y0; y <= y1; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)s.pixels + (size_t)y * s.pitch);
        float py = y + 0.5f;

        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float w0 = (bx - px) * (cy - py) - (by - py) * (cx - px);
            float w1 = (cx - px) * (ay - py) - (cy - py) * (ax - px);
            float w2 = (ax - px) * (by - py) - (ay - py) * (bx - px);

            if ((w0 <= 0 && w1 <= 0 && w2 <= 0) || (w0 >= 0 && w1 >= 0 && w2 >= 0)) {
                row[x] = colour;
            }
        }
    }
}

/* The frame: the cube projected, its turned-away faces dropped, the rest
 * drawn back to front. How many were drawn. */
static int render(struct kw_surface s, mat4 mvp)
{
    float sx[8], sy[8], sz[8];
    int order[12], count = 0;
    float depth[12];

    for (int i = 0; i < 8; i++) {
        float x = vertices[i][0], y = vertices[i][1], z = vertices[i][2];
        const float *m = mvp.m;
        float cw = x * m[3] + y * m[7] + z * m[11] + m[15];

        sx[i] = W / 2.0f + (x * m[0] + y * m[4] + z * m[8] + m[12]) / cw * (W / 2.0f);
        sy[i] = H / 2.0f - (x * m[1] + y * m[5] + z * m[9] + m[13]) / cw * (H / 2.0f);
        sz[i] = cw;
    }

    for (int f = 0; f < 12; f++) {
        int a = faces[f][0], b = faces[f][1], c = faces[f][2];

        if ((sx[b] - sx[a]) * (sy[c] - sy[a]) - (sy[b] - sy[a]) * (sx[c] - sx[a]) < 0) {
            int j = count++;

            /* Kept in order as it goes: the farthest first. */
            float d = sz[a] + sz[b] + sz[c];

            while (j > 0 && depth[j - 1] < d) {
                order[j] = order[j - 1];
                depth[j] = depth[j - 1];
                j--;
            }

            order[j] = f;
            depth[j] = d;
        }
    }

    for (int i = 0; i < count; i++) {
        int f = order[i];

        triangle(s, sx[faces[f][0]], sy[faces[f][0]], sx[faces[f][1]], sy[faces[f][1]],
                 sx[faces[f][2]], sy[faces[f][2]], colours[f]);
    }

    return count;
}

/* A digit, a point, m or s, three pixels by five, scaled: enough to say a
 * time without a font. */
static void glyph(struct kw_surface s, int x, int y, char c, int scale)
{
    static const char *shapes[] = {
        "111101101101111", "010110010010111", "111001111100111", "111001111001111",
        "101101111001001", "111100111001111", "111100111101111", "111001001001001",
        "111101111101111", "111101111001111",
    };
    const char *shape = (c >= '0' && c <= '9') ? shapes[c - '0']
                      : c == '.' ? "000000000000010"
                      : c == 'm' ? "000000111111101"
                      : c == 's' ? "000011010001110" : "000000000000000";

    for (int r = 0; r < 5; r++) {
        for (int k = 0; k < 3; k++) {
            if (shape[r * 3 + k] != '1') continue;

            for (int dy = 0; dy < scale; dy++) {
                uint32_t *row = (uint32_t *)((uint8_t *)s.pixels + (size_t)(y + r * scale + dy) * s.pitch);

                for (int dx = 0; dx < scale; dx++) row[x + k * scale + dx] = 0xffc8d4e8u;
            }
        }
    }
}

static void say(struct kw_surface s, const char *text)
{
    for (int i = 0; text[i]; i++) glyph(s, 8 + i * 8, 8, text[i], 2);
}

/* main(counter_hz): the window until it is closed. */
static int l_main(lua_State *L)
{
    unsigned long hz = (unsigned long)luaL_optinteger(L, 1, 62500000);
    struct kw_window *w = kw_open("Cube in C", W, H, KW_CENTRE);
    struct kw_event e;
    mat4 view_proj;
    float angle = 0;
    unsigned long spent = 0, since = kosmos_ticks();
    int frames = 0;
    char shown[32] = "";

    if (w == NULL) {
        lua_pushfstring(L, "cube: no window: %s", kw_why());
        return 1;
    }

    make_cube(1.6f);
    view_proj = multiply(look_at(0, 0, -4.5f), perspective(3.14159265f / 4, (float)W / H, 0.1f, 100));

    for (;;) {
        struct kw_surface s = kw_surface(w);

        if (s.pixels != NULL) {
            unsigned long t0 = kosmos_ticks();

            for (unsigned y = 0; y < s.height; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)s.pixels + (size_t)y * s.pitch);

                for (unsigned x = 0; x < s.width; x++) row[x] = BG;
            }

            render(s, multiply(multiply(rotation_x(angle * 0.7f), rotation_y(angle)), view_proj));
            spent += kosmos_ticks() - t0;
            frames++;

            /* The time a frame, averaged over the last second. */
            if (kosmos_ticks() - since >= hz) {
                unsigned long tenths = spent * 10000 / hz / (unsigned long)frames;

                snprintf(shown, sizeof shown, "%lu.%lums", tenths / 10, tenths % 10);
                spent = 0;
                frames = 0;
                since = kosmos_ticks();
            }

            say(s, shown);
            kw_commit(w, 0, 0, s.width, s.height);
        }

        angle += 0.03f;

        while (kw_poll(w, &e, 4)) {
            if (e.type == KW_CLOSE || (e.type == KW_KEY && e.key == 27)) {
                kw_close(w);
                lua_pushfstring(L, "cube.c: %s a frame", shown);
                return 1;
            }
        }
    }
}

KOSMOS_KIT(cube)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_main);
    lua_setfield(L, -2, "main");
}
