/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A sandbox's keyboard and pointer: virtio-input (`vmminput.h`).
 *
 * Two devices, as QEMU's are: a keyboard, and a tablet - an absolute
 * pointer, because the window knows where the pointer is over the picture
 * and a mouse's movements would have to be steered there. Each tells the
 * guest what it is through its configuration space, asked by `select` and
 * `subsel` (VIRTIO 1.2 §5.8.2): its name, its ids, the events it can send
 * and, for the tablet, the range of each axis.
 *
 * Events come from the window through the ring in the screen region's
 * first page (`vmmscreen.h`) - Linux's own `input_event`s, already in the
 * guest's codes - and each goes into a buffer the guest has put on that
 * device's event queue. One taken when a buffer is there and not before,
 * in the order they came: a ring full of events for a guest that is not
 * reading them waits rather than losing one out of order.
 */

#include <stddef.h>
#include <string.h>

#include "vdev.h"
#include "vmminput.h"
#include "vmmscreen.h"

#define VIRTIO_ID_INPUT     18u

#define CFG_UNSET           0x00u
#define CFG_ID_NAME         0x01u
#define CFG_ID_DEVIDS       0x03u
#define CFG_EV_BITS         0x11u
#define CFG_ABS_INFO        0x12u

#define EV_KEY              0x01u
#define EV_REL              0x02u
#define EV_ABS              0x03u
#define REL_WHEEL           0x08u
#define BTN_LEFT            0x110u
#define BTN_MIDDLE          0x112u

struct device {
    struct vdev dev;
    const char *name;
    uint16_t    product;
    uint8_t     select, subsel;
};

static struct device D[VMM_INPUT_DEVICES];
static struct vmm_input_ring *ring;
static bool on;

static void bit(uint8_t *bits, unsigned n)
{
    bits[n / 8] = (uint8_t)(bits[n / 8] | 1u << (n % 8));
}

/* What `select` and `subsel` ask, into `data`; its size. */
static uint8_t answer(const struct device *d, uint8_t *data)
{
    bool keyboard = d == &D[VMM_INPUT_KEYBOARD];
    uint8_t size = 0;

    memset(data, 0, 128);

    switch (d->select) {
    case CFG_ID_NAME:
        size = (uint8_t)strlen(d->name);
        memcpy(data, d->name, size);
        break;
    case CFG_ID_DEVIDS:
        data[0] = 0x06;                     /* BUS_VIRTUAL */
        data[2] = 0x4b;                     /* "K" */
        data[4] = (uint8_t)d->product;
        data[6] = 1;
        size = 8;
        break;
    case CFG_EV_BITS:
        if (keyboard && d->subsel == EV_KEY) {
            for (unsigned k = 1; k < 248; k++) bit(data, k);
            size = 31;
        } else if (!keyboard && d->subsel == EV_KEY) {
            for (unsigned k = BTN_LEFT; k <= BTN_MIDDLE; k++) bit(data, k);
            size = BTN_MIDDLE / 8 + 1;
        } else if (!keyboard && d->subsel == EV_ABS) {
            bit(data, 0);                   /* ABS_X */
            bit(data, 1);                   /* ABS_Y */
            size = 1;
        } else if (!keyboard && d->subsel == EV_REL) {
            bit(data, REL_WHEEL);
            size = 2;
        }
        break;
    case CFG_ABS_INFO:
        if (!keyboard && d->subsel <= 1) {
            uint32_t most = VMM_INPUT_ABS_MOST;

            memcpy(data + 4, &most, 4);     /* min 0, max, fuzz, flat, res 0 */
            size = 20;
        }
        break;
    default:
        break;
    }

    return size;
}

static void config(struct vdev *v, uint64_t off, uint8_t *out, unsigned size)
{
    struct device *d = (struct device *)v->owner;
    uint8_t cfg[136];

    memset(cfg, 0, sizeof cfg);
    cfg[0] = d->select;
    cfg[1] = d->subsel;
    cfg[2] = answer(d, cfg + 8);

    for (unsigned k = 0; k < size; k++) {
        out[k] = off + k < sizeof cfg ? cfg[off + k] : 0;
    }
}

static void config_write(struct vdev *v, uint64_t off, const uint8_t *in, unsigned size)
{
    struct device *d = (struct device *)v->owner;

    for (unsigned k = 0; k < size; k++) {
        if (off + k == 0) d->select = in[k];
        if (off + k == 1) d->subsel = in[k];
    }
}

/* The status queue: LEDs the guest sets, taken and let go. */
static void notify(struct vdev *v, unsigned q)
{
    struct vdev_chain c;

    if (q == 1) {
        while (vdev_pop(v, q, &c)) {
            vdev_push(v, q, c.head, 0);
        }
    }

    vmminput_pump();
}

bool vmminput_start(void *screen_page)
{
    static const char *const names[VMM_INPUT_DEVICES] = { "Kosmos keyboard", "Kosmos tablet" };

    ring = (struct vmm_input_ring *)((uint8_t *)screen_page + VMM_INPUT_AT);

    for (unsigned i = 0; i < VMM_INPUT_DEVICES; i++) {
        memset(&D[i], 0, sizeof D[i]);
        D[i].name = names[i];
        D[i].product = (uint16_t)(i + 1);
        D[i].dev.id = VIRTIO_ID_INPUT;
        D[i].dev.queues = 2;
        D[i].dev.owner = &D[i];
        D[i].dev.config = config;
        D[i].dev.config_write = config_write;
        D[i].dev.notify = notify;
    }

    on = true;
    return true;
}

void vmminput_pump(void)
{
    uint32_t tail, head;

    if (!on) {
        return;
    }

    tail = ring->tail;
    head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

    while (tail != head) {
        const struct vmm_input_event *e = &ring->ev[tail % VMM_INPUT_EVENTS];
        struct vdev_chain c;
        uint8_t out[8];

        if (e->device >= VMM_INPUT_DEVICES) {
            tail++;
            continue;
        }

        if (!vdev_pop(&D[e->device].dev, 0, &c)) {
            break;                          /* the guest has no buffer yet */
        }

        memcpy(out, &e->type, 2);
        memcpy(out + 2, &e->code, 2);
        memcpy(out + 4, &e->value, 4);
        vdev_push(&D[e->device].dev, 0, c.head, vdev_scatter(&c, out, sizeof out));
        tail++;
    }

    __atomic_store_n(&ring->tail, tail, __ATOMIC_RELEASE);
}

bool vmminput_on(void)
{
    return on;
}

bool vmminput_mmio(unsigned i, uint64_t off, unsigned size, bool write, uint64_t *value)
{
    return on && i < VMM_INPUT_DEVICES && vdev_mmio(&D[i].dev, off, size, write, value);
}

bool vmminput_raised(unsigned i)
{
    return on && i < VMM_INPUT_DEVICES && vdev_raised(&D[i].dev);
}
