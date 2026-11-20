/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A binary glTF into parts to draw (`k3d_glb.h` has what and why).
 *
 * Three steps, in order: the file's two chunks found and its JSON made
 * tokens; the nodes placed, each knowing which movable it belongs to; and
 * every primitive of every placed mesh made one part or, skinned, one per
 * movable its triangles fall to. The movables' sizes come last, from the
 * parts.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "k3d_glb.h"

/* ---------------------------------------------------------------------- */
/* JSON, as tokens: each value one, in the order the text has them, each
 * knowing the token after everything inside it - which is all walking an
 * object or an array needs. Strings keep their escapes. */

enum { J_OBJECT, J_ARRAY, J_STRING, J_NUMBER, J_LITERAL };

struct jtok {
    uint8_t type;
    uint32_t start, end;            /* a string's: inside its quotes */
    uint32_t size;                  /* an object's keys, an array's values */
    uint32_t next;                  /* the token after this one's insides */
};

struct json {
    const char *s;
    uint32_t n, at;
    struct jtok *t;
    uint32_t count, cap;
};

#define J_DEPTH 64                  /* glTF nests five deep */

static void j_space(struct json *j)
{
    while (j->at < j->n && (j->s[j->at] == ' ' || j->s[j->at] == '\t' ||
                            j->s[j->at] == '\n' || j->s[j->at] == '\r')) {
        j->at++;
    }
}

static int j_push(struct json *j, uint8_t type)
{
    if (j->count == j->cap) {
        uint32_t cap = j->cap ? j->cap * 2 : 1024;
        struct jtok *t = realloc(j->t, (size_t)cap * sizeof *t);

        if (!t) return -1;
        j->t = t;
        j->cap = cap;
    }
    j->t[j->count] = (struct jtok){ .type = type, .start = j->at };
    return (int)j->count++;
}

static bool j_string(struct json *j)
{
    int i = j_push(j, J_STRING);

    if (i < 0) return false;
    j->at++;                                    /* the opening quote */
    j->t[i].start = j->at;
    while (j->at < j->n && j->s[j->at] != '"') {
        if ((unsigned char)j->s[j->at] < 0x20) return false;
        j->at += j->s[j->at] == '\\' ? 2 : 1;
    }
    if (j->at >= j->n) return false;
    j->t[i].end = j->at++;
    j->t[i].next = j->count;
    return true;
}

static bool j_value(struct json *j, int depth)
{
    j_space(j);
    if (j->at >= j->n || depth > J_DEPTH) return false;

    char c = j->s[j->at];

    if (c == '"') return j_string(j);

    if (c == '{' || c == '[') {
        int i = j_push(j, c == '{' ? J_OBJECT : J_ARRAY);
        char close = c == '{' ? '}' : ']';
        uint32_t size = 0;

        if (i < 0) return false;
        j->at++;
        j_space(j);
        if (j->at < j->n && j->s[j->at] == close) {
            j->at++;
        } else {
            for (;;) {
                if (c == '{') {
                    j_space(j);
                    if (j->at >= j->n || j->s[j->at] != '"' || !j_string(j)) return false;
                    j_space(j);
                    if (j->at >= j->n || j->s[j->at] != ':') return false;
                    j->at++;
                }
                if (!j_value(j, depth + 1)) return false;
                size++;
                j_space(j);
                if (j->at >= j->n) return false;
                if (j->s[j->at] == ',') {
                    j->at++;
                } else if (j->s[j->at] == close) {
                    j->at++;
                    break;
                } else {
                    return false;
                }
            }
        }
        j->t[i].size = size;
        j->t[i].end = j->at;
        j->t[i].next = j->count;
        return true;
    }

    /* A number or true, false, null: what is held to its form is only
     * what is read later, as a number. */
    int i = j_push(j, (c == '-' || (c >= '0' && c <= '9')) ? J_NUMBER : J_LITERAL);

    if (i < 0) return false;
    while (j->at < j->n) {
        c = j->s[j->at];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-' || c == '+' ||
              c == '.' || c == 'E')) break;
        j->at++;
    }
    if (j->at == j->t[i].start) return false;
    j->t[i].end = j->at;
    j->t[i].next = j->count;
    return true;
}

static bool j_is(const struct json *j, int k, const char *word)
{
    size_t len = strlen(word);

    return k >= 0 && j->t[k].type == J_STRING && j->t[k].end - j->t[k].start == len &&
           memcmp(j->s + j->t[k].start, word, len) == 0;
}

/* An object's value for `key`, or -1. */
static int j_get(const struct json *j, int obj, const char *key)
{
    if (obj < 0 || j->t[obj].type != J_OBJECT) return -1;

    uint32_t k = (uint32_t)obj + 1;

    for (uint32_t i = 0; i < j->t[obj].size; i++) {
        if (j_is(j, (int)k, key)) return (int)k + 1;
        k = j->t[k + 1].next;
    }
    return -1;
}

/* An array's `n`th value, or -1. */
static int j_at(const struct json *j, int arr, uint32_t n)
{
    if (arr < 0 || j->t[arr].type != J_ARRAY || n >= j->t[arr].size) return -1;

    uint32_t k = (uint32_t)arr + 1;

    while (n--) k = j->t[k].next;
    return (int)k;
}

static uint32_t j_len(const struct json *j, int arr)
{
    return arr >= 0 && j->t[arr].type == J_ARRAY ? j->t[arr].size : 0;
}

static bool j_number(const struct json *j, int k, double *out)
{
    char text[64];
    char *end;
    uint32_t len;

    if (k < 0 || j->t[k].type != J_NUMBER) return false;
    len = j->t[k].end - j->t[k].start;
    if (len >= sizeof text) return false;
    memcpy(text, j->s + j->t[k].start, len);
    text[len] = 0;
    *out = strtod(text, &end);
    return end == text + len;
}

/* A whole number from 0 to 2^31 - 1, or `fallback` when absent; -2 when
 * present and not one, which every caller treats as out of range. */
static int64_t j_index(const struct json *j, int k, int64_t fallback)
{
    double v;

    if (k < 0) return fallback;
    if (!j_number(j, k, &v) || v < 0 || v > 2147483647.0 || v != (double)(int64_t)v) return -2;
    return (int64_t)v;
}

static double j_real(const struct json *j, int k, double fallback)
{
    double v;

    return j_number(j, k, &v) ? v : fallback;
}

/* ---------------------------------------------------------------------- */
/* Matrices: column-major, in doubles while they are being multiplied. */

static void m_identity(double m[16])
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void m_mul(double out[16], const double a[16], const double b[16])
{
    double r[16];

    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            double s = 0;

            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    }
    memcpy(out, r, sizeof r);
}

static void m_point(const double m[16], const float p[3], double out[3])
{
    for (int r = 0; r < 3; r++) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

/* The 3 by 3's inverse transpose, up to a positive factor: its cofactors,
 * signed by its determinant. What turns a normal, which is unit length
 * again after. */
static void m_normal(const double m[16], double n[9])
{
    double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    double cof[9] = { e * i - f * h, -(d * i - f * g), d * h - e * g,
                      -(b * i - c * h), a * i - c * g, -(a * h - b * g),
                      b * f - c * e, -(a * f - c * d), a * e - b * d };
    double det = a * cof[0] + b * cof[1] + c * cof[2];

    /* cof[3 * row + col] is the cofactor of row, col; the inverse
     * transpose is the cofactors over the determinant, row for row. */
    for (int k = 0; k < 9; k++) n[k] = det < 0 ? -cof[k] : cof[k];
}

static void unit(float v[3], float fallback_y)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (l > 1e-20f) {
        v[0] /= l, v[1] /= l, v[2] /= l;
    } else {
        v[0] = 0, v[1] = fallback_y, v[2] = 0;
    }
}

/* ---------------------------------------------------------------------- */
/* The file: its BIN chunk, its buffer views and accessors held to it. */

struct view {
    uint64_t offset, length;        /* into the BIN chunk */
    uint32_t stride;                /* 0: packed */
    bool ok;                        /* in the BIN chunk at all */
};

struct accessor {
    int view;
    uint64_t offset;
    uint32_t component, count, ncomp;
    bool normalized, sparse;
};

struct node {
    int mesh, skin, parent;
    int movable;
    bool placed;
    double world[16];
};

struct reader {
    struct json j;
    const uint8_t *file;
    const uint8_t *bin;
    uint64_t binlen;
    struct view *views;
    uint32_t nviews;
    struct accessor *acc;
    uint32_t nacc;
    struct node *nodes;
    uint32_t nnodes;
    struct k3d_glb *out;
    uint32_t partcap;
};

static uint32_t component_size(uint32_t c)
{
    switch (c) {
    case 5120: case 5121: return 1;             /* (unsigned) byte */
    case 5122: case 5123: return 2;             /* (unsigned) short */
    case 5125: case 5126: return 4;             /* unsigned int, float */
    default: return 0;
    }
}

/* An accessor whose every element lies inside its view, read as `ncomp`
 * components of type `want` (0: any) - or NULL. */
static const struct accessor *accessor(struct reader *r, int64_t i, uint32_t ncomp)
{
    if (i < 0 || i >= r->nacc) return NULL;

    const struct accessor *a = &r->acc[i];

    if (a->sparse || a->view < 0 || a->ncomp != ncomp || !component_size(a->component)) return NULL;

    const struct view *v = &r->views[a->view];
    uint64_t elem = (uint64_t)component_size(a->component) * a->ncomp;
    uint64_t stride = v->stride ? v->stride : elem;

    if (!v->ok || a->count == 0) return a->count == 0 && v->ok ? a : NULL;
    if (a->offset + (uint64_t)(a->count - 1) * stride + elem > v->length) return NULL;
    return a;
}

static const uint8_t *element(const struct reader *r, const struct accessor *a, uint32_t k)
{
    const struct view *v = &r->views[a->view];
    uint64_t elem = (uint64_t)component_size(a->component) * a->ncomp;
    uint64_t stride = v->stride ? v->stride : elem;

    return r->bin + v->offset + a->offset + (uint64_t)k * stride;
}

/* Component `c` of element `k`, as a float glTF means it: normalised
 * integers to 0..1 or -1..1, others as their value. */
static float component(const struct reader *r, const struct accessor *a, uint32_t k, uint32_t c)
{
    const uint8_t *p = element(r, a, k) + (size_t)c * component_size(a->component);

    switch (a->component) {
    case 5126: { float f; memcpy(&f, p, 4); return f; }
    case 5121: return a->normalized ? p[0] / 255.0f : p[0];
    case 5120: { float f = (int8_t)p[0]; return a->normalized ? (f / 127 < -1 ? -1 : f / 127) : f; }
    case 5123: { uint16_t u; memcpy(&u, p, 2); return a->normalized ? u / 65535.0f : u; }
    case 5122: { int16_t s; memcpy(&s, p, 2); float f = s;
                 return a->normalized ? (f / 32767 < -1 ? -1 : f / 32767) : f; }
    default: { uint32_t u; memcpy(&u, p, 4); return (float)u; }
    }
}

static uint32_t whole(const struct reader *r, const struct accessor *a, uint32_t k, uint32_t c)
{
    const uint8_t *p = element(r, a, k) + (size_t)c * component_size(a->component);

    switch (a->component) {
    case 5121: return p[0];
    case 5123: { uint16_t u; memcpy(&u, p, 2); return u; }
    case 5125: { uint32_t u; memcpy(&u, p, 4); return u; }
    default: return UINT32_MAX;                 /* not an index's type */
    }
}

static int count_of(const struct json *j, int k)
{
    static const char *const types[] = { "SCALAR", "VEC2", "VEC3", "VEC4", "MAT4" };
    static const int counts[] = { 1, 2, 3, 4, 16 };

    for (int i = 0; i < 5; i++) {
        if (j_is(j, k, types[i])) return counts[i];
    }
    return 0;
}

static bool read_tables(struct reader *r, const char **why)
{
    struct json *j = &r->j;
    int buffers = j_get(j, 0, "buffers");
    int views = j_get(j, 0, "bufferViews");
    int accs = j_get(j, 0, "accessors");
    int first = j_at(j, buffers, 0);
    double length = 0;

    /* The first buffer is the BIN chunk when it names no file. */
    bool binbuf = first >= 0 && j_get(j, first, "uri") < 0 && r->bin &&
                  j_number(j, j_get(j, first, "byteLength"), &length) && length <= (double)r->binlen;

    r->nviews = j_len(j, views);
    r->nacc = j_len(j, accs);
    r->views = calloc(r->nviews ? r->nviews : 1, sizeof *r->views);
    r->acc = calloc(r->nacc ? r->nacc : 1, sizeof *r->acc);
    if (!r->views || !r->acc) {
        *why = "out of memory";
        return false;
    }

    for (uint32_t i = 0; i < r->nviews; i++) {
        int v = j_at(j, views, i);
        int64_t buf = j_index(j, j_get(j, v, "buffer"), -1);
        int64_t off = j_index(j, j_get(j, v, "byteOffset"), 0);
        int64_t len = j_index(j, j_get(j, v, "byteLength"), -1);
        int64_t stride = j_index(j, j_get(j, v, "byteStride"), 0);

        r->views[i].ok = buf == 0 && binbuf && off >= 0 && len >= 0 && stride >= 0 &&
                         stride <= 252 && (uint64_t)off + (uint64_t)len <= (uint64_t)length;
        r->views[i].offset = off < 0 ? 0 : (uint64_t)off;
        r->views[i].length = len < 0 ? 0 : (uint64_t)len;
        r->views[i].stride = stride < 0 ? 0 : (uint32_t)stride;
    }

    for (uint32_t i = 0; i < r->nacc; i++) {
        int a = j_at(j, accs, i);
        int64_t view = j_index(j, j_get(j, a, "bufferView"), -1);
        int64_t off = j_index(j, j_get(j, a, "byteOffset"), 0);
        int64_t comp = j_index(j, j_get(j, a, "componentType"), 0);
        int64_t count = j_index(j, j_get(j, a, "count"), -1);
        int norm = j_get(j, a, "normalized");

        r->acc[i].view = view >= 0 && view < r->nviews ? (int)view : -1;
        r->acc[i].offset = off < 0 ? UINT64_MAX / 4 : (uint64_t)off;
        r->acc[i].component = comp < 0 ? 0 : (uint32_t)comp;
        r->acc[i].count = count < 0 ? 0 : (uint32_t)count;
        r->acc[i].ncomp = (uint32_t)count_of(j, j_get(j, a, "type"));
        r->acc[i].normalized = norm >= 0 && j->t[norm].type == J_LITERAL &&
                               j->s[j->t[norm].start] == 't';
        r->acc[i].sparse = j_get(j, a, "sparse") >= 0 || count < 0;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* The nodes: each placed in the model's frame, and given its movable -
 * its own name's, or the nearest ancestor's that has one. */

static void local_matrix(const struct json *j, int node, double m[16])
{
    int mat = j_get(j, node, "matrix");

    m_identity(m);
    if (j_len(j, mat) == 16) {
        for (uint32_t k = 0; k < 16; k++) m[k] = j_real(j, j_at(j, mat, k), m[k]);
        return;
    }

    int t = j_get(j, node, "translation"), q = j_get(j, node, "rotation"), s = j_get(j, node, "scale");
    double x = 0, y = 0, z = 0, w = 1, sx = 1, sy = 1, sz = 1;

    if (j_len(j, q) == 4) {
        x = j_real(j, j_at(j, q, 0), 0), y = j_real(j, j_at(j, q, 1), 0);
        z = j_real(j, j_at(j, q, 2), 0), w = j_real(j, j_at(j, q, 3), 1);

        double l = sqrt(x * x + y * y + z * z + w * w);

        if (l > 0) x /= l, y /= l, z /= l, w /= l;
        else x = y = z = 0, w = 1;
    }
    if (j_len(j, s) == 3) {
        sx = j_real(j, j_at(j, s, 0), 1), sy = j_real(j, j_at(j, s, 1), 1);
        sz = j_real(j, j_at(j, s, 2), 1);
    }

    /* T R S: the rotation's columns, each scaled, then the translation. */
    m[0] = (1 - 2 * (y * y + z * z)) * sx, m[1] = 2 * (x * y + z * w) * sx, m[2] = 2 * (x * z - y * w) * sx;
    m[4] = 2 * (x * y - z * w) * sy, m[5] = (1 - 2 * (x * x + z * z)) * sy, m[6] = 2 * (y * z + x * w) * sy;
    m[8] = 2 * (x * z + y * w) * sz, m[9] = 2 * (y * z - x * w) * sz, m[10] = (1 - 2 * (x * x + y * y)) * sz;
    if (j_len(j, t) == 3) {
        for (uint32_t k = 0; k < 3; k++) m[12 + k] = j_real(j, j_at(j, t, k), 0);
    }
}

static bool place_nodes(struct reader *r, const char *const *names, unsigned count, const char **why)
{
    struct json *j = &r->j;
    int nodes = j_get(j, 0, "nodes");
    int scenes = j_get(j, 0, "scenes");
    int64_t which = j_index(j, j_get(j, 0, "scene"), 0);
    int scene = j_at(j, scenes, which < 0 ? 0 : (uint32_t)which);
    int roots = j_get(j, scene, "nodes");

    r->nnodes = j_len(j, nodes);
    r->nodes = calloc(r->nnodes ? r->nnodes : 1, sizeof *r->nodes);

    uint32_t *stack = malloc((r->nnodes ? r->nnodes : 1) * sizeof *stack);

    if (!r->nodes || !stack) {
        free(stack);
        *why = "out of memory";
        return false;
    }

    for (uint32_t i = 0; i < r->nnodes; i++) {
        int n = j_at(j, nodes, i);
        int name = j_get(j, n, "name");

        r->nodes[i].mesh = (int)j_index(j, j_get(j, n, "mesh"), -1);
        r->nodes[i].skin = (int)j_index(j, j_get(j, n, "skin"), -1);
        r->nodes[i].parent = -1;
        r->nodes[i].movable = -1;
        for (unsigned m = 0; m < count; m++) {
            if (j_is(j, name, names[m])) {
                r->nodes[i].movable = (int)m;
                r->out->movables[m].node = (int)i;
            }
        }
    }

    /* Depth first from the scene's roots; a node reached twice is a file
     * that is not a tree, and is placed where it was first reached. */
    uint32_t top = 0;

    for (uint32_t k = 0; k < j_len(j, roots); k++) {
        int64_t i = j_index(j, j_at(j, roots, k), -1);

        if (i < 0 || i >= r->nnodes || r->nodes[i].placed) continue;
        r->nodes[i].placed = true;
        local_matrix(j, j_at(j, nodes, (uint32_t)i), r->nodes[i].world);
        stack[top++] = (uint32_t)i;

        while (top) {
            uint32_t p = stack[--top];
            int kids = j_get(j, j_at(j, nodes, p), "children");

            for (uint32_t c = 0; c < j_len(j, kids); c++) {
                int64_t ch = j_index(j, j_at(j, kids, c), -1);
                double own[16];

                if (ch < 0 || ch >= r->nnodes || r->nodes[ch].placed) continue;
                r->nodes[ch].placed = true;
                r->nodes[ch].parent = (int)p;
                if (r->nodes[ch].movable < 0) r->nodes[ch].movable = r->nodes[p].movable;
                local_matrix(j, j_at(j, nodes, (uint32_t)ch), own);
                m_mul(r->nodes[ch].world, r->nodes[p].world, own);
                stack[top++] = (uint32_t)ch;
            }
        }
    }
    free(stack);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Parts. */

static struct k3d_glb_part *new_part(struct reader *r)
{
    struct k3d_glb *g = r->out;

    if (g->nparts == r->partcap) {
        uint32_t cap = r->partcap ? r->partcap * 2 : 32;
        struct k3d_glb_part *p = realloc(g->parts, (size_t)cap * sizeof *p);

        if (!p) return NULL;
        g->parts = p;
        r->partcap = cap;
    }

    struct k3d_glb_part *p = &g->parts[g->nparts++];

    memset(p, 0, sizeof *p);
    return p;
}

/* Normals from the faces, for a primitive that came without: each
 * vertex the sum of its triangles' areas' normals. */
static void face_normals(struct k3d_glb_part *p)
{
    for (uint32_t v = 0; v < p->nvertices; v++) memset(p->vertices[v].normal, 0, sizeof p->vertices[v].normal);
    for (uint32_t t = 0; t + 2 < p->nindices; t += 3) {
        float *a = p->vertices[p->indices[t]].position, *b = p->vertices[p->indices[t + 1]].position;
        float *c = p->vertices[p->indices[t + 2]].position;
        float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float n[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };

        for (int k = 0; k < 3; k++) {
            for (int e = 0; e < 3; e++) p->vertices[p->indices[t + k]].normal[e] += n[e];
        }
    }
}

struct primitive {
    const struct accessor *pos, *nrm, *uv, *idx, *joints, *weights;
    uint32_t nverts, nidx;
    int material, texture, mesh;
    float colour[4];
};

/* One vertex as the part keeps it, placed by `m` (NULL: as it is). */
static void put_vertex(const struct reader *r, const struct primitive *pr, uint32_t v,
                       const double *m, const double *nm, struct k3d_glb_vertex *out)
{
    float p[3], n[3] = { 0, 0, 0 };

    for (uint32_t c = 0; c < 3; c++) p[c] = component(r, pr->pos, v, c);
    if (pr->nrm) {
        for (uint32_t c = 0; c < 3; c++) n[c] = component(r, pr->nrm, v, c);
    }
    if (m) {
        double q[3];

        m_point(m, p, q);
        for (int c = 0; c < 3; c++) p[c] = (float)q[c];

        float t[3];

        for (int row = 0; row < 3; row++) {
            t[row] = (float)(nm[3 * row] * n[0] + nm[3 * row + 1] * n[1] + nm[3 * row + 2] * n[2]);
        }
        memcpy(n, t, sizeof n);
    }
    unit(n, 1);
    memcpy(out->position, p, sizeof p);
    memcpy(out->normal, n, sizeof n);
    out->uv[0] = pr->uv ? component(r, pr->uv, v, 0) : 0;
    out->uv[1] = pr->uv ? component(r, pr->uv, v, 1) : 0;
}

static uint32_t index_at(const struct reader *r, const struct primitive *pr, uint32_t k)
{
    return pr->idx ? whole(r, pr->idx, k, 0) : k;
}

/* A part of the triangles `which` picks (NULL: all), its vertices
 * renumbered from nought, each placed by its own joint's matrix when
 * `joint_m` is given. */
static bool emit(struct reader *r, const struct primitive *pr, const int *tri_movable, int movable,
                 const uint32_t *dominant, const double *joint_m, const double *joint_n,
                 const double *node_m, uint32_t *map)
{
    uint32_t tris = 0, verts = 0;

    for (uint32_t t = 0; t < pr->nidx / 3; t++) {
        if (tri_movable && tri_movable[t] != movable) continue;
        tris++;
        for (int k = 0; k < 3; k++) {
            uint32_t v = index_at(r, pr, t * 3 + k);

            if (map[v] == UINT32_MAX) map[v] = verts++;
        }
    }
    if (!tris) return true;

    struct k3d_glb_part *p = new_part(r);

    if (!p) return false;
    p->vertices = malloc((size_t)verts * sizeof *p->vertices);
    p->indices = malloc((size_t)tris * 3 * sizeof *p->indices);
    if (!p->vertices || !p->indices) return false;
    p->nvertices = verts;
    p->nindices = tris * 3;
    p->texture = pr->texture;
    p->material = pr->material;
    p->mesh = pr->mesh;
    p->movable = movable;
    memcpy(p->colour, pr->colour, sizeof p->colour);

    uint32_t at = 0;

    for (uint32_t t = 0; t < pr->nidx / 3; t++) {
        if (tri_movable && tri_movable[t] != movable) continue;
        for (int k = 0; k < 3; k++) {
            uint32_t v = index_at(r, pr, t * 3 + k);

            if (map[v] != UINT32_MAX && map[v] < verts && !(map[v] & 0x80000000u)) {
                uint32_t mine = map[v];

                if (joint_m) {
                    put_vertex(r, pr, v, joint_m + 16 * dominant[v], joint_n + 9 * dominant[v],
                               &p->vertices[mine]);
                } else {
                    put_vertex(r, pr, v, NULL, NULL, &p->vertices[mine]);
                }
                map[v] = mine | 0x80000000u;    /* written: once is enough */
            }
            p->indices[at++] = map[v] & 0x7fffffffu;
        }
    }

    /* Undo the marks, so the next part of this primitive starts afresh. */
    for (uint32_t t = 0; t < pr->nidx; t++) map[index_at(r, pr, t)] = UINT32_MAX;

    if (!pr->nrm) {
        face_normals(p);
        for (uint32_t v = 0; v < p->nvertices; v++) unit(p->vertices[v].normal, 1);
    }

    double m[16];

    if (joint_m || !node_m) m_identity(m);
    else memcpy(m, node_m, sizeof m);
    for (int k = 0; k < 16; k++) p->matrix[k] = (float)m[k];
    r->out->triangles += tris;
    return true;
}

/* The skin's joints, each its node's world matrix times its inverse bind
 * matrix, and that one's normal matrix; with each joint's movable. NULL
 * on a skin this cannot use. */
static double *joint_matrices(struct reader *r, int64_t skin, uint32_t *njoints, int **movable,
                              double **normals)
{
    struct json *j = &r->j;
    int s = j_at(j, j_get(j, 0, "skins"), skin < 0 ? UINT32_MAX : (uint32_t)skin);
    int joints = j_get(j, s, "joints");
    uint32_t n = j_len(j, joints);
    int64_t ibm_i = j_index(j, j_get(j, s, "inverseBindMatrices"), -1);
    const struct accessor *ibm = ibm_i >= 0 ? accessor(r, ibm_i, 16) : NULL;

    if (s < 0 || !n || (ibm_i >= 0 && (!ibm || ibm->count < n || ibm->component != 5126))) return NULL;

    double *m = malloc((size_t)n * 16 * sizeof *m);
    double *nm = malloc((size_t)n * 9 * sizeof *nm);
    int *mov = malloc((size_t)n * sizeof *mov);

    if (!m || !nm || !mov) {
        free(m), free(nm), free(mov);
        return NULL;
    }
    for (uint32_t k = 0; k < n; k++) {
        int64_t node = j_index(j, j_at(j, joints, k), -1);
        double inv[16];

        if (node < 0 || node >= r->nnodes || !r->nodes[node].placed) {
            free(m), free(nm), free(mov);
            return NULL;
        }
        m_identity(inv);
        if (ibm) {
            for (uint32_t c = 0; c < 16; c++) inv[c] = component(r, ibm, k, c);
        }
        m_mul(m + 16 * k, r->nodes[node].world, inv);
        m_normal(m + 16 * k, nm + 9 * k);
        mov[k] = r->nodes[node].movable;
    }
    *njoints = n;
    *movable = mov;
    *normals = nm;
    return m;
}

/* A primitive's accessors and material, or false for one not drawn. */
static bool primitive_of(struct reader *r, int prim, struct primitive *pr)
{
    struct json *j = &r->j;
    int attrs = j_get(j, prim, "attributes");
    int64_t mode = j_index(j, j_get(j, prim, "mode"), 4);
    int64_t mat = j_index(j, j_get(j, prim, "material"), -1);
    int64_t texcoord = 0;

    memset(pr, 0, sizeof *pr);
    pr->material = mat >= 0 ? (int)mat : -1;
    pr->texture = -1;
    pr->colour[0] = pr->colour[1] = pr->colour[2] = pr->colour[3] = 1;
    if (mode != 4) return false;

    int material = mat >= 0 ? j_at(j, j_get(j, 0, "materials"), (uint32_t)mat) : -1;
    int pbr = j_get(j, material, "pbrMetallicRoughness");
    int factor = j_get(j, pbr, "baseColorFactor");
    int tex = j_get(j, pbr, "baseColorTexture");

    if (j_len(j, factor) == 4) {
        for (uint32_t k = 0; k < 4; k++) pr->colour[k] = (float)j_real(j, j_at(j, factor, k), 1);
    }
    if (tex >= 0) {
        int64_t t = j_index(j, j_get(j, tex, "index"), -1);

        texcoord = j_index(j, j_get(j, tex, "texCoord"), 0);
        if (t >= 0 && t < r->out->ntextures) pr->texture = (int)t;
    }

    static const char *const coords[] = { "TEXCOORD_0", "TEXCOORD_1", "TEXCOORD_2", "TEXCOORD_3" };

    pr->pos = accessor(r, j_index(j, j_get(j, attrs, "POSITION"), -1), 3);
    if (!pr->pos || pr->pos->component != 5126) return false;
    pr->nverts = pr->pos->count;

    int nk = j_get(j, attrs, "NORMAL");

    if (nk >= 0) {
        pr->nrm = accessor(r, j_index(j, nk, -1), 3);
        if (!pr->nrm || pr->nrm->component != 5126 || pr->nrm->count != pr->nverts) return false;
    }

    int uk = texcoord >= 0 && texcoord < 4 ? j_get(j, attrs, coords[texcoord]) : -1;

    if (uk >= 0) {
        pr->uv = accessor(r, j_index(j, uk, -1), 2);
        if (!pr->uv || pr->uv->count != pr->nverts ||
            !(pr->uv->component == 5126 || pr->uv->normalized)) return false;
    }

    int ik = j_get(j, prim, "indices");

    if (ik >= 0) {
        pr->idx = accessor(r, j_index(j, ik, -1), 1);
        if (!pr->idx || !(pr->idx->component == 5121 || pr->idx->component == 5123 ||
                          pr->idx->component == 5125)) return false;
        pr->nidx = pr->idx->count;
    } else {
        pr->nidx = pr->nverts;
    }
    if (pr->nidx % 3) return false;
    for (uint32_t k = 0; k < pr->nidx; k++) {
        if (index_at(r, pr, k) >= pr->nverts) return false;
    }

    int jk = j_get(j, attrs, "JOINTS_0"), wk = j_get(j, attrs, "WEIGHTS_0");

    if (jk >= 0 && wk >= 0) {
        pr->joints = accessor(r, j_index(j, jk, -1), 4);
        pr->weights = accessor(r, j_index(j, wk, -1), 4);
        if (!pr->joints || !pr->weights || pr->joints->count != pr->nverts ||
            pr->weights->count != pr->nverts ||
            !(pr->joints->component == 5121 || pr->joints->component == 5123)) return false;
    }
    return true;
}

static bool read_meshes(struct reader *r, const char **why)
{
    struct json *j = &r->j;
    int meshes = j_get(j, 0, "meshes");
    uint32_t nmov = r->out->nmovables;

    for (uint32_t i = 0; i < r->nnodes; i++) {
        struct node *nd = &r->nodes[i];
        int mesh = nd->mesh >= 0 ? j_at(j, meshes, (uint32_t)nd->mesh) : -1;
        int prims = j_get(j, mesh, "primitives");

        if (!nd->placed || mesh < 0) continue;

        uint32_t njoints = 0;
        int *joint_mov = NULL;
        double *joint_n = NULL;
        double *joint_m = nd->skin >= 0 ? joint_matrices(r, nd->skin, &njoints, &joint_mov, &joint_n) : NULL;

        for (uint32_t k = 0; k < j_len(j, prims); k++) {
            struct primitive pr;

            if (!primitive_of(r, j_at(j, prims, k), &pr)) {
                r->out->skipped++;
                continue;
            }
            pr.mesh = nd->mesh;

            uint32_t *map = malloc((pr.nverts ? pr.nverts : 1) * sizeof *map);
            bool ok = map != NULL;

            if (ok) memset(map, 0xff, (pr.nverts ? pr.nverts : 1) * sizeof *map);

            if (ok && joint_m && pr.joints) {
                /* Rigid skinning: each vertex its heaviest joint's. */
                uint32_t *dominant = malloc((size_t)pr.nverts * sizeof *dominant + 1);
                int *tri = malloc((size_t)(pr.nidx / 3) * sizeof *tri + 1);
                bool bad = false;

                ok = dominant && tri;
                for (uint32_t v = 0; ok && v < pr.nverts; v++) {
                    uint32_t best = 0;
                    float most = -1;

                    for (uint32_t c = 0; c < 4; c++) {
                        float w = component(r, pr.weights, v, c);

                        if (w > most) most = w, best = c;
                    }
                    dominant[v] = whole(r, pr.joints, v, best);
                    if (dominant[v] >= njoints) bad = true;
                }
                for (uint32_t t = 0; ok && !bad && t < pr.nidx / 3; t++) {
                    int a = joint_mov[dominant[index_at(r, &pr, t * 3)]];
                    int b = joint_mov[dominant[index_at(r, &pr, t * 3 + 1)]];
                    int c = joint_mov[dominant[index_at(r, &pr, t * 3 + 2)]];

                    tri[t] = (b == c && a != b) ? b : a;
                }
                if (ok && bad) {
                    r->out->skipped++;
                } else {
                    for (int m = -1; ok && m < (int)nmov; m++) {
                        ok = emit(r, &pr, tri, m, dominant, joint_m, joint_n, NULL, map);
                    }
                }
                free(dominant);
                free(tri);
            } else if (ok) {
                ok = emit(r, &pr, NULL, nd->movable, NULL, NULL, NULL, nd->world, map);
            }
            free(map);
            if (!ok) {
                free(joint_m), free(joint_n), free(joint_mov);
                *why = "out of memory";
                return false;
            }
        }
        free(joint_m), free(joint_n), free(joint_mov);
    }
    return true;
}

static void read_textures(struct reader *r)
{
    struct json *j = &r->j;
    int textures = j_get(j, 0, "textures");
    int images = j_get(j, 0, "images");
    struct k3d_glb *g = r->out;

    g->ntextures = j_len(j, textures);
    g->textures = calloc(g->ntextures ? g->ntextures : 1, sizeof *g->textures);
    if (!g->textures) {
        g->ntextures = 0;
        return;
    }
    for (uint32_t i = 0; i < g->ntextures; i++) {
        int64_t src = j_index(j, j_get(j, j_at(j, textures, i), "source"), -1);
        int img = src >= 0 ? j_at(j, images, (uint32_t)src) : -1;
        int64_t view = j_index(j, j_get(j, img, "bufferView"), -1);
        int mime = j_get(j, img, "mimeType");

        if (view >= 0 && view < r->nviews && r->views[view].ok) {
            g->textures[i].offset = (size_t)((r->bin - r->file) + r->views[view].offset);
            g->textures[i].length = (size_t)r->views[view].length;
        }
        if (mime >= 0 && j->t[mime].type == J_STRING) {
            uint32_t len = j->t[mime].end - j->t[mime].start;

            if (len < sizeof g->textures[i].mime) {
                memcpy(g->textures[i].mime, j->s + j->t[mime].start, len);
            }
        }
    }
}

/* Each movable's axes, and its parts' size along them. */
static void measure(struct reader *r)
{
    struct k3d_glb *g = r->out;

    for (unsigned m = 0; m < g->nmovables; m++) {
        struct k3d_glb_movable *mv = &g->movables[m];
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        const char letters[3] = { 'X', 'Y', 'Z' };

        if (mv->node < 0 || !r->nodes[mv->node].placed) continue;

        const double *w = r->nodes[mv->node].world;

        for (int k = 0; k < 3; k++) {
            mv->pivot[k] = (float)w[12 + k];
            for (int e = 0; e < 3; e++) mv->axes[k][e] = (float)w[4 * k + e];
            unit(mv->axes[k], 0);
        }
        for (uint32_t i = 0; i < g->nparts; i++) {
            const struct k3d_glb_part *p = &g->parts[i];
            double pm[16];

            if (p->movable != (int)m) continue;
            mv->triangles += p->nindices / 3;
            for (int k = 0; k < 16; k++) pm[k] = p->matrix[k];
            for (uint32_t v = 0; v < p->nvertices; v++) {
                double q[3];

                m_point(pm, p->vertices[v].position, q);
                for (int k = 0; k < 3; k++) {
                    float d = (float)((q[0] - mv->pivot[0]) * mv->axes[k][0] +
                                      (q[1] - mv->pivot[1]) * mv->axes[k][1] +
                                      (q[2] - mv->pivot[2]) * mv->axes[k][2]);

                    if (d < lo[k]) lo[k] = d;
                    if (d > hi[k]) hi[k] = d;
                }
            }
        }

        int longest = 0, big = 0;

        for (int k = 0; k < 3; k++) {
            mv->extent[k] = hi[k] >= lo[k] ? hi[k] - lo[k] : 0;
            if (mv->extent[k] > mv->extent[longest]) longest = k;
        }
        mv->hinge_axis = letters[longest];
        for (int e = 1; e < 3; e++) {
            if (fabsf(mv->axes[longest][e]) > fabsf(mv->axes[longest][big])) big = e;
        }
        for (int e = 0; e < 3; e++) {
            mv->hinge[e] = mv->axes[longest][big] < 0 ? -mv->axes[longest][e] : mv->axes[longest][e];
        }
    }
}

static void bounds(struct k3d_glb *g)
{
    for (int k = 0; k < 3; k++) g->lo[k] = 1e30f, g->hi[k] = -1e30f;
    for (uint32_t i = 0; i < g->nparts; i++) {
        double pm[16];

        for (int k = 0; k < 16; k++) pm[k] = g->parts[i].matrix[k];
        for (uint32_t v = 0; v < g->parts[i].nvertices; v++) {
            double q[3];

            m_point(pm, g->parts[i].vertices[v].position, q);
            for (int k = 0; k < 3; k++) {
                if (q[k] < g->lo[k]) g->lo[k] = (float)q[k];
                if (q[k] > g->hi[k]) g->hi[k] = (float)q[k];
            }
        }
    }
    if (!g->nparts) {
        for (int k = 0; k < 3; k++) g->lo[k] = g->hi[k] = 0;
    }
}

/* ---------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void k3d_glb_free(struct k3d_glb *g)
{
    if (!g) return;
    for (uint32_t i = 0; i < g->nparts; i++) {
        free(g->parts[i].vertices);
        free(g->parts[i].indices);
    }
    free(g->parts);
    free(g->textures);
    memset(g, 0, sizeof *g);
}

bool k3d_glb_read(const uint8_t *bytes, size_t n, const char *const *names, unsigned count,
                  struct k3d_glb *out, const char **why)
{
    struct reader r;
    const char *json = NULL;
    uint32_t jsonlen = 0;
    bool ok = false;

    memset(out, 0, sizeof *out);
    memset(&r, 0, sizeof r);
    *why = NULL;
    if (count > K3D_GLB_MOVABLES) {
        *why = "more movables named than a model may have";
        return false;
    }
    if (n < 20 || memcmp(bytes, "glTF", 4) != 0) {
        *why = "not a binary glTF file";
        return false;
    }
    if (le32(bytes + 4) != 2) {
        *why = "a binary glTF of another version than 2";
        return false;
    }
    if (le32(bytes + 8) > n) {
        *why = "a binary glTF cut short";
        return false;
    }

    /* The chunks: the first JSON and the first BIN, others passed over. */
    uint64_t end = le32(bytes + 8), at = 12;

    while (at + 8 <= end) {
        uint64_t size = le32(bytes + at);
        uint32_t kind = le32(bytes + at + 4);

        if (at + 8 + size > end) {
            *why = "a chunk running past the file's end";
            return false;
        }
        if (kind == 0x4e4f534a && !json) {                  /* "JSON" */
            json = (const char *)bytes + at + 8;
            jsonlen = (uint32_t)size;
        } else if (kind == 0x004e4942 && !r.bin) {          /* "BIN\0" */
            r.bin = bytes + at + 8;
            r.binlen = size;
        }
        at += 8 + size;
    }
    if (!json) {
        *why = "a binary glTF with no JSON in it";
        return false;
    }

    r.file = bytes;
    r.out = out;
    r.j.s = json;
    r.j.n = jsonlen;
    out->nmovables = count;
    for (unsigned m = 0; m < count; m++) out->movables[m].node = -1;

    if (!j_value(&r.j, 0) || r.j.t[0].type != J_OBJECT) {
        *why = r.j.t ? "its JSON does not parse, or is not an object" : "out of memory";
        goto done;
    }

    int asset = j_get(&r.j, 0, "asset");

    if (!j_is(&r.j, j_get(&r.j, asset, "version"), "2.0")) {
        *why = "not a glTF 2.0 file";
        goto done;
    }
    if (!read_tables(&r, why) || !place_nodes(&r, names, count, why)) goto done;
    read_textures(&r);
    if (!read_meshes(&r, why)) goto done;
    measure(&r);
    bounds(out);
    ok = true;

done:
    free(r.j.t);
    free(r.views);
    free(r.acc);
    free(r.nodes);
    if (!ok) {
        k3d_glb_free(out);
        if (!*why) *why = "not a glTF file this reads";
    }
    return ok;
}
