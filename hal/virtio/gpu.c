/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * virtio-gpu, its 2D half: the screen as a resource the device scans out,
 * backed by guest pages, and nothing drawn reaching it until it is sent
 * (`roadmap.md` 4h a).
 *
 * **What ramfb did for free and this does on purpose.** ramfb scans the
 * guest's memory out on QEMU's own schedule, all of it, every refresh.
 * virtio-gpu keeps its own copy - the host's image of the resource - and
 * shows that; a rectangle drawn in guest memory reaches it only through
 * TRANSFER_TO_HOST_2D, and reaches the screen only through RESOURCE_FLUSH.
 * So a screen here is a promise the drawer keeps: say what changed. That is
 * `hal_fb_flush`, which `CLAUDE.md` said this device would earn the HAL, and
 * what it buys is the host copying and showing only the rectangles that
 * changed - and, later, a cursor that moves without a frame and a screen the
 * device can resize (4h b, c).
 *
 * **Everything below was checked against QEMU 11.1.1**, not remembered:
 * `include/standard-headers/linux/virtio_gpu.h` for the structures and
 * numbers, and `hw/display/virtio-gpu.c` for two behaviours this depends on
 * (the comments at each say which).
 *
 * **Polled, one command at a time, under a lock** - `blk.c`'s shape and for
 * its reasons: a flush is two commands and a wait of microseconds, the
 * console flushes from places where it cannot sleep, and a queue of fences
 * would be machinery for a device QEMU answers in its own time anyway.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mmu.h"
#include "hal.h"
#include "spinlock.h"
#include "virtio.h"
#include "fbpixels.h"
#include "gpu.h"

#define QUEUE_SIZE      8
#define CONTROL_QUEUE   0
#define RESOURCE        1u      /* the screen; there is one */
#define SCANOUT         0u

struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };

/* virtio_gpu.h */
#define CMD_RESOURCE_CREATE_2D      0x0101u
#define CMD_SET_SCANOUT             0x0103u
#define CMD_RESOURCE_FLUSH          0x0104u
#define CMD_TRANSFER_TO_HOST_2D     0x0105u
#define CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define RESP_OK_NODATA              0x1100u

/*
 * `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM`: bytes B, G, R, X in memory, which is
 * the 32-bit word 0x00RRGGBB that `struct fb` promises. QEMU agrees:
 * `virtio_gpu_get_pixman_format` maps it to PIXMAN_BE_b8g8r8x8, which on a
 * little-endian host is PIXMAN_x8r8g8b8 - the format ramfb's XRGB8888
 * becomes, so the two devices show the same colours from the same pixels.
 */
#define FORMAT_B8G8R8X8             2u

struct ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
};

struct rect {
    uint32_t x, y, width, height;
};

struct create_2d {
    struct ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct set_scanout {
    struct ctrl_hdr hdr;
    struct rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct resource_flush {
    struct ctrl_hdr hdr;
    struct rect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct transfer_2d {
    struct ctrl_hdr hdr;
    struct rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

/* The backing, with its one entry after it in the same descriptor: QEMU
 * reads the entries from the request's bytes just past the struct. */
struct attach_backing {
    struct ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    uint64_t addr;              /* virtio_gpu_mem_entry */
    uint32_t length;
    uint32_t padding;
};

_Static_assert(sizeof(struct ctrl_hdr) == 24, "virtio_gpu_ctrl_hdr");
_Static_assert(sizeof(struct create_2d) == 40, "resource_create_2d");
_Static_assert(sizeof(struct set_scanout) == 48, "set_scanout");
_Static_assert(sizeof(struct resource_flush) == 48, "resource_flush");
_Static_assert(sizeof(struct transfer_2d) == 56, "transfer_to_host_2d");
_Static_assert(sizeof(struct attach_backing) == 48,
               "attach_backing and one mem_entry");

static struct {
    struct virtio_device dev;
    bool      present;
    uint16_t  last_used;

    uint32_t  width, height;    /* what is shown */
    uint32_t  pitch;            /* bytes a row, in the backing and the host */

    struct vqueue queue;

    _Alignas(16) union {
        struct ctrl_hdr       hdr;
        struct create_2d      create;
        struct set_scanout    scanout;
        struct resource_flush flush;
        struct transfer_2d    transfer;
        struct attach_backing backing;
    } req;

    _Alignas(16) struct ctrl_hdr resp;
} gpu;

static struct spinlock gpu_lock = SPINLOCK("virtio-gpu");

/*
 * One command and its answer: the request the device reads, the header it
 * writes back. False when it did not answer OK, or not at all.
 */
static bool command(size_t bytes)
{
    unsigned long spins;
    uint16_t at;

    gpu.req.hdr.flags = 0;
    gpu.req.hdr.fence_id = 0;
    gpu.req.hdr.ctx_id = 0;
    gpu.req.hdr.ring_idx = 0;
    memset(&gpu.resp, 0, sizeof(gpu.resp));

    gpu.queue.desc[0].addr  = (uint64_t)virt_to_phys(&gpu.req);
    gpu.queue.desc[0].len   = (uint32_t)bytes;
    gpu.queue.desc[0].flags = VRING_DESC_F_NEXT;
    gpu.queue.desc[0].next  = 1;

    gpu.queue.desc[1].addr  = (uint64_t)virt_to_phys(&gpu.resp);
    gpu.queue.desc[1].len   = sizeof(gpu.resp);
    gpu.queue.desc[1].flags = VRING_DESC_F_WRITE;
    gpu.queue.desc[1].next  = 0;

    at = gpu.queue.avail.idx % QUEUE_SIZE;
    gpu.queue.avail.ring[at] = 0;

    virtio_publish();
    gpu.queue.avail.idx++;
    virtio_publish();

    virtio_notify(&gpu.dev, CONTROL_QUEUE);

    for (spins = 0; spins < 100000000UL; spins++) {
        virtio_consume();

        if (gpu.queue.used.idx != gpu.last_used) {
            gpu.last_used = gpu.queue.used.idx;

            /* Acknowledged though nothing waits on it, as `blk.c` does: a
             * status left set is an interrupt delivered for ever the moment
             * the line is unmasked. */
            (void)virtio_ack_interrupt(&gpu.dev);

            virtio_consume();
            return gpu.resp.type == RESP_OK_NODATA;
        }
    }

    return false;
}

/*
 * The rectangle, clipped to the screen, to the host and onto the screen.
 * Called with the lock held.
 */
static void flush_locked(unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (!gpu.present || x >= gpu.width || y >= gpu.height || w == 0 || h == 0) {
        return;
    }

    if (w > gpu.width - x) {
        w = gpu.width - x;
    }

    if (h > gpu.height - y) {
        h = gpu.height - y;
    }

    /*
     * `offset` is where the rectangle starts in the backing. QEMU reads row
     * `n` of it at `offset + n * stride`, with the stride its own image's -
     * the resource's width times four (`virtio_gpu_transfer_to_host_2d`) -
     * which is why the resource is as wide as the pitch (`virtio_gpu_init`).
     */
    memset(&gpu.req, 0, sizeof(gpu.req));
    gpu.req.transfer.hdr.type    = CMD_TRANSFER_TO_HOST_2D;
    gpu.req.transfer.r.x         = x;
    gpu.req.transfer.r.y         = y;
    gpu.req.transfer.r.width     = w;
    gpu.req.transfer.r.height    = h;
    gpu.req.transfer.offset      = (uint64_t)y * gpu.pitch + (uint64_t)x * 4u;
    gpu.req.transfer.resource_id = RESOURCE;

    if (!command(sizeof(gpu.req.transfer))) {
        return;
    }

    memset(&gpu.req, 0, sizeof(gpu.req));
    gpu.req.flush.hdr.type    = CMD_RESOURCE_FLUSH;
    gpu.req.flush.r.x         = x;
    gpu.req.flush.r.y         = y;
    gpu.req.flush.r.width     = w;
    gpu.req.flush.r.height    = h;
    gpu.req.flush.resource_id = RESOURCE;

    (void)command(sizeof(gpu.req.flush));
}

bool virtio_gpu_init(struct fb *out)
{
    struct fb_pixels px;
    unsigned from = 0;
    unsigned long flags;

    gpu.present = false;

    while (virtio_open(VIRTIO_ID_GPU, from, &gpu.dev)) {
        from = gpu.dev.index + 1;

        virtio_begin(&gpu.dev);

        /* No virgl, no EDID, no blobs: the 2D commands are what every
         * virtio-gpu answers, and each feature taken is one to honour. */
        if (!virtio_features(&gpu.dev, 0)) {
            continue;
        }

        memset(&gpu.queue, 0, sizeof(gpu.queue));
        gpu.last_used = 0;

        if (!virtio_queue_attach(&gpu.dev, CONTROL_QUEUE, QUEUE_SIZE,
                                 &gpu.queue.desc, &gpu.queue.avail,
                                 &gpu.queue.used)) {
            virtio_fail(&gpu.dev);
            continue;
        }

        virtio_ready(&gpu.dev);

        if (!fb_pixels_take(&px)) {
            virtio_fail(&gpu.dev);
            return false;
        }

        flags = spin_lock(&gpu_lock);

        /*
         * **The resource is as wide as the pitch, and the screen is the part
         * of it that is shown.** QEMU takes a transfer's rows at its own
         * image's stride, the resource's width times four, so a backing whose
         * rows are padded - and Kosmos pads them on purpose (`fbpixels.c`) -
         * has to be described as a resource that wide. SET_SCANOUT then shows
         * the `width` by `height` rectangle at its corner, which QEMU allows
         * (`virtio_gpu_check_scanout_bounds`) and builds its display surface
         * from at the resource's stride.
         */
        memset(&gpu.req, 0, sizeof(gpu.req));
        gpu.req.create.hdr.type    = CMD_RESOURCE_CREATE_2D;
        gpu.req.create.resource_id = RESOURCE;
        gpu.req.create.format      = FORMAT_B8G8R8X8;
        gpu.req.create.width       = px.pitch / 4u;
        gpu.req.create.height      = px.height;

        bool ok = command(sizeof(gpu.req.create));

        if (ok) {
            memset(&gpu.req, 0, sizeof(gpu.req));
            gpu.req.backing.hdr.type    = CMD_RESOURCE_ATTACH_BACKING;
            gpu.req.backing.resource_id = RESOURCE;
            gpu.req.backing.nr_entries  = 1;
            gpu.req.backing.addr        = (uint64_t)virt_to_phys(px.pixels);
            gpu.req.backing.length      = px.pitch * px.height;
            ok = command(sizeof(gpu.req.backing));
        }

        if (ok) {
            memset(&gpu.req, 0, sizeof(gpu.req));
            gpu.req.scanout.hdr.type    = CMD_SET_SCANOUT;
            gpu.req.scanout.r.width     = px.width;
            gpu.req.scanout.r.height    = px.height;
            gpu.req.scanout.scanout_id  = SCANOUT;
            gpu.req.scanout.resource_id = RESOURCE;
            ok = command(sizeof(gpu.req.scanout));
        }

        if (ok) {
            gpu.width = px.width;
            gpu.height = px.height;
            gpu.pitch = px.pitch;
            gpu.present = true;

            /* Black, as the pixels are: the host's image starts as whatever
             * QEMU made it, and a screen is shown from that. */
            flush_locked(0, 0, gpu.width, gpu.height);
        }

        spin_unlock(&gpu_lock, flags);

        if (!ok) {
            virtio_fail(&gpu.dev);
            return false;
        }

        out->pixels = (volatile uint32_t *)(void *)px.pixels;
        out->phys   = virt_to_phys(px.pixels);
        out->width  = px.width;
        out->height = px.height;
        out->pitch  = px.pitch;
        return true;
    }

    return false;
}

bool virtio_gpu_present(void)
{
    return gpu.present;
}

void virtio_gpu_flush(unsigned x, unsigned y, unsigned w, unsigned h)
{
    unsigned long flags;

    if (!gpu.present) {
        return;
    }

    flags = spin_lock(&gpu_lock);
    flush_locked(x, y, w, h);
    spin_unlock(&gpu_lock, flags);
}
