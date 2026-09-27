/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Other programs' files, as triangles (`roadmap.md` 4l, 5c): STL, what 3D
 * printing's libraries hand out, and Wavefront OBJ, the oldest common
 * format and still everywhere - read into points and triangles, and written
 * from them.
 *
 * **Loops over bytes, so C.** A model from Thingiverse is a hundred
 * thousand triangles and an OBJ from TurboSquid a few megabytes of text:
 * welding STL's repeated corners and scanning OBJ's numbers are exactly the
 * per-byte work the rest of Kosmos keeps out of Lua. What is decided about
 * the results - names, materials, where a part goes - is the translators'
 * (`/Kosmos/Libraries/translators/`), in Lua.
 *
 * **Nothing here trusts the file.** Every count is held to what the bytes
 * can hold, every index to the points there are, and a refusal is a
 * sentence rather than a half-read model.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "k3d.h"

#define MAX_POINTS  (1u << 24)
#define MAX_TRIS    (1u << 24)

void k3d_soup_free(struct k3d_soup *s)
{
    free(s->pos);
    free(s->tri);
    s->pos = NULL;
    s->tri = NULL;
    s->npos = s->ntri = 0;
}

static bool grow(void **items, uint32_t *cap, uint32_t want, size_t size)
{
    uint32_t next = *cap ? *cap : 256;
    void *bigger;

    if (want <= *cap) return true;

    while (next < want) {
        if (next > (1u << 30)) return false;
        next *= 2;
    }

    bigger = realloc(*items, (size_t)next * size);

    if (bigger == NULL) return false;

    *items = bigger;
    *cap = next;
    return true;
}

/*--------------------------------------------------------------------------
 * Welding: one point for every corner at exactly the same place, found by
 * the bits of its three floats in an open table.
 *------------------------------------------------------------------------*/

struct weld {
    uint32_t *slot;             /* point + 1, or 0 for empty */
    uint32_t  mask;
    uint32_t  cap;              /* of the soup's points, in points */
};

static uint32_t hash3f(const float p[3])
{
    uint32_t a, b, c, h;

    memcpy(&a, &p[0], 4);
    memcpy(&b, &p[1], 4);
    memcpy(&c, &p[2], 4);
    h = a * 0x9e3779b1u ^ b * 0x85ebca77u ^ c * 0xc2b2ae3du;
    h ^= h >> 15;
    return h * 0x2c1b3c6du;
}

/* The point `p` in the soup, added if it is not there; UINT32_MAX when the
 * soup is full or there is no memory. */
static uint32_t weld_point(struct weld *w, struct k3d_soup *s, const float p[3])
{
    uint32_t i = hash3f(p) & w->mask;

    for (;;) {
        uint32_t at = w->slot[i];

        if (at == 0) break;

        if (memcmp(&s->pos[(at - 1) * 3], p, 12) == 0) return at - 1;

        i = (i + 1) & w->mask;
    }

    if (s->npos >= MAX_POINTS || !grow((void **)&s->pos, &w->cap, s->npos + 1, 12)) {
        return UINT32_MAX;
    }

    memcpy(&s->pos[s->npos * 3], p, 12);
    w->slot[i] = ++s->npos;
    return s->npos - 1;
}

/*--------------------------------------------------------------------------
 * STL.
 *------------------------------------------------------------------------*/

static float le_float(const unsigned char *b)
{
    uint32_t v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16
                 | (uint32_t)b[3] << 24;
    float f;

    memcpy(&f, &v, 4);
    return f;
}

static bool finite3(const float p[3])
{
    int k;

    for (k = 0; k < 3; k++) {
        if (!(p[k] == p[k]) || p[k] > 1e30f || p[k] < -1e30f) return false;
    }

    return true;
}

/*
 * A triangle's three corners into the soup. Degenerate ones - two corners
 * welded into one - are dropped, which STL exporters write more of than
 * anyone would guess and which draw nothing.
 */
static const char *put_triangle(struct weld *w, struct k3d_soup *s, uint32_t *tcap,
                                float c[3][3])
{
    uint32_t v[3];
    int k;

    for (k = 0; k < 3; k++) {
        if (!finite3(c[k])) return "a corner that is not a number";

        v[k] = weld_point(w, s, c[k]);

        if (v[k] == UINT32_MAX) return "more points than a model may have";
    }

    if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) return NULL;

    if (s->ntri >= MAX_TRIS || !grow((void **)&s->tri, tcap, (s->ntri + 1) * 3, 4)) {
        return "more triangles than a model may have";
    }

    memcpy(&s->tri[s->ntri * 3], v, sizeof(v));
    s->ntri++;
    return NULL;
}

static bool weld_begin(struct weld *w, uint32_t corners)
{
    uint32_t n = 1024;

    while (n < corners * 2u && n < (1u << 26)) n *= 2;

    w->slot = calloc(n, sizeof(uint32_t));
    w->mask = n - 1;
    w->cap = 0;
    return w->slot != NULL;
}

/* A number in a line of text, which must end before `end`. */
static bool number_in(const char **at, const char *end, float *out)
{
    const char *p = *at;
    char buf[64];
    size_t n = 0;
    char *stop;

    while (p < end && (*p == ' ' || *p == '\t')) p++;

    while (p < end && n < sizeof(buf) - 1 && *p != ' ' && *p != '\t' && *p != '/') {
        buf[n++] = *p++;
    }

    if (n == 0) return false;

    buf[n] = '\0';
    *out = (float)strtod(buf, &stop);

    if (stop == buf || *stop != '\0') return false;

    *at = p;
    return true;
}

/* The next line of text: where it starts, and where it ends. */
static bool next_line(const char **at, const char *end, const char **line, const char **stop)
{
    const char *p = *at;

    if (p >= end) return false;

    *line = p;

    while (p < end && *p != '\n' && *p != '\r') p++;

    *stop = p;

    while (p < end && (*p == '\n' || *p == '\r')) p++;

    *at = p;
    return true;
}

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static bool starts(const char *p, const char *end, const char *word)
{
    size_t n = strlen(word);

    return (size_t)(end - p) >= n && memcmp(p, word, n) == 0
           && ((size_t)(end - p) == n || p[n] == ' ' || p[n] == '\t');
}

static const char *stl_text(const char *text, size_t len, struct k3d_soup *s)
{
    const char *at = text, *end = text + len, *line, *stop, *why;
    struct weld w;
    uint32_t tcap = 0;
    float c[3][3];
    int corner = 0;

    if (!weld_begin(&w, (uint32_t)(len / 40 + 16))) return "no memory to read it";

    while (next_line(&at, end, &line, &stop)) {
        const char *p = skip_space(line, stop);

        if (!starts(p, stop, "vertex")) continue;

        p += 6;

        if (!number_in(&p, stop, &c[corner][0]) || !number_in(&p, stop, &c[corner][1])
            || !number_in(&p, stop, &c[corner][2])) {
            free(w.slot);
            return "a vertex line that is not three numbers";
        }

        if (++corner == 3) {
            corner = 0;
            why = put_triangle(&w, s, &tcap, c);

            if (why) {
                free(w.slot);
                return why;
            }
        }
    }

    free(w.slot);
    return s->ntri > 0 ? NULL : "no triangles in it";
}

const char *k3d_stl_read(const unsigned char *bytes, size_t len, struct k3d_soup *out)
{
    uint32_t n, t, tcap = 0;
    struct weld w;
    const char *why;

    memset(out, 0, sizeof(*out));

    if (len < 84) {
        return (len >= 5 && memcmp(bytes, "solid", 5) == 0)
               ? stl_text((const char *)bytes, len, out) : "too short to be STL";
    }

    n = (uint32_t)bytes[80] | (uint32_t)bytes[81] << 8 | (uint32_t)bytes[82] << 16
        | (uint32_t)bytes[83] << 24;

    /*
     * Binary exactly when the count says so: plenty of binary files begin
     * with "solid" in their header, which is the text form's first word,
     * and the count is the one thing that tells them apart.
     */
    if ((uint64_t)84 + (uint64_t)n * 50 != len) {
        if (memcmp(bytes, "solid", 5) == 0) {
            why = stl_text((const char *)bytes, len, out);

            if (why) k3d_soup_free(out);

            return why;
        }

        return "an STL whose triangle count does not match its length";
    }

    if (n == 0) return "no triangles in it";
    if (n > MAX_TRIS) return "more triangles than a model may have";
    if (!weld_begin(&w, n * 3)) return "no memory to read it";

    for (t = 0; t < n; t++) {
        const unsigned char *r = bytes + 84 + (size_t)t * 50 + 12;
        float c[3][3];
        int k;

        for (k = 0; k < 9; k++) c[k / 3][k % 3] = le_float(r + k * 4);

        why = put_triangle(&w, out, &tcap, c);

        if (why) {
            free(w.slot);
            k3d_soup_free(out);
            return why;
        }
    }

    free(w.slot);

    if (out->ntri == 0) return "no triangles in it";

    return NULL;
}

static void put_le(unsigned char *b, uint32_t v)
{
    b[0] = (unsigned char)v;
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
}

static void put_float(unsigned char *b, float f)
{
    uint32_t v;

    memcpy(&v, &f, 4);
    put_le(b, v);
}

unsigned char *k3d_stl_write(const struct k3d_soup *soups, size_t n, float scale, size_t *len)
{
    size_t total = 0, i;
    unsigned char *out, *at;

    for (i = 0; i < n; i++) total += soups[i].ntri;

    if (total > MAX_TRIS) return NULL;

    *len = 84 + total * 50;
    out = calloc(1, *len);

    if (out == NULL) return NULL;

    memcpy(out, "Cafesa3D, Kosmos", 16);
    put_le(out + 80, (uint32_t)total);
    at = out + 84;

    for (i = 0; i < n; i++) {
        const struct k3d_soup *s = &soups[i];
        uint32_t t;

        for (t = 0; t < s->ntri; t++) {
            const float *a = &s->pos[s->tri[t * 3] * 3];
            const float *b = &s->pos[s->tri[t * 3 + 1] * 3];
            const float *c = &s->pos[s->tri[t * 3 + 2] * 3];
            float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float nrm[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                             u[0] * v[1] - u[1] * v[0] };
            float l = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
            int k;

            if (l > 0) {
                float r = 1.0f / (float)sqrt((double)l);

                for (k = 0; k < 3; k++) nrm[k] *= r;
            }

            for (k = 0; k < 3; k++) put_float(at + k * 4, nrm[k]);
            for (k = 0; k < 3; k++) put_float(at + 12 + k * 4, a[k] * scale);
            for (k = 0; k < 3; k++) put_float(at + 24 + k * 4, b[k] * scale);
            for (k = 0; k < 3; k++) put_float(at + 36 + k * 4, c[k] * scale);

            at += 50;
        }
    }

    return out;
}

/*--------------------------------------------------------------------------
 * Wavefront OBJ.
 *------------------------------------------------------------------------*/

void k3d_obj_free(struct k3d_obj *o)
{
    size_t i;

    for (i = 0; i < o->nparts; i++) k3d_soup_free(&o->parts[i].soup);

    free(o->parts);
    o->parts = NULL;
    o->nparts = 0;
}

/* A word of the rest of a line, cut to fit `out`. */
static void word_into(const char *p, const char *end, char *out, size_t cap)
{
    size_t n = 0;

    p = skip_space(p, end);

    while (p < end && n < cap - 1 && *p != '\0') out[n++] = *p++;

    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;

    out[n] = '\0';
}

/*
 * The whole file's points first, then each part - a run of faces under one
 * object or group name and one material - with only the points it uses,
 * numbered from nought for it. Faces of more than three corners are fanned
 * into triangles, and an index may count back from the last point, as
 * `f -1 -2 -3` does.
 */
const char *k3d_obj_read(const char *text, size_t len, struct k3d_obj *out)
{
    const char *at = text, *end = text + len, *line, *stop, *why = NULL;
    float *pos = NULL;
    uint32_t npos = 0, poscap = 0;
    uint32_t *faces = NULL, nfaces = 0, facecap = 0;    /* global corner indices */
    struct part { char name[64], material[64]; uint32_t first, count; } *parts = NULL;
    uint32_t nparts = 0, partcap = 0;
    char name[64] = "Object", material[64] = "";
    int32_t *local = NULL;
    size_t i;

    memset(out, 0, sizeof(*out));

    while (next_line(&at, end, &line, &stop)) {
        const char *p = skip_space(line, stop);

        if (starts(p, stop, "v")) {
            float v[3];

            p += 1;

            if (!number_in(&p, stop, &v[0]) || !number_in(&p, stop, &v[1])
                || !number_in(&p, stop, &v[2]) || !finite3(v)) {
                why = "a point that is not three numbers";
                break;
            }

            if (npos >= MAX_POINTS || !grow((void **)&pos, &poscap, (npos + 1) * 3, 4)) {
                why = "more points than a model may have";
                break;
            }

            memcpy(&pos[npos * 3], v, 12);
            npos++;
        } else if (starts(p, stop, "o") || starts(p, stop, "g") || starts(p, stop, "usemtl")) {
            char word[64];
            bool is_mtl = p[0] == 'u';

            word_into(p + (is_mtl ? 6 : 1), stop, word, sizeof(word));

            if (is_mtl) {
                memcpy(material, word, sizeof(material));
            } else if (word[0]) {
                memcpy(name, word, sizeof(name));
            }
        } else if (starts(p, stop, "mtllib")) {
            word_into(p + 6, stop, out->mtllib, sizeof(out->mtllib));
        } else if (starts(p, stop, "f")) {
            uint32_t corner[3], k = 0;

            p += 1;

            /* A new part when the name or the material changed. */
            if (nparts == 0 || strcmp(parts[nparts - 1].name, name) != 0
                || strcmp(parts[nparts - 1].material, material) != 0) {
                if (!grow((void **)&parts, &partcap, nparts + 1, sizeof(*parts))) {
                    why = "no memory to read it";
                    break;
                }

                memcpy(parts[nparts].name, name, sizeof(name));
                memcpy(parts[nparts].material, material, sizeof(material));
                parts[nparts].first = nfaces;
                parts[nparts].count = 0;
                nparts++;
            }

            for (;;) {
                float f;
                long index;

                p = skip_space(p, stop);

                if (p >= stop) break;

                if (!number_in(&p, stop, &f) || f != (float)(long)f) {
                    why = "a face corner that is not a point's number";
                    break;
                }

                /* The texture and normal numbers after it, which a mesh
                 * here has no use for. */
                while (p < stop && *p != ' ' && *p != '\t') p++;

                index = (long)f;
                index = index < 0 ? (long)npos + index : index - 1;

                if (index < 0 || index >= (long)npos) {
                    why = "a face naming a point the file has not got";
                    break;
                }

                if (k < 3) {
                    corner[k++] = (uint32_t)index;
                } else {
                    corner[1] = corner[2];
                    corner[2] = (uint32_t)index;
                }

                if (k == 3) {
                    if (nfaces >= MAX_TRIS
                        || !grow((void **)&faces, &facecap, (nfaces + 1) * 3, 4)) {
                        why = "more triangles than a model may have";
                        break;
                    }

                    memcpy(&faces[nfaces * 3], corner, sizeof(corner));
                    nfaces++;
                    parts[nparts - 1].count++;
                }
            }

            if (why) break;
        }
    }

    if (!why && nfaces == 0) why = "no faces in it";

    /* Each part with only its own points, numbered from nought. */
    if (!why) {
        local = malloc((size_t)npos * sizeof(int32_t));
        out->parts = calloc(nparts, sizeof(struct k3d_obj_part));

        if (!local || !out->parts) why = "no memory to read it";
    }

    for (i = 0; !why && i < nparts; i++) {
        struct k3d_obj_part *op = &out->parts[out->nparts];
        struct k3d_soup *s = &op->soup;
        uint32_t t, cap = 0, used = 0;

        if (parts[i].count == 0) continue;

        memcpy(op->name, parts[i].name, sizeof(op->name));
        memcpy(op->material, parts[i].material, sizeof(op->material));
        memset(local, 0xff, (size_t)npos * sizeof(int32_t));

        s->tri = malloc((size_t)parts[i].count * 3 * sizeof(uint32_t));

        if (!s->tri) {
            why = "no memory to read it";
            break;
        }

        for (t = 0; t < parts[i].count * 3; t++) {
            uint32_t g = faces[parts[i].first * 3 + t];

            if (local[g] < 0) {
                if (!grow((void **)&s->pos, &cap, (used + 1) * 3, 4)) {
                    why = "no memory to read it";
                    break;
                }

                memcpy(&s->pos[used * 3], &pos[g * 3], 12);
                local[g] = (int32_t)used++;
            }

            s->tri[t] = (uint32_t)local[g];
        }

        s->npos = used;
        s->ntri = parts[i].count;
        out->nparts++;
    }

    free(local);
    free(pos);
    free(faces);
    free(parts);

    if (why) k3d_obj_free(out);

    return why;
}

/* A growing text, and what adds to it; false when there is no memory. */
struct text { char *s; size_t len, cap; };

static bool append(struct text *t, const char *bytes, size_t n)
{
    if (t->len + n + 1 > t->cap) {
        size_t next = t->cap ? t->cap * 2 : 65536;
        char *bigger;

        while (next < t->len + n + 1) next *= 2;

        bigger = realloc(t->s, next);

        if (!bigger) return false;

        t->s = bigger;
        t->cap = next;
    }

    memcpy(t->s + t->len, bytes, n);
    t->len += n;
    t->s[t->len] = '\0';
    return true;
}

static bool point_line(struct text *t, const float *p)
{
    char line[128];
    int n = snprintf(line, sizeof(line), "v %.6g %.6g %.6g\n", (double)p[0], (double)p[1],
                     (double)p[2]);

    return n > 0 && (size_t)n < sizeof(line) && append(t, line, (size_t)n);
}

static bool face_line(struct text *t, unsigned long a, unsigned long b, unsigned long c)
{
    char line[96];
    int n = snprintf(line, sizeof(line), "f %lu %lu %lu\n", a, b, c);

    return n > 0 && (size_t)n < sizeof(line) && append(t, line, (size_t)n);
}

/* A keyword and a name, which is one word in OBJ: a space becomes an
 * underscore, and what is not printable goes. */
static bool word_line(struct text *t, const char *key, const char *word)
{
    char clean[64];
    size_t i, n = 0;

    for (i = 0; word[i] && n < sizeof(clean); i++) {
        unsigned char c = (unsigned char)word[i];

        if (c > 32 && c < 127) clean[n++] = (char)c;
        else if (c == ' ') clean[n++] = '_';
    }

    if (n == 0) return true;

    return append(t, key, strlen(key)) && append(t, clean, n) && append(t, "\n", 1);
}

char *k3d_obj_write(const struct k3d_obj_part *parts, size_t n, const char *mtllib, size_t *len)
{
    struct text t = { NULL, 0, 0 };
    unsigned long base = 1;
    size_t i;

    if (!append(&t, "# Cafesa3D, Kosmos\n", 19)) goto fail;
    if (mtllib && mtllib[0] && !word_line(&t, "mtllib ", mtllib)) goto fail;

    for (i = 0; i < n; i++) {
        const struct k3d_soup *s = &parts[i].soup;
        uint32_t k;

        if (!word_line(&t, "o ", parts[i].name)) goto fail;
        if (parts[i].material[0] && !word_line(&t, "usemtl ", parts[i].material)) goto fail;

        for (k = 0; k < s->npos; k++) {
            if (!point_line(&t, &s->pos[k * 3])) goto fail;
        }

        for (k = 0; k < s->ntri; k++) {
            if (!face_line(&t, s->tri[k * 3] + base, s->tri[k * 3 + 1] + base,
                           s->tri[k * 3 + 2] + base)) goto fail;
        }

        base += s->npos;
    }

    *len = t.len;
    return t.s;

fail:
    free(t.s);
    return NULL;
}
