/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A sandbox's screen: virtio-gpu, 2D (`vmmgpu.h`).
 *
 * The guest's driver makes **resources** - pictures with a format and a
 * size - and gives each **backing**, pages of its own RAM. It draws into
 * the backing, asks for a rectangle to be **transferred** into the
 * resource, and **flushes** the rectangle when it is to be seen. One
 * resource is **set as the scanout**: what the display shows. So a
 * resource's pixels are kept here, a copy of the backing as of the last
 * transfer, and a flush of the scanout's lays its rectangle into the
 * screen region, converted to Kosmos's pixels, and counts a frame.
 *
 * The control queue is answered at once, on the vCPU's thread; the cursor
 * queue is taken and left unanswered beyond being used - Kosmos draws its
 * own pointer over the window, so the guest's is not drawn.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "vdev.h"
#include "vmmgpu.h"
#include "vmmscreen.h"

#define VIRTIO_ID_GPU           16u

#define CMD_GET_DISPLAY_INFO    0x0100u
#define CMD_RESOURCE_CREATE_2D  0x0101u
#define CMD_RESOURCE_UNREF      0x0102u
#define CMD_SET_SCANOUT         0x0103u
#define CMD_RESOURCE_FLUSH      0x0104u
#define CMD_TRANSFER_TO_HOST_2D 0x0105u
#define CMD_ATTACH_BACKING      0x0106u
#define CMD_DETACH_BACKING      0x0107u

#define RESP_OK_NODATA          0x1100u
#define RESP_OK_DISPLAY_INFO    0x1101u
#define RESP_ERR_UNSPEC         0x1200u
#define RESP_ERR_OUT_OF_MEMORY  0x1201u
#define RESP_ERR_SCANOUT_ID     0x1202u
#define RESP_ERR_RESOURCE_ID    0x1203u
#define RESP_ERR_PARAMETER      0x1205u

#define FLAG_FENCE              1u
#define HDR                     24u
#define SCANOUTS_MOST           16u

/* The formats a 2D guest draws in, by the bytes each pixel is in memory. */
#define FMT_B8G8R8A8            1u
#define FMT_B8G8R8X8            2u
#define FMT_R8G8B8A8            67u
#define FMT_R8G8B8X8            134u

struct span {
    uint64_t addr;
    uint32_t len;
};

struct resource {
    uint32_t     id, format, width, height;
    uint32_t    *pixels;                /* the backing as last transferred */
    struct span *backing;
    uint32_t     spans;
};

static struct {
    bool               on;
    struct vdev        dev;
    struct vmm_screen *screen;
    uint32_t          *out;             /* the screen's pixels */
    struct resource   *res;
    uint32_t           res_n, res_room;
    uint32_t           scanout;         /* its resource; 0: none */
    uint32_t           scan_x, scan_y;  /* where in it the display starts */
    uint8_t           *req;
    uint32_t           req_room;
    uint32_t           width, height;   /* the display's, as the guest is told */
    uint32_t           sized;           /* the window's count of sizes, as last seen */
    uint32_t           events;          /* `events_read`: 1, the display changed */
} G;

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static struct resource *find(uint32_t id)
{
    for (uint32_t i = 0; i < G.res_n; i++) {
        if (G.res[i].id == id) {
            return &G.res[i];
        }
    }

    return NULL;
}

static void forget(struct resource *r)
{
    free(r->pixels);
    free(r->backing);
    *r = G.res[--G.res_n];

    if (G.scanout != 0 && find(G.scanout) == NULL) {
        G.scanout = 0;
    }
}

/* `n` bytes of a resource's backing from `off`, into `to`: false past it. */
static bool backing_read(const struct resource *r, uint64_t off, uint8_t *to, uint32_t n)
{
    for (uint32_t i = 0; i < r->spans && n > 0; i++) {
        uint32_t len = r->backing[i].len;

        if (off >= len) {
            off -= len;
            continue;
        }

        uint32_t take = len - (uint32_t)off < n ? len - (uint32_t)off : n;
        const uint8_t *at = vdev_guest(r->backing[i].addr + off, take);

        if (at == NULL) {
            return false;
        }

        memcpy(to, at, take);
        to += take;
        n -= take;
        off = 0;
    }

    return n == 0;
}

/* A rectangle clipped to `w` by `h`: false when nothing is left. */
static bool clip(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h, uint32_t cw, uint32_t ch)
{
    if (*x >= cw || *y >= ch || *w == 0 || *h == 0) {
        return false;
    }

    if (*w > cw - *x) *w = cw - *x;
    if (*h > ch - *y) *h = ch - *y;
    return true;
}

static uint32_t create(const uint8_t *q, uint32_t n)
{
    uint32_t id, format, w, h;
    struct resource *r;

    if (n < HDR + 16) return RESP_ERR_PARAMETER;

    id = get32(q + HDR);
    format = get32(q + HDR + 4);
    w = get32(q + HDR + 8);
    h = get32(q + HDR + 12);

    if (id == 0 || find(id) != NULL) return RESP_ERR_RESOURCE_ID;

    if (w == 0 || h == 0 || w > VMM_SCREEN_MOST || h > VMM_SCREEN_MOST
        || (format != FMT_B8G8R8A8 && format != FMT_B8G8R8X8
            && format != FMT_R8G8B8A8 && format != FMT_R8G8B8X8)) {
        return RESP_ERR_PARAMETER;
    }

    if (G.res_n == G.res_room) {
        uint32_t room = G.res_room ? G.res_room * 2 : 8;
        struct resource *more = realloc(G.res, room * sizeof *more);

        if (more == NULL) return RESP_ERR_OUT_OF_MEMORY;

        G.res = more;
        G.res_room = room;
    }

    r = &G.res[G.res_n];
    memset(r, 0, sizeof *r);
    r->pixels = calloc((size_t)w * h, 4);

    if (r->pixels == NULL) return RESP_ERR_OUT_OF_MEMORY;

    r->id = id;
    r->format = format;
    r->width = w;
    r->height = h;
    G.res_n++;
    return RESP_OK_NODATA;
}

static uint32_t attach(const uint8_t *q, uint32_t n)
{
    struct resource *r;
    uint32_t count;

    if (n < HDR + 8) return RESP_ERR_PARAMETER;

    r = find(get32(q + HDR));
    count = get32(q + HDR + 4);

    if (r == NULL) return RESP_ERR_RESOURCE_ID;
    if (count == 0 || (uint64_t)count * 16 > n - HDR - 8) return RESP_ERR_PARAMETER;

    free(r->backing);
    r->backing = malloc((size_t)count * sizeof *r->backing);
    r->spans = 0;

    if (r->backing == NULL) return RESP_ERR_OUT_OF_MEMORY;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = q + HDR + 8 + (size_t)i * 16;

        r->backing[i].addr = get64(e);
        r->backing[i].len = get32(e + 8);
    }

    r->spans = count;
    return RESP_OK_NODATA;
}

static uint32_t transfer(const uint8_t *q, uint32_t n)
{
    struct resource *r;
    uint32_t x, y, w, h;
    uint64_t offset;

    if (n < HDR + 32) return RESP_ERR_PARAMETER;

    x = get32(q + HDR);
    y = get32(q + HDR + 4);
    w = get32(q + HDR + 8);
    h = get32(q + HDR + 12);
    offset = get64(q + HDR + 16);
    r = find(get32(q + HDR + 24));

    if (r == NULL) return RESP_ERR_RESOURCE_ID;
    if (r->spans == 0) return RESP_ERR_UNSPEC;
    if (!clip(&x, &y, &w, &h, r->width, r->height)) return RESP_OK_NODATA;

    /* A row at a time: the backing is the resource's pixels, `width * 4`
     * bytes a row, and `offset` is where the rectangle's first one is. */
    for (uint32_t row = 0; row < h; row++) {
        if (!backing_read(r, offset + (uint64_t)row * r->width * 4,
                          (uint8_t *)(r->pixels + (size_t)(y + row) * r->width + x),
                          w * 4)) {
            return RESP_ERR_PARAMETER;
        }
    }

    return RESP_OK_NODATA;
}

static uint32_t scanout(const uint8_t *q, uint32_t n)
{
    uint32_t id;

    if (n < HDR + 24) return RESP_ERR_PARAMETER;
    if (get32(q + HDR + 16) != 0) return RESP_ERR_SCANOUT_ID;

    id = get32(q + HDR + 20);

    if (id != 0 && find(id) == NULL) return RESP_ERR_RESOURCE_ID;

    G.scanout = id;
    G.scan_x = get32(q + HDR);
    G.scan_y = get32(q + HDR + 4);
    return RESP_OK_NODATA;
}

/* The scanout's rectangle into the screen region: Kosmos's pixels, opaque. */
static uint32_t flush(const uint8_t *q, uint32_t n)
{
    struct resource *r;
    uint32_t x, y, w, h;
    bool swap;

    if (n < HDR + 24) return RESP_ERR_PARAMETER;

    x = get32(q + HDR);
    y = get32(q + HDR + 4);
    w = get32(q + HDR + 8);
    h = get32(q + HDR + 12);
    r = find(get32(q + HDR + 16));

    if (r == NULL) return RESP_ERR_RESOURCE_ID;
    if (r->id != G.scanout) return RESP_OK_NODATA;
    if (!clip(&x, &y, &w, &h, r->width, r->height)) return RESP_OK_NODATA;

    /* From the resource's place to the display's. */
    if (x < G.scan_x || y < G.scan_y) return RESP_OK_NODATA;

    {
        uint32_t sx = x - G.scan_x, sy = y - G.scan_y;

        if (!clip(&sx, &sy, &w, &h, G.width, G.height)) {
            return RESP_OK_NODATA;
        }

        swap = r->format == FMT_R8G8B8A8 || r->format == FMT_R8G8B8X8;

        for (uint32_t row = 0; row < h; row++) {
            const uint32_t *from = r->pixels + (size_t)(y + row) * r->width + x;
            uint32_t *to = G.out + (size_t)(sy + row) * G.width + sx;

            for (uint32_t i = 0; i < w; i++) {
                uint32_t p = from[i];

                if (swap) {
                    p = (p & 0x0000ff00u) | (p >> 16 & 0xffu) | (p & 0xffu) << 16;
                }

                to[i] = p | 0xff000000u;
            }
        }

        G.screen->x = sx;
        G.screen->y = sy;
        G.screen->w = w;
        G.screen->h = h;
        __atomic_store_n(&G.screen->shown, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&G.screen->frames, G.screen->frames + 1u, __ATOMIC_RELEASE);
    }

    return RESP_OK_NODATA;
}

static void notify(struct vdev *d, unsigned qi)
{
    struct vdev_chain c;

    while (vdev_pop(d, qi, &c)) {
        uint32_t in = 0, n, type, answer, written = 0;
        uint8_t reply[HDR + SCANOUTS_MOST * 24];

        for (unsigned i = 0; i < c.nin; i++) {
            in += c.in_len[i];
        }

        if (in > G.req_room) {
            uint8_t *more = realloc(G.req, in);

            if (more == NULL) {
                vdev_push(d, qi, c.head, 0);
                continue;
            }

            G.req = more;
            G.req_room = in;
        }

        n = vdev_gather(&c, G.req, G.req_room);

        /* The cursor's: used, and nothing said back. */
        if (qi == 1 || n < HDR) {
            vdev_push(d, qi, c.head, 0);
            continue;
        }

        type = get32(G.req);
        memset(reply, 0, sizeof reply);

        switch (type) {
        case CMD_GET_DISPLAY_INFO:
            answer = RESP_OK_DISPLAY_INFO;
            put32(reply + HDR + 8, G.width);
            put32(reply + HDR + 12, G.height);
            put32(reply + HDR + 16, 1);         /* enabled */
            written = HDR + SCANOUTS_MOST * 24;
            break;
        case CMD_RESOURCE_CREATE_2D: answer = create(G.req, n); break;
        case CMD_RESOURCE_UNREF: {
            struct resource *r = n >= HDR + 4 ? find(get32(G.req + HDR)) : NULL;

            answer = r ? RESP_OK_NODATA : RESP_ERR_RESOURCE_ID;

            if (r) forget(r);
            break;
        }
        case CMD_SET_SCANOUT:         answer = scanout(G.req, n); break;
        case CMD_RESOURCE_FLUSH:      answer = flush(G.req, n); break;
        case CMD_TRANSFER_TO_HOST_2D: answer = transfer(G.req, n); break;
        case CMD_ATTACH_BACKING:      answer = attach(G.req, n); break;
        case CMD_DETACH_BACKING: {
            struct resource *r = n >= HDR + 4 ? find(get32(G.req + HDR)) : NULL;

            answer = r ? RESP_OK_NODATA : RESP_ERR_RESOURCE_ID;

            if (r) {
                free(r->backing);
                r->backing = NULL;
                r->spans = 0;
            }
            break;
        }
        default:
            answer = RESP_ERR_UNSPEC;       /* 3D, EDID, capsets: not offered */
            break;
        }

        put32(reply, answer);

        /* A fence asked for is a fence kept: everything here is done. */
        if (get32(G.req + 4) & FLAG_FENCE) {
            put32(reply + 4, FLAG_FENCE);
            memcpy(reply + 8, G.req + 8, 8);
            memcpy(reply + 16, G.req + 16, 4);
        }

        written = vdev_scatter(&c, reply, written > HDR ? written : HDR);
        vdev_push(d, qi, c.head, written);
    }
}

/* `struct virtio_gpu_config`: its events, one scanout, no 3D capsets. */
static void config(struct vdev *d, uint64_t off, uint8_t *out, unsigned size)
{
    uint8_t cfg[16];

    (void)d;
    memset(cfg, 0, sizeof cfg);
    put32(cfg, G.events);
    put32(cfg + 8, 1);

    for (unsigned k = 0; k < size; k++) {
        out[k] = off + k < sizeof cfg ? cfg[off + k] : 0;
    }
}

/* `events_clear`: the guest has read what changed. */
static void config_write(struct vdev *d, uint64_t off, const uint8_t *in, unsigned size)
{
    (void)d;

    if (off == 4 && size == 4) {
        G.events &= ~get32(in);
    }
}

void vmmgpu_pump(void)
{
    uint32_t sized, w, h;

    if (!G.on || (sized = __atomic_load_n(&G.screen->sized, __ATOMIC_ACQUIRE)) == G.sized) {
        return;
    }

    G.sized = sized;
    w = G.screen->width;
    h = G.screen->height;

    /* A size the region holds, or the one it had: a window's word is not
     * room. */
    if (w == 0 || w % 16 != 0 || h == 0 || w > G.screen->most_width
        || h > G.screen->most_height || (w == G.width && h == G.height)) {
        return;
    }

    G.width = w;
    G.height = h;
    G.events |= 1u;                         /* VIRTIO_GPU_EVENT_DISPLAY */
    vdev_config_changed(&G.dev);
}

bool vmmgpu_start(long screen_cap)
{
    long at, pages;

    if (screen_cap < 0 || (pages = kosmos_mem_size(screen_cap)) <= 0
        || (at = kosmos_mem_map(screen_cap)) < 0) {
        return false;
    }

    G.screen = (struct vmm_screen *)(uintptr_t)at;
    G.out = (uint32_t *)(uintptr_t)(at + VMM_SCREEN_HEAD);

    if (G.screen->magic != VMM_SCREEN_MAGIC || G.screen->width == 0
        || G.screen->width % 16 != 0 || G.screen->height == 0
        || G.screen->width > VMM_SCREEN_MOST || G.screen->height > VMM_SCREEN_MOST
        || (uint64_t)pages * 4096 < VMM_SCREEN_HEAD
                                    + (uint64_t)G.screen->width * G.screen->height * 4) {
        return false;
    }

    G.screen->pitch = G.screen->width * 4;
    G.width = G.screen->width;
    G.height = G.screen->height;

    /* The largest it may become: what the region was made for, and no
     * less than what it is. */
    if (G.screen->most_width < G.width || G.screen->most_height < G.height
        || (uint64_t)pages * 4096 < VMM_SCREEN_HEAD
                                    + (uint64_t)G.screen->most_width * G.screen->most_height * 4) {
        G.screen->most_width = G.width;
        G.screen->most_height = G.height;
    }

    G.sized = G.screen->sized;

    /* The input ring's shape, said for `sys.ring_push` (`vmmscreen.h`). */
    {
        struct vmm_input_ring *ring = (struct vmm_input_ring *)((uint8_t *)G.screen + VMM_INPUT_AT);

        ring->count = VMM_INPUT_EVENTS;
        ring->item = sizeof(struct vmm_input_event);
    }
    G.dev.id = VIRTIO_ID_GPU;
    G.dev.queues = 2;
    G.dev.config = config;
    G.dev.config_write = config_write;
    G.dev.notify = notify;
    G.on = true;
    return true;
}

bool vmmgpu_on(void)
{
    return G.on;
}

void *vmmgpu_page(void)
{
    return G.on ? (void *)G.screen : NULL;
}

bool vmmgpu_mmio(uint64_t off, unsigned size, bool write, uint64_t *value)
{
    return G.on && vdev_mmio(&G.dev, off, size, write, value);
}

bool vmmgpu_raised(void)
{
    return G.on && vdev_raised(&G.dev);
}
