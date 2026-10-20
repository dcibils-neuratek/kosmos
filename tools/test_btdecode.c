/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Bluetooth adapter's bytes, decoded on the host (`bluetooth.md` B1).
 *
 * QEMU has no Bluetooth adapter, so what `xhci.c` reads of one in a machine
 * comes from a real one, and until one is plugged in these are the bytes:
 *
 *   - **a configuration** in the layout the Core specification gives
 *     (Volume 4 Part B 2.1), which a CSR8510 and a Realtek RTL8761B both
 *     have: interface 0 with an interrupt IN of 16 bytes and two bulk
 *     endpoints of 64, interface 1's voice settings after it. Composed from
 *     the specification, not printed from a device; the first adapter's own
 *     bytes go in beside it;
 *   - **the answers** to HCI Reset and the three reads, composed from
 *     Volume 4 Part E 7.4 for a CSR8510: Bluetooth 4.0, company 10;
 *   - **events cut** into 16-byte packets as an interrupt endpoint hands
 *     them over, two in one packet, and Read Local Name's 254 bytes across
 *     sixteen.
 *
 * And what no adapter sends: every length of the configuration cut short
 * before a page that is not mapped, a descriptor of length zero, an HCI
 * interface missing an endpoint, setting 1 before setting 0, a second
 * adapter's interface after the first, and answers to the wrong command,
 * with a failed status, or shorter than their parameters.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../user/drivers/usb/btusb_decode.h"
#include "../user/drivers/bluetooth/hci_decode.h"
#include "../user/drivers/bluetooth/btsim.h"
#include "../user/include/hciring.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

/* An adapter's configuration, as Volume 4 Part B 2.1 lays it out. */
static const uint8_t ADAPTER[] = {
    9, 0x02, 0x4F, 0, 2, 1, 0, 0xE0, 50,            /* configuration 1, 2 ifs */
    9, 0x04, 0, 0, 3, 0xE0, 0x01, 0x01, 0,          /* HCI, 3 endpoints */
    7, 0x05, 0x81, 0x03, 0x10, 0x00, 1,             /* EP 1 IN, interrupt, 16 */
    7, 0x05, 0x02, 0x02, 0x40, 0x00, 1,             /* EP 2 OUT, bulk, 64 */
    7, 0x05, 0x82, 0x02, 0x40, 0x00, 1,             /* EP 2 IN, bulk, 64 */
    9, 0x04, 1, 0, 2, 0xE0, 0x01, 0x01, 0,          /* voice, setting 0 */
    7, 0x05, 0x03, 0x01, 0x00, 0x00, 1,             /* EP 3 OUT, iso, 0 */
    7, 0x05, 0x83, 0x01, 0x00, 0x00, 1,             /* EP 3 IN, iso, 0 */
    9, 0x04, 1, 1, 2, 0xE0, 0x01, 0x01, 0,          /* voice, setting 1 */
    7, 0x05, 0x03, 0x01, 0x09, 0x00, 1,             /* EP 3 OUT, iso, 9 */
    7, 0x05, 0x83, 0x01, 0x09, 0x00, 1,             /* EP 3 IN, iso, 9 */
};

static void test_adapter(void)
{
    struct usb_bluetooth b;

    btusb_decode_config(ADAPTER, sizeof ADAPTER, &b);
    check(b.ok, "the adapter: read");
    check(!b.malformed, "the adapter: not malformed");
    check(b.configuration == 1, "the adapter: configuration 1");
    check(b.interface == 0, "the adapter: interface 0");
    check(b.event_endpoint == 1 && b.event_packet == 16
          && b.event_interval == 1, "the adapter: events on EP 1, 16 bytes");
    check(b.acl_in == 2 && b.acl_in_packet == 64, "the adapter: ACL in on EP 2");
    check(b.acl_out == 2 && b.acl_out_packet == 64,
          "the adapter: ACL out on EP 2");
}

/* Interface 1's isochronous endpoints are never taken for interface 0's. */
static void test_voice_ignored(void)
{
    uint8_t c[sizeof ADAPTER];
    struct usb_bluetooth b;

    memcpy(c, ADAPTER, sizeof c);
    c[9 + 4] = 2;                                   /* it claims two endpoints */
    memmove(c + 25, c + 32, sizeof c - 32);         /* and the bulk OUT goes */
    btusb_decode_config(c, sizeof c - 7, &b);
    check(!b.ok, "no bulk OUT: not read");
    check(b.acl_out == 0, "no bulk OUT: interface 1's iso OUT is not taken");
}

/* Every length short of the whole, against a page that is not there. */
static void test_cut_short(void)
{
    long page = sysconf(_SC_PAGESIZE);
    uint8_t *two = mmap(0, 2 * (size_t)page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned n, bad = 0;

    if (two == MAP_FAILED || mprotect(two + page, (size_t)page, PROT_NONE)) {
        check(0, "cut short: the guard page");
        return;
    }

    for (n = 0; n < sizeof ADAPTER; n++) {
        uint8_t *at = two + page - n;
        struct usb_bluetooth b;

        memcpy(at, ADAPTER, n);
        btusb_decode_config(at, n, &b);

        /* A cut inside a descriptor is malformed and refuses the whole,
         * the voice's included; on a boundary, read once the bulk IN is. */
        bool boundary = false;
        for (unsigned at = 0; at <= n; at += ADAPTER[at]) {
            if (at == n) { boundary = true; break; }
        }
        if (b.malformed != !boundary || b.ok != (boundary && n >= 39)) bad++;
    }

    check(bad == 0, "cut short: read exactly when whole to its endpoints");
    munmap(two, 2 * (size_t)page);
}

static void test_malformed(void)
{
    static const uint8_t zero[] = { 9, 0x02, 0x12, 0, 1, 1, 0, 0xE0, 50,
                                    0, 0x04, 0, 0, 3, 0xE0, 1, 1, 0 };
    static const uint8_t over[] = { 9, 0x02, 0x12, 0, 1, 1, 0, 0xE0, 50,
                                    40, 0x04, 0, 0, 3, 0xE0, 1, 1, 0 };
    struct usb_bluetooth b;

    btusb_decode_config(zero, sizeof zero, &b);
    check(b.malformed && !b.ok, "a descriptor of length zero: malformed");
    btusb_decode_config(over, sizeof over, &b);
    check(b.malformed && !b.ok, "a length past the end: malformed");
}

/* Not Bluetooth: a Wi-Fi adapter, vendor-specific, with the same endpoints. */
static void test_not_bluetooth(void)
{
    uint8_t c[sizeof ADAPTER];
    struct usb_bluetooth b;

    memcpy(c, ADAPTER, sizeof c);
    c[9 + 5] = 0xFF;
    c[9 + 6] = 0xFF;
    c[9 + 7] = 0xFF;
    c[50 + 5] = c[66 + 5] = 0xFF;
    btusb_decode_config(c, sizeof c, &b);
    check(!b.ok && b.event_endpoint == 0, "vendor-specific: not read");
}

/* Setting 1 of an HCI interface first: only setting 0 is the one. */
static void test_setting_one_first(void)
{
    static const uint8_t c[] = {
        9, 0x02, 0x3B, 0, 1, 1, 0, 0xE0, 50,
        9, 0x04, 0, 1, 3, 0xE0, 0x01, 0x01, 0,      /* setting 1 */
        7, 0x05, 0x84, 0x03, 0x20, 0x00, 1,
        7, 0x05, 0x05, 0x02, 0x40, 0x00, 1,
        7, 0x05, 0x85, 0x02, 0x40, 0x00, 1,
        9, 0x04, 0, 0, 3, 0xE0, 0x01, 0x01, 0,      /* setting 0 */
        7, 0x05, 0x81, 0x03, 0x10, 0x00, 1,
        7, 0x05, 0x02, 0x02, 0x40, 0x00, 1,
        7, 0x05, 0x82, 0x02, 0x40, 0x00, 1,
    };
    struct usb_bluetooth b;

    btusb_decode_config(c, sizeof c, &b);
    check(b.ok && b.event_endpoint == 1 && b.acl_in == 2 && b.acl_out == 2,
          "setting 1 first: setting 0's endpoints");
}

/* Two radios: the second interface's endpoints are not the first's. */
static void test_two_radios(void)
{
    static const uint8_t c[] = {
        9, 0x02, 0x3B, 0, 2, 1, 0, 0xE0, 50,
        9, 0x04, 0, 0, 2, 0xE0, 0x01, 0x01, 0,      /* no bulk OUT */
        7, 0x05, 0x81, 0x03, 0x10, 0x00, 1,
        7, 0x05, 0x82, 0x02, 0x40, 0x00, 1,
        9, 0x04, 1, 0, 3, 0xE0, 0x01, 0x01, 0,      /* a second radio */
        7, 0x05, 0x83, 0x03, 0x10, 0x00, 1,
        7, 0x05, 0x04, 0x02, 0x40, 0x00, 1,
        7, 0x05, 0x84, 0x02, 0x40, 0x00, 1,
    };
    struct usb_bluetooth b;

    btusb_decode_config(c, sizeof c, &b);
    check(!b.ok && b.acl_out == 0, "two radios: the second is not mixed in");
}

/* ---- HCI ---------------------------------------------------------------- */

static void test_commands(void)
{
    uint8_t out[300], params[256];
    unsigned n;

    n = hci_command(out, sizeof out, HCI_RESET, 0, 0);
    check(n == 3 && out[0] == 0x03 && out[1] == 0x0C && out[2] == 0,
          "Reset: 03 0C 00");
    n = hci_command(out, sizeof out, HCI_READ_BD_ADDR, 0, 0);
    check(n == 3 && out[0] == 0x09 && out[1] == 0x10, "Read BD_ADDR: 09 10 00");

    memset(params, 0xA5, sizeof params);
    n = hci_command(out, sizeof out, 0x0C13, params, 248);  /* Write Local Name */
    check(n == 251 && out[2] == 248 && out[250] == 0xA5,
          "Write Local Name: 248 parameter bytes");
    check(hci_command(out, sizeof out, 0x0C13, params, 256) == 0,
          "256 parameter bytes: refused");
    check(hci_command(out, 250, 0x0C13, params, 248) == 0,
          "too little room: refused");
    check(hci_command(out, 3, HCI_RESET, 0, 0) == 3, "exactly room: built");
}

/* The four answers, as a CSR8510 gives them. */
static const uint8_t RESET_DONE[] = { 0x0E, 4, 1, 0x03, 0x0C, 0x00 };
static const uint8_t VERSION_DONE[] = {
    0x0E, 12, 1, 0x01, 0x10, 0x00,
    6, 0xBB, 0x22, 6, 0x0A, 0x00, 0xBB, 0x22,
};
static const uint8_t ADDRESS_DONE[] = {
    0x0E, 10, 1, 0x09, 0x10, 0x00, 0x13, 0x71, 0xDA, 0x7D, 0x1A, 0x00,
};
static const uint8_t FEATURES_DONE[] = {
    0x0E, 12, 1, 0x03, 0x10, 0x00,
    0xFF, 0xFE, 0x8F, 0xFE, 0xD8, 0x3F, 0x5B, 0x87,
};

static bool answer_of(const uint8_t *bytes, unsigned length,
                      struct hci_answer *a)
{
    struct hci_event e;

    return hci_decode_event(bytes, length, &e) && hci_decode_answer(&e, a);
}

static void test_answers(void)
{
    struct hci_answer a;
    struct hci_version v;
    uint8_t address[6], features[8];
    char text[18];

    check(answer_of(RESET_DONE, sizeof RESET_DONE, &a) && a.complete
          && a.opcode == HCI_RESET && a.status == 0 && a.commands == 1
          && a.ret_length == 0, "Reset: complete, success");

    check(answer_of(VERSION_DONE, sizeof VERSION_DONE, &a)
          && hci_decode_version(&a, &v), "Read Local Version: read");
    check(v.hci_version == 6 && v.hci_subversion == 0x22BB
          && v.lmp_version == 6 && v.manufacturer == 10
          && v.lmp_subversion == 0x22BB, "Read Local Version: each field");
    check(strcmp(hci_version_name(v.hci_version), "4.0") == 0, "version 6: 4.0");
    check(strcmp(hci_manufacturer_name(v.manufacturer), "Qualcomm (CSR)") == 0,
          "company 10: Qualcomm (CSR)");
    check(strcmp(hci_manufacturer_name(93), "Realtek") == 0, "93: Realtek");
    check(hci_manufacturer_name(9999) == 0, "an unknown company: none");
    check(strcmp(hci_version_name(14), "6.0") == 0 && hci_version_name(15) == 0,
          "versions: 6.0 the last known");

    check(answer_of(ADDRESS_DONE, sizeof ADDRESS_DONE, &a)
          && hci_decode_address(&a, address), "Read BD_ADDR: read");
    hci_format_address(address, text);
    check(strcmp(text, "00:1A:7D:DA:71:13") == 0,
          "the address: most significant first");

    check(answer_of(FEATURES_DONE, sizeof FEATURES_DONE, &a)
          && hci_decode_features(&a, features), "Read Local Features: read");
    check(hci_feature(features, HCI_FEATURE_LE), "features: LE");
    check(!hci_feature(features, HCI_FEATURE_BREDR_NOT_SUPPORTED),
          "features: BR/EDR supported");
    check(hci_feature(features, HCI_FEATURE_SSP), "features: SSP");
    check(hci_feature(features, 0) && !hci_feature(features, 64),
          "features: bit 0, and none past 63");

    /* The wrong command's answer is not taken for this one's. */
    check(answer_of(VERSION_DONE, sizeof VERSION_DONE, &a)
          && !hci_decode_address(&a, address), "version's answer: no address");
}

static void test_bad_answers(void)
{
    static const uint8_t failed[] = {
        0x0E, 12, 1, 0x01, 0x10, 0x01, 6, 0, 0, 6, 0x0A, 0, 0, 0,
    };
    static const uint8_t short_version[] = {
        0x0E, 11, 1, 0x01, 0x10, 0x00, 6, 0, 0, 6, 0x0A, 0, 0,
    };
    static const uint8_t pending[] = { 0x0F, 4, 0x00, 1, 0x05, 0x04 };
    static const uint8_t only_count[] = { 0x0E, 3, 1, 0x00, 0x00 };
    static const uint8_t other[] = { 0x05, 4, 0, 0x40, 0x00, 0x13 };
    struct hci_answer a;
    struct hci_version v;
    struct hci_event e;

    check(answer_of(failed, sizeof failed, &a) && a.status == 1
          && !hci_decode_version(&a, &v), "a failed status: not read");
    check(answer_of(short_version, sizeof short_version, &a)
          && !hci_decode_version(&a, &v), "seven bytes of eight: not read");
    check(answer_of(pending, sizeof pending, &a) && !a.complete
          && a.opcode == 0x0405 && a.status == 0 && a.commands == 1,
          "Command Status: Create Connection pending");
    check(answer_of(only_count, sizeof only_count, &a) && a.opcode == 0
          && a.ret == 0, "Command Complete with opcode 0: a count only");
    check(hci_decode_event(other, sizeof other, &e)
          && !hci_decode_answer(&e, &a), "Disconnection Complete: no answer");
    check(!hci_decode_event(VERSION_DONE, sizeof VERSION_DONE - 1, &e),
          "an event a byte short: refused");
    check(!hci_decode_event(VERSION_DONE, 1, &e), "one byte: refused");
}

/* Bytes handed over `size` at a time, as an endpoint of that packet size
 * would; every whole event copied to `out`, and how many there were. */
static unsigned assemble(const uint8_t *in, unsigned length, unsigned size,
                         uint8_t out[][HCI_EVENT_MAX], unsigned *lengths)
{
    struct hci_assembler a = { .have = 0 };
    unsigned at = 0, events = 0;

    while (at < length) {
        unsigned packet = length - at < size ? length - at : size;
        unsigned off = 0;

        while (off < packet) {
            unsigned used;

            if (hci_assemble(&a, in + at + off, packet - off, &used)) {
                /* More than the stream holds is an assembler that does
                 * not let go: a failure, not an overrun. */
                if (events == 8) return 99;
                memcpy(out[events], a.bytes, a.have);
                lengths[events++] = a.have;
            }
            if (used == 0) return 99;           /* it would never finish */
            off += used;
        }
        at += packet;
    }
    return events;
}

static void test_assembly(void)
{
    static uint8_t out[8][HCI_EVENT_MAX];
    unsigned lengths[8];
    uint8_t stream[600];
    unsigned n = 0, events, size;
    bool all = true;

    /* Read Local Name's answer: 254 bytes, sixteen packets of 16. */
    stream[n++] = 0x0E;
    stream[n++] = 252;
    stream[n++] = 1;
    stream[n++] = 0x14;
    stream[n++] = 0x0C;
    stream[n++] = 0x00;
    memset(stream + n, 'k', 248);
    n += 248;

    /* Then two events, one starting inside the last packet of the name. */
    memcpy(stream + n, RESET_DONE, sizeof RESET_DONE);
    n += sizeof RESET_DONE;
    memcpy(stream + n, VERSION_DONE, sizeof VERSION_DONE);
    n += sizeof VERSION_DONE;

    events = assemble(stream, n, 16, out, lengths);
    check(events == 3, "16-byte packets: three events");
    check(lengths[0] == 254 && out[0][253] == 'k' && out[0][1] == 252,
          "the name's answer: whole across sixteen packets");
    check(lengths[1] == sizeof RESET_DONE
          && memcmp(out[1], RESET_DONE, sizeof RESET_DONE) == 0,
          "Reset's answer: from the name's last packet");
    check(lengths[2] == sizeof VERSION_DONE
          && memcmp(out[2], VERSION_DONE, sizeof VERSION_DONE) == 0,
          "the version: whole after it");

    /* Every packet size from one byte to all of it, the same three events. */
    for (size = 1; size <= n; size++) {
        if (assemble(stream, n, size, out, lengths) != 3
            || lengths[0] != 254 || lengths[1] != sizeof RESET_DONE
            || memcmp(out[2], VERSION_DONE, sizeof VERSION_DONE) != 0) {
            all = false;
        }
    }
    check(all, "every packet size: the same three events");

    /* An event with no parameters is whole at its header. */
    {
        static const uint8_t empty[] = { 0x10, 0 };     /* Hardware Error, 0 */
        struct hci_assembler a = { .have = 0 };
        unsigned used;

        check(hci_assemble(&a, empty, 2, &used) && used == 2 && a.have == 2,
              "no parameters: whole at its header");
    }
}

/* ---- btsim ---------------------------------------------------------- */

/* A command as the stack puts it in the ring: its type, then the packet. */
static unsigned ring_command(uint8_t *out, uint16_t opcode)
{
    out[0] = HCI_PACKET_COMMAND;
    return 1u + hci_command(out + 1, 300, opcode, 0, 0);
}

/* The simulator's next packet, read as the stack reads it. */
static bool sim_answer(struct btsim *s, struct hci_answer *a, uint8_t *keep)
{
    unsigned n = btsim_next(s, keep, BTSIM_PACKET_MAX);
    struct hci_event e;

    return n > 1 && keep[0] == HCI_PACKET_EVENT
        && hci_decode_event(keep + 1, n - 1, &e) && hci_decode_answer(&e, a);
}

static void test_btsim(void)
{
    static struct btsim s;
    uint8_t packet[300], keep[BTSIM_PACKET_MAX];
    struct hci_answer a;
    struct hci_version v;
    struct hci_buffers b;
    uint8_t address[6], features[8];
    char text[18];
    unsigned i;

    btsim_start(&s);
    check(btsim_next(&s, keep, sizeof keep) == 0, "btsim: silent until asked");

    btsim_take(&s, packet, ring_command(packet, HCI_RESET));
    check(sim_answer(&s, &a, keep) && a.opcode == HCI_RESET && a.status == 0
          && s.reset, "btsim: Reset answered");

    /* The setup the stack sends, in its order, each answer read back by the
     * decoders the stack uses. */
    btsim_take(&s, packet, ring_command(packet, HCI_READ_LOCAL_VERSION));
    btsim_take(&s, packet, ring_command(packet, HCI_READ_BD_ADDR));
    btsim_take(&s, packet, ring_command(packet, HCI_READ_LOCAL_FEATURES));
    btsim_take(&s, packet, ring_command(packet, HCI_READ_BUFFER_SIZE));

    check(sim_answer(&s, &a, keep) && hci_decode_version(&a, &v)
          && v.hci_version == BTSIM_VERSION && v.manufacturer == BTSIM_COMPANY,
          "btsim: Bluetooth 5.1, the testing company");
    check(strcmp(hci_version_name(v.hci_version), "5.1") == 0
          && hci_manufacturer_name(v.manufacturer) != 0,
          "btsim: its version and company have names");
    check(sim_answer(&s, &a, keep) && hci_decode_address(&a, address),
          "btsim: an address");
    hci_format_address(address, text);
    check(strcmp(text, "02:4B:4F:53:4D:4F") == 0,
          "btsim: 02:4B:4F:53:4D:4F, locally administered");
    check(sim_answer(&s, &a, keep) && hci_decode_features(&a, features)
          && hci_feature(features, HCI_FEATURE_LE)
          && hci_feature(features, HCI_FEATURE_SSP)
          && !hci_feature(features, HCI_FEATURE_BREDR_NOT_SUPPORTED),
          "btsim: Classic and LE, with SSP");
    check(sim_answer(&s, &a, keep) && hci_decode_buffers(&a, &b)
          && b.acl_length == BTSIM_ACL_LENGTH && b.acl_packets == BTSIM_ACL_PACKETS,
          "btsim: 8 ACL buffers of 1021 bytes");
    check(btsim_next(&s, keep, sizeof keep) == 0, "btsim: each answered once");

    /* What it does not know, it says so as a controller does. */
    btsim_take(&s, packet, ring_command(packet, 0x0C13));
    check(sim_answer(&s, &a, keep) && a.complete && a.opcode == 0x0C13
          && a.status == 0x01, "btsim: Unknown HCI Command for the rest");

    /* What is not a whole command is dropped, as a controller drops it. */
    i = ring_command(packet, HCI_RESET);
    btsim_take(&s, packet, i - 1u);
    packet[0] = HCI_PACKET_ACL;
    btsim_take(&s, packet, i);
    btsim_take(&s, packet, 0);
    check(btsim_next(&s, keep, sizeof keep) == 0 && s.commands == 6,
          "btsim: short packets and non-commands ignored");

    /* More than it holds: the rest counted, never written past the queue. */
    for (i = 0; i < BTSIM_QUEUE + 3u; i++) {
        btsim_take(&s, packet, ring_command(packet, HCI_READ_BD_ADDR));
    }
    check(s.count == BTSIM_QUEUE && s.dropped == 3,
          "btsim: a full queue counts what it drops");

    /* A Reset ends what was waiting, and is answered alone. */
    btsim_take(&s, packet, ring_command(packet, HCI_RESET));
    check(s.count == 1 && sim_answer(&s, &a, keep) && a.opcode == HCI_RESET,
          "btsim: Reset clears what was waiting");
}

int main(void)
{
    test_adapter();
    test_voice_ignored();
    test_cut_short();
    test_malformed();
    test_not_bluetooth();
    test_setting_one_first();
    test_two_radios();
    test_commands();
    test_answers();
    test_bad_answers();
    test_assembly();
    test_btsim();

    printf("btdecode: %d checks, %d failed\n", checks + fails, fails);

    return fails == 0 ? 0 : 1;
}
