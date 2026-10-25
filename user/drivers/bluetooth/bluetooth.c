/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Bluetooth host stack** (`bluetooth.md` B2, B4): a driver process of
 * its own, which speaks HCI to whatever controller its transport carries -
 * a USB adapter through `xhci.c`, or `btsim` in its place - and knows
 * nothing of how the packets travel.
 *
 * - **Setup**, as Linux's `hci_core.c` begins it: Reset, then Read Local
 *   Version, Read BD_ADDR, Read Local Supported Features and Read Buffer
 *   Size, and one line in the log saying what the adapter is. An Intel
 *   adapter is asked Intel Read Version first, as `btintel.c` asks it.
 *   Then the configuration discovery needs: the event mask, with Low
 *   Energy's events in it, and the inquiry mode that brings names.
 * - **What is nearby** (B4): Classic's inquiry and Low Energy's scan,
 *   together, for as long as a program holds a lease on them; each device
 *   heard kept in a table, and a Classic one that gave no name asked for
 *   it after each inquiry.
 * - **`/Devices/bluetooth`**, this process's own endpoint, answered from
 *   that table in `btproto.h`'s shape.
 *
 * **One command at a time.** HCI lets a host send only as many commands as
 * the controller says it can take, and every controller here says one, so
 * the commands wait in a queue and the next goes when the last is answered
 * - by Command Complete, or by Command Status for the ones whose result
 * comes later as an event of its own (an inquiry, a name).
 *
 * **It never waits on the controller.** A command goes into the ring and
 * the transport is told; the answer comes when it comes, as a pulse, on
 * the same receive that hears every request - so a program asking what is
 * nearby is answered at once from what has been heard. A command not
 * answered in two seconds is said in the log and given up.
 *
 * `bt_` names are this file's; `hci_` and `gap_` names are the decoders'.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "init/say.h"
#include "btproto.h"
#include "hciproto.h"
#include "hciring.h"
#include "hci_decode.h"
#include "gap_decode.h"

/* How long a command may go unanswered before it is given up on: Reset is
 * the slowest, at a quarter of a second on some chips. */
#define ANSWER_MS        2000u

/* Commands beyond the setup (Volume 4 Part E 7). */
#define CMD_INQUIRY             0x0401u
#define CMD_INQUIRY_CANCEL      0x0402u
#define CMD_REMOTE_NAME         0x0419u
#define CMD_SET_EVENT_MASK      0x0C01u
#define CMD_WRITE_INQUIRY_MODE  0x0C45u
#define CMD_LE_SCAN_PARAMETERS  0x200Bu
#define CMD_LE_SCAN_ENABLE      0x200Cu

/* Features that decide the inquiry mode (Volume 2 Part C 3.3). */
#define FEATURE_RSSI_INQUIRY    17u
#define FEATURE_EIR             48u

/* An inquiry of 4 x 1.28 s, then another while a lease lasts: long enough
 * to hear what answers slowly, short enough that names are asked often. */
#define INQUIRY_LENGTH          4u

/* Intel's vendor ID on USB: its chips are asked what they are first, as
 * Linux's `btintel.c` asks, because one still in its bootloader may not be
 * a controller at all until its firmware is sent (`bluetooth.md` B3). */
#define USB_VENDOR_INTEL 0x8087u

/*
 * **What has been heard**, at most this many at once: a cache of what is
 * near rather than a limit on it, since the one heard longest ago gives
 * way to a new one. A crowded room has more than this in range; nobody
 * pairs with the sixty-fifth.
 */
#define DEVICES_KEPT     64u

#define QUEUE            16u        /* commands waiting their turn */

struct pending {
    uint16_t opcode;
    uint8_t  length;
    uint8_t  params[16];
};

struct heard {
    bool          used;
    struct gap_found found;
    unsigned long heard;            /* the counter when last heard */
    bool          name_asked;
};

static long console = -1;
static long transport = -1;
static unsigned tick_hz = 250;              /* scheduler ticks, for waiting */
static unsigned long counter_hz = 1000000;  /* the counter's, for measuring */

static struct {
    struct hci_ring *ring;
    long             region;
    bool             attached;

    /* The adapter, while there is one. */
    bool             present;
    uint32_t         generation;
    struct hci_reply about;         /* what the transport said of it */

    /* Commands: the queue, and the one sent and not yet answered. */
    struct pending   queue[QUEUE];
    unsigned         head, count;
    bool             outstanding;
    uint16_t         sent_opcode;
    unsigned long    sent_counter;
    bool             failed;        /* a setup command failed: not used */

    struct hci_version version;
    uint8_t          address[6];
    uint8_t          features[8];
    struct hci_buffers buffers;
    bool             ready;

    /* Looking: until when, and what is in progress. */
    bool             scanning;
    unsigned long    scan_until;    /* the counter, while `scanning` */
    bool             inquiring;
    bool             naming;        /* a Remote Name Request is out */

    struct heard     heard[DEVICES_KEPT];

    unsigned long    events;        /* events read from `up`, all told */
    unsigned long    unasked;       /* events nothing here waited for */
} bt;

static unsigned ticks_for(unsigned long ms)
{
    return (unsigned)((ms * tick_hz + 999u) / 1000u);
}

static unsigned long counter_ms(unsigned long counted)
{
    return (unsigned long)((uint64_t)counted * 1000u / counter_hz);
}

static bool can_le(void)
{
    return hci_feature(bt.features, HCI_FEATURE_LE);
}

static bool can_classic(void)
{
    return !hci_feature(bt.features, HCI_FEATURE_BREDR_NOT_SUPPORTED);
}

/*------------------------------------------------------------- the transport */

/* One request to the transport; the reply in `out`. */
static bool ask(uint32_t op, long cap, struct hci_reply *out)
{
    struct hci_request req;
    struct message msg, rep;

    memset(&req, 0, sizeof(req));
    req.op = op;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    msg.cap_plus_one = cap < 0 ? 0 : (uint32_t)(cap + 1);
    memcpy(msg.data, &req, sizeof(req));
    memset(&rep, 0, sizeof(rep));

    if (kosmos_call(transport, &msg, &rep) != 0
        || rep.length < sizeof(*out)) {
        return false;
    }

    memcpy(out, rep.data, sizeof(*out));
    return true;
}

/* The ring made and handed over: once, for the life of the process. */
static bool attach(struct say_line *line)
{
    struct hci_reply rep;
    long at;

    bt.region = kosmos_mem_create((HCI_RING_REGION + 4095u) / 4096u);
    at = bt.region < 0 ? bt.region : kosmos_mem_map(bt.region);

    if (at < 0) {
        say_text(line, "bluetooth: no memory for the ring; not started");
        say_send(console, line);
        return false;
    }

    bt.ring = (struct hci_ring *)(uintptr_t)at;
    memset(bt.ring, 0, sizeof(*bt.ring));
    bt.ring->magic = HCI_RING_MAGIC;
    bt.ring->slots = HCI_RING_SLOTS;
    bt.ring->slot_bytes = HCI_RING_SLOT;

    memset(&rep, 0, sizeof(rep));

    if (!ask(HCI_OP_ATTACH, bt.region, &rep) || rep.error != HCI_OK) {
        say_text(line, "bluetooth: the transport refused the ring (");
        say_dec(line, rep.error);
        say_text(line, "); not started");
        say_send(console, line);
        return false;
    }

    bt.attached = true;

    if (rep.present) {
        bt.about = rep;
    }

    return rep.present != 0;
}

/*-------------------------------------------------------------- commands */

static bool enqueue(uint16_t opcode, const uint8_t *params, unsigned length)
{
    struct pending *p;

    if (bt.count == QUEUE || length > sizeof(p->params)) {
        return false;
    }

    p = &bt.queue[(bt.head + bt.count) % QUEUE];
    p->opcode = opcode;
    p->length = (uint8_t)length;

    if (length != 0) {
        memcpy(p->params, params, length);
    }

    bt.count++;
    return true;
}

/* The next command into `down`, and the transport told - if none is out. */
static void pump(void)
{
    struct pending *p;
    struct hci_reply rep;
    uint32_t write, read;
    uint8_t *slot;
    unsigned n;

    if (bt.outstanding || bt.count == 0 || !bt.present) {
        return;
    }

    write = bt.ring->down_write;
    read = hci_ring_acquire(&bt.ring->down_read);

    if (hci_ring_space(write, read) == 0) {
        return;
    }

    p = &bt.queue[bt.head];
    slot = hci_ring_down(bt.ring, write);
    slot[0] = HCI_PACKET_COMMAND;
    n = hci_command(slot + 1, HCI_RING_SLOT - 1u, p->opcode, p->params,
                    p->length);

    bt.head = (bt.head + 1u) % QUEUE;
    bt.count--;

    if (n == 0) {
        return;
    }

    bt.ring->down_length[write % HCI_RING_SLOTS] = 1u + n;
    hci_ring_publish(&bt.ring->down_write, write + 1u);

    bt.outstanding = true;
    bt.sent_opcode = p->opcode;
    bt.sent_counter = kosmos_ticks();

    (void)ask(HCI_OP_SEND, -1, &rep);
}

static void forget_commands(void)
{
    bt.count = 0;
    bt.outstanding = false;
}

/*-------------------------------------------------------------- the setup */

/* An adapter, from the start: what the transport said, and its setup
 * queued - Intel's question first for an Intel one. */
static void begin(const struct hci_reply *about)
{
    bt.present = true;
    bt.generation = about->generation;
    bt.about = *about;
    bt.ready = false;
    bt.failed = false;
    bt.scanning = bt.inquiring = bt.naming = false;
    forget_commands();

    if (about->bus == HCI_BUS_USB && about->vendor == USB_VENDOR_INTEL) {
        (void)enqueue(HCI_INTEL_READ_VERSION, 0, 0);
    }

    (void)enqueue(HCI_RESET, 0, 0);
    (void)enqueue(HCI_READ_LOCAL_VERSION, 0, 0);
    (void)enqueue(HCI_READ_BD_ADDR, 0, 0);
    (void)enqueue(HCI_READ_LOCAL_FEATURES, 0, 0);
    (void)enqueue(HCI_READ_BUFFER_SIZE, 0, 0);
    pump();
}

/*
 * **What the adapter is, in one line**, which is all a person needs and
 * what a boot screen has room for: "bluetooth: USB port 10, Bluetooth 5.1
 * from Intel, 8C:90:2D:4A:17:E3: Classic and Low Energy, SSP, 8 ACL
 * buffers of 1021 bytes". Within the console's 160 characters for every
 * name in the tables.
 */
static void say_adapter(struct say_line *line)
{
    const char *name;
    char address[18];

    say_text(line, "bluetooth: ");
    if (bt.about.bus == HCI_BUS_SIMULATED) {
        say_text(line, "simulated");
    } else {
        say_text(line, "USB port ");
        say_dec(line, bt.about.port);
    }

    say_text(line, ", Bluetooth ");
    name = hci_version_name(bt.version.hci_version);
    if (name != NULL) {
        say_text(line, name);
    } else {
        say_text(line, "version ");
        say_dec(line, bt.version.hci_version);
    }

    say_text(line, " from ");
    name = hci_manufacturer_name(bt.version.manufacturer);
    if (name != NULL) {
        say_text(line, name);
    } else {
        say_text(line, "company ");
        say_dec(line, bt.version.manufacturer);
    }

    say_text(line, ", ");
    hci_format_address(bt.address, address);
    say_text(line, address);
    say_text(line, !can_classic() ? ": Low Energy only"
                   : can_le() ? ": Classic and Low Energy" : ": Classic only");
    if (hci_feature(bt.features, HCI_FEATURE_SSP)) {
        say_text(line, ", SSP");
    }
    say_text(line, ", ");
    say_dec(line, bt.buffers.acl_packets);
    say_text(line, " ACL buffers of ");
    say_dec(line, bt.buffers.acl_length);
    say_text(line, " bytes");
    say_send(console, line);
}

/*
 * **Intel's own account of the chip**: its answer's bytes as they came, and
 * read, when they are the fixed form, as Linux's `struct intel_version`.
 */
static void say_intel(const struct hci_answer *a, struct say_line *line)
{
    struct hci_intel_version v;
    unsigned i;

    say_text(line, "bluetooth: Intel Read Version: status ");
    say_dec(line, a->status);
    say_text(line, ", ");
    say_dec(line, a->ret_length);
    say_text(line, " bytes:");
    for (i = 0; i < a->ret_length && i < 24u; i++) {
        say_text(line, " ");
        say_hex(line, a->ret[i], 2);
    }
    say_send(console, line);

    if (!hci_decode_intel_version(a, &v)) {
        return;
    }

    say_text(line, "bluetooth: Intel hardware variant ");
    say_hex(line, v.hw_variant, 2);
    say_text(line, " revision ");
    say_hex(line, v.hw_revision, 2);
    say_text(line, v.fw_variant == HCI_INTEL_BOOTLOADER
                   ? ", in its bootloader: it waits for firmware"
                   : v.fw_variant == HCI_INTEL_OPERATIONAL
                     ? ", its firmware running" : ", firmware variant ");
    if (v.fw_variant != HCI_INTEL_BOOTLOADER
        && v.fw_variant != HCI_INTEL_OPERATIONAL) {
        say_hex(line, v.fw_variant, 2);
    }
    say_text(line, " (build ");
    say_dec(line, v.fw_build_num);
    say_text(line, ", week ");
    say_dec(line, v.fw_build_ww);
    say_text(line, " of 20");
    say_dec(line, v.fw_build_yy);
    say_text(line, ")");
    say_send(console, line);
}

/* The configuration discovery needs, once the adapter is known. */
static void configure(void)
{
    uint8_t mask[8];
    uint8_t mode;

    /*
     * The default event mask, with LE Meta (bit 61) added - without it a
     * controller keeps every advertising report to itself (7.3.1).
     */
    mask[0] = mask[1] = mask[2] = mask[3] = mask[4] = 0xFF;
    mask[5] = 0x1F;
    mask[6] = 0x00;
    mask[7] = 0x20;
    (void)enqueue(CMD_SET_EVENT_MASK, mask, 8);

    /* Results with RSSI and the device's own EIR - its name - when the
     * controller can give them (7.3.50). */
    if (can_classic()) {
        mode = hci_feature(bt.features, FEATURE_EIR) ? 2u
             : hci_feature(bt.features, FEATURE_RSSI_INQUIRY) ? 1u : 0u;

        if (mode != 0) {
            (void)enqueue(CMD_WRITE_INQUIRY_MODE, &mode, 1);
        }
    }
}

/*------------------------------------------------------------ what is near */

static bool same(const struct gap_found *a, const struct gap_found *b)
{
    return memcmp(a->address, b->address, 6) == 0
        && (a->transport == GAP_CLASSIC) == (b->transport == GAP_CLASSIC);
}

/* A device heard: its place, a new one taking the oldest's when full. */
static struct heard *place_for(const struct gap_found *f)
{
    struct heard *oldest = NULL;
    unsigned i;

    for (i = 0; i < DEVICES_KEPT; i++) {
        if (bt.heard[i].used && same(&bt.heard[i].found, f)) {
            return &bt.heard[i];
        }
    }

    for (i = 0; i < DEVICES_KEPT; i++) {
        if (!bt.heard[i].used) {
            return &bt.heard[i];
        }

        if (oldest == NULL || bt.heard[i].heard < oldest->heard) {
            oldest = &bt.heard[i];
        }
    }

    return oldest;
}

/* What it said this time, over what it said before: a name, a class or an
 * appearance once said is kept until said otherwise. */
static void heard(void *ctx, const struct gap_found *f)
{
    struct heard *h = place_for(f);
    struct gap_found *k = &h->found;

    (void)ctx;

    if (!h->used || !same(k, f)) {
        memset(h, 0, sizeof(*h));
        *k = *f;
        h->used = true;
    } else {
        k->transport = f->transport;
        k->connectable = k->connectable || f->connectable;
        k->hid = k->hid || f->hid;

        if (f->has_rssi) {
            k->rssi = f->rssi;
            k->has_rssi = true;
        }
        if (f->cod != 0) {
            k->cod = f->cod;
        }
        if (f->has_appearance) {
            k->appearance = f->appearance;
            k->has_appearance = true;
        }
        if (f->name[0] != '\0' && (f->name_complete || !k->name_complete)) {
            memcpy(k->name, f->name, sizeof(k->name));
            k->name_complete = f->name_complete;
        }
    }

    h->heard = kosmos_ticks();
}

/* The next Classic device with no name, asked for it; false when none. */
static bool ask_a_name(void)
{
    unsigned i;

    for (i = 0; i < DEVICES_KEPT; i++) {
        struct heard *h = &bt.heard[i];
        uint8_t p[10];

        if (!h->used || h->name_asked || h->found.transport != GAP_CLASSIC
            || h->found.name[0] != '\0') {
            continue;
        }

        /* Its address, Page_Scan_Repetition_Mode R1 - what every
         * inquiry result here has said - a reserved byte, no clock offset
         * (7.1.19). */
        memcpy(p, h->found.address, 6);
        p[6] = 1;
        p[7] = 0;
        p[8] = p[9] = 0;
        h->name_asked = true;

        if (enqueue(CMD_REMOTE_NAME, p, 10)) {
            bt.naming = true;
            return true;
        }
    }

    return false;
}

static void inquire(void)
{
    /* The General Inquiry Access Code, 9E8B33, least significant first;
     * the length; no limit on responses (7.1.1). */
    static const uint8_t p[5] = { 0x33, 0x8B, 0x9E, INQUIRY_LENGTH, 0 };

    if (enqueue(CMD_INQUIRY, p, 5)) {
        bt.inquiring = true;
    }
}

static void scan_start(void)
{
    /* Active scanning, so a device's scan response - where many keep
     * their name - is asked for; every 60 ms for 30 of them; a public
     * address of its own; every advertiser (7.8.10). */
    static const uint8_t parameters[7] = { 1, 0x60, 0x00, 0x30, 0x00, 0, 0 };
    /* On, and each advertiser said once rather than every time (7.8.11). */
    static const uint8_t on[2] = { 1, 1 };

    bt.scanning = true;

    if (can_le()) {
        (void)enqueue(CMD_LE_SCAN_PARAMETERS, parameters, 7);
        (void)enqueue(CMD_LE_SCAN_ENABLE, on, 2);
    }

    if (can_classic() && !bt.inquiring && !bt.naming) {
        inquire();
    }

    pump();
}

static void scan_stop(void)
{
    static const uint8_t off[2] = { 0, 0 };

    if (!bt.scanning) {
        return;
    }

    bt.scanning = false;

    if (can_le()) {
        (void)enqueue(CMD_LE_SCAN_ENABLE, off, 2);
    }

    if (bt.inquiring) {
        (void)enqueue(CMD_INQUIRY_CANCEL, 0, 0);
    }

    pump();
}

/* An inquiry over: names asked for, one at a time, then another inquiry
 * while the lease lasts. */
static void after_inquiry(void)
{
    bt.inquiring = false;

    if (ask_a_name()) {
        return;
    }

    if (bt.scanning && can_classic()) {
        inquire();
    }
}

/*------------------------------------------------------------ the answers */

/* A setup command answered: read, and the setup goes on or ends. */
static void setup_answered(uint16_t opcode, const struct hci_answer *a,
                           struct say_line *line)
{
    bool ok = a->complete && a->status == 0;

    if (opcode == HCI_INTEL_READ_VERSION) {
        say_intel(a, line);
        return;
    }

    if (ok) {
        switch (opcode) {
        case HCI_READ_LOCAL_VERSION:
            ok = hci_decode_version(a, &bt.version);
            break;
        case HCI_READ_BD_ADDR:
            ok = hci_decode_address(a, bt.address);
            break;
        case HCI_READ_LOCAL_FEATURES:
            ok = hci_decode_features(a, bt.features);
            break;
        case HCI_READ_BUFFER_SIZE:
            ok = hci_decode_buffers(a, &bt.buffers);
            break;
        default:
            break;
        }
    }

    if (!ok) {
        say_text(line, "bluetooth: the adapter answered ");
        say_hex(line, opcode, 4);
        say_text(line, " with status ");
        say_dec(line, a->status);
        say_text(line, "; it is not used");
        say_send(console, line);
        bt.failed = true;
        forget_commands();
        return;
    }

    if (opcode == HCI_READ_BUFFER_SIZE) {
        bt.ready = true;
        say_adapter(line);
        configure();
    }
}

/* Any other command answered: said if refused, and what it changes. */
static void answered(uint16_t opcode, const struct hci_answer *a,
                     struct say_line *line)
{
    bool ok = a->status == 0;

    switch (opcode) {
    case HCI_INTEL_READ_VERSION:
    case HCI_RESET:
    case HCI_READ_LOCAL_VERSION:
    case HCI_READ_BD_ADDR:
    case HCI_READ_LOCAL_FEATURES:
    case HCI_READ_BUFFER_SIZE:
        setup_answered(opcode, a, line);
        return;

    case CMD_INQUIRY:
        if (!ok) {
            bt.inquiring = false;
        }
        break;

    case CMD_REMOTE_NAME:
        if (!ok) {
            bt.naming = false;
            after_inquiry();
        }
        break;

    default:
        break;
    }

    if (!ok) {
        say_text(line, "bluetooth: the adapter refused ");
        say_hex(line, opcode, 4);
        say_text(line, " (status ");
        say_dec(line, a->status);
        say_text(line, ")");
        say_send(console, line);
    }
}

/* One whole event from `up`. */
static void take(const struct hci_event *e, struct say_line *line)
{
    struct hci_answer a;
    uint8_t address[6];
    char name[GAP_NAME_BYTES];
    uint8_t status;

    bt.events++;

    if (hci_decode_answer(e, &a)) {
        if (bt.outstanding && a.opcode == bt.sent_opcode) {
            bt.outstanding = false;
            answered(a.opcode, &a, line);
        } else {
            bt.unasked++;
        }
        return;
    }

    if (gap_decode_inquiry(e, heard, NULL) != 0
        || gap_decode_advertising(e, heard, NULL) != 0) {
        return;
    }

    if (gap_decode_inquiry_complete(e, &status)) {
        after_inquiry();
        return;
    }

    if (e->code == HCI_EVENT_REMOTE_NAME) {
        if (gap_decode_remote_name(e, address, name)) {
            struct gap_found f;

            memset(&f, 0, sizeof(f));
            memcpy(f.address, address, 6);
            f.transport = GAP_CLASSIC;
            memcpy(f.name, name, sizeof(f.name));
            f.name_complete = true;
            heard(NULL, &f);
        }

        bt.naming = false;
        after_inquiry();
        return;
    }

    bt.unasked++;
}

/* Everything in `up`, read: events. ACL data is B5's. */
static void drain(struct say_line *line)
{
    uint32_t read = bt.ring->up_read;
    uint32_t write = hci_ring_acquire(&bt.ring->up_write);

    while (read != write) {
        const uint8_t *slot = hci_ring_up(bt.ring, read);
        uint32_t length = bt.ring->up_length[read % HCI_RING_SLOTS];
        struct hci_event e;

        if (hci_ring_length_ok(length) && slot[0] == HCI_PACKET_EVENT
            && hci_decode_event(slot + 1, length - 1u, &e)) {
            take(&e, line);
        }

        read++;
        hci_ring_publish(&bt.ring->up_read, read);
    }

    pump();
}

/*------------------------------------------------------ /Devices/bluetooth */

/* The order a LIST gives: nearest first, those that gave no RSSI last. */
static int nearness(const struct heard *h)
{
    return h->found.has_rssi ? h->found.rssi : -1000;
}

static void fill_device(struct bt_device *d, const struct heard *h,
                        unsigned long now)
{
    const struct gap_found *f = &h->found;

    memset(d, 0, sizeof(*d));
    memcpy(d->address, f->address, 6);
    d->transport = f->transport;
    d->kind = (uint8_t)gap_kind(f);
    d->rssi = f->has_rssi ? f->rssi : 127;
    d->appearance = f->has_appearance ? f->appearance : 0;
    d->cod = f->cod;
    d->heard_ms = (uint32_t)counter_ms(now - h->heard);
    gap_copy_name(d->name, sizeof(d->name), (const uint8_t *)f->name,
                  (unsigned)strlen(f->name));
}

static void fill_list(struct bt_reply *rep, uint32_t first)
{
    const struct heard *order[DEVICES_KEPT];
    unsigned long now = kosmos_ticks();
    unsigned n = 0, i, j;

    for (i = 0; i < DEVICES_KEPT; i++) {
        if (bt.heard[i].used) {
            order[n++] = &bt.heard[i];
        }
    }

    /* Sixty-four at most: an insertion sort is the whole of it. */
    for (i = 1; i < n; i++) {
        const struct heard *h = order[i];

        for (j = i; j > 0 && nearness(order[j - 1]) < nearness(h); j--) {
            order[j] = order[j - 1];
        }
        order[j] = h;
    }

    rep->devices = n;
    rep->first = first;

    for (i = first; i < n && rep->count < BT_LIST_PAGE; i++) {
        fill_device(&rep->list[rep->count++], order[i], now);
    }
}

static void fill_state(struct bt_reply *rep)
{
    const char *maker;
    unsigned i;

    rep->present = bt.present && bt.ready ? 1u : 0u;
    rep->bus = (uint8_t)bt.about.bus;
    rep->port = bt.about.port;
    rep->scanning = bt.scanning ? 1u : 0u;
    rep->devices = 0;

    for (i = 0; i < DEVICES_KEPT; i++) {
        rep->devices += bt.heard[i].used ? 1u : 0u;
    }

    if (!rep->present) {
        return;
    }

    rep->flags = (uint8_t)((can_classic() ? BT_CAN_CLASSIC : 0u)
                         | (can_le() ? BT_CAN_LE : 0u)
                         | (hci_feature(bt.features, HCI_FEATURE_SSP)
                            ? BT_CAN_SSP : 0u));
    rep->version = bt.version.hci_version;
    rep->manufacturer = bt.version.manufacturer;
    rep->acl_length = bt.buffers.acl_length;
    rep->acl_packets = bt.buffers.acl_packets;
    memcpy(rep->address, bt.address, 6);

    maker = hci_manufacturer_name(bt.version.manufacturer);
    if (maker != NULL) {
        gap_copy_name(rep->maker, sizeof(rep->maker), (const uint8_t *)maker,
                      (unsigned)strlen(maker));
    }
}

/* One request on `/Devices/bluetooth`, answered from what is kept. */
static void serve(const struct message *msg, uint64_t sender)
{
    static struct bt_reply rep;
    struct bt_request req;
    struct message out;

    memset(&rep, 0, sizeof(rep));
    memset(&req, 0, sizeof(req));

    if (msg->length >= sizeof(req)) {
        memcpy(&req, msg->data, sizeof(req));
    } else {
        req.op = 0;
    }

    fill_state(&rep);

    switch (req.op) {
    case BT_OP_STATE:
        break;

    case BT_OP_SCAN:
        if (!bt.present) {
            rep.error = BT_ERR_NONE;
        } else if (!bt.ready) {
            rep.error = BT_ERR_NOT_READY;
        } else {
            unsigned long ms = req.arg < BT_SCAN_MOST ? req.arg : BT_SCAN_MOST;
            unsigned long until = kosmos_ticks()
                                + (unsigned long)((uint64_t)ms * counter_hz
                                                  / 1000u);

            if (!bt.scanning) {
                bt.scan_until = until;
                scan_start();
            } else if ((long)(until - bt.scan_until) > 0) {
                bt.scan_until = until;
            }
            rep.scanning = 1;
        }
        break;

    case BT_OP_STOP:
        scan_stop();
        rep.scanning = 0;
        break;

    case BT_OP_LIST:
        fill_list(&rep, req.arg);
        break;

    default:
        rep.error = BT_ERR_REQUEST;
        break;
    }

    memset(&out, 0, sizeof(out));
    out.tag = msg->tag;
    out.length = sizeof(rep);
    memcpy(out.data, &rep, sizeof(rep));
    (void)kosmos_reply(sender, &out);
}

/*---------------------------------------------------------------- the loop */

/* Until the next thing this must do by itself: a command's deadline, or
 * the end of a lease. 0 is "until something comes". */
static unsigned long next_wait(void)
{
    unsigned long now = kosmos_ticks(), wait = 0;

    if (bt.outstanding) {
        unsigned long passed = counter_ms(now - bt.sent_counter);

        wait = passed >= ANSWER_MS ? 1u : ticks_for(ANSWER_MS - passed);
    }

    if (bt.scanning) {
        unsigned long left = (long)(bt.scan_until - now) > 0
                           ? ticks_for(counter_ms(bt.scan_until - now)) : 1u;

        if (left == 0) {
            left = 1;
        }
        if (wait == 0 || left < wait) {
            wait = left;
        }
    }

    return wait;
}

/* What is due: a command given up on, a lease run out. */
static void due(struct say_line *line)
{
    unsigned long now = kosmos_ticks();

    if (bt.outstanding && counter_ms(now - bt.sent_counter) >= ANSWER_MS) {
        say_text(line, "bluetooth: the adapter did not answer ");
        say_hex(line, bt.sent_opcode, 4);
        say_text(line, " in two seconds");
        say_text(line, bt.ready ? "" : "; it is not used");
        say_send(console, line);

        bt.outstanding = false;

        if (!bt.ready) {
            bt.failed = true;
            forget_commands();
        } else if (bt.sent_opcode == CMD_INQUIRY) {
            bt.inquiring = false;
        } else if (bt.sent_opcode == CMD_REMOTE_NAME) {
            bt.naming = false;
        }
        pump();
    }

    if (bt.scanning && (long)(now - bt.scan_until) >= 0) {
        scan_stop();
    }
}

void bluetooth_server(long console_cap, long endpoint, long transport_cap)
{
    struct sysinfo info = { 0 };
    struct say_line line;

    console = console_cap;
    transport = transport_cap;
    say_begin(&line);

    if (kosmos_sysinfo(&info) == 0) {
        tick_hz = info.tick_hz != 0 ? info.tick_hz : tick_hz;
        counter_hz = info.counter_hz != 0 ? (unsigned long)info.counter_hz
                                          : counter_hz;
    }

    if (attach(&line)) {
        begin(&bt.about);
    }

    if (!bt.attached) {
        return;
    }

    for (;;) {
        struct message msg;
        uint64_t sender = 0;
        long got = kosmos_receive(endpoint, &msg, &sender, 0, next_wait());

        if (got == 0 && msg.tag == KOSMOS_PULSE_TAG) {
            struct kosmos_pulse p;

            memcpy(&p, msg.data, sizeof(p));

            if (p.code == HCI_PULSE_ADAPTER
                && !(bt.present && p.value == bt.generation)) {
                struct hci_reply rep;

                /* Asked about, for its bus and port. */
                memset(&rep, 0, sizeof(rep));
                if (ask(HCI_OP_ABOUT, -1, &rep) && rep.present) {
                    begin(&rep);
                }
            } else if (p.code == HCI_PULSE_MORE) {
                drain(&line);
            } else if (p.code == HCI_PULSE_GONE && bt.present
                       && p.value == bt.generation) {
                say_text(&line, "bluetooth: the adapter left");
                say_send(console, &line);
                memset(&bt.version, 0, sizeof(bt.version));
                memset(bt.heard, 0, sizeof(bt.heard));
                bt.present = bt.ready = false;
                bt.scanning = bt.inquiring = bt.naming = false;
                forget_commands();
            }
        } else if (got == 0) {
            serve(&msg, sender);
        }

        due(&line);
    }
}
