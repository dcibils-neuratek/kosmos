/*
 * A keyboard: virtio-input over virtio-mmio.
 *
 * `virt` has no PS/2 controller, so a real key press has to come from either
 * USB or virtio. **virtio-mmio rather than virtio-pci** is what makes this a
 * few hundred lines instead of the best part of a thousand: there is no PCI
 * bus to enumerate, no ECAM window to walk and no capability list to parse.
 * There are thirty-two fixed windows in the memory map, each with a magic
 * number and a device id at the top of it, and the whole of discovery is
 * reading two registers thirty-two times.
 *
 * Read out of QEMU's own device tree rather than remembered:
 *
 *     virtio_mmio@a000000, reg = <0x0 0xa000000 0x0 0x200>
 *     ... through virtio_mmio@a003e00
 *
 * so thirty-two windows, 0x200 apart. Every register offset below is from
 * include/standard-headers/linux/virtio_mmio.h at the tag this QEMU was
 * built from, and the device id, status bits, feature bit, ring layout and
 * event structure likewise from virtio_ids.h, virtio_config.h,
 * virtio_ring.h and virtio_input.h.
 *
 * **Polled, not interrupt-driven.** The UART is polled too, and the console
 * server already yields between polls, so a key waiting in the used ring is
 * found on the same schedule a character in the UART is. `roadmap.md` wants
 * input on a highest-priority thread eventually, and that is the point at
 * which the interrupt matters; wiring one now would add a GIC route and a
 * handler to solve a problem the system does not have yet.
 *
 * **And the next device is nearly free.** Everything above the last two
 * functions is the transport, and virtio-gpu is the same transport with a
 * different device id and different commands - which is where a real
 * dirty-rectangle flush and a vblank come from. The transport is not split
 * into its own file yet, because there is one device: splitting it now would
 * be inventing an interface against a single caller, which is the mistake
 * `hal.md` spends a page warning about. It comes out when the GPU arrives.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mmio.h"
#include "hal.h"
#include "qemu-virt.h"

/* include/standard-headers/linux/virtio_mmio.h */
#define REG_MAGIC               0x000
#define REG_VERSION             0x004
#define REG_DEVICE_ID           0x008
#define REG_DEVICE_FEATURES     0x010
#define REG_DEVICE_FEATURES_SEL 0x014
#define REG_DRIVER_FEATURES     0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL           0x030
#define REG_QUEUE_NUM_MAX       0x034
#define REG_QUEUE_NUM           0x038
#define REG_QUEUE_READY         0x044
#define REG_QUEUE_NOTIFY        0x050
#define REG_INTERRUPT_STATUS    0x060
#define REG_INTERRUPT_ACK       0x064
#define REG_STATUS              0x070
#define REG_QUEUE_DESC_LOW      0x080
#define REG_QUEUE_DESC_HIGH     0x084
#define REG_QUEUE_AVAIL_LOW     0x090
#define REG_QUEUE_AVAIL_HIGH    0x094
#define REG_QUEUE_USED_LOW      0x0a0
#define REG_QUEUE_USED_HIGH     0x0a4

#define VIRTIO_MAGIC        0x74726976u     /* 'virt', little endian */
#define VIRTIO_VERSION_1    2               /* the modern interface */

/* virtio_ids.h */
#define VIRTIO_ID_INPUT     18

/* virtio_config.h */
#define STATUS_ACKNOWLEDGE  1u
#define STATUS_DRIVER       2u
#define STATUS_DRIVER_OK    4u
#define STATUS_FEATURES_OK  8u
#define STATUS_FAILED       0x80u

/* Feature bit 32. Split across the two 32-bit feature windows, so it is bit
 * 0 of the high one - which is the entire reason those selector registers
 * exist and the one detail of feature negotiation that catches people. */
#define FEATURE_VERSION_1_BIT   0u

/* virtio_ring.h */
#define VRING_DESC_F_WRITE  2u

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

/*
 * How many events can be outstanding.
 *
 * A key press is eight bytes and the console reads far faster than anyone
 * types, so this only has to cover a burst. Sixteen is four keys' worth of
 * press, release and the EV_SYN that follows each, which is more than a
 * person produces between two polls.
 */
#define QUEUE_SIZE  16

/* virtio_input.h */
struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

/* virtio_mmio.h: the device's own configuration space. */
#define REG_CONFIG              0x100
#define CFG_SELECT              0       /* uint8: which field is being asked */
#define CFG_SUBSEL              1       /* uint8: which part of it */
#define CFG_SIZE                2       /* uint8: how many bytes came back */
#define CFG_DATA                8       /* the union */

/* virtio_input.h */
#define VIRTIO_INPUT_CFG_EV_BITS    0x11
#define VIRTIO_INPUT_CFG_ABS_INFO   0x12

/* input-event-codes.h */
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_ABS          0x03
#define ABS_X           0x00
#define ABS_Y           0x01
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
/*
 * The keys that are not characters.
 *
 * A keymap has one byte per keycode, and an arrow is not one byte: over a
 * serial line it arrives as escape, '[', and a letter, because that is what
 * a terminal sends. On a real keyboard it is a single keycode with no
 * character at all - so with a keymap alone, `keymap_plain[108]` is zero,
 * the key produces nothing, and arrows work over the cable and do nothing
 * in the window.
 *
 * That is exactly what happened, and it hid for a while: every automated
 * check types over the serial line, where arrows had always worked.
 */
#define KEY_HOME        102
#define KEY_UP          103
#define KEY_PAGEUP      104
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_END         107
#define KEY_DOWN        108
#define KEY_PAGEDOWN    109
#define KEY_DELETE      111

#define KEY_LEFTCTRL    29
#define KEY_LEFTSHIFT   42
#define KEY_RIGHTSHIFT  54
#define KEY_CAPSLOCK    58
#define KEY_RIGHTCTRL   97

/*
 * The split virtqueue.
 *
 * One structure rather than three allocations because there is no allocator:
 * this is `.bss`, which is identity mapped, so its virtual address is its
 * physical one and the device can be handed a pointer straight out of it.
 * That equality is load-bearing and stops being true the day the kernel
 * moves to TTBR1.
 *
 * The three parts get their own address registers, so they need not be
 * adjacent - only aligned. 16 for the descriptor table, 2 for the available
 * ring, 4 for the used ring, which is what the alignment attributes below
 * are for.
 *
 * The memory is Normal cached and the device reads it directly. Under QEMU
 * that is fine because TCG does not model caches; a real device on a real
 * board would want this non-cacheable, and that is a difference worth
 * knowing about before this file is pointed at hardware.
 */
struct vqueue {
    _Alignas(16) struct vring_desc desc[QUEUE_SIZE];

    _Alignas(2) struct {
        uint16_t flags;
        uint16_t idx;
        uint16_t ring[QUEUE_SIZE];
        uint16_t used_event;
    } avail;

    _Alignas(4) struct {
        uint16_t flags;
        uint16_t idx;
        struct {
            uint32_t id;
            uint32_t len;
        } ring[QUEUE_SIZE];
        uint16_t avail_event;
    } used;
};

/*
 * One of these per device.
 *
 * This file used to hold a single set of globals, because there was a single
 * device. There are two now - a keyboard and a tablet - and they are the
 * *same kind* of device: both are virtio-input, both answer to device id 18,
 * and the only way to tell them apart is to ask each one whether it has
 * absolute axes. So the ring, the buffers and the position in the used ring
 * all belong to an instance rather than to the file.
 *
 * `.bss`, and that is load-bearing rather than incidental: it is identity
 * mapped, so the virtual address of a queue is its physical address and the
 * device can be handed a pointer straight out of it. That equality stops
 * being true the day the kernel moves to TTBR1.
 */
struct vinput {
    uintptr_t base;                 /* the window it was found in */
    unsigned  slot;                 /* which one, for its interrupt */
    bool      present;
    uint16_t  last_used;            /* how far through the used ring we are */

    struct vqueue queue;
    struct virtio_input_event events[QUEUE_SIZE];
};

static struct vinput keyboard;
static struct vinput tablet;

/*
 * Set by an interrupt, cleared by whoever was waiting.
 *
 * A flag rather than a queue: everything above this wants to know "is there
 * anything", not "what exactly", and the events themselves are already in a
 * virtqueue that survives until somebody reads it.
 */
static volatile bool input_arrived;

/*
 * Bytes waiting to come out of `hal_getchar`, most recent last.
 *
 * One keypress can be more than one character. Every key that is not a
 * character is turned into the escape sequence a terminal would have sent
 * for it, so that everything above this layer sees one input language
 * rather than two - the shell's line editor, the window manager and the
 * widget kit all already understand `ESC [ A`, because that is what arrives
 * over the cable.
 *
 * Translating here rather than above is what keeps that true. The
 * alternative is a second set of key codes that only exist on a real
 * keyboard, and then every consumer has to know about both.
 */
static char     pending[8];
static unsigned pending_len;
static unsigned pending_at;

static void queue(const char *s)
{
    unsigned n = 0;

    while (s[n] != '\0' && n < sizeof(pending)) {
        pending[n] = s[n];
        n++;
    }

    pending_len = n;
    pending_at = 0;
}

/* The escape sequence for a key that is not a character, or NULL. */
static const char *sequence_for(unsigned code)
{
    switch (code) {
    case KEY_UP:       return "\x1b[A";
    case KEY_DOWN:     return "\x1b[B";
    case KEY_RIGHT:    return "\x1b[C";
    case KEY_LEFT:     return "\x1b[D";
    case KEY_HOME:     return "\x1b[H";
    case KEY_END:      return "\x1b[F";
    case KEY_PAGEUP:   return "\x1b[5~";
    case KEY_PAGEDOWN: return "\x1b[6~";
    case KEY_DELETE:   return "\x1b[3~";
    default:           return NULL;
    }
}

/* Modifier state, which belongs to the keyboard and to nothing else. */
static bool      shift;
static bool      ctrl;
static bool      caps;

/*
 * Where the pointer is, rebuilt from the events as they arrive.
 *
 * A tablet reports absolute position, so this is the position rather than a
 * delta - which is the right kind of device for a virtual machine, because
 * there is no acceleration curve to agree on with the host and the guest
 * cursor cannot drift away from the real one.
 *
 * `moved` is set by a SYN_REPORT and cleared when somebody looks, so a
 * caller can tell "nothing has happened" from "it is still where it was".
 */
static struct {
    uint32_t x, y;
    uint32_t min_x, max_x;
    uint32_t min_y, max_y;
    uint32_t buttons;
    bool     moved;
} cursor;

static const unsigned char keymap_plain[128] = {
    0x00, 0x1b, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,   /*   0  esc 1234567 */
    0x37, 0x38, 0x39, 0x30, 0x2d, 0x3d, 0x08, 0x09,   /*   8  890-= bs tab */
    0x71, 0x77, 0x65, 0x72, 0x74, 0x79, 0x75, 0x69,   /*  16  qwertyui */
    0x6f, 0x70, 0x5b, 0x5d, 0x0a, 0x00, 0x61, 0x73,   /*  24  op[] enter ctrl as */
    0x64, 0x66, 0x67, 0x68, 0x6a, 0x6b, 0x6c, 0x3b,   /*  32  dfghjkl; */
    0x27, 0x60, 0x00, 0x5c, 0x7a, 0x78, 0x63, 0x76,   /*  40  '` shift \ zxcv */
    0x62, 0x6e, 0x6d, 0x2c, 0x2e, 0x2f, 0x00, 0x00,   /*  48  bnm,./ shift */
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  56  alt space caps */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  64 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  72 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  80 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  88 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  96 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 104 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 112 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 120 */
};

static const unsigned char keymap_shift[128] = {
    0x00, 0x1b, 0x21, 0x40, 0x23, 0x24, 0x25, 0x5e,   /*   0  esc !@#$%^ */
    0x26, 0x2a, 0x28, 0x29, 0x5f, 0x2b, 0x08, 0x09,   /*   8  &*()_+ bs tab */
    0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49,   /*  16  QWERTYUI */
    0x4f, 0x50, 0x7b, 0x7d, 0x0a, 0x00, 0x41, 0x53,   /*  24  OP{} enter ctrl AS */
    0x44, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c, 0x3a,   /*  32  DFGHJKL: */
    0x22, 0x7e, 0x00, 0x7c, 0x5a, 0x58, 0x43, 0x56,   /*  40  "~ shift | ZXCV */
    0x42, 0x4e, 0x4d, 0x3c, 0x3e, 0x3f, 0x00, 0x00,   /*  48  BNM<>? shift */
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  56  alt space caps */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  64 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  72 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  80 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  88 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  96 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 104 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 112 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 120 */
};

static uint32_t reg_read(const struct vinput *v, unsigned offset)
{
    return mmio_read32(v->base + offset);
}

static void reg_write(const struct vinput *v, unsigned offset, uint32_t value)
{
    mmio_write32(v->base + offset, value);
}

/*
 * Orders our writes to the ring against the register write that tells the
 * device to look at it.
 *
 * `oshst` and not `ishst`: the device is outside the inner shareable domain
 * the cores share, so an inner barrier would not order against it. This is
 * the same reasoning `mmio.h` gives, applied to memory the device reads
 * rather than to the register itself.
 */
static void publish(void)
{
    __asm__ volatile("dmb oshst" ::: "memory");
}

/* And the other direction: the used ring has to be in hand before anything
 * reads what it points at. */
static void consume(void)
{
    __asm__ volatile("dmb oshld" ::: "memory");
}

/* Hands descriptor `i` back to the device as somewhere to put an event. */
static void offer(struct vinput *v, unsigned i)
{
    uint16_t at = v->queue.avail.idx % QUEUE_SIZE;

    v->queue.desc[i].addr  = (uint64_t)(uintptr_t)&v->events[i];
    v->queue.desc[i].len   = sizeof(v->events[i]);
    v->queue.desc[i].flags = VRING_DESC_F_WRITE;   /* the device writes it */
    v->queue.desc[i].next  = 0;

    v->queue.avail.ring[at] = (uint16_t)i;

    /* The entry has to be visible before the index that publishes it. This
     * is the barrier that is easy to leave out and produces a device reading
     * a descriptor that has not been written yet. */
    publish();
    v->queue.avail.idx++;
}

/*
 * Does the device in this window have absolute axes?
 *
 * This is the whole of telling a keyboard from a tablet. Both are
 * virtio-input and both answer to device id 18; what differs is the set of
 * event types each says it can produce. Asking is a write of a selector and
 * a sub-selector into configuration space and a read of the length that
 * comes back: non-zero means the device has a bitmap of EV_ABS codes to
 * offer, and a keyboard has none.
 *
 * Bytes, not words. The three header fields are single bytes, and reading
 * them as one 32-bit access happens to work here and is allowed to fail on
 * a device that decodes the access width - which devices may do.
 */
static bool has_absolute_axes(uintptr_t base)
{
    mmio_write8(base + REG_CONFIG + CFG_SELECT, VIRTIO_INPUT_CFG_EV_BITS);
    mmio_write8(base + REG_CONFIG + CFG_SUBSEL, EV_ABS);

    return mmio_read8(base + REG_CONFIG + CFG_SIZE) != 0;
}

/* The range one axis reports in, so a caller can scale without guessing. */
static void absolute_range(uintptr_t base, uint8_t axis,
                           uint32_t *min, uint32_t *max)
{
    mmio_write8(base + REG_CONFIG + CFG_SELECT, VIRTIO_INPUT_CFG_ABS_INFO);
    mmio_write8(base + REG_CONFIG + CFG_SUBSEL, axis);

    /* struct virtio_input_absinfo: min, max, fuzz, flat, res. */
    *min = mmio_read32(base + REG_CONFIG + CFG_DATA);
    *max = mmio_read32(base + REG_CONFIG + CFG_DATA + 4);
}

/*
 * Finds a window holding the kind of input device we want, and brings it up.
 *
 * `want_absolute` is what separates the two callers, and the check for it
 * costs a partial bring-up: configuration space is read after the device has
 * been told a driver is present, so a window that turns out to hold the
 * wrong kind has been reset and acknowledged and then left alone. That is
 * harmless - claiming it later starts with another reset - and it is the
 * price of the devices being indistinguishable from outside.
 */
static bool claim(struct vinput *v, bool want_absolute)
{
    unsigned i;

    for (i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        uint32_t status;
        unsigned max;
        unsigned n;

        if (mmio_read32(base + REG_MAGIC) != VIRTIO_MAGIC) {
            continue;
        }

        /*
         * Version 2 is the modern interface. Version 1 is the legacy one,
         * with a different ring layout reached through QUEUE_PFN and
         * GUEST_PAGE_SIZE, and reading a legacy device with modern
         * structures produces garbage rather than an error.
         *
         * **QEMU's virtio-mmio defaults to legacy.** `virt` reports version
         * 1 unless the machine is started with
         * `-global virtio-mmio.force-legacy=false`, which is what the
         * Makefile passes and what Linux asks for too. Skipping rather than
         * failing is deliberate: this is a scan, and a legacy device in one
         * window says nothing about a modern one in another.
         */
        if (mmio_read32(base + REG_VERSION) != VIRTIO_VERSION_1) {
            continue;
        }

        if (mmio_read32(base + REG_DEVICE_ID) != VIRTIO_ID_INPUT) {
            continue;               /* zero means the window is empty */
        }

        /* Not somebody else's. Two devices of one kind would otherwise both
         * be answered by whichever asked first. */
        if ((keyboard.present && keyboard.base == base)
            || (tablet.present && tablet.base == base)) {
            continue;
        }

        v->base = base;
        v->slot = i;

        /*
         * The bring-up sequence, in the order the specification requires.
         * Each step is a promise to the device about what the driver has
         * done, and doing them out of order is a device that stays silent.
         */
        reg_write(v, REG_STATUS, 0);                       /* reset */
        reg_write(v, REG_STATUS, STATUS_ACKNOWLEDGE);
        reg_write(v, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

        if (has_absolute_axes(base) != want_absolute) {
            continue;               /* the other kind; leave it be */
        }

        /*
         * Feature negotiation, and the one bit that matters.
         *
         * VIRTIO_F_VERSION_1 is feature 32, which is why there are selector
         * registers: features are read and written thirty-two bits at a
         * time, so bit 32 is bit 0 of window 1. Without it the device speaks
         * the legacy layout and every structure here is the wrong shape.
         */
        reg_write(v, REG_DEVICE_FEATURES_SEL, 1);
        if ((reg_read(v, REG_DEVICE_FEATURES)
             & (1u << FEATURE_VERSION_1_BIT)) == 0) {
            reg_write(v, REG_STATUS, STATUS_FAILED);
            continue;
        }

        reg_write(v, REG_DRIVER_FEATURES_SEL, 1);
        reg_write(v, REG_DRIVER_FEATURES, 1u << FEATURE_VERSION_1_BIT);
        reg_write(v, REG_DRIVER_FEATURES_SEL, 0);
        reg_write(v, REG_DRIVER_FEATURES, 0);

        reg_write(v, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER
                                 | STATUS_FEATURES_OK);

        /* Read back rather than assumed: the device clears the bit to
         * refuse. */
        status = reg_read(v, REG_STATUS);
        if ((status & STATUS_FEATURES_OK) == 0) {
            reg_write(v, REG_STATUS, STATUS_FAILED);
            continue;
        }

        /* Queue 0 is the event queue. Queue 1 carries status back to the
         * device - LEDs and such - and there is nothing this system wants to
         * say. */
        reg_write(v, REG_QUEUE_SEL, 0);

        max = reg_read(v, REG_QUEUE_NUM_MAX);
        if (max == 0 || max < QUEUE_SIZE) {
            reg_write(v, REG_STATUS, STATUS_FAILED);
            continue;               /* absent, or smaller than we are built for */
        }

        reg_write(v, REG_QUEUE_NUM, QUEUE_SIZE);

        reg_write(v, REG_QUEUE_DESC_LOW,
                  (uint32_t)(uintptr_t)&v->queue.desc);
        reg_write(v, REG_QUEUE_DESC_HIGH,
                  (uint32_t)((uint64_t)(uintptr_t)&v->queue.desc >> 32));
        reg_write(v, REG_QUEUE_AVAIL_LOW,
                  (uint32_t)(uintptr_t)&v->queue.avail);
        reg_write(v, REG_QUEUE_AVAIL_HIGH,
                  (uint32_t)((uint64_t)(uintptr_t)&v->queue.avail >> 32));
        reg_write(v, REG_QUEUE_USED_LOW,
                  (uint32_t)(uintptr_t)&v->queue.used);
        reg_write(v, REG_QUEUE_USED_HIGH,
                  (uint32_t)((uint64_t)(uintptr_t)&v->queue.used >> 32));

        reg_write(v, REG_QUEUE_READY, 1);

        reg_write(v, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER
                                 | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

        /* Every buffer offered at once, because a device with nowhere to put
         * an event drops it, and a dropped key is a key the person
         * pressed. */
        for (n = 0; n < QUEUE_SIZE; n++) {
            offer(v, n);
        }

        publish();
        reg_write(v, REG_QUEUE_NOTIFY, 0);

        /*
         * And its interrupt. Until this, every reader of this device had to
         * come back and ask - which is what a window manager polling a
         * keyboard in a loop is, and why an idle desktop kept a core at a
         * hundred per cent.
         */
        gic_enable_spi(VIRTIO_INTID_BASE + v->slot);

        v->present = true;
        return true;
    }

    return false;
}

/*
 * The next event this device has finished with, or false when there is none.
 *
 * The buffer is handed straight back before the event is looked at, because
 * every caller of this returns from inside its loop and a descriptor that
 * was not returned is one the device cannot write to again.
 */
static bool next_event(struct vinput *v, struct virtio_input_event *out)
{
    unsigned slot;

    if (!v->present) {
        return false;
    }

    consume();

    if (v->queue.used.idx == v->last_used) {
        return false;
    }

    slot = v->queue.used.ring[v->last_used % QUEUE_SIZE].id % QUEUE_SIZE;
    *out = v->events[slot];

    v->last_used++;

    offer(v, slot);
    publish();
    reg_write(v, REG_QUEUE_NOTIFY, 0);

    return true;
}

/*
 * A device has events waiting.
 *
 * The interrupt is acknowledged here and nothing is decoded: reading the
 * queue happens in whatever thread asked for input, in its own time, and
 * doing it in the handler would mean the keymap and the cursor state being
 * touched from an interrupt.
 *
 * What this is *for* is the wakeup. A thread sleeping for input is asleep
 * with a deadline; this drops the deadline to now, so the key is noticed at
 * interrupt speed rather than at the end of whatever the sleeper asked for.
 */
void input_interrupt(unsigned slot)
{
    struct vinput *v = NULL;

    if (keyboard.present && keyboard.slot == slot) {
        v = &keyboard;
    } else if (tablet.present && tablet.slot == slot) {
        v = &tablet;
    }

    if (v == NULL) {
        return;
    }

    /* The device raised it; the device is told it was seen. Without the ack
     * the status bit stays set and the interrupt fires for ever. */
    reg_write(v, REG_INTERRUPT_ACK, reg_read(v, REG_INTERRUPT_STATUS));

    input_arrived = true;
}

bool hal_keyboard_init(void)
{
    /*
     * Idempotent, and it has to be said rather than assumed. Running the
     * sequence again would write 0 to STATUS, which resets the device and
     * puts its used index back to zero - while `last_used` here kept
     * counting. The two would disagree for the next sixty-five thousand
     * events, which is to say for ever, and every key would be read out of
     * the wrong slot.
     */
    if (keyboard.present) {
        return true;
    }

    /* Without absolute axes: a keyboard. Started without
     * `-device virtio-keyboard-device` there is simply none. */
    return claim(&keyboard, false);
}

bool hal_pointer_init(void)
{
    if (tablet.present) {
        return true;
    }

    if (!claim(&tablet, true)) {
        return false;               /* no -device virtio-tablet-device */
    }

    /*
     * Where it will report. A tablet's range is its own business and is not
     * the screen's - QEMU's is 0 to 32767 on both axes whatever the display
     * is - so it is read and handed on rather than assumed, and the scaling
     * happens wherever somebody knows how big the screen is.
     *
     * That is the same division the rest of this layer draws: `sysinfo`
     * passes the processor's ID registers out undecoded, because what they
     * mean is not the kernel's business.
     */
    absolute_range(tablet.base, ABS_X, &cursor.min_x, &cursor.max_x);
    absolute_range(tablet.base, ABS_Y, &cursor.min_y, &cursor.max_y);

    /* Start in the middle rather than at a corner, so a cursor exists before
     * the first movement instead of appearing out of the top left. */
    cursor.x = cursor.min_x + (cursor.max_x - cursor.min_x) / 2;
    cursor.y = cursor.min_y + (cursor.max_y - cursor.min_y) / 2;
    cursor.moved = true;

    return true;
}

bool hal_input_pending_peek(void)
{
    return input_arrived;
}

bool hal_input_pending(void)
{
    bool pending = input_arrived;

    input_arrived = false;
    return pending;
}

bool hal_pointer_poll(struct pointer_state *out)
{
    struct virtio_input_event event;

    if (!tablet.present) {
        return false;
    }

    /*
     * Everything waiting, not one event. A single movement is at least an
     * ABS_X, an ABS_Y and the SYN_REPORT that ends the group, and answering
     * after the first would report a position with one axis from this
     * movement and one from the last - which reads as the cursor moving in
     * an L rather than in a line.
     */
    while (next_event(&tablet, &event)) {
        if (event.type == EV_ABS) {
            if (event.code == ABS_X) {
                cursor.x = event.value;
            } else if (event.code == ABS_Y) {
                cursor.y = event.value;
            }
        } else if (event.type == EV_KEY) {
            uint32_t bit = 0;

            if (event.code == BTN_LEFT)  { bit = 1u; }
            if (event.code == BTN_RIGHT) { bit = 2u; }

            if (bit != 0) {
                if (event.value != 0) {
                    cursor.buttons |= bit;
                } else {
                    cursor.buttons &= ~bit;
                }

                /* A press is news even when nothing moved. */
                cursor.moved = true;
            }
        } else if (event.type == EV_SYN) {
            cursor.moved = true;
        }
    }

    out->x       = cursor.x;
    out->y       = cursor.y;
    out->min_x   = cursor.min_x;
    out->max_x   = cursor.max_x;
    out->min_y   = cursor.min_y;
    out->max_y   = cursor.max_y;
    out->buttons = cursor.buttons;
    out->moved   = cursor.moved ? 1u : 0u;

    cursor.moved = false;
    return true;
}

int keyboard_getchar(void)
{
    /* Whatever the last key still owes. An arrow is three bytes and they
     * leave one at a time, in order, like any other input. */
    if (pending_at < pending_len) {
        return (unsigned char)pending[pending_at++];
    }

    if (!keyboard.present) {
        return -1;
    }

    /*
     * Everything the device has finished with since the last look. A loop
     * rather than one entry, because a single key produces at least a press
     * and a release and only one of them is a character - returning after
     * the first would leave the release to be found next time and halve the
     * effective rate.
     */
    for (;;) {
        struct virtio_input_event event;
        unsigned char c;

        if (!next_event(&keyboard, &event)) {
            return -1;              /* nothing waiting */
        }

        if (event.type != EV_KEY || event.code >= 128) {
            continue;               /* EV_SYN, and anything off the map */
        }

        /* value: 0 released, 1 pressed, 2 auto-repeat. Both 1 and 2 are a
         * character; a release only matters for the modifiers. */
        if (event.code == KEY_LEFTSHIFT || event.code == KEY_RIGHTSHIFT) {
            shift = (event.value != 0);
            continue;
        }

        if (event.code == KEY_LEFTCTRL || event.code == KEY_RIGHTCTRL) {
            ctrl = (event.value != 0);
            continue;
        }

        if (event.code == KEY_CAPSLOCK) {
            if (event.value == 1) {
                caps = !caps;
            }
            continue;
        }

        if (event.value == 0) {
            continue;               /* a release of an ordinary key */
        }

        /*
         * A key with no character of its own. Turned into the sequence a
         * terminal would have sent, and handed out a byte at a time.
         */
        {
            const char *sequence = sequence_for(event.code);

            if (sequence != NULL) {
                queue(sequence);
                return (unsigned char)pending[pending_at++];
            }
        }

        c = shift ? keymap_shift[event.code] : keymap_plain[event.code];

        /*
         * Caps lock is not a second shift: it applies to letters and to
         * nothing else, so 1 stays 1 rather than becoming !. Checking the
         * unshifted letter rather than the result is what makes shift and
         * caps together give a lower-case letter, which is what a keyboard
         * does.
         */
        if (caps) {
            unsigned char plain = keymap_plain[event.code];

            if (plain >= 'a' && plain <= 'z') {
                c = shift ? plain : keymap_shift[event.code];
            }
        }

        /*
         * Control turns a letter into the control character it names: C
         * becomes 3, D becomes 4, and so on down the first 32 codes. That
         * mapping is not a convention somebody chose here - it is what the
         * ASCII table is arranged for, which is why clearing bit 6 of the
         * upper-case letter is the whole of it.
         *
         * Only letters. Control-1 is not a character, and inventing one
         * would put a byte on the wire that no program is expecting.
         *
         * The serial line already does this, because the terminal on the
         * other end does it before the byte ever arrives. Without these ten
         * lines Control-C works over the cable and does nothing in the
         * window, which is the kind of difference that gets blamed on the
         * program rather than on the driver.
         */
        if (ctrl) {
            unsigned char plain = keymap_plain[event.code];

            if (plain >= 'a' && plain <= 'z') {
                return (int)((plain - 'a') + 1);
            }

            continue;               /* control-anything-else says nothing */
        }

        if (c != 0) {
            return (int)c;
        }
    }
}

bool keyboard_present(void)
{
    return keyboard.present;
}
