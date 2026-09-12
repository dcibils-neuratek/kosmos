/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
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
#include "keys.h"
#include "input.h"
#include "spinlock.h"
#include "virtio.h"

/*
 * The input devices' lock.
 *
 * **A queue of key transitions and a cursor position**, filled by
 * `input_interrupt` and drained by `keyboard_getchar` and
 * `hal_pointer_poll` - the clearest interrupt-versus-syscall pair in the
 * tree, and the one whose failure a person would actually see: a torn
 * head and tail index is a keystroke that arrives twice or not at all.
 *
 * One lock for both devices rather than one each. They share the decode
 * state and the queue, and two locks over one queue is two ways to be
 * wrong.
 */
static struct spinlock input_lock = SPINLOCK("virtio-input");

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
struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };

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
    struct virtio_device dev;       /* the window it was found in, and its slot */
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

/* Modifier state, which belongs to the keyboard and to nothing else. */
static bool      shift;

/* Super, and whether a key was pressed while it was held. See the key
 * handler below for why the second is needed. */
static bool      super;
static bool      super_used;
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

/*
 * Which keys are down, one bit per keycode.
 *
 * Four words covers the 128 codes this keymap knows, which is the same
 * bound the decode loop already enforces (`event.code >= 128` is skipped).
 * A bitmap rather than a queue on purpose: what a caller wants to know is
 * whether a key is held *now*, and a queue of transitions answers that only
 * if you have consumed every one of them in order - which the console, the
 * window manager and an application cannot all do at once.
 */
static uint32_t held[4];

/*
 * And the transitions, in order.
 *
 * The bitmap answers "is W down now", which is what a game asks once a
 * frame. It cannot answer "the menu key was pressed", because a press and
 * its release inside one frame leave the bitmap exactly as it found it -
 * and a key you tap is a key that never appears to have been held.
 *
 * So both: the bitmap for state, this for events. They are filled on the
 * same pass through the same virtqueue, so there is no second reader and
 * nothing can see one and miss the other.
 *
 * Sixty-four is four frames of frantic typing. Full, the *oldest* goes: a
 * lost press is an action you did not take, and a lost release is a key
 * stuck down for ever, which is worse than either.
 */
#define KEYQ 64

static struct {
    uint16_t code;
    uint8_t  down;
} keyq[KEYQ];

static unsigned keyq_head, keyq_tail;

static void keyq_put(unsigned code, int down)
{
    unsigned next = (keyq_head + 1) % KEYQ;

    keyq[keyq_head].code = (uint16_t)code;
    keyq[keyq_head].down = (uint8_t)(down ? 1 : 0);
    keyq_head = next;

    if (keyq_head == keyq_tail) {
        keyq_tail = (keyq_tail + 1) % KEYQ;
    }
}

bool virtio_key_event(unsigned *code, bool *down)
{
    if (keyq_head == keyq_tail) {
        return false;
    }

    *code = keyq[keyq_tail].code;
    *down = keyq[keyq_tail].down != 0;
    keyq_tail = (keyq_tail + 1) % KEYQ;

    return true;
}

bool virtio_key_held(unsigned code)
{
    if (code >= 128) {
        return false;
    }

    return (held[code >> 5] & (1u << (code & 31))) != 0;
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
    virtio_publish();
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
static bool has_absolute_axes(const struct virtio_device *dev)
{
    virtio_config_write8(dev, CFG_SELECT, VIRTIO_INPUT_CFG_EV_BITS);
    virtio_config_write8(dev, CFG_SUBSEL, EV_ABS);

    return virtio_config8(dev, CFG_SIZE) != 0;
}

/* The range one axis reports in, so a caller can scale without guessing. */
static void absolute_range(const struct virtio_device *dev, uint8_t axis,
                           uint32_t *min, uint32_t *max)
{
    virtio_config_write8(dev, CFG_SELECT, VIRTIO_INPUT_CFG_ABS_INFO);
    virtio_config_write8(dev, CFG_SUBSEL, axis);

    /* struct virtio_input_absinfo: min, max, fuzz, flat, res. */
    *min = virtio_config32(dev, CFG_DATA);
    *max = virtio_config32(dev, CFG_DATA + 4);
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
    unsigned from = 0;
    unsigned n;

    while (virtio_open(VIRTIO_ID_INPUT, from, &v->dev)) {
        from = v->dev.index + 1;

        /*
         * Not somebody else's, asked *before* anything is written.
         *
         * Two devices of one kind would otherwise both be answered by
         * whichever asked first - and worse, `virtio_begin` below resets
         * what it is given, so a scan that walked over the running keyboard
         * on its way to the tablet used to leave the keyboard with no
         * queues and no DRIVER_OK. It typed nothing after that and said
         * nothing about why.
         *
         * By ordinal rather than by address, because on virtio-pci a
         * device's registers are wherever `mmu_map_device` put them and
         * opening the same device twice gives two virtual addresses for one
         * BAR. `index` is what the board counts devices by, so it is the
         * same device both times.
         */
        if ((keyboard.present && &keyboard != v
             && keyboard.dev.index == v->dev.index)
            || (tablet.present && &tablet != v
                && tablet.dev.index == v->dev.index)) {
            continue;
        }

        /* Ours to disturb, now that nobody else holds it. */
        virtio_begin(&v->dev);

        /*
         * The check that separates the two callers, and it is why the
         * handshake is split in two: configuration space may only be read
         * once the device has been told a driver is present, so telling a
         * keyboard from a tablet needs a place to stand in the middle of
         * it.
         *
         * Left acknowledged rather than failed - the other caller will want
         * it, and claiming it starts with another reset.
         */
        if (has_absolute_axes(&v->dev) != want_absolute) {
            continue;
        }

        /* Nothing beyond feature 32. A keyboard has no optional feature this
         * system wants. */
        if (!virtio_features(&v->dev, 0)) {
            continue;
        }

        /* Queue 0 is the event queue. Queue 1 carries status back to the
         * device - LEDs and such - and there is nothing this system wants to
         * say. */
        if (!virtio_queue_attach(&v->dev, 0, QUEUE_SIZE, &v->queue.desc,
                                 &v->queue.avail, &v->queue.used)) {
            virtio_fail(&v->dev);
            continue;
        }

        virtio_ready(&v->dev);

        /* Every buffer offered at once, because a device with nowhere to put
         * an event drops it, and a dropped key is a key the person
         * pressed. */
        for (n = 0; n < QUEUE_SIZE; n++) {
            offer(v, n);
        }

        virtio_publish();
        virtio_notify(&v->dev, 0);

        /*
         * And its interrupt. Until this, every reader of this device had to
         * come back and ask - which is what a window manager polling a
         * keyboard in a loop is, and why an idle desktop kept a core at a
         * hundred per cent.
         */
        virtio_enable_interrupt(&v->dev);

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

    virtio_consume();

    if (v->queue.used.idx == v->last_used) {
        return false;
    }

    slot = v->queue.used.ring[v->last_used % QUEUE_SIZE].id % QUEUE_SIZE;
    *out = v->events[slot];

    v->last_used++;

    offer(v, slot);
    virtio_publish();
    virtio_notify(&v->dev, 0);

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
/*
 * One line, and it may belong to both of them.
 *
 * This was an `if`/`else if` picking a single device, which is right when a
 * line identifies one - an MMIO window index does. **A PCI interrupt line
 * does not.** Four pins are shared out among every slot on the bus, so a
 * keyboard and a tablet answering on the same number is ordinary, and the
 * `else` meant whichever was checked second was never acknowledged: its
 * status bit stayed set, the controller saw a line still asserted, and no
 * further interrupt arrived from either.
 *
 * So both are offered it and each decides from its own status byte, which
 * is what `virtio_ack_interrupt` reads. A device that did not raise it
 * answers zero and nothing happens.
 */
static void service(struct vinput *v, unsigned line)
{
    if (!v->present || v->dev.slot != line) {
        return;
    }

    
/* The device raised it; the device is told it was seen. Without the ack
     * the status bit stays set and the interrupt fires for ever. */
    if (virtio_ack_interrupt(&v->dev) != 0) {
        input_arrived = true;
    }
}

static void input_interrupt_locked(unsigned line)
{
    service(&keyboard, line);
    service(&tablet, line);
}

/*
 * Fills the key queue and the cursor from the handler; the three below
 * drain them from a syscall.
 */
void virtio_input_interrupt(unsigned line)
{
    unsigned long flags = spin_lock(&input_lock);

    input_interrupt_locked(line);
    spin_unlock(&input_lock, flags);
}

bool virtio_keyboard_init(void)
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

bool virtio_pointer_init(void)
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
    absolute_range(&tablet.dev, ABS_X, &cursor.min_x, &cursor.max_x);
    absolute_range(&tablet.dev, ABS_Y, &cursor.min_y, &cursor.max_y);

    /* Start in the middle rather than at a corner, so a cursor exists before
     * the first movement instead of appearing out of the top left. */
    cursor.x = cursor.min_x + (cursor.max_x - cursor.min_x) / 2;
    cursor.y = cursor.min_y + (cursor.max_y - cursor.min_y) / 2;
    cursor.moved = true;

    return true;
}

bool virtio_input_pending_peek(void)
{
    return input_arrived;
}

bool virtio_input_pending(void)
{
    bool pending = input_arrived;

    input_arrived = false;
    return pending;
}

static bool hal_pointer_poll_locked(struct pointer_state *out)
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

/*
 * Drains every pending event into the cursor and reports where it is.
 */
bool virtio_pointer_poll(struct pointer_state *out)
{
    unsigned long flags = spin_lock(&input_lock);
    bool r = hal_pointer_poll_locked(out);

    spin_unlock(&input_lock, flags);
    return r;
}

static int keyboard_getchar_locked(void)
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
        int c;

        if (!next_event(&keyboard, &event)) {
            return -1;              /* nothing waiting */
        }

        if (event.type != EV_KEY || event.code >= 128) {
            continue;               /* EV_SYN, and anything off the map */
        }

        /*
         * Recorded before anything below returns or continues.
         *
         * Everything after this point is the *character* path, and it takes
         * several early exits - a modifier is consumed, a release is
         * dropped, a key with no character becomes a sequence and returns
         * mid-loop. Putting this first is what makes the two streams agree:
         * every transition the device reported is here, whatever the
         * character half decided to do with it.
         */
        held[event.code >> 5] = (event.value != 0)
            ? (held[event.code >> 5] |  (1u << (event.code & 31)))
            : (held[event.code >> 5] & ~(1u << (event.code & 31)));

        /* value 2 is auto-repeat, which is not a transition: the key was
         * already down and still is. */
        if (event.value != 2) {
            keyq_put(event.code, event.value != 0);
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

        /*
         * Super - Windows on a PC keyboard, Command on an Apple one. Unlike
         * shift and control it means something *by itself*, so its release
         * is a keystroke rather than bookkeeping: tapped it opens the menu,
         * held it makes a combination. `super_used` is what tells the two
         * apart, and without it Win+Q would close the window and then open
         * the menu on the way back up.
         */
        if (event.code == KEY_LEFTMETA || event.code == KEY_RIGHTMETA) {
            if (event.value != 0) {
                super = true;
                super_used = false;
            } else {
                bool tapped = super && !super_used;

                super = false;

                if (tapped) {
                    queue(hal_key_super(0));
                    return (unsigned char)pending[pending_at++];
                }
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
            const char *sequence = hal_key_sequence(event.code);

            if (sequence != NULL) {
                queue(sequence);
                return (unsigned char)pending[pending_at++];
            }
        }

        /*
         * The rules are the same on every keyboard, so they live in
         * `hal/keys.c` with the tables: caps lock is not a second shift,
         * and control names a control character. -1 is a key that says
         * nothing, which is not the same as a key with no character.
         */
        c = hal_key_char(event.code, shift, ctrl, caps);

        if (c < 0) {
            continue;
        }

        /* Held with Super: a command rather than a character. */
        if (super) {
            super_used = true;
            queue(hal_key_super(c));
            return (unsigned char)pending[pending_at++];
        }

        return c;
    }
}

/*
 * Takes one byte out of the queue the interrupt fills.
 */
int virtio_keyboard_getchar(void)
{
    unsigned long flags = spin_lock(&input_lock);
    int r = keyboard_getchar_locked();

    spin_unlock(&input_lock, flags);
    return r;
}

bool virtio_keyboard_present(void)
{
    return keyboard.present;
}
