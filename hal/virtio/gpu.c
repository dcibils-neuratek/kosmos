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
 * changed - and a cursor that moves without a frame (4h b, below), and later
 * a screen the device can resize (4h c).
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
#define CURSOR_QUEUE    1
#define RESOURCE        1u      /* the screen; there is one */
#define CURSOR_RESOURCE 2u      /* the pointer's picture (4h b) */
#define SCANOUT         0u
#define CURSOR_SIDE     64u     /* QEMU's cursor is 64 by 64, always */

struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };

/* virtio_gpu.h */
#define CMD_RESOURCE_CREATE_2D      0x0101u
#define CMD_SET_SCANOUT             0x0103u
#define CMD_RESOURCE_FLUSH          0x0104u
#define CMD_TRANSFER_TO_HOST_2D     0x0105u
#define CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define CMD_UPDATE_CURSOR           0x0300u
#define CMD_MOVE_CURSOR             0x0301u
#define RESP_OK_NODATA              0x1100u

/*
 * `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM`: bytes B, G, R, X in memory, which is
 * the 32-bit word 0x00RRGGBB that `struct fb` promises. QEMU agrees:
 * `virtio_gpu_get_pixman_format` maps it to PIXMAN_BE_b8g8r8x8, which on a
 * little-endian host is PIXMAN_x8r8g8b8 - the format ramfb's XRGB8888
 * becomes, so the two devices show the same colours from the same pixels.
 */
#define FORMAT_B8G8R8X8             2u

/*
 * And the pointer's, with its alpha: `B8G8R8A8_UNORM`, bytes B, G, R, A -
 * the word 0xAARRGGBB, the format QEMU's own cursor keeps
 * (`virtio_gpu_update_cursor_data` copies the resource's image into it
 * word for word).
 */
#define FORMAT_B8G8R8A8             1u

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

/*
 * The cursor queue's one command, for UPDATE_CURSOR and MOVE_CURSOR alike
 * (`virtio_gpu_update_cursor`): the position always, the picture's resource
 * and its hot spot only on an update - though a move carries the resource
 * too, since QEMU hides the pointer when it is 0 (`update_cursor` hands it to
 * `qemu_console_set_mouse` as whether it is shown).
 */
struct update_cursor {
    struct ctrl_hdr hdr;
    uint32_t scanout_id;        /* virtio_gpu_cursor_pos */
    uint32_t x;
    uint32_t y;
    uint32_t pos_padding;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
};

_Static_assert(sizeof(struct ctrl_hdr) == 24, "virtio_gpu_ctrl_hdr");
_Static_assert(sizeof(struct update_cursor) == 56, "virtio_gpu_update_cursor");
_Static_assert(sizeof(struct create_2d) == 40, "resource_create_2d");
_Static_assert(sizeof(struct set_scanout) == 48, "set_scanout");
_Static_assert(sizeof(struct resource_flush) == 48, "resource_flush");
_Static_assert(sizeof(struct transfer_2d) == 56, "transfer_to_host_2d");
_Static_assert(sizeof(struct attach_backing) == 48,
               "attach_backing and one mem_entry");

static struct {
    struct virtio_device dev;
    bool      present;
    bool      given_up;         /* a command never came back (`wait.c`) */

    uint32_t  width, height;    /* what is shown */
    uint32_t  pitch;            /* bytes a row, in the backing and the host */

    struct vqueue queue;
    struct vqueue cursor_queue;
    bool      cursor_ready;     /* its picture made, and the queue there */
    bool      cursor_shown;

    _Alignas(16) union {
        struct ctrl_hdr       hdr;
        struct create_2d      create;
        struct set_scanout    scanout;
        struct resource_flush flush;
        struct transfer_2d    transfer;
        struct attach_backing backing;
    } req;

    _Alignas(16) struct ctrl_hdr resp;

    _Alignas(16) struct update_cursor cursor;
} gpu;

/* The pointer's picture, 64 by 64 words, the resource's backing: in the
 * kernel's image, so physically one run of pages, and nothing allocated. */
static _Alignas(4096) uint32_t cursor_pixels[CURSOR_SIDE * CURSOR_SIDE];

static struct spinlock gpu_lock = SPINLOCK("virtio-gpu");

/*
 * One command and its answer: the request the device reads, the header it
 * writes back. False when it did not answer OK, or not at all - and a
 * command that never came back leaves the screen reset and given up on
 * (`wait.c`).
 */
static bool command(size_t bytes)
{
    uint16_t at;

    if (gpu.given_up) {
        return false;
    }

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

    if (!virtio_wait_done(&gpu.queue.used.idx, gpu.queue.avail.idx)) {
        gpu.given_up = true;
        virtio_give_up(&gpu.dev, "virtio-gpu");
        return false;
    }

    /* Acknowledged though nothing waits on it, as `blk.c` does: a status
     * left set is an interrupt delivered for ever the moment the line is
     * unmasked. */
    (void)virtio_ack_interrupt(&gpu.dev);

    virtio_consume();
    return gpu.resp.type == RESP_OK_NODATA;
}

/*
 * One command on the cursor queue (`virtio_gpu_handle_cursor`): read, done,
 * and handed back with nothing written - there is no answer to wait for
 * beyond the device having taken it. Called with the lock held.
 */
static void cursor_command(void)
{
    uint16_t at;

    if (gpu.given_up) {
        return;
    }

    gpu.cursor.hdr.flags = 0;
    gpu.cursor.hdr.fence_id = 0;
    gpu.cursor.hdr.ctx_id = 0;
    gpu.cursor.hdr.ring_idx = 0;
    gpu.cursor.scanout_id = SCANOUT;

    gpu.cursor_queue.desc[0].addr  = (uint64_t)virt_to_phys(&gpu.cursor);
    gpu.cursor_queue.desc[0].len   = sizeof(gpu.cursor);
    gpu.cursor_queue.desc[0].flags = 0;
    gpu.cursor_queue.desc[0].next  = 0;

    at = gpu.cursor_queue.avail.idx % QUEUE_SIZE;
    gpu.cursor_queue.avail.ring[at] = 0;

    virtio_publish();
    gpu.cursor_queue.avail.idx++;
    virtio_publish();

    virtio_notify(&gpu.dev, CURSOR_QUEUE);

    if (!virtio_wait_done(&gpu.cursor_queue.used.idx, gpu.cursor_queue.avail.idx)) {
        gpu.given_up = true;
        virtio_give_up(&gpu.dev, "virtio-gpu's cursor");
        return;
    }

    (void)virtio_ack_interrupt(&gpu.dev);
    virtio_consume();
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
        gpu.given_up = false;

        if (!virtio_queue_attach(&gpu.dev, CONTROL_QUEUE, QUEUE_SIZE,
                                 &gpu.queue.desc, &gpu.queue.avail,
                                 &gpu.queue.used)) {
            virtio_fail(&gpu.dev);
            continue;
        }

        /* The cursor's queue, which every virtio-gpu has; a device that
         * refused it would still show a screen, with the pointer drawn by
         * whoever draws it otherwise. */
        memset(&gpu.cursor_queue, 0, sizeof(gpu.cursor_queue));
        gpu.cursor_ready = virtio_queue_attach(&gpu.dev, CURSOR_QUEUE, QUEUE_SIZE,
                                               &gpu.cursor_queue.desc,
                                               &gpu.cursor_queue.avail,
                                               &gpu.cursor_queue.used);
        gpu.cursor_shown = false;

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

        /*
         * **The pointer's picture** (`roadmap.md` 4h b): a second resource,
         * 64 by 64 with alpha, backed by `cursor_pixels`. Made now and shown
         * when a picture is given (`virtio_gpu_cursor_set`); a device that
         * will not make it simply has no cursor of its own.
         */
        if (ok && gpu.cursor_ready) {
            memset(&gpu.req, 0, sizeof(gpu.req));
            gpu.req.create.hdr.type    = CMD_RESOURCE_CREATE_2D;
            gpu.req.create.resource_id = CURSOR_RESOURCE;
            gpu.req.create.format      = FORMAT_B8G8R8A8;
            gpu.req.create.width       = CURSOR_SIDE;
            gpu.req.create.height      = CURSOR_SIDE;
            gpu.cursor_ready = command(sizeof(gpu.req.create));

            if (gpu.cursor_ready) {
                memset(&gpu.req, 0, sizeof(gpu.req));
                gpu.req.backing.hdr.type    = CMD_RESOURCE_ATTACH_BACKING;
                gpu.req.backing.resource_id = CURSOR_RESOURCE;
                gpu.req.backing.nr_entries  = 1;
                gpu.req.backing.addr        = (uint64_t)virt_to_phys(cursor_pixels);
                gpu.req.backing.length      = sizeof(cursor_pixels);
                gpu.cursor_ready = command(sizeof(gpu.req.backing));
            }
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

/*
 * **The pointer, drawn by the device** (`roadmap.md` 4h b): a picture once,
 * and then a position - the host draws it over the screen, so moving it
 * composes no frame and sends no pixels. `argb` is 64 by 64 words of
 * 0xAARRGGBB; `hot_x`, `hot_y` the point of it that is the pointer's
 * position. False when this device has no cursor to give.
 */
bool virtio_gpu_cursor_set(const uint32_t *argb, unsigned hot_x, unsigned hot_y,
                           unsigned x, unsigned y)
{
    unsigned long flags;

    if (!gpu.present || !gpu.cursor_ready || hot_x >= CURSOR_SIDE
        || hot_y >= CURSOR_SIDE) {
        return false;
    }

    flags = spin_lock(&gpu_lock);
    memcpy(cursor_pixels, argb, sizeof(cursor_pixels));

    /* The picture into the host's image of it first: QEMU copies the
     * cursor from there, not from the backing. */
    memset(&gpu.req, 0, sizeof(gpu.req));
    gpu.req.transfer.hdr.type    = CMD_TRANSFER_TO_HOST_2D;
    gpu.req.transfer.r.width     = CURSOR_SIDE;
    gpu.req.transfer.r.height    = CURSOR_SIDE;
    gpu.req.transfer.resource_id = CURSOR_RESOURCE;

    if (!command(sizeof(gpu.req.transfer))) {
        spin_unlock(&gpu_lock, flags);
        return false;
    }

    memset(&gpu.cursor, 0, sizeof(gpu.cursor));
    gpu.cursor.hdr.type    = CMD_UPDATE_CURSOR;
    gpu.cursor.x           = x;
    gpu.cursor.y           = y;
    gpu.cursor.resource_id = CURSOR_RESOURCE;
    gpu.cursor.hot_x       = hot_x;
    gpu.cursor.hot_y       = hot_y;
    cursor_command();
    gpu.cursor_shown = true;
    spin_unlock(&gpu_lock, flags);

    return true;
}

void virtio_gpu_cursor_move(unsigned x, unsigned y)
{
    unsigned long flags;

    if (!gpu.present || !gpu.cursor_shown) {
        return;
    }

    flags = spin_lock(&gpu_lock);
    memset(&gpu.cursor, 0, sizeof(gpu.cursor));
    gpu.cursor.hdr.type    = CMD_MOVE_CURSOR;
    gpu.cursor.x           = x;
    gpu.cursor.y           = y;
    gpu.cursor.resource_id = CURSOR_RESOURCE;
    cursor_command();
    spin_unlock(&gpu_lock, flags);
}

/* Not shown: an update with no picture, which QEMU takes as hidden. */
void virtio_gpu_cursor_hide(void)
{
    unsigned long flags;

    if (!gpu.present || !gpu.cursor_shown) {
        return;
    }

    flags = spin_lock(&gpu_lock);
    memset(&gpu.cursor, 0, sizeof(gpu.cursor));
    gpu.cursor.hdr.type = CMD_UPDATE_CURSOR;
    cursor_command();
    gpu.cursor_shown = false;
    spin_unlock(&gpu_lock, flags);
}
