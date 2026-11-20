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
#include "diskask.h"
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
static uint32_t finder_failed;          /* searches that failed */
static unsigned long counter_hz = 62500000ul;      /* from sysinfo, at start */

/*
 * **A fetch is given twenty seconds**, then let go: a connection that
 * stalls - a handshake lost, a host that stops answering - would otherwise
 * hold its fetch for good, with nothing said and nothing else fetched. The
 * M700, 8 October: both connections stalled after a boot, the TileJSON and
 * a search waiting with no end, and nothing in the log to say so.
 */
#define FETCH_SECONDS 20ul

static bool too_long(unsigned long since)
{
    return kosmos_ticks() - since > FETCH_SECONDS * counter_hz;
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
    char     why[128];          /* the last failure's reason */
};

static void note(struct link *l, const char *text)
{
    snprintf(l->why, sizeof l->why, "%s", text);
}

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
            note(l, "the source's name was not found");
            return false;
        }

        l->resolved = true;
    }

    if (net_client_connect(&l->conn, l->address, l->port) != NET_OK) {
        note(l, "the source would not connect");
        return false;
    }

    if (l->tls) {
        struct tls_io t = { &l->conn, tls_net_write, tls_net_read };
        const char *whyt = NULL;

        if (tls_core_open(&l->secure, l->host, NULL, NULL, 0, 0, t, &whyt) != 0) {
            note(l, whyt ? whyt : "TLS would not begin");
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


/* ------------------------------------------------------------------------
 * The disk, through this server's door.
 * --------------------------------------------------------------------- */

static struct diskask disk = { .ep = -1 };

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
            (void)diskask(&disk, DISK_OP_MKDIR, at, 0, 0, 0, -1, NULL, 0);
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

/* `n` bytes into the cache at `dir`/`name`, the folders made first; what
 * went wrong said on `l`. */
static bool cache_put(struct link *l, const char *dir, const char *name,
                      const uint8_t *bytes, size_t n)
{
    char path[DISK_PATH_MAX];

    snprintf(path, sizeof path, "%s/%s", dir, name);
    folders(dir);

    if (!region_room(n)) {
        note(l, "no memory for a tile on its way to the cache");
        return false;
    }

    memcpy(region_at, bytes, n);

    uint32_t e = diskask(&disk, DISK_OP_WRITE, path, DISK_REGION, 0, (uint64_t)n, region_cap, NULL, 0);

    if (e != 0) {
        char text[96];

        snprintf(text, sizeof text, "a tile would not go into the cache: disk error %u", (unsigned)e);
        note(l, text);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------------
 * The sources: where tiles come from (`docs/flight.md` F1). As many as are
 * asked for, each made once and kept: a source is an address and a format,
 * and asking for the same one again is answered with the same number - so
 * Maps and Flight, both wanting the same imagery, share its cache and its
 * connection. Each has its own connection, queue and arrivals, so tiles
 * from one never wait behind another's.
 * --------------------------------------------------------------------- */

struct source {
    struct source *next;
    uint32_t number;            /* what callers name it by; from 1 */
    char     url[TILES_URL_MAX + 1];
    char     format[TILES_FORMAT_MAX + 1];
    bool     ready;             /* the template is known */
    bool     paused;            /* would not answer: tried again at the next ask */
    char     template_path[512];    /* "/planet/2025.../{z}/{x}/{y}.pbf" */
    char     tilejson_path[512];    /* when the source was a TileJSON */
    char     key[17];           /* the cache folder's name, from the URL */
    uint32_t failed;            /* fetches that failed */
    struct link link;

    struct tiles_id wanted[TILES_MAX];
    uint32_t wanted_count;

    struct {
        bool            busy;
        bool            tilejson;   /* fetching the source's TileJSON */
        struct tiles_id tile;
        unsigned long   since;      /* counter ticks, when it began */
    } now;

    struct tiles_id arrived[ARRIVED_KEPT];
    uint32_t arrived_seq;           /* how many have ever arrived */
};

static struct source *sources;      /* newest first */
static uint32_t source_count;

static struct source *source_numbered(uint32_t number)
{
    for (struct source *s = sources; s; s = s->next) {
        if (s->number == number) return s;
    }

    return NULL;
}

/* A name for the cache folder: the source URL's FNV-1a, sixteen hex digits -
 * the address alone, so Maps' folder from before there were formats is the
 * folder still. */
static void make_key(struct source *s)
{
    uint64_t h = 1469598103934665603ull;

    for (const char *p = s->url; *p; p++) {
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    }

    snprintf(s->key, sizeof s->key, "%016llx", (unsigned long long)h);
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

/* A format is letters and digits, one to seven of them: it becomes part of
 * a path, so nothing else is let through. */
static bool format_ok(const char *f)
{
    size_t n = 0;

    for (; f[n]; n++) {
        char c = f[n];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }

    return n >= 1 && n < TILES_FORMAT_MAX;
}

/* The source for this address and format: the one there is, or a new one.
 * NULL, and `why` said, when the address cannot be read or memory is out. */
static struct source *source_for(const char *url, const char *format, const char **why)
{
    bool tls_;
    char host[256], path[512];
    uint16_t port;
    struct source *s;

    if (!format_ok(format)) {
        *why = "a format is one to seven lower-case letters and digits";
        return NULL;
    }

    if (strlen(url) > TILES_URL_MAX
        || !parse_url(url, &tls_, host, sizeof host, &port, path, sizeof path)) {
        *why = "not an http or https address";
        return NULL;
    }

    for (s = sources; s; s = s->next) {
        if (strcmp(s->url, url) == 0 && strcmp(s->format, format) == 0) {
            s->paused = false;
            return s;
        }
    }

    if ((s = calloc(1, sizeof *s)) == NULL) {
        *why = "no memory for another source";
        return NULL;
    }

    snprintf(s->url, sizeof s->url, "%s", url);
    snprintf(s->format, sizeof s->format, "%s", format);
    make_key(s);
    link_reset(&s->link, TILE_MOST);
    link_aim(&s->link, tls_, host, port);

    if (strstr(path, "{z}") != NULL) {
        snprintf(s->template_path, sizeof s->template_path, "%s", path);
        s->ready = true;
    } else {
        snprintf(s->tilejson_path, sizeof s->tilejson_path, "%s", path);
    }

    s->number = ++source_count;
    s->next = sources;
    sources = s;

    {
        char line[TILES_URL_MAX + 64];

        snprintf(line, sizeof line, "tiles: source %u is %s, as .%s\n",
                 (unsigned)s->number, s->url, s->format);
        say(console, line);
    }

    return s;
}

/* ------------------------------------------------------------------------
 * What is wanted, what is under way, what has come - for each source.
 * --------------------------------------------------------------------- */

static void came(struct source *s, const struct tiles_id *t)
{
    s->arrived[s->arrived_seq % ARRIVED_KEPT] = *t;
    s->arrived_seq++;
}

static bool same(const struct tiles_id *a, const struct tiles_id *b)
{
    return a->z == b->z && a->x == b->x && a->y == b->y;
}

/* Whether a tile came lately, so is in the cache already: the application
 * asks for what it has not yet heard came, and hears only when it asks, so
 * a tile can be asked for again in between - fetched twice, without this. */
static bool came_lately(const struct source *s, const struct tiles_id *t)
{
    uint32_t kept = s->arrived_seq < ARRIVED_KEPT ? s->arrived_seq : ARRIVED_KEPT;

    for (uint32_t k = 1; k <= kept; k++) {
        if (same(&s->arrived[(s->arrived_seq - k) % ARRIVED_KEPT], t)) return true;
    }

    return false;
}

static bool cache_write(struct source *s, const struct tiles_id *t, const uint8_t *bytes, size_t n)
{
    char dir[DISK_PATH_MAX], name[48];

    snprintf(dir, sizeof dir, CACHE_ROOT "/%s/%u", s->key, (unsigned)t->z);
    snprintf(name, sizeof name, "%u-%u.%s", (unsigned)t->x, (unsigned)t->y, s->format);
    return cache_put(&s->link, dir, name, bytes, n);
}

/* `{z}`, `{x}` and `{y}` filled in. */
static bool tile_path(const struct source *s, const struct tiles_id *t, char *out, size_t room)
{
    const char *p = s->template_path;
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
static bool tilejson_template(struct source *s, const uint8_t *body, size_t n)
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

        if (!parse_url(url, &tls_, host, sizeof host, &port, s->template_path,
                       sizeof s->template_path)) {
            return false;
        }

        /* The tiles on another host than the TileJSON's: not followed - the
         * connection is kept to one host. */
        if (strcmp(host, s->link.host) != 0 || port != s->link.port || tls_ != s->link.tls) {
            return false;
        }
    }

    return strstr(s->template_path, "{z}") != NULL;
}

/* Whether a source has fetching to do. */
static bool source_work(const struct source *s)
{
    return s->now.busy || (!s->paused && (s->wanted_count > 0 || !s->ready));
}

/* The next fetch begun, if there is one to begin. */
static void begin(struct source *s)
{
    char path[600];

    if (s->now.busy) return;

    if (!s->link.ready && !link_dial(&s->link)) {
        s->failed++;
        s->wanted_count = 0;        /* the source is not answering: let go, */
        s->paused = true;           /* until somebody asks again */
        return;
    }

    if (!s->ready) {
        if (httpc_get(&s->link.http, s->tilejson_path) != 0) return;

        s->now.busy = true;
        s->now.since = kosmos_ticks();
        s->now.tilejson = true;
        return;
    }

    if (s->wanted_count == 0) return;

    s->now.tile = s->wanted[0];
    memmove(s->wanted, s->wanted + 1, (s->wanted_count - 1) * sizeof s->wanted[0]);
    s->wanted_count--;

    if (!tile_path(s, &s->now.tile, path, sizeof path) || httpc_get(&s->link.http, path) != 0) {
        s->failed++;
        note(&s->link, "a tile's address is too long");
        return;
    }

    s->now.busy = true;
    s->now.since = kosmos_ticks();
    s->now.tilejson = false;
}

/* The fetch under way, moved on; what it came to, kept. */
static void step(struct source *s)
{
    struct link *l = &s->link;
    int r;

    begin(s);

    if (!s->now.busy) return;

    r = httpc_step(&l->http);

    if (r != HTTPC_DONE && r != HTTPC_FAILED && too_long(s->now.since)) {
        s->failed++;
        note(l, "the source did not answer within twenty seconds");
        say(console, "tiles: the source did not answer within twenty seconds; tried again\n");
        s->now.busy = false;
        link_hang_up(l);
        return;
    }

    if (r == HTTPC_DONE) {
        s->now.busy = false;

        if (s->now.tilejson) {
            if (l->http.status != 200 || !tilejson_template(s, l->http.body, l->http.body_len)) {
                s->failed++;
                note(l, "the source's TileJSON names no tiles this can fetch");
                s->paused = true;
            } else {
                s->ready = true;
                say(console, "tiles: the source's tiles are at ");
                say(console, s->template_path);
                say(console, "\n");
            }
        } else if (l->http.status == 200 || l->http.status == 204 || l->http.status == 404) {
            /* A tile there is none of - the sea, beyond the data - is kept
             * as an empty file, so it is not asked for again. */
            size_t n = l->http.status == 200 ? l->http.body_len : 0;

            if (cache_write(s, &s->now.tile, l->http.body, n)) {
                came(s, &s->now.tile);
            } else {
                s->failed++;
            }
        } else {
            char text[64];

            s->failed++;
            snprintf(text, sizeof text, "the source answered %d", l->http.status);
            note(l, text);
        }

        if (!httpc_reusable(&l->http)) link_hang_up(l);
    } else if (r == HTTPC_FAILED) {
        s->failed++;
        note(l, l->http.why ? l->http.why : "a fetch failed");
        s->now.busy = false;
        link_hang_up(l);
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
            note(&finder.link, "a search too long to ask");
            finder_failed++;
            finder_answered(0);
            return;
        }

        if ((!finder.link.ready && !link_dial(&finder.link))
            || httpc_get(&finder.link.http, path) != 0) {
            finder_failed++;
            link_hang_up(&finder.link);
            finder_answered(0);
            return;
        }
    }

    if (finder.now == 0) return;

    int r = httpc_step(&finder.link.http);

    if (r != HTTPC_DONE && r != HTTPC_FAILED && too_long(finder.began)) {
        finder_failed++;
        note(&finder.link, "the finder did not answer within twenty seconds");
        say(console, "tiles: the finder did not answer within twenty seconds\n");
        link_hang_up(&finder.link);
        finder_answered(0);
        return;
    }

    if (r == HTTPC_DONE) {
        char name[24];
        uint32_t status = (uint32_t)finder.link.http.status;

        snprintf(name, sizeof name, "%u.json", (unsigned)finder.now);

        if (!cache_put(&finder.link, CACHE_ROOT "/places", name, finder.link.http.body,
                       status == 200 ? finder.link.http.body_len : 0)) {
            finder_failed++;
            status = 0;
        }

        if (!httpc_reusable(&finder.link.http)) link_hang_up(&finder.link);

        finder_answered(status);
    } else if (r == HTTPC_FAILED) {
        finder_failed++;
        note(&finder.link, finder.link.http.why ? finder.link.http.why : "a search failed");
        link_hang_up(&finder.link);
        finder_answered(0);
    }
}

/* ------------------------------------------------------------------------
 * The door.
 * --------------------------------------------------------------------- */

/* The source a request names, or the reply refused. */
static struct source *named(const struct tiles_request *rq, struct tiles_reply *rp)
{
    struct source *s = source_numbered(rq->source);

    if (s == NULL) {
        rp->status = TILES_ERR_SOURCE;
        snprintf(rp->why, sizeof rp->why, "no source %u", (unsigned)rq->source);
    }

    return s;
}

static void answer(const struct message *in, struct message *out)
{
    const struct tiles_request *rq = (const struct tiles_request *)in->data;
    struct tiles_reply *rp = (struct tiles_reply *)out->data;
    struct source *s = NULL;

    memset(out, 0, sizeof *out);
    out->length = sizeof *rp;

    if (in->length != sizeof *rq) {
        rp->status = TILES_ERR_BAD_OP;
        return;
    }

    switch (rq->op) {
    case TILES_OP_SOURCE: {
        char url[sizeof rq->u.url + 1], format[TILES_FORMAT_MAX + 1];
        const char *why = NULL;

        memcpy(url, rq->u.url, sizeof rq->u.url);
        url[sizeof rq->u.url] = '\0';
        memcpy(format, rq->format, TILES_FORMAT_MAX);
        format[TILES_FORMAT_MAX] = '\0';

        if ((s = source_for(url, format, &why)) == NULL) {
            rp->status = TILES_ERR_SOURCE;
            snprintf(rp->why, sizeof rp->why, "%s", why);
            return;
        }
        break;
    }

    case TILES_OP_WANT:
        if (rq->count > TILES_MAX) {
            rp->status = TILES_ERR_BAD_OP;
            return;
        }

        if ((s = named(rq, rp)) == NULL) return;

        /* The new list, less the one already under way. */
        s->wanted_count = 0;
        s->paused = false;

        for (uint32_t i = 0; i < rq->count; i++) {
            if (rq->u.tiles[i].z > 22) continue;
            if (s->now.busy && !s->now.tilejson && same(&rq->u.tiles[i], &s->now.tile)) continue;
            if (came_lately(s, &rq->u.tiles[i])) continue;

            s->wanted[s->wanted_count++] = rq->u.tiles[i];
        }
        break;

    case TILES_OP_ARRIVED: {
        uint32_t from = rq->since;

        if ((s = named(rq, rp)) == NULL) return;

        if (s->arrived_seq - from > ARRIVED_KEPT) from = s->arrived_seq - ARRIVED_KEPT;

        while (from < s->arrived_seq && rp->count < TILES_MAX) {
            rp->tiles[rp->count++] = s->arrived[from % ARRIVED_KEPT];
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

    /* What is said of failures is the source's when one is named, and the
     * finder's for a search. */
    if (s != NULL) {
        rp->source = s->number;
        rp->outstanding = s->wanted_count + (s->now.busy ? 1u : 0u);
        rp->failed = s->failed;
        snprintf(rp->why, sizeof rp->why, "%s", s->link.why);
        snprintf(rp->cache, sizeof rp->cache, CACHE_ROOT "/%s", s->key);
    } else {
        rp->failed = finder_failed;
        snprintf(rp->why, sizeof rp->why, "%s", finder.link.why);
    }
}

/* Whether any source, or the finder, has something to do. */
static bool any_work(void)
{
    for (struct source *s = sources; s; s = s->next) {
        if (source_work(s)) return true;
    }

    return finder_work();
}

void tiles_server(long endpoint, long net_ep, long disk_ep, long console_ep)
{
    static struct message in, out;

    console = console_ep;
    disk.ep = disk_ep;
    net_client_start(net_ep);
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
        long r = kosmos_receive(endpoint, &in, &sender, 0, any_work() ? 1ul : 0ul);

        if (r == 0) {
            answer(&in, &out);
            (void)kosmos_reply(sender, &out);
        }

        /* Steps while they finish things - a tile done, the next begun -
         * and back to the door as soon as every one under way is waiting
         * on the network. */
        for (int i = 0; i < 32 && any_work(); i++) {
            bool waiting = true;

            for (struct source *s = sources; s; s = s->next) {
                if (source_work(s)) step(s);
                if (s->now.busy && !link_waiting(&s->link)) waiting = false;
            }

            if (finder_work()) finder_step();
            if (finder.now != 0 && !link_waiting(&finder.link)) waiting = false;

            if (waiting) break;
        }
    }
}
