/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Bluetooth host stack** (`bluetooth.md` B2): a driver process of
 * its own, which speaks HCI to whatever controller its transport carries -
 * a USB adapter through `xhci.c`, or `btsim` in its place - and knows
 * nothing of how the packets travel.
 *
 * What it does at B2 is set the controller up as Linux's `hci_core.c`
 * does the first part of it: Reset, then Read Local Version, Read BD_ADDR,
 * Read Local Supported Features and Read Buffer Size, one after another,
 * and say in the log what it found. Scanning, connections, pairing and HID
 * are B4 to B6, and `/Devices/bluetooth` - this process's own endpoint - is
 * answered from B4; until then a request to it is refused.
 *
 * **It never waits on the controller.** Each command goes into the ring
 * and the transport is told; the answer arrives when it arrives, as a
 * pulse that says there is something in `up`, on the same receive that
 * hears every request. A command not answered in two seconds is said in
 * the log, and the stack waits for the next adapter rather than for that
 * one.
 *
 * `bt_` names are this file's; `hci_` names are the decoders'.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "init/say.h"
#include "hciproto.h"
#include "hciring.h"
#include "hci_decode.h"

/* How long a command may go unanswered before the controller is given up
 * on: Reset is the slowest, at a quarter of a second on some chips. */
#define ANSWER_MS   2000u

/* The controller's setup, in order (`hci_core.c`'s `hci_init1_req` and
 * the first of `hci_init2_req`). */
static const uint16_t SETUP[] = {
    HCI_RESET,
    HCI_READ_LOCAL_VERSION,
    HCI_READ_BD_ADDR,
    HCI_READ_LOCAL_FEATURES,
    HCI_READ_BUFFER_SIZE,
};

#define SETUP_STEPS (sizeof SETUP / sizeof SETUP[0])

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

    /* Its setup: the step whose answer is awaited, or SETUP_STEPS when
     * done; and when the command went, on the counter. */
    unsigned         step;
    bool             waiting;
    unsigned long    sent_counter;

    struct hci_version version;
    uint8_t          address[6];
    uint8_t          features[8];
    struct hci_buffers buffers;
    bool             ready;

    unsigned long    events;        /* events read from `up`, all told */
    unsigned long    unasked;       /* events nothing here waited for */
} bt;

static unsigned ticks_for(unsigned long ms)
{
    return (unsigned)((ms * tick_hz + 999u) / 1000u);
}

/* Milliseconds since the command went, measured on the counter. */
static unsigned long waited_ms(void)
{
    return (unsigned long)((uint64_t)(kosmos_ticks() - bt.sent_counter)
                           * 1000u / counter_hz);
}

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

/* One command into `down`, and the transport told. */
static bool command(uint16_t opcode)
{
    uint32_t write = bt.ring->down_write;
    uint32_t read = hci_ring_acquire(&bt.ring->down_read);
    struct hci_reply rep;
    uint8_t *slot;
    unsigned n;

    if (hci_ring_space(write, read) == 0) {
        return false;
    }

    slot = hci_ring_down(bt.ring, write);
    slot[0] = HCI_PACKET_COMMAND;
    n = hci_command(slot + 1, HCI_RING_SLOT - 1u, opcode, 0, 0);

    if (n == 0) {
        return false;
    }

    bt.ring->down_length[write % HCI_RING_SLOTS] = 1u + n;
    hci_ring_publish(&bt.ring->down_write, write + 1u);

    return ask(HCI_OP_SEND, -1, &rep) && rep.error == HCI_OK;
}

static void next_step(void)
{
    if (bt.step >= SETUP_STEPS) {
        bt.waiting = false;
        return;
    }

    bt.waiting = command(SETUP[bt.step]);
    bt.sent_counter = kosmos_ticks();
}

/* An adapter, from the start: what the transport said, and Reset. */
static void begin(const struct hci_reply *about)
{
    bt.present = true;
    bt.generation = about->generation;
    bt.about = *about;
    bt.step = 0;
    bt.ready = false;
    next_step();
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
    say_text(line, hci_feature(bt.features, HCI_FEATURE_BREDR_NOT_SUPPORTED)
                   ? ": Low Energy only"
                   : hci_feature(bt.features, HCI_FEATURE_LE)
                     ? ": Classic and Low Energy" : ": Classic only");
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

/* An answer to the step that was waiting, read; the next step sent. */
static void answered(const struct hci_answer *a, struct say_line *line)
{
    uint16_t want = SETUP[bt.step];
    bool ok = a->complete && a->status == 0;

    if (ok) {
        switch (want) {
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
        say_hex(line, want, 4);
        say_text(line, " with status ");
        say_dec(line, a->status);
        say_text(line, "; it is not used");
        say_send(console, line);
        bt.waiting = false;
        bt.step = SETUP_STEPS;
        return;
    }

    bt.step++;

    if (bt.step == SETUP_STEPS) {
        bt.waiting = false;
        bt.ready = true;
        say_adapter(line);
        return;
    }

    next_step();
}

/* Everything in `up`, read. At B2 that is events; ACL data is B5's. */
static void drain(struct say_line *line)
{
    uint32_t read = bt.ring->up_read;
    uint32_t write = hci_ring_acquire(&bt.ring->up_write);

    while (read != write) {
        const uint8_t *slot = hci_ring_up(bt.ring, read);
        uint32_t length = bt.ring->up_length[read % HCI_RING_SLOTS];
        struct hci_event e;
        struct hci_answer a;

        if (hci_ring_length_ok(length) && slot[0] == HCI_PACKET_EVENT
            && hci_decode_event(slot + 1, length - 1u, &e)) {
            bt.events++;

            if (bt.waiting && hci_decode_answer(&e, &a)
                && a.opcode == SETUP[bt.step]) {
                answered(&a, line);
            } else {
                bt.unasked++;
            }
        }

        read++;
        hci_ring_publish(&bt.ring->up_read, read);
    }
}

/* A request on `/Devices/bluetooth`: none is answered before B4. */
static void refuse(const struct message *msg, uint64_t sender)
{
    struct message out;
    uint32_t error = HCI_ERR_BAD_OP;

    memset(&out, 0, sizeof(out));
    out.tag = msg->tag;
    out.length = sizeof(error);
    memcpy(out.data, &error, sizeof(error));
    (void)kosmos_reply(sender, &out);
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
        unsigned long timeout = 0;      /* none: until something comes */
        long got;

        if (bt.waiting) {
            unsigned long passed = waited_ms();

            timeout = passed >= ANSWER_MS ? 1u : ticks_for(ANSWER_MS - passed);
        }

        got = kosmos_receive(endpoint, &msg, &sender, 0, timeout);

        if (got != 0) {
            if (bt.waiting && waited_ms() >= ANSWER_MS) {
                say_text(&line, "bluetooth: the adapter did not answer ");
                say_hex(&line, SETUP[bt.step], 4);
                say_text(&line, " in two seconds; it is not used");
                say_send(console, &line);
                bt.waiting = false;
                bt.step = SETUP_STEPS;
            }
            continue;
        }

        if (msg.tag == KOSMOS_PULSE_TAG) {
            struct kosmos_pulse p;

            memcpy(&p, msg.data, sizeof(p));

            if (p.code == HCI_PULSE_ADAPTER && !(bt.present
                                                && p.value == bt.generation)) {
                struct hci_reply rep;

                /* Asked about, for its bus and port. */
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
                bt.present = false;
                bt.ready = false;
                bt.waiting = false;
            }
            continue;
        }

        refuse(&msg, sender);
    }
}
