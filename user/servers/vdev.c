/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A virtio device on virtio-mmio, from the device's side - `vdev.h` has
 * the argument. The registers (VIRTIO 1.2, 4.2.2) and the split queues
 * (2.7), once, for every device a sandbox's monitor offers.
 *
 * Everything here runs on the vCPU's thread while the guest is stopped at
 * the exit that brought it here, so the guest touches none of the queues
 * at the same moment and no barrier is wanted between the two.
 */

#include <string.h>

#include "vdev.h"

#define MAGIC           0x000
#define VERSION         0x004
#define DEVICE_ID       0x008
#define VENDOR_ID       0x00c
#define DEV_FEATURES    0x010
#define DEV_FEAT_SEL    0x014
#define DRV_FEATURES    0x020
#define DRV_FEAT_SEL    0x024
#define QUEUE_SEL       0x030
#define QUEUE_NUM_MAX   0x034
#define QUEUE_NUM       0x038
#define QUEUE_READY     0x044
#define QUEUE_NOTIFY    0x050
#define INT_STATUS      0x060
#define INT_ACK         0x064
#define STATUS          0x070
#define DESC_LOW        0x080
#define DESC_HIGH       0x084
#define AVAIL_LOW       0x090
#define AVAIL_HIGH      0x094
#define USED_LOW        0x0a0
#define USED_HIGH       0x0a4
#define CONFIG_GEN      0x0fc
#define CONFIG          0x100

#define SHM_LEN_LOW     0x0b0u
#define SHM_LEN_HIGH    0x0b4u
#define SHM_BASE_LOW    0x0b8u
#define SHM_BASE_HIGH   0x0bcu

#define DESC_NEXT       1u
#define DESC_WRITE      2u

#define VIRTIO_F_VERSION_1  (1ull << 32)

static uint8_t *ram;
static uint64_t ram_bytes, ram_base;

void vdev_memory(uint8_t *guest_ram, uint64_t bytes, uint64_t base)
{
    ram = guest_ram;
    ram_bytes = bytes;
    ram_base = base;
}

/* The guest's memory at `gpa`, `len` bytes of it, or NULL outside its RAM. */
uint8_t *vdev_guest(uint64_t gpa, uint64_t len)
{
    if (ram == NULL || gpa < ram_base || gpa - ram_base > ram_bytes
        || len > ram_bytes - (gpa - ram_base)) {
        return NULL;
    }

    return ram + (gpa - ram_base);
}

static void reset(struct vdev *d)
{
    d->status = d->int_status = 0;
    d->dev_sel = d->drv_sel = d->sel = 0;
    memset(d->q, 0, sizeof(d->q));
}

bool vdev_mmio(struct vdev *d, uint64_t off, unsigned size, bool write, uint64_t *value)
{
    struct vdev_queue *q = &d->q[d->sel < d->queues ? d->sel : 0];
    uint64_t offered = d->features | VIRTIO_F_VERSION_1;

    if (off >= CONFIG) {
        uint8_t bytes[8];
        unsigned k;

        if (write || size > sizeof(bytes)) {
            return true;                    /* nothing here is written */
        }

        memset(bytes, 0, sizeof(bytes));
        d->config(d, off - CONFIG, bytes, size);
        *value = 0;

        for (k = 0; k < size; k++) {
            *value |= (uint64_t)bytes[k] << (8 * k);
        }

        return true;
    }

    if (write) {
        uint32_t v = (uint32_t)*value;

        switch (off) {
        case DEV_FEAT_SEL: d->dev_sel = v; break;
        case DRV_FEAT_SEL: d->drv_sel = v; break;
        case QUEUE_SEL:    d->sel = v; break;
        case QUEUE_NUM:    q->num = v <= VDEV_QUEUE_MOST ? v : VDEV_QUEUE_MOST; break;
        case QUEUE_READY:  q->ready = v != 0; break;
        case QUEUE_NOTIFY:
            if (v < d->queues && d->notify != NULL) {
                d->notify(d, v);
            }
            break;
        case INT_ACK:      d->int_status &= ~v; break;
        case STATUS:
            if (v == 0) {
                reset(d);
            } else {
                d->status = v;
            }
            break;
        case DESC_LOW:     q->desc = (q->desc & ~0xffffffffull) | v; break;
        case DESC_HIGH:    q->desc = (q->desc & 0xffffffffull) | (uint64_t)v << 32; break;
        case AVAIL_LOW:    q->avail = (q->avail & ~0xffffffffull) | v; break;
        case AVAIL_HIGH:   q->avail = (q->avail & 0xffffffffull) | (uint64_t)v << 32; break;
        case USED_LOW:     q->used = (q->used & ~0xffffffffull) | v; break;
        case USED_HIGH:    q->used = (q->used & 0xffffffffull) | (uint64_t)v << 32; break;
        default:           break;           /* the driver's features: what was offered */
        }

        return true;
    }

    switch (off) {
    case MAGIC:         *value = 0x74726976; break;         /* "virt" */
    case VERSION:       *value = 2; break;
    case DEVICE_ID:     *value = d->id; break;
    case VENDOR_ID:     *value = 0x534f4d4b; break;         /* "KMOS" */
    case DEV_FEATURES:  *value = d->dev_sel < 2 ? (uint32_t)(offered >> (32 * d->dev_sel)) : 0; break;
    case QUEUE_NUM_MAX: *value = d->sel < d->queues ? VDEV_QUEUE_MOST : 0; break;
    case QUEUE_READY:   *value = q->ready ? 1 : 0; break;
    case INT_STATUS:    *value = d->int_status; break;
    case STATUS:        *value = d->status; break;
    case CONFIG_GEN:    *value = 0; break;
    /*
     * **No shared memory region**, which virtio-mmio says as a length of
     * all ones (VIRTIO 1.2 4.2.2, SHMLen): a driver asks every device, and
     * virtio-gpu read the 0 this used to answer as a region of no bytes at
     * 0, failed to reserve it, and gave up (`docs/linux.md` L2).
     */
    case SHM_LEN_LOW:
    case SHM_LEN_HIGH:
    case SHM_BASE_LOW:
    case SHM_BASE_HIGH: *value = 0xffffffffu; break;
    default:            *value = 0; break;
    }

    return true;
}

static bool queue_in_memory(struct vdev_queue *q, uint8_t **avail, uint8_t **used, uint8_t **desc)
{
    if (!q->ready || q->num == 0) {
        return false;
    }

    *avail = vdev_guest(q->avail, 4 + 2 * (uint64_t)q->num);
    *used = vdev_guest(q->used, 4 + 8 * (uint64_t)q->num);
    *desc = vdev_guest(q->desc, 16 * (uint64_t)q->num);
    return *avail != NULL && *used != NULL && *desc != NULL;
}

bool vdev_waiting(struct vdev *d, unsigned qi)
{
    struct vdev_queue *q = qi < d->queues ? &d->q[qi] : NULL;
    uint8_t *avail, *used, *desc;

    return q != NULL && queue_in_memory(q, &avail, &used, &desc) && q->last != vget16(avail + 2);
}

bool vdev_pop(struct vdev *d, unsigned qi, struct vdev_chain *c)
{
    struct vdev_queue *q = qi < d->queues ? &d->q[qi] : NULL;
    uint8_t *avail, *used, *desc;
    uint16_t at;
    unsigned hops = 0;

    if (q == NULL || !queue_in_memory(q, &avail, &used, &desc) || q->last == vget16(avail + 2)) {
        return false;
    }

    c->head = vget16(avail + 4 + 2 * (q->last % q->num));
    c->nin = c->nout = 0;
    q->last++;
    at = c->head;

    for (;;) {
        uint8_t *e = desc + 16 * (uint64_t)(at % q->num);
        uint32_t len = vget32(e + 8);
        uint16_t flags = vget16(e + 12);
        uint8_t *p = vdev_guest(vget64(e), len);

        if (p != NULL) {
            if ((flags & DESC_WRITE) != 0 && c->nout < VDEV_SEGMENTS) {
                c->out[c->nout] = p;
                c->out_len[c->nout++] = len;
            } else if ((flags & DESC_WRITE) == 0 && c->nin < VDEV_SEGMENTS) {
                c->in[c->nin] = p;
                c->in_len[c->nin++] = len;
            }
        }

        if ((flags & DESC_NEXT) == 0 || ++hops >= q->num) {
            break;
        }

        at = vget16(e + 14);
    }

    (void)used;
    return true;
}

void vdev_push(struct vdev *d, unsigned qi, uint16_t head, uint32_t written)
{
    struct vdev_queue *q = qi < d->queues ? &d->q[qi] : NULL;
    uint8_t *avail, *used, *desc;
    uint16_t idx;

    if (q == NULL || !queue_in_memory(q, &avail, &used, &desc)) {
        return;
    }

    idx = vget16(used + 2);
    vput32(used + 4 + 8 * (idx % q->num), head);
    vput32(used + 4 + 8 * (idx % q->num) + 4, written);
    vput16(used + 2, (uint16_t)(idx + 1));
    d->int_status |= 1;
}

uint32_t vdev_scatter(const struct vdev_chain *c, const uint8_t *from, uint32_t n)
{
    uint32_t done = 0;
    unsigned i;

    for (i = 0; i < c->nout && done < n; i++) {
        uint32_t k = n - done < c->out_len[i] ? n - done : c->out_len[i];

        memcpy(c->out[i], from + done, k);
        done += k;
    }

    return done;
}

uint32_t vdev_gather(const struct vdev_chain *c, uint8_t *to, uint32_t room)
{
    uint32_t done = 0;
    unsigned i;

    for (i = 0; i < c->nin && done < room; i++) {
        uint32_t k = room - done < c->in_len[i] ? room - done : c->in_len[i];

        memcpy(to + done, c->in[i], k);
        done += k;
    }

    return done;
}

bool vdev_raised(const struct vdev *d)
{
    return d->int_status != 0;
}
