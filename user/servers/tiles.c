/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`/Tiles`: the map's tiles, fetched from the network** (`tileproto.h`,
 * `docs/maps.md` M6d) - agreed by Diego on 8 October: OpenFreeMap as the
 * source, a server in C that every application with a map can ask, and the
 * tiles kept in `/Home/Cache/Maps`.
 *
 * **It never makes its callers wait.** Every request is answered at once:
 * a `want` replaces what is queued, an `arrived` says what has come, and
 * the fetching goes on between answers - the server receives with a short
 * timeout while anything is outstanding and steps the fetch each time it
 * comes back. The one wait is a name looked up, once a source, a second at
 * most (`net_client_resolve`).
 *
 * **The tiles never travel in a message.** Each is written into the cache
 * through this server's own door to the disk, which reaches
 * `/Home/Cache/Maps` and nothing else (`diskfs.c`'s doors); a caller reads
 * it from there, as it reads anything on `/Home`.
 *
 * **What it is made of is the system's**: the Network Kit's C client
 * (`netclient.h`) for TCP, the TLS core (`tls_core.h`) for HTTPS, `httpc`
 * for HTTP - all three written for this, and all three anyone's.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "diskproto.h"
#include "netproto.h"
#include "../init/say.h"
#include "tileproto.h"
#include "kits/network/httpc.h"
#include "kits/network/netclient.h"
#include "kits/tls/tls_core.h"

void tiles_server(long endpoint, long net_ep, long disk_ep, long console_ep);

#define CACHE_ROOT "/Home/Cache/Maps"
#define TILE_MOST  (4u * 1024u * 1024u)     /* a tile larger is refused */
#define ARRIVED_KEPT 512u                    /* what `arrived` can look back on */

static long console = -1;
static long disk = -1;

/* ------------------------------------------------------------------------
 * The source: where tiles come from.
 * --------------------------------------------------------------------- */

static struct {
    bool     set;
    bool     ready;             /* the template is known */
    char     template_path[512];    /* "/planet/2025.../{z}/{x}/{y}.pbf" */
    char     tilejson_path[512];    /* when the source was a TileJSON */
    char     key[17];           /* the cache folder's name, from the URL */
    bool     paused;            /* would not answer: tried again at the next ask */
} src;

static uint32_t failed;
static char why[128] = "";

static void note(const char *text)
{
    snprintf(why, sizeof why, "%s", text);
}

/* A name for the cache folder: the source URL's FNV-1a, sixteen hex digits. */
static void make_key(const char *url)
{
    uint64_t h = 1469598103934665603ull;

    for (const char *p = url; *p; p++) {
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    }

    snprintf(src.key, sizeof src.key, "%016llx", (unsigned long long)h);
}

/* "https://host[:port]/path" taken apart. */
static bool parse_url(const char *url, bool *tls, char *host, size_t host_room,
                      uint16_t *port, char *path, size_t path_room)
{
    const char *p, *slash, *colon;
    size_t hn;

    if (strncmp(url, "https://", 8) == 0) {
        *tls = true, p = url + 8, *port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        *tls = false, p = url + 7, *port = 80;
    } else {
        return false;
    }

    slash = strchr(p, '/');
    hn = slash ? (size_t)(slash - p) : strlen(p);
    colon = memchr(p, ':', hn);

    if (colon) {
        long n = strtol(colon + 1, NULL, 10);

        if (n <= 0 || n > 65535) return false;

        *port = (uint16_t)n;
        hn = (size_t)(colon - p);
    }

    if (hn == 0 || hn >= host_room) return false;

    memcpy(host, p, hn);
    host[hn] = '\0';
    snprintf(path, path_room, "%s", slash ? slash : "/");
    return true;
}

/* ------------------------------------------------------------------------
 * The disk, through this server's door.
 * --------------------------------------------------------------------- */

static struct message dmsg, drep;

static uint32_t disk_ask(uint32_t op, const char *path, uint32_t flags, uint64_t bytes,
                         long region)
{
    struct disk_request *rq = (struct disk_request *)dmsg.data;
    const struct disk_reply *rp = (const struct disk_reply *)drep.data;

    if (disk < 0) return DISK_ERR_NO_DISK;

    memset(&dmsg, 0, sizeof dmsg);
    memset(&drep, 0, sizeof drep);
    dmsg.length = sizeof *rq;
    dmsg.cap_plus_one = region >= 0 ? (uint32_t)region + 1 : 0;
    rq->op = op;
    rq->flags = flags;
    rq->bytes = bytes;
    snprintf(rq->path, sizeof rq->path, "%s", path);

    if (kosmos_call(disk, &dmsg, &drep) != 0 || drep.length < sizeof *rp) {
        return DISK_ERR_NO_DISK;
    }

    return rp->error;
}

/* The folders down to `path`, made where they are not (an error from one
 * that is there already is not one). */
static void folders(const char *path)
{
    char at[DISK_PATH_MAX];
    size_t n = strlen(path);

    if (n >= sizeof at) return;

    for (size_t i = 1; i <= n; i++) {
        if (path[i] == '/' || path[i] == '\0') {
            memcpy(at, path, i);
            at[i] = '\0';
            (void)disk_ask(DISK_OP_MKDIR, at, 0, 0, -1);
        }
    }
}

/* One region, kept and grown, that each tile is written from. */
static long region_cap = -1;
static uint8_t *region_at;
static size_t region_pages;

static bool region_room(size_t bytes)
{
    size_t pages = (bytes + 4095u) / 4096u;
    long at;

    if (pages == 0) pages = 1;
    if (region_cap >= 0 && pages <= region_pages) return true;

    if (region_cap >= 0) {
        kosmos_share_unmap((unsigned long)(uintptr_t)region_at, region_pages);
        kosmos_cap_drop(region_cap);
        region_cap = -1;
    }

    if ((region_cap = kosmos_mem_create(pages)) < 0) return false;

    if ((at = kosmos_mem_map(region_cap)) < 0) {
        kosmos_cap_drop(region_cap);
        region_cap = -1;
        return false;
    }

    region_at = (uint8_t *)(uintptr_t)at;
    region_pages = pages;
    return true;
}

/* `n` bytes into the cache at `dir`/`name`, the folders made first. */
static bool cache_put(const char *dir, const char *name, const uint8_t *bytes, size_t n)
{
    char path[DISK_PATH_MAX];

    snprintf(path, sizeof path, "%s/%s", dir, name);
    folders(dir);

    if (!region_room(n)) {
        note("no memory for a tile on its way to the cache");
        return false;
    }

    memcpy(region_at, bytes, n);

    uint32_t e = disk_ask(DISK_OP_WRITE, path, DISK_REGION, (uint64_t)n, region_cap);

    if (e != 0) {
        char text[96];

        snprintf(text, sizeof text, "a tile would not go into the cache: disk error %u", (unsigned)e);
        note(text);
        return false;
    }

    return true;
}

static bool cache_write(const struct tiles_id *t, const uint8_t *bytes, size_t n)
{
    char dir[DISK_PATH_MAX], name[48];

    snprintf(dir, sizeof dir, CACHE_ROOT "/%s/%u", src.key, (unsigned)t->z);
    snprintf(name, sizeof name, "%u-%u.pbf", (unsigned)t->x, (unsigned)t->y);
    return cache_put(dir, name, bytes, n);
}

/* ------------------------------------------------------------------------
 * A connection: TCP, and TLS over it when the host is https - one for the
 * tiles' source and one for the place finder, which is another host.
 * --------------------------------------------------------------------- */

struct link {
    bool     tls;               /* the host is https */
    char     host[256];
    uint16_t port;
    uint8_t  address[4];
    bool     resolved;
    size_t   most;              /* a reply's body, at most */

    struct net_conn conn;
    struct tls_conn secure;
    bool     secure_on;
    struct httpc http;
    bool     ready;             /* `http` is over an open connection */
};

static size_t tls_net_write(void *user, const unsigned char *p, size_t n)
{
    long w = net_client_write(user, p, n);

    return w > 0 ? (size_t)w : 0;
}

static size_t tls_net_read(void *user, unsigned char *buf, size_t max)
{
    long r = net_client_read(user, buf, max);

    return r > 0 ? (size_t)r : 0;
}

static long plain_write(void *user, const void *p, size_t n)
{
    return net_client_write(&((struct link *)user)->conn, p, n);
}

static long plain_read(void *user, void *buf, size_t max)
{
    return net_client_read(&((struct link *)user)->conn, buf, max);
}

static long secure_write(void *user, const void *p, size_t n)
{
    struct link *l = user;
    size_t took;

    if (tls_core_state(&l->secure, NULL, NULL, NULL) == TLS_CLOSED) return -1;

    /* Sent now: the engine otherwise holds a request until a record fills,
     * which a GET never does, and the far end hangs up waiting for it -
     * "the connection closed before the reply", every fifteen seconds, on
     * the M700 against OpenFreeMap (8 October). */
    took = tls_core_write(&l->secure, p, n);

    if (took > 0) tls_core_flush(&l->secure);

    return (long)took;
}

static long secure_read(void *user, void *buf, size_t max)
{
    struct link *l = user;
    size_t n = tls_core_read(&l->secure, buf, max);

    if (n == 0 && (tls_core_state(&l->secure, NULL, NULL, NULL) == TLS_CLOSED
                   || net_client_over(&l->conn))) {
        return -1;
    }

    return (long)n;
}

static void link_reset(struct link *l, size_t most)
{
    memset(l, 0, sizeof *l);
    l->conn.region = -1;
    l->most = most;
}

static void link_hang_up(struct link *l)
{
    if (l->secure_on) {
        tls_core_free(&l->secure);
        l->secure_on = false;
    }

    if (l->ready) {
        httpc_free(&l->http);
        l->ready = false;
    }

    net_client_close(&l->conn);
}

/* A new host: what was open to the old one let go. */
static void link_aim(struct link *l, bool tls, const char *host, uint16_t port)
{
    link_hang_up(l);
    l->tls = tls;
    snprintf(l->host, sizeof l->host, "%s", host);
    l->port = port;
    l->resolved = false;
}

/* A connection to the host, TLS begun on it if it is https. */
static bool link_dial(struct link *l)
{
    struct httpc_io io;

    if (!l->resolved) {
        uint32_t s = net_client_resolve(l->host, l->address, 250);

        if (s != NET_OK) {
            note("the source's name was not found");
            return false;
        }

        l->resolved = true;
    }

    if (net_client_connect(&l->conn, l->address, l->port) != NET_OK) {
        note("the source would not connect");
        return false;
    }

    if (l->tls) {
        struct tls_io t = { &l->conn, tls_net_write, tls_net_read };
        const char *whyt = NULL;

        if (tls_core_open(&l->secure, l->host, NULL, NULL, 0, 0, t, &whyt) != 0) {
            note(whyt ? whyt : "TLS would not begin");
            net_client_close(&l->conn);
            return false;
        }

        l->secure_on = true;
        io = (struct httpc_io){ l, secure_write, secure_read };
    } else {
        io = (struct httpc_io){ l, plain_write, plain_read };
    }

    httpc_init(&l->http, io, l->host, l->most);
    l->ready = true;
    return true;
}

/* Whether a fetch on it is waiting for the network. */
static bool link_waiting(const struct link *l)
{
    return l->ready && (l->http.state == HTTPC_SENDING || l->http.state == HTTPC_HEAD
                        || l->http.state == HTTPC_BODY);
}

static struct link tl;          /* the tiles' source */

/* ------------------------------------------------------------------------
 * What is wanted, what is under way, what has come.
 * --------------------------------------------------------------------- */

static struct tiles_id wanted[TILES_MAX];
static uint32_t wanted_count;

static struct {
    bool            busy;
    bool            tilejson;   /* fetching the source's TileJSON */
    struct tiles_id tile;
} now;

static struct tiles_id arrived[ARRIVED_KEPT];
static uint32_t arrived_seq;            /* how many have ever arrived */

static void came(const struct tiles_id *t)
{
    arrived[arrived_seq % ARRIVED_KEPT] = *t;
    arrived_seq++;
}

static bool same(const struct tiles_id *a, const struct tiles_id *b)
{
    return a->z == b->z && a->x == b->x && a->y == b->y;
}

/* Whether a tile came lately, so is in the cache already: the application
 * asks for what it has not yet heard came, and hears only when it asks, so
 * a tile can be asked for again in between - fetched twice, without this. */
static bool came_lately(const struct tiles_id *t)
{
    uint32_t kept = arrived_seq < ARRIVED_KEPT ? arrived_seq : ARRIVED_KEPT;

    for (uint32_t k = 1; k <= kept; k++) {
        if (same(&arrived[(arrived_seq - k) % ARRIVED_KEPT], t)) return true;
    }

    return false;
}

/* `{z}`, `{x}` and `{y}` filled in. */
static bool tile_path(const struct tiles_id *t, char *out, size_t room)
{
    const char *p = src.template_path;
    size_t o = 0;

    while (*p && o + 12 < room) {
        if (strncmp(p, "{z}", 3) == 0) {
            o += (size_t)snprintf(out + o, room - o, "%u", (unsigned)t->z), p += 3;
        } else if (strncmp(p, "{x}", 3) == 0) {
            o += (size_t)snprintf(out + o, room - o, "%u", (unsigned)t->x), p += 3;
        } else if (strncmp(p, "{y}", 3) == 0) {
            o += (size_t)snprintf(out + o, room - o, "%u", (unsigned)t->y), p += 3;
        } else {
            out[o++] = *p++;
        }
    }

    out[o] = '\0';
    return *p == '\0';
}

/*
 * The TileJSON's first tile URL: `"tiles": [ "https://..." ]`, its `\/`
 * unescaped. A reader of the one field this needs, not of JSON.
 */
static bool tilejson_template(const uint8_t *body, size_t n)
{
    char url[512];
    const char *at = NULL;
    size_t o = 0;

    for (size_t i = 0; i + 7 < n; i++) {
        if (memcmp(body + i, "\"tiles\"", 7) == 0) {
            at = (const char *)body + i + 7;
            break;
        }
    }

    if (at == NULL) return false;

    while (at < (const char *)body + n && *at != '"') at++;      /* the URL's quote */

    for (at++; at < (const char *)body + n && *at != '"' && o + 1 < sizeof url; at++) {
        if (*at == '\\' && at + 1 < (const char *)body + n) at++;
        url[o++] = *at;
    }

    url[o] = '\0';

    {
        bool tls_;
        char host[256];
        uint16_t port;

        if (!parse_url(url, &tls_, host, sizeof host, &port, src.template_path,
                       sizeof src.template_path)) {
            return false;
        }

        /* The tiles on another host than the TileJSON's: not followed - the
         * connection is kept to one host. */
        if (strcmp(host, tl.host) != 0 || port != tl.port || tls_ != tl.tls) {
            return false;
        }
    }

    return strstr(src.template_path, "{z}") != NULL;
}

/* The next fetch begun, if there is one to begin. */
static void begin(void)
{
    char path[600];

    if (now.busy || !src.set) return;

    if (!tl.ready && !link_dial(&tl)) {
        failed++;
        wanted_count = 0;           /* the source is not answering: let go, */
        src.paused = true;          /* until somebody asks again */
        return;
    }

    if (!src.ready) {
        if (httpc_get(&tl.http, src.tilejson_path) != 0) return;

        now.busy = true;
        now.tilejson = true;
        return;
    }

    if (wanted_count == 0) return;

    now.tile = wanted[0];
    memmove(wanted, wanted + 1, (wanted_count - 1) * sizeof wanted[0]);
    wanted_count--;

    if (!tile_path(&now.tile, path, sizeof path) || httpc_get(&tl.http, path) != 0) {
        failed++;
        note("a tile's address is too long");
        return;
    }

    now.busy = true;
    now.tilejson = false;
}

/* The fetch under way, moved on; what it came to, kept. */
static void step(void)
{
    int r;

    begin();

    if (!now.busy) return;

    r = httpc_step(&tl.http);

    if (r == HTTPC_DONE) {
        now.busy = false;

        if (now.tilejson) {
            if (tl.http.status != 200 || !tilejson_template(tl.http.body, tl.http.body_len)) {
                failed++;
                note("the source's TileJSON names no tiles this can fetch");
                src.set = false;
            } else {
                src.ready = true;
                say(console, "tiles: the source's tiles are at ");
                say(console, src.template_path);
                say(console, "\n");
            }
        } else if (tl.http.status == 200 || tl.http.status == 204 || tl.http.status == 404) {
            /* A tile there is none of - the sea, beyond the data - is kept
             * as an empty file, so it is not asked for again. */
            size_t n = tl.http.status == 200 ? tl.http.body_len : 0;

            if (cache_write(&now.tile, tl.http.body, n)) {
                came(&now.tile);
            } else {
                failed++;
            }
        } else {
            char text[64];

            failed++;
            snprintf(text, sizeof text, "the source answered %d", tl.http.status);
            note(text);
        }

        if (!httpc_reusable(&tl.http)) link_hang_up(&tl);
    } else if (r == HTTPC_FAILED) {
        failed++;
        note(tl.http.why ? tl.http.why : "a fetch failed");
        now.busy = false;
        link_hang_up(&tl);
    }
}

/* ------------------------------------------------------------------------
 * Places, found by name (`docs/maps.md` M6e): Nominatim by default.
 *
 * The answer is not read here. It is JSON from outside, and the system's
 * reader of that is `json.lua`, which cannot overflow a buffer - so it is
 * written into the cache as it came, as a tile is, and the application
 * reads it there. One search at a time, a second apart at the least, as
 * Nominatim's policy asks; a search not yet begun gives way to the next.
 * --------------------------------------------------------------------- */

#define FIND_MOST (256u * 1024u)    /* an answer larger is refused */

static struct {
    struct link link;
    char     base[512];         /* the path "?q=" is added to */
    uint32_t next;              /* the number the next search gets */
    uint32_t wanted;            /* waiting to begin: its number, 0 for none */
    char     words[TILES_URL_MAX + 256];
    uint32_t now;               /* under way */
    uint32_t done, done_status;
    unsigned long began;        /* counter ticks, when the last one began */
    bool     began_once;
} finder;

static unsigned long counter_hz = 62500000ul;

static bool finder_aim(const char *url)
{
    bool tls_;
    char host[256];
    uint16_t port;

    if (!parse_url(url, &tls_, host, sizeof host, &port, finder.base, sizeof finder.base)) {
        return false;
    }

    link_aim(&finder.link, tls_, host, port);
    finder.now = 0;
    return true;
}

/* The words as a query: letters, digits and "-._~" as they are, a space
 * as "+", every other byte - UTF-8's included - as %XX. */
static bool query(char *out, size_t room, const char *base, const char *words)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = (size_t)snprintf(out, room, "%s?q=", base);

    for (const unsigned char *p = (const unsigned char *)words; *p && o + 4 < room; p++) {
        unsigned char c = *p;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%', out[o++] = hex[c >> 4], out[o++] = hex[c & 15];
        }
    }

    out[o] = '\0';

    return o + 64 < room
        && (size_t)snprintf(out + o, room - o, "&format=jsonv2&limit=8&accept-language=en")
           < room - o;
}

static bool finder_work(void)
{
    return finder.now != 0 || finder.wanted != 0;
}

static void finder_answered(uint32_t status)
{
    finder.done = finder.now;
    finder.done_status = status;
    finder.now = 0;
}

static void finder_step(void)
{
    if (finder.now == 0 && finder.wanted != 0) {
        char path[TILES_URL_MAX * 3 + 640];
        unsigned long t = kosmos_ticks();

        if (finder.began_once && t - finder.began < counter_hz) return;     /* a second apart */

        finder.now = finder.wanted;
        finder.wanted = 0;
        finder.began = t;
        finder.began_once = true;

        if (!query(path, sizeof path, finder.base, finder.words)) {
            note("a search too long to ask");
            failed++;
            finder_answered(0);
            return;
        }

        if ((!finder.link.ready && !link_dial(&finder.link))
            || httpc_get(&finder.link.http, path) != 0) {
            failed++;
            link_hang_up(&finder.link);
            finder_answered(0);
            return;
        }
    }

    if (finder.now == 0) return;

    int r = httpc_step(&finder.link.http);

    if (r == HTTPC_DONE) {
        char name[24];
        uint32_t status = (uint32_t)finder.link.http.status;

        snprintf(name, sizeof name, "%u.json", (unsigned)finder.now);

        if (!cache_put(CACHE_ROOT "/places", name, finder.link.http.body,
                       status == 200 ? finder.link.http.body_len : 0)) {
            failed++;
            status = 0;
        }

        if (!httpc_reusable(&finder.link.http)) link_hang_up(&finder.link);

        finder_answered(status);
    } else if (r == HTTPC_FAILED) {
        failed++;
        note(finder.link.http.why ? finder.link.http.why : "a search failed");
        link_hang_up(&finder.link);
        finder_answered(0);
    }
}

/* ------------------------------------------------------------------------
 * The door.
 * --------------------------------------------------------------------- */

static void answer(const struct message *in, struct message *out)
{
    const struct tiles_request *rq = (const struct tiles_request *)in->data;
    struct tiles_reply *rp = (struct tiles_reply *)out->data;

    memset(out, 0, sizeof *out);
    out->length = sizeof *rp;

    if (in->length != sizeof *rq) {
        rp->status = TILES_ERR_BAD_OP;
        return;
    }

    switch (rq->op) {
    case TILES_OP_SOURCE: {
        char url[sizeof rq->u.url + 1];
        char path[512];

        memcpy(url, rq->u.url, sizeof rq->u.url);
        url[sizeof rq->u.url] = '\0';

        bool tls_;
        char host[256];
        uint16_t port;

        if (!parse_url(url, &tls_, host, sizeof host, &port, path, sizeof path)) {
            rp->status = TILES_ERR_SOURCE;
            snprintf(rp->why, sizeof rp->why, "not an http or https address");
            return;
        }

        /* A new source: what was under way for the old one is let go. */
        link_aim(&tl, tls_, host, port);
        now.busy = false;
        wanted_count = 0;
        src.set = true;
        src.paused = false;
        make_key(url);

        if (strstr(path, "{z}") != NULL) {
            snprintf(src.template_path, sizeof src.template_path, "%s", path);
            src.ready = true;
        } else {
            snprintf(src.tilejson_path, sizeof src.tilejson_path, "%s", path);
            src.ready = false;
        }

        say(console, "tiles: the source is ");
        say(console, url);
        say(console, "\n");
        break;
    }

    case TILES_OP_WANT:
        if (rq->count > TILES_MAX) {
            rp->status = TILES_ERR_BAD_OP;
            return;
        }

        /* The new list, less the one already under way. */
        wanted_count = 0;
        src.paused = false;

        for (uint32_t i = 0; i < rq->count; i++) {
            if (rq->u.tiles[i].z > 22) continue;
            if (now.busy && !now.tilejson && same(&rq->u.tiles[i], &now.tile)) continue;
            if (came_lately(&rq->u.tiles[i])) continue;

            wanted[wanted_count++] = rq->u.tiles[i];
        }
        break;

    case TILES_OP_ARRIVED: {
        uint32_t from = rq->since;

        if (arrived_seq - from > ARRIVED_KEPT) from = arrived_seq - ARRIVED_KEPT;

        while (from < arrived_seq && rp->count < TILES_MAX) {
            rp->tiles[rp->count++] = arrived[from % ARRIVED_KEPT];
            from++;
        }

        rp->seq = from;
        break;
    }

    case TILES_OP_FIND: {
        const char *end = memchr(rq->u.url, '\0', sizeof rq->u.url);
        size_t n = end ? (size_t)(end - rq->u.url) : sizeof rq->u.url;

        if (n == 0) {
            rp->status = TILES_ERR_BAD_OP;
            return;
        }

        memcpy(finder.words, rq->u.url, n);
        finder.words[n] = '\0';
        finder.wanted = ++finder.next;
        rp->seq = finder.wanted;
        break;
    }

    case TILES_OP_FINDER: {
        char url[sizeof rq->u.url + 1];

        memcpy(url, rq->u.url, sizeof rq->u.url);
        url[sizeof rq->u.url] = '\0';

        if (!finder_aim(url)) {
            rp->status = TILES_ERR_SOURCE;
            snprintf(rp->why, sizeof rp->why, "not an http or https address");
            return;
        }
        break;
    }

    default:
        rp->status = TILES_ERR_BAD_OP;
        return;
    }

    rp->found = finder.done;
    rp->found_status = finder.done_status;
    rp->outstanding = wanted_count + (now.busy ? 1u : 0u);
    rp->failed = failed;
    snprintf(rp->why, sizeof rp->why, "%s", why);

    if (src.set) snprintf(rp->cache, sizeof rp->cache, CACHE_ROOT "/%s", src.key);
}

void tiles_server(long endpoint, long net_ep, long disk_ep, long console_ep)
{
    static struct message in, out;

    console = console_ep;
    disk = disk_ep;
    net_client_start(net_ep);
    link_reset(&tl, TILE_MOST);
    link_reset(&finder.link, FIND_MOST);
    (void)finder_aim("https://nominatim.openstreetmap.org/search");

    {
        struct sysinfo info;

        memset(&info, 0, sizeof info);

        if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) counter_hz = info.counter_hz;
    }

    folders(CACHE_ROOT);

    for (;;) {
        uint64_t sender = 0;
        bool busy = now.busy || (src.set && !src.paused && (wanted_count > 0 || !src.ready))
                    || finder_work();
        long r = kosmos_receive(endpoint, &in, &sender, 0, busy ? 1ul : 0ul);

        if (r == 0) {
            answer(&in, &out);
            (void)kosmos_reply(sender, &out);
        }

        /* Steps while they finish things - a tile done, the next begun -
         * and back to the door as soon as one is waiting on the network. */
        for (int i = 0; i < 32; i++) {
            bool tiles = now.busy || (src.set && !src.paused && (wanted_count > 0 || !src.ready));

            if (!tiles && !finder_work()) break;

            if (tiles) step();
            if (finder_work()) finder_step();

            if ((!now.busy || link_waiting(&tl))
                && (finder.now == 0 || link_waiting(&finder.link))) {
                break;
            }
        }
    }
}
