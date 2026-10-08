/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A PMTiles archive, read (`docs/maps.md` M2): version 3's header, its
 * directories, and a tile found by its zoom, column and row. `pmtiles.c`
 * is the argument; this is the shape the Map Kit and the host test
 * (`tools/test_map.c`) compile against.
 *
 * The archive is bytes the caller holds for as long as it is open - the
 * Map Kit keeps the Lua string it was read into. Nothing here reads a
 * file: the bytes come from whoever has them.
 */

#ifndef KOSMOS_MAP_PMTILES_H
#define KOSMOS_MAP_PMTILES_H

#include <stddef.h>
#include <stdint.h>

/* What opening or finding came to. */
enum {
    PMT_OK         =  0,
    PMT_NOT_FOUND  = -1,        /* no such tile in the archive            */
    PMT_NOT_PMTILES = -2,       /* the first bytes are not PMTiles 3      */
    PMT_DAMAGED    = -3,        /* a length or an offset past the end      */
    PMT_UNSUPPORTED = -4,       /* compressed other than none or gzip      */
    PMT_NO_MEMORY  = -5
};

/* One run of a directory: tiles `id` .. `id + run - 1` all at `offset`,
 * `length` bytes; a run of 0 is a leaf directory there instead. */
struct pmt_entry {
    uint64_t id;
    uint64_t offset;
    uint32_t length;
    uint32_t run;
};

struct pmtiles {
    const uint8_t *data;
    size_t         len;

    /* The header, as it says. */
    uint64_t root_off, root_len, meta_off, meta_len;
    uint64_t leaf_off, leaf_len, data_off, data_len;
    uint8_t  internal_gzip;         /* directories gzipped */
    uint8_t  tile_gzip;             /* tiles gzipped */
    uint8_t  tile_type;             /* 1: a Mapbox Vector Tile */
    uint8_t  min_zoom, max_zoom, centre_zoom;
    int32_t  min_lon_e7, min_lat_e7, max_lon_e7, max_lat_e7;
    int32_t  centre_lon_e7, centre_lat_e7;

    /* The root directory, decoded once; and the last leaf asked for. */
    struct pmt_entry *root;
    size_t            nroot;
    struct pmt_entry *leaf;
    size_t            nleaf;
    uint64_t          leaf_at;      /* which one `leaf` is; ~0 for none */

    void *gunzip;                   /* the inflater's work, the caller's */
};

/* The tile number PMTiles gives (z, x, y): every tile of the zooms before
 * it, then the place of (x, y) along the Hilbert curve at `z`. */
uint64_t pmt_tile_id(unsigned z, uint32_t x, uint32_t y);

/* Opened over `len` bytes at `data`, the root directory decoded. `gunzip`
 * is a `struct gunzip_work` (`compress/gzip.h`) the caller keeps. */
int  pmt_open(struct pmtiles *a, const uint8_t *data, size_t len, void *gunzip);
void pmt_close(struct pmtiles *a);

/* Tile (z, x, y): `*bytes`, `*n` - into the archive, still gzipped when
 * `a->tile_gzip` says so. */
int  pmt_find(struct pmtiles *a, unsigned z, uint32_t x, uint32_t y,
              const uint8_t **bytes, size_t *n);

/* The sentence for a result. */
const char *pmt_said(int result);

#endif
