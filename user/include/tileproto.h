/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `/Tiles`: the map's tiles fetched from the network (`user/servers/tiles.c`,
 * `docs/maps.md` M6d) - the shapes both sides compile against, and `maps.lua`
 * packs with `string.pack` to the format beside each.
 *
 * Asked by an application that declares `kosmos: needs tiles`. Every
 * request is answered at once: what is wanted is queued, what has come is
 * said, and the fetching goes on between the server's answers. The tiles
 * themselves never travel in a message - each is written into the cache,
 * `/Home/Cache/Maps/<source>/<z>/<x>-<y>.pbf`, and read from there.
 */

#ifndef KOSMOS_TILEPROTO_H
#define KOSMOS_TILEPROTO_H

#include <stdint.h>

/* Where tiles come from: a URL with {z}, {x} and {y} in it, or a TileJSON's
 * URL (OpenFreeMap's `https://tiles.openfreemap.org/planet`), whose first
 * tile URL is used. Answered with the cache folder for that source. */
#define TILES_OP_SOURCE   1u

/* These tiles are wanted, the first most: the list replaces the last one.
 * Answered at once. */
#define TILES_OP_WANT     2u

/* Which tiles have come since `since` - each written to the cache, or
 * known not to exist (a 404: the sea, beyond the world's data). */
#define TILES_OP_ARRIVED  3u

#define TILES_OK          0u
#define TILES_ERR_BAD_OP  1u
#define TILES_ERR_SOURCE  2u    /* no source yet, or one this cannot read */

#define TILES_MAX       64u
#define TILES_URL_MAX  512u

struct tiles_id {
    uint32_t z, x, y;
};

/* 16 + 768 = 784 bytes: "<I4I4I4I4" and 192 "I4", or a URL in c768. */
struct tiles_request {
    uint32_t op;
    uint32_t count;             /* WANT: tiles in `u.tiles` */
    uint32_t since;             /* ARRIVED */
    uint32_t reserved;
    union {
        struct tiles_id tiles[TILES_MAX];
        char            url[TILES_MAX * sizeof(struct tiles_id)];
    } u;
};

/*
 * 24 + 64 + 128 + 768 = 984 bytes: "<I4I4I4I4I4I4c64c128" and 192 "I4".
 * `seq` is the next `since`; `outstanding` how many are still to fetch;
 * `failed` how many fetches have failed since the server started, `why`
 * the last one's reason; `cache` the source's folder.
 */
struct tiles_reply {
    uint32_t status;
    uint32_t count;
    uint32_t seq;
    uint32_t outstanding;
    uint32_t failed;
    uint32_t reserved;
    char     cache[64];
    char     why[128];
    struct tiles_id tiles[TILES_MAX];
};

_Static_assert(sizeof(struct tiles_request) == 784, "tiles_request is 784 bytes");
_Static_assert(sizeof(struct tiles_reply) == 984, "tiles_reply is 984 bytes");

#endif
