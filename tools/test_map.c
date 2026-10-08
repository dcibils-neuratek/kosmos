/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Map Kit's reading half, on the Mac (`user/kits/map/`, `docs/maps.md`
 * M2): a PMTiles archive opened and a vector tile decoded - held to the
 * PMTiles specification's own tile numbers and to the made-up Port Alder
 * that `tools/mapcity.py` writes, whose every street and place is known.
 *
 *     test_map port-alder.pmtiles
 */

#include "kits/compress/gzip.h"
#include "mapdraw.h"
#include "mvt.h"
#include "pmtiles.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static struct gunzip_work work;

struct gather { uint8_t *bytes; size_t n, cap; };

static int put(void *user, const uint8_t *b, size_t n)
{
    struct gather *g = user;

    if (g->n + n > g->cap) {
        g->cap = (g->n + n) * 2;
        g->bytes = realloc(g->bytes, g->cap);
    }

    memcpy(g->bytes + g->n, b, n);
    g->n += n;
    return 1;
}

/* The tile (z, x, y) of `a`, inflated and decoded into `t`. */
static int tile(struct pmtiles *a, unsigned z, uint32_t x, uint32_t y, struct mvt_tile *t)
{
    const uint8_t *bytes;
    size_t n, got;
    struct gather g = { 0 };
    int r = pmt_find(a, z, x, y, &bytes, &n);

    if (r != PMT_OK) return r;

    if (a->tile_gzip) {
        if (kosmos_gunzip(bytes, n, &work, put, &g, &got) != GUNZIP_WHOLE) return -100;
        bytes = g.bytes, n = g.n;
    }

    r = mvt_decode(t, bytes, n);
    free(g.bytes);
    return r;
}

/* The tile at zoom `z` holding longitude and latitude (lon, lat). */
static void tile_of(double lon, double lat, unsigned z, uint32_t *x, uint32_t *y)
{
    double n = (double)(1u << z);
    double r = lat * M_PI / 180.0;

    *x = (uint32_t)((lon + 180.0) / 360.0 * n);
    *y = (uint32_t)((1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n);
}

static const struct mvt_feature *named(const struct mvt_tile *t, const char *name)
{
    for (size_t i = 0; i < t->nfeatures; i++) {
        if (t->features[i].name && strcmp(t->features[i].name, name) == 0) return &t->features[i];
    }

    return NULL;
}

/* A vector tile written here, a protocol buffer at a time. */
struct pbw { uint8_t b[512]; size_t n; };

static void pbw_varint(struct pbw *w, uint64_t v)
{
    while (v >= 0x80) {
        w->b[w->n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }

    w->b[w->n++] = (uint8_t)v;
}

static void pbw_bytes(struct pbw *w, unsigned field, const void *p, size_t n)
{
    pbw_varint(w, (uint64_t)field << 3 | 2);
    pbw_varint(w, n);
    memcpy(w->b + w->n, p, n);
    w->n += n;
}

/* One point in a `place` layer, its tags as key and value indices. */
static void pbw_point(struct pbw *layer, const uint8_t *tags, size_t ntags)
{
    struct pbw f = { .n = 0 };
    static const uint8_t geom[] = { 9, 50, 34 };   /* MoveTo once, (25, 17) */

    pbw_bytes(&f, 2, tags, ntags);
    pbw_varint(&f, 3 << 3);                         /* type: */
    pbw_varint(&f, 1);                              /* a point */
    pbw_bytes(&f, 4, geom, sizeof geom);
    pbw_bytes(layer, 2, f.b, f.n);
}

static size_t count(const struct mvt_tile *t, int layer, const char *klass)
{
    size_t n = 0;

    for (size_t i = 0; i < t->nfeatures; i++) {
        if (t->features[i].layer == layer
            && (klass == NULL || strcmp(t->features[i].klass, klass) == 0)) n++;
    }

    return n;
}

int main(int argc, char **argv)
{
    struct pmtiles a;
    struct mvt_tile t;
    uint8_t *data;
    long len;
    char said[256];
    FILE *f;

    if (argc < 2 || (f = fopen(argv[1], "rb")) == NULL) {
        printf("FAIL: test_map needs Port Alder's archive (tools/mapcity.py)\n");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)len);
    if (fread(data, 1, (size_t)len, f) != (size_t)len) return 1;
    fclose(f);

    /* 1. The specification's own tile numbers. */
    check(pmt_tile_id(0, 0, 0) == 0 && pmt_tile_id(1, 0, 0) == 1 && pmt_tile_id(1, 0, 1) == 2
          && pmt_tile_id(1, 1, 1) == 3 && pmt_tile_id(1, 1, 0) == 4 && pmt_tile_id(2, 0, 0) == 5,
          "the Hilbert tile numbers are not the specification's");

    /* 2. The header: Port Alder at zooms 11 to 16, gzipped, on Null Island. */
    {
        int r = pmt_open(&a, data, (size_t)len, &work);

        snprintf(said, sizeof said, "Port Alder's archive did not open: %s", pmt_said(r));
        check(r == PMT_OK, said);
        check(a.min_zoom == 11 && a.max_zoom == 16 && a.tile_type == 1
              && a.internal_gzip && a.tile_gzip, "the header is not what mapcity.py wrote");
        check(a.min_lon_e7 < 0 && a.max_lon_e7 > 0 && a.min_lat_e7 < 0 && a.max_lat_e7 > 0,
              "Port Alder's bounds do not hold Null Island");
        snprintf(said, sizeof said, "the root directory has %zu entries, not 138", a.nroot);
        check(a.nroot == 138, said);
    }

    /* 3. Zoom 11: one tile with the city's name and no buildings yet. */
    {
        uint32_t x, y;

        /* Null Island is where four tiles meet; the city's name stands on
         * the south-east one's corner. */
        tile_of(0.0001, -0.0001, 11, &x, &y);
        check(tile(&a, 11, x, y, &t) == MVT_OK, "zoom 11's tile over Null Island did not decode");
        check(named(&t, "Port Alder") != NULL && named(&t, "Port Alder")->layer == L_PLACE
              && strcmp(named(&t, "Port Alder")->klass, "city") == 0,
              "zoom 11 does not name Port Alder as a city");
        check(count(&t, L_BUILDING, NULL) == 0 && count(&t, L_WATER, NULL) > 0
              && count(&t, L_TRANSPORTATION, "motorway") > 0,
              "zoom 11 has buildings, or lacks the bay or the Ring Road");
        check(t.extent == 4096, "a tile's extent is not 4096");
        mvt_free(&t);
    }

    /* 4. Zoom 16, where Lantern Street Market is: buildings, streets, the
     * market, each part of each feature inside the tile and its buffer. */
    {
        uint32_t x, y;
        double c = cos(-8 * M_PI / 180), s = sin(-8 * M_PI / 180);
        double mx = 20 * c - (-330) * s, my = 20 * s + (-330) * c;   /* lean(20, -330) */
        double lon = mx / 6378137.0 * 180.0 / M_PI;
        double lat = (2 * atan(exp(my / 6378137.0)) - M_PI / 2) * 180.0 / M_PI;
        const struct mvt_feature *m;
        int inside = 1, clockwise = 1;

        tile_of(lon, lat, 16, &x, &y);
        check(tile(&a, 16, x, y, &t) == MVT_OK, "zoom 16's tile at the market did not decode");
        m = named(&t, "Lantern Street Market");
        check(m != NULL && m->layer == L_POI && m->type == MVT_POINT
              && strcmp(m->klass, "marketplace") == 0 && m->rank == 1,
              "Lantern Street Market is not a marketplace point of interest at zoom 16");
        snprintf(said, sizeof said, "zoom 16 has %zu buildings and %zu primary roads",
                 count(&t, L_BUILDING, NULL), count(&t, L_TRANSPORTATION, "primary"));
        check(count(&t, L_BUILDING, NULL) > 4 && count(&t, L_TRANSPORTATION, "primary") >= 1, said);

        for (size_t i = 0; i < t.nfeatures; i++) {
            const struct mvt_feature *ft = &t.features[i];

            for (uint32_t pi = 0; pi < ft->nparts; pi++) {
                const struct mvt_part *pt = &t.parts[ft->part0 + pi];
                double area = 0;

                for (uint32_t k = 0; k < pt->count; k++) {
                    float px = t.xy[2 * (pt->first + k)], py = t.xy[2 * (pt->first + k) + 1];
                    uint32_t j = (k + 1) % pt->count;

                    if (px < -64 || px > 4096 + 64 || py < -64 || py > 4096 + 64) inside = 0;

                    area += (double)px * t.xy[2 * (pt->first + j) + 1]
                          - (double)t.xy[2 * (pt->first + j)] * py;
                }

                /* The outer ring of every building clockwise on the screen:
                 * positive by the specification's formula, y down. */
                if (ft->layer == L_BUILDING && pi == 0 && area <= 0) clockwise = 0;
            }
        }

        check(inside, "a point of zoom 16's tile lies outside the tile and its buffer");
        check(clockwise, "a building's outer ring is not wound clockwise");
        mvt_free(&t);
    }

    /* 5. Drawn: zoom 16 at the market, 512 pixels a side, in a style of
     * four rules - buildings grey, minor streets white, main roads yellow,
     * water blue - over a ground of black. Each colour where it should be,
     * a clip keeping half the pixels untouched, and the same pixels twice. */
    {
        enum { S = 512 };
        static uint32_t px[S][S], again[S][S];
        struct map_rule rules[4] = { 0 };
        struct map_style style = { 0x000000, rules, 4, 1 };
        struct map_matches m = { 0 };
        struct gfx_path path = { 0 };
        uint32_t x, y;
        double c = cos(-8 * M_PI / 180), sn = sin(-8 * M_PI / 180);
        double mx = 20 * c - (-330) * sn, my = 20 * sn + (-330) * c;
        double lon = mx / 6378137.0 * 180.0 / M_PI;
        double lat = (2 * atan(exp(my / 6378137.0)) - M_PI / 2) * 180.0 / M_PI;
        long grey = 0, white = 0, yellow = 0, other = 0;
        int drew;

        rules[0].layer = L_WATER, rules[0].kind = MAP_FILL, rules[0].colour = 0x0000ff;
        rules[0].maxzoom = 99;
        rules[1].layer = L_BUILDING, rules[1].kind = MAP_FILL, rules[1].colour = 0x808080;
        rules[1].minzoom = 14, rules[1].maxzoom = 99;
        rules[2].layer = L_TRANSPORTATION, rules[2].kind = MAP_LINE, rules[2].colour = 0xffffff;
        strcpy(rules[2].classes, "minor,service");
        rules[2].maxzoom = 99, rules[2].nstops = 2;
        rules[2].stops[0] = 14, rules[2].stops[1] = 2, rules[2].stops[2] = 18, rules[2].stops[3] = 18;
        rules[3].layer = L_TRANSPORTATION, rules[3].kind = MAP_LINE, rules[3].colour = 0xffff00;
        strcpy(rules[3].classes, "primary,secondary");
        rules[3].maxzoom = 99, rules[3].nstops = 1;
        rules[3].stops[0] = 0, rules[3].stops[1] = 10;

        check(map_width(&rules[2], 16) == 10.0f && map_width(&rules[2], 10) == 2.0f
              && map_width(&rules[2], 20) == 18.0f, "a rule's width does not follow its stops");

        tile_of(lon, lat, 16, &x, &y);
        tile(&a, 16, x, y, &t);

        drew = map_draw(&t, &m, &style, &path, &px[0][0], S * 4, S, S, 0, 0, S, 16.0f,
                        0, 0, S, S);
        snprintf(said, sizeof said, "%d of 4 rules drew at the market", drew);
        check(drew >= 3, said);

        for (int yy = 0; yy < S; yy++)
            for (int xx = 0; xx < S; xx++) {
                uint32_t v = px[yy][xx] & 0xffffff;

                if (v == 0x808080) grey++;
                else if (v == 0xffffff) white++;
                else if (v == 0xffff00) yellow++;
                else if (v != 0) other++;
            }

        snprintf(said, sizeof said, "the market's tile drew %ld building, %ld street and %ld "
                 "main road pixels, and %ld of edges between", grey, white, yellow, other);
        check(grey > 5000 && white > 2000 && yellow > 1000, said);

        /* Again, through the matches kept from the first time. */
        memcpy(again, px, sizeof px);
        memset(px, 0, sizeof px);
        map_draw(&t, &m, &style, &path, &px[0][0], S * 4, S, S, 0, 0, S, 16.0f, 0, 0, S, S);
        check(memcmp(px, again, sizeof px) == 0, "the same tile drew different pixels twice");

        /* Clipped to its left half: the right half untouched. */
        memset(px, 0, sizeof px);
        map_draw(&t, &m, &style, &path, &px[0][0], S * 4, S, S, 0, 0, S, 16.0f, 0, 0, S / 2, S);
        {
            int right = 0, left = 0;

            for (int yy = 0; yy < S; yy++)
                for (int xx = 0; xx < S; xx++) {
                    if (px[yy][xx] && xx >= S / 2) right++;
                    if (px[yy][xx] && xx < S / 2) left++;
                }

            check(right == 0 && left > 1000, "a clip to the left half drew on the right");
        }

        mvt_free(&t);
        map_matches_free(&m);
        gfx_path_free(&path);
    }

    /* 6. What is not there: a tile off the region, a zoom past it. */
    {
        const uint8_t *b;
        size_t n;

        check(pmt_find(&a, 16, 0, 0, &b, &n) == PMT_NOT_FOUND, "a tile off the region was found");
        check(pmt_find(&a, 17, 65536, 65536, &b, &n) == PMT_NOT_FOUND, "a zoom past the region was found");
    }

    pmt_close(&a);

    /* 7. Bad bytes are refused, never followed: not PMTiles, cut short. */
    {
        uint8_t junk[200];
        struct pmtiles b;

        memset(junk, 0x5a, sizeof junk);
        check(pmt_open(&b, junk, sizeof junk, &work) == PMT_NOT_PMTILES, "junk opened as an archive");
        pmt_close(&b);
        check(pmt_open(&b, data, 200, &work) != PMT_OK, "an archive cut short opened");
        pmt_close(&b);
        check(mvt_decode(&t, junk, sizeof junk) == MVT_DAMAGED, "junk decoded as a vector tile");
        check(mvt_decode(&t, data + 127, 3) != MVT_OK || t.nfeatures == 0,
              "three bytes decoded into features");
        mvt_free(&t);
    }

    free(data);

    /* 8. A place's name drawn in the Latin alphabet: English, else its
     * Latin spelling, else its own - whichever order the tags come in. */
    {
        struct pbw layer = { .n = 0 }, tile = { .n = 0 };
        struct pbw v;
        static const char *keys[] = { "name", "name:latin", "name:en", "class" };
        static const char *values[] = { "\xce\x95\xce\xbb\xce\xbb\xce\xac\xce\xb4\xce\xb1",
                                         "Ellada", "Greece", "country", "Lantern Port" };
        static const uint8_t own_latin_en[] = { 0, 0, 1, 1, 2, 2, 3, 3 };
        static const uint8_t en_first[]     = { 2, 2, 0, 0, 3, 3 };
        static const uint8_t latin_own[]    = { 1, 1, 0, 0 };
        static const uint8_t own_only[]     = { 0, 4 };

        pbw_varint(&layer, 15 << 3);
        pbw_varint(&layer, 2);
        pbw_bytes(&layer, 1, "place", 5);
        pbw_point(&layer, own_latin_en, sizeof own_latin_en);
        pbw_point(&layer, en_first, sizeof en_first);
        pbw_point(&layer, latin_own, sizeof latin_own);
        pbw_point(&layer, own_only, sizeof own_only);

        for (size_t i = 0; i < 4; i++) pbw_bytes(&layer, 3, keys[i], strlen(keys[i]));

        for (size_t i = 0; i < 5; i++) {
            v.n = 0;
            pbw_bytes(&v, 1, values[i], strlen(values[i]));
            pbw_bytes(&layer, 4, v.b, v.n);
        }

        pbw_varint(&layer, 5 << 3);
        pbw_varint(&layer, 4096);
        pbw_bytes(&tile, 3, layer.b, layer.n);

        int r = mvt_decode(&t, tile.b, tile.n);

        check(r == MVT_OK && t.nfeatures == 4, "the tile of four named places did not decode");

        if (r == MVT_OK && t.nfeatures == 4) {
            check(t.features[0].name && strcmp(t.features[0].name, "Greece") == 0,
                  "a place named in its own script, in Latin and in English was not named in English");
            check(t.features[1].name && strcmp(t.features[1].name, "Greece") == 0,
                  "English given before the place's own name was replaced by it");
            check(t.features[2].name && strcmp(t.features[2].name, "Ellada") == 0,
                  "a Latin spelling given before the place's own name was replaced by it");
            check(t.features[3].name && strcmp(t.features[3].name, "Lantern Port") == 0,
                  "a place with only its own name lost it");
        }

        mvt_free(&t);
    }

    if (fails == 0) {
        printf("PASS: %d checks on the Map Kit's reading (the specification's tile numbers, "
               "Port Alder's header and directory, its zoom 11 and zoom 16 tiles decoded, "
               "zoom 16 drawn in a style, what is not there, bad bytes refused, a place named in "
               "English, else in Latin, else in its own script)\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on the Map Kit's reading\n", fails, checks + fails);
    return 1;
}
