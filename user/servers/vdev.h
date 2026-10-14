/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_VDEV_H
#define USER_SERVERS_VDEV_H

/*
 * **A virtio device on virtio-mmio, from the device's side** - the
 * transport every device a sandbox's monitor offers stands on: virtio-fs
 * for its folders (`vmmfs.c`), virtio-net for its network (`vmmnet.c`).
 * VIRTIO 1.2: the registers 4.2.2, the split queues 2.7.
 *
 * A device is a `struct vdev`: its number, its features, its queues and
 * the two things only it knows - what its configuration space says, and
 * what to do when the guest says a queue has work. Everything else - the
 * registers' state, the queues' addresses, the interrupt status - is here,
 * once. The guest's RAM is read and written where the monitor maps it.
 */

#include <stdbool.h>
#include <stdint.h>

#define VDEV_QUEUES     4u
#define VDEV_QUEUE_MOST 256u
#define VDEV_SEGMENTS   64u

struct vdev_queue {
    uint32_t num;
    bool     ready;
    uint64_t desc, avail, used;
    uint16_t last;                          /* the next available entry to take */
};

struct vdev {
    uint32_t id;                            /* 26 virtio-fs, 1 virtio-net */
    uint64_t features;                      /* offered; VERSION_1 always */
    unsigned queues;
    void    *owner;                         /* the device's own state */

    /* Its configuration space, from `off`: `size` bytes into `out`. */
    void   (*config)(struct vdev *d, uint64_t off, uint8_t *out, unsigned size);

    /* Its configuration space written, from `off`: `size` bytes of `in`.
     * NULL: nothing there is written. */
    void   (*config_write)(struct vdev *d, uint64_t off, const uint8_t *in, unsigned size);

    /* The guest said queue `q` has work. */
    void   (*notify)(struct vdev *d, unsigned q);

    uint32_t status, int_status, dev_sel, drv_sel, sel;
    struct vdev_queue q[VDEV_QUEUES];
};

/*
 * One request off a queue: its head, and its buffers - those the device
 * reads (`in`, the guest wrote them) and those it writes (`out`).
 */
struct vdev_chain {
    uint16_t head;
    unsigned nin, nout;
    uint8_t *in[VDEV_SEGMENTS];
    uint32_t in_len[VDEV_SEGMENTS];
    uint8_t *out[VDEV_SEGMENTS];
    uint32_t out_len[VDEV_SEGMENTS];
};

/* Where the guest's RAM is in this process, and at which guest address. */
void vdev_memory(uint8_t *ram, uint64_t bytes, uint64_t base);

/* The guest's memory at `gpa`, `len` bytes of it, or NULL outside its RAM:
 * for a device whose buffers are named by guest address in a request
 * rather than by the queue - virtio-gpu's backing pages. */
uint8_t *vdev_guest(uint64_t gpa, uint64_t len);

/* A register access to `d`: answers false for one it has no register at. */
bool vdev_mmio(struct vdev *d, uint64_t off, unsigned size, bool write, uint64_t *value);

/* The next request on queue `q`, or false when there is none. */
bool vdev_pop(struct vdev *d, unsigned q, struct vdev_chain *c);

/* Whether queue `q` has a request waiting, without taking it. */
bool vdev_waiting(struct vdev *d, unsigned q);

/* A request answered: `written` bytes into its `out` buffers; the guest's
 * interrupt raised. */
void vdev_push(struct vdev *d, unsigned q, uint16_t head, uint32_t written);

/* `n` bytes of `from` scattered into a chain's `out` buffers: how many fit. */
uint32_t vdev_scatter(const struct vdev_chain *c, const uint8_t *from, uint32_t n);

/* A chain's `in` buffers gathered into `to`, at most `room`: how many. */
uint32_t vdev_gather(const struct vdev_chain *c, uint8_t *to, uint32_t room);

/* Whether its interrupt should be raised. */
bool vdev_raised(const struct vdev *d);

/* Little-endian, as virtio's own fields are. */
static inline uint16_t vget16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t vget32(const uint8_t *p) { return (uint32_t)vget16(p) | (uint32_t)vget16(p + 2) << 16; }
static inline uint64_t vget64(const uint8_t *p) { return (uint64_t)vget32(p) | (uint64_t)vget32(p + 4) << 32; }
static inline void vput16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void vput32(uint8_t *p, uint32_t v) { vput16(p, (uint16_t)v); vput16(p + 2, (uint16_t)(v >> 16)); }
static inline void vput64(uint8_t *p, uint64_t v) { vput32(p, (uint32_t)v); vput32(p + 4, (uint32_t)(v >> 32)); }

#endif /* USER_SERVERS_VDEV_H */
