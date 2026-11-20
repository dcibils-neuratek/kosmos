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
 * `/Home/Cache/Maps/<source>/<z>/<x>-<y>.<format>`, and read from there.
 *
 * **Several sources at once** (`docs/flight.md` F1): Flight wants satellite
 * imagery and elevation together, and Maps, open beside it, its own vector
 * tiles. A `SOURCE` answers with a source's number, and `WANT` and
 * `ARRIVED` name the source they are about; each source has its own
 * connection, queue, arrivals and cache folder, so one never waits behind
 * another. The same address and format asked for again is the same source.
 */

#ifndef KOSMOS_TILEPROTO_H
#define KOSMOS_TILEPROTO_H

#include <stdint.h>

/* Where tiles come from: a URL with {z}, {x} and {y} in it, or a TileJSON's
 * URL (OpenFreeMap's `https://tiles.openfreemap.org/planet`), whose first
 * tile URL is used; and `format`, the extension a tile is kept under -
 * "pbf", "png", "jpg" - since a picture is decoded by what its name says
 * and Esri's imagery addresses name none. Answered with the source's
 * number in `source` and its cache folder in `cache`. */
#define TILES_OP_SOURCE   1u

/* These tiles of `source` are wanted, the first most: the list replaces
 * that source's last one. Answered at once. */
#define TILES_OP_WANT     2u

/* Which tiles of `source` have come since `since` - each written to the
 * cache, or known not to exist (a 404: the sea, beyond the world's data). */
#define TILES_OP_ARRIVED  3u

/* A place looked up by name (`docs/maps.md` M6e): `u.url` the words, as a
 * person typed them. Answered at once with the search's number in `seq`;
 * the answer - the finder's JSON, as it sent it - is written to
 * `/Home/Cache/Maps/places/<number>.json`, and `ARRIVED` says when, in
 * `found`. One search at a time and one a second at most, as Nominatim
 * asks: a search not yet begun is replaced by the next. */
#define TILES_OP_FIND     4u

/* Where places are looked up: `u.url` an address to which "?q=..." is
 * added - Nominatim's `https://nominatim.openstreetmap.org/search` unless
 * told otherwise. */
#define TILES_OP_FINDER   5u

#define TILES_OK          0u
#define TILES_ERR_BAD_OP  1u
#define TILES_ERR_SOURCE  2u    /* no such source, or one this cannot read */

#define TILES_MAX       64u
#define TILES_URL_MAX  512u
#define TILES_FORMAT_MAX 8u     /* "pbf", "png", "jpg", "webp": letters and digits */

struct tiles_id {
    uint32_t z, x, y;
};

/* 16 + 8 + 768 = 792 bytes: "<I4I4I4I4c8" and 192 "I4", or a URL in c768. */
struct tiles_request {
    uint32_t op;
    uint32_t count;             /* WANT: tiles in `u.tiles` */
    uint32_t since;             /* ARRIVED */
    uint32_t source;            /* WANT, ARRIVED: the number SOURCE gave */
    char     format[TILES_FORMAT_MAX];  /* SOURCE: the tiles' extension */
    union {
        struct tiles_id tiles[TILES_MAX];
        char            url[TILES_MAX * sizeof(struct tiles_id)];
    } u;
};

/*
 * 32 + 64 + 128 + 768 = 992 bytes: "<I4I4I4I4I4I4I4I4c64c128" and 192 "I4".
 * `source` the source asked about, or the one SOURCE made or found;
 * `seq` is the next `since` (for `find`, the search's number);
 * `outstanding` how many are still to fetch; `failed` how many fetches
 * have failed since the server started, `why` the last one's reason;
 * `found` the last search answered, and `found_status` how: the finder's
 * HTTP status, or 0 when it could not be asked; `cache` the source's folder.
 */
struct tiles_reply {
    uint32_t status;
    uint32_t count;
    uint32_t seq;
    uint32_t outstanding;
    uint32_t failed;
    uint32_t found;
    uint32_t found_status;
    uint32_t source;
    char     cache[64];
    char     why[128];
    struct tiles_id tiles[TILES_MAX];
};

_Static_assert(sizeof(struct tiles_request) == 792, "tiles_request is 792 bytes");
_Static_assert(sizeof(struct tiles_reply) == 992, "tiles_reply is 992 bytes");

#endif
